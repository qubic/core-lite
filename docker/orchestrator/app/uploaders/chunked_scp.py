from __future__ import annotations

import asyncio
import hashlib
import json
import logging
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Optional

from app.models import UploadResult
from app.uploaders.scp import ScpUploader

logger = logging.getLogger(__name__)

DEFAULT_CHUNK_SIZE_MB = 512
DEFAULT_CHUNK_TIMEOUT = 600
DEFAULT_PARALLEL_CHUNKS = 2
DEFAULT_MIN_CHUNK_SIZE_GB = 2
DEFAULT_CHUNK_RETRY_COUNT = 3
DEFAULT_CHUNK_RETRY_DELAY = 30
BUFFER_SIZE = 1024 * 1024


@dataclass
class ChunkEntry:
    index: int
    filename: str
    size: int
    checksum: str
    uploaded: bool = False

    def to_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict) -> ChunkEntry:
        return cls(**data)


@dataclass
class ChunkManifest:
    """Tracks chunked upload progress. Persisted inside snap-t{tick}/.manifest.json
    during upload so we can resume after failures. Removed once upload completes."""

    epoch: int
    tick: int
    total_size: int
    status: str  # "uploading" | "complete" | "failed"
    chunks: list[ChunkEntry] = field(default_factory=list)
    created_at: str = ""
    node_id: str = ""

    def to_json(self) -> str:
        return json.dumps({
            "epoch": self.epoch,
            "tick": self.tick,
            "total_size": self.total_size,
            "status": self.status,
            "chunks": [c.to_dict() for c in self.chunks],
            "created_at": self.created_at,
            "node_id": self.node_id,
        }, indent=2)

    @classmethod
    def from_json(cls, data: str) -> ChunkManifest:
        d = json.loads(data)
        chunks = [ChunkEntry.from_dict(c) for c in d.get("chunks", [])]
        return cls(
            epoch=d["epoch"],
            tick=d["tick"],
            total_size=d["total_size"],
            status=d["status"],
            chunks=chunks,
            created_at=d.get("created_at", ""),
            node_id=d.get("node_id", ""),
        )

    @property
    def pending_chunks(self) -> list[ChunkEntry]:
        return [c for c in self.chunks if not c.uploaded]

    @property
    def uploaded_count(self) -> int:
        return sum(1 for c in self.chunks if c.uploaded)

    @property
    def uploaded_bytes(self) -> int:
        return sum(c.size for c in self.chunks if c.uploaded)


class ChunkedScpUploader(ScpUploader):
    """Uploads pre-split snapshot chunks to a remote folder snap-t{tick}/.

    Chunks stay in place on the remote — no reassembly step. The caller is
    expected to publish a sidecar/index that lists the chunks so downloaders
    can fetch them.

    For non-chunked uploads (small files, sidecars, index), this class
    inherits ScpUploader.upload() / put_small_file() / etc.
    """

    def __init__(
        self,
        host: str,
        user: str = "",
        port: int = 22,
        dest_path: str = "/snapshots",
        key_file: str = "",
        timeout: int = 1800,
        chunk_size_mb: int = DEFAULT_CHUNK_SIZE_MB,
        chunk_timeout: int = DEFAULT_CHUNK_TIMEOUT,
        parallel_chunks: int = DEFAULT_PARALLEL_CHUNKS,
        min_chunk_size_gb: int = DEFAULT_MIN_CHUNK_SIZE_GB,
        chunk_retry_count: int = DEFAULT_CHUNK_RETRY_COUNT,
        chunk_retry_delay: int = DEFAULT_CHUNK_RETRY_DELAY,
    ) -> None:
        super().__init__(
            host=host,
            user=user,
            port=port,
            dest_path=dest_path,
            key_file=key_file,
            timeout=timeout,
        )
        self._chunk_size_mb = chunk_size_mb
        self._chunk_timeout = chunk_timeout
        self._parallel_chunks = parallel_chunks
        self._min_chunk_size = min_chunk_size_gb * 1024 * 1024 * 1024
        self._chunk_retry_count = chunk_retry_count
        self._chunk_retry_delay = chunk_retry_delay

    @staticmethod
    def snapshot_dir_key(epoch: int, tick: int) -> str:
        """Remote folder holding the chunks for a given snapshot."""
        return f"{epoch}/snap-t{tick}"

    def _manifest_remote_key(self, epoch: int, tick: int) -> str:
        return f"{self.snapshot_dir_key(epoch, tick)}/.manifest.json"

    async def upload_chunks(
        self,
        chunk_source,
        metadata: dict,
        total_uncompressed_size: int = 0,
    ) -> UploadResult:
        """Upload snapshot chunks, streaming them from ``chunk_source``.

        ``chunk_source`` may be either:

        * a sequence (``list``/``tuple``) of ``Path`` objects pointing at
          already-complete chunk files, or
        * an async iterable yielding either ``Path`` objects or
          ``(Path, total_uncompressed_size)`` tuples — used to pipeline
          packaging and upload (chunks are uploaded as the packager finalises
          them).

        Each chunk is uploaded on its own task with independent retry/verify.
        Successful chunks are deleted from the local staging directory. If a
        chunk exhausts its retries the whole upload fails and remaining
        in-flight tasks are cancelled.
        """
        from datetime import datetime, timezone

        start = time.monotonic()
        epoch = int(metadata.get("epoch", 0))
        tick = int(metadata.get("tick", 0))
        node_id = str(metadata.get("node_id", ""))
        snap_dir_key = self.snapshot_dir_key(epoch, tick)

        # Streaming packaging produces a fresh set of chunks whose byte content
        # is not guaranteed to match any previous attempt — wipe anything
        # lingering from a prior run so we start from a clean slate.
        await self._cleanup_remote_snapshot_dir(epoch, tick)
        await self._ensure_remote_dir_key(snap_dir_key)

        manifest = ChunkManifest(
            epoch=epoch,
            tick=tick,
            total_size=0,
            status="uploading",
            chunks=[],
            created_at=datetime.now(timezone.utc).isoformat(),
            node_id=node_id,
        )
        await self._save_remote_manifest(epoch, tick, manifest)

        semaphore = asyncio.Semaphore(self._parallel_chunks)
        upload_start = time.monotonic()

        failure_msgs: Optional[str] = None
        try:
            async with asyncio.TaskGroup() as tg:
                async for chunk_path in self._iter_chunks(chunk_source):
                    size = chunk_path.stat().st_size
                    checksum = await asyncio.to_thread(
                        self._compute_file_checksum, chunk_path
                    )
                    entry = ChunkEntry(
                        index=len(manifest.chunks),
                        filename=chunk_path.name,
                        size=size,
                        checksum=checksum,
                        uploaded=False,
                    )
                    manifest.chunks.append(entry)
                    manifest.total_size += size
                    await self._save_remote_manifest(epoch, tick, manifest)

                    tg.create_task(self._upload_chunk_with_retries(
                        chunk_path=chunk_path,
                        entry=entry,
                        epoch=epoch,
                        tick=tick,
                        manifest=manifest,
                        semaphore=semaphore,
                        upload_start=upload_start,
                    ))
        except* Exception as eg:
            failure_msgs = "; ".join(str(e) for e in eg.exceptions)

        if failure_msgs is not None:
            logger.error(f"Chunked upload failed: {failure_msgs}")
            manifest.status = "failed"
            try:
                await self._save_remote_manifest(epoch, tick, manifest)
            except Exception:
                pass
            return UploadResult(
                success=False,
                error_message=failure_msgs,
                duration_seconds=time.monotonic() - start,
            )

        if not manifest.chunks:
            manifest.status = "failed"
            await self._save_remote_manifest(epoch, tick, manifest)
            return UploadResult(
                success=False,
                error_message="No chunks produced",
                duration_seconds=time.monotonic() - start,
            )

        manifest.status = "complete"
        await self._save_remote_manifest(epoch, tick, manifest)
        await self.delete_file(self._manifest_remote_key(epoch, tick))

        total_size = manifest.total_size
        logger.info(
            f"Chunk upload complete: {len(manifest.chunks)} chunks, "
            f"{total_size} bytes in snap-t{tick}/"
        )

        remote_target = f"{self._target()}:{self._remote_path(snap_dir_key)}"
        return UploadResult(
            success=True,
            remote_url=remote_target,
            bytes_uploaded=total_size,
            chunks=[c.to_dict() for c in manifest.chunks],
            remote_dir=snap_dir_key,
            duration_seconds=time.monotonic() - start,
        )

    @staticmethod
    async def _iter_chunks(source):
        """Normalise a sync sequence or async iterable into an async iterator of Paths.

        Items yielded by the source may be ``Path`` or ``(Path, extra)`` — the
        extra element (if present) is ignored.
        """
        if hasattr(source, "__aiter__"):
            async for item in source:
                yield item[0] if isinstance(item, tuple) else item
        else:
            for item in source:
                yield item[0] if isinstance(item, tuple) else item

    async def _upload_chunk_with_retries(
        self,
        chunk_path: Path,
        entry: "ChunkEntry",
        epoch: int,
        tick: int,
        manifest: ChunkManifest,
        semaphore: asyncio.Semaphore,
        upload_start: float,
    ) -> None:
        """Upload a single chunk, retrying on failure. Raises if retries exhausted."""
        last_err: Optional[str] = None
        for attempt in range(self._chunk_retry_count + 1):
            async with semaphore:
                ok, err = await self._try_upload_chunk(
                    chunk_path=chunk_path,
                    entry=entry,
                    epoch=epoch,
                    tick=tick,
                    manifest=manifest,
                    upload_start=upload_start,
                    attempt=attempt,
                )
            if ok:
                # Free local staging as we go.
                try:
                    if chunk_path.exists():
                        chunk_path.unlink()
                except OSError as e:
                    logger.debug(
                        f"Could not delete local chunk {chunk_path.name}: {e}"
                    )
                return
            last_err = err
            if attempt < self._chunk_retry_count:
                logger.warning(
                    f"Chunk {entry.filename} attempt {attempt + 1} failed "
                    f"({err}); retrying in {self._chunk_retry_delay}s"
                )
                await asyncio.sleep(self._chunk_retry_delay)
        raise RuntimeError(
            f"Chunk {entry.filename} failed after "
            f"{self._chunk_retry_count + 1} attempts: {last_err}"
        )

    def _compute_file_checksum(self, file_path: Path) -> str:
        sha256 = hashlib.sha256()
        with open(file_path, "rb") as f:
            while chunk := f.read(BUFFER_SIZE):
                sha256.update(chunk)
        return sha256.hexdigest()

    async def _ensure_remote_dir_key(self, remote_key: str) -> None:
        """Ensure a remote directory exists, given a relative key."""
        remote_dir = self._remote_path(remote_key)
        code, _, stderr = await self._run_ssh(
            "mkdir", "-p", remote_dir, timeout=30
        )
        if code != 0:
            logger.warning(
                f"Failed to create remote dir {remote_dir}: {stderr.strip()}"
            )

    async def _load_remote_manifest(
        self, epoch: int, tick: int
    ) -> Optional[ChunkManifest]:
        content = await self.get_small_file(self._manifest_remote_key(epoch, tick))
        if content:
            try:
                return ChunkManifest.from_json(content.decode())
            except Exception as e:
                logger.warning(f"Failed to parse remote manifest: {e}")
        return None

    async def _save_remote_manifest(
        self, epoch: int, tick: int, manifest: ChunkManifest
    ) -> bool:
        content = manifest.to_json().encode()
        return await self.put_small_file(
            self._manifest_remote_key(epoch, tick), content
        )

    async def _try_upload_chunk(
        self,
        chunk_path: Path,
        entry: ChunkEntry,
        epoch: int,
        tick: int,
        manifest: ChunkManifest,
        upload_start: float,
        attempt: int,
    ) -> tuple[bool, Optional[str]]:
        """One attempt at uploading + verifying a single chunk.

        Returns (ok, error_message). ``ok=True`` means the chunk is safely on
        the remote side with checksum verified; the caller is free to delete
        the local staging file.
        """
        if not chunk_path.exists():
            return False, f"local chunk file {chunk_path} not found"

        snap_dir_key = self.snapshot_dir_key(epoch, tick)
        remote_chunk_key = f"{snap_dir_key}/{entry.filename}"
        scp_target = f"{self._target()}:{self._remote_path(remote_chunk_key)}"

        cmd = ["scp"] + self._scp_opts() + [str(chunk_path), scp_target]
        chunk_start = time.monotonic()

        try:
            proc = await asyncio.create_subprocess_exec(
                *cmd,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            try:
                _, stderr = await asyncio.wait_for(
                    proc.communicate(), timeout=self._chunk_timeout
                )
            except asyncio.TimeoutError:
                proc.kill()
                await proc.wait()
                return False, f"timed out after {self._chunk_timeout}s"

            if proc.returncode != 0:
                return False, (
                    f"scp exit {proc.returncode}: "
                    f"{stderr.decode().strip()}"
                )

            remote_chunk_path = self._remote_path(remote_chunk_key)
            if not await self._verify_remote_chunk_checksum(
                remote_chunk_path, entry.checksum, entry.filename
            ):
                await self._run_ssh(f"rm -f {remote_chunk_path}", timeout=30)
                return False, "remote checksum mismatch"

            entry.uploaded = True
            await self._save_remote_manifest(epoch, tick, manifest)

            chunk_duration = time.monotonic() - chunk_start
            total_elapsed = time.monotonic() - upload_start
            speed_mbps = (
                (entry.size / chunk_duration) / (1024 * 1024)
                if chunk_duration > 0 else 0
            )
            logger.info(
                f"Uploaded chunk {entry.filename} "
                f"({manifest.uploaded_count} complete, "
                f"{manifest.uploaded_bytes / (1024**3):.2f} GB) "
                f"@ {speed_mbps:.1f} MB/s - {total_elapsed:.0f}s elapsed"
                + (f" [attempt {attempt + 1}]" if attempt > 0 else "")
            )
            return True, None

        except Exception as e:
            return False, f"{type(e).__name__}: {e}"

    async def _verify_remote_chunk_checksum(
        self,
        remote_path: str,
        expected_checksum: str,
        chunk_name: str,
    ) -> bool:
        try:
            code, stdout, stderr = await self._run_ssh(
                f"sha256sum {remote_path}", timeout=120
            )
            if code != 0:
                logger.error(
                    f"Remote chunk checksum command failed for {chunk_name}: "
                    f"{stderr.strip()}"
                )
                return False

            remote_checksum = stdout.strip().split()[0]
            if remote_checksum != expected_checksum:
                logger.error(
                    f"Chunk {chunk_name} checksum mismatch: "
                    f"expected {expected_checksum[:16]}..., "
                    f"got {remote_checksum[:16]}..."
                )
                return False

            logger.debug(f"Chunk {chunk_name} checksum verified")
            return True

        except asyncio.TimeoutError:
            logger.error(f"Chunk {chunk_name} checksum verification timed out")
            return False
        except Exception as e:
            logger.error(f"Chunk {chunk_name} checksum verification error: {e}")
            return False

    async def _cleanup_remote_snapshot_dir(self, epoch: int, tick: int) -> bool:
        return await self.delete_remote_dir(self.snapshot_dir_key(epoch, tick))

    def get_name(self) -> str:
        return "chunked_scp"
