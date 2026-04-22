from __future__ import annotations

import glob
import hashlib
import logging
import os
import re
import shutil
import struct
import tempfile
import zipfile
import zlib
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Optional

from app.downloaders.base import BaseDownloader
from app.models import SnapshotMeta

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# ZIP format constants
# ---------------------------------------------------------------------------
_LOCAL_FILE_HEADER_SIG = 0x04034B50
_CENTRAL_DIR_SIG = 0x02014B50
_END_CENTRAL_DIR_SIG = 0x06054B50
_ZIP64_END_SIG = 0x06064B50
_ZIP64_LOCATOR_SIG = 0x07064B50
_CHUNK_SIZE = 1024 * 1024  # 1 MB


def _compress_file_worker(
    file_path: str,
    arcname: str,
    tmp_dir: str,
) -> dict:
    """Compress a single file using raw deflate (zlib releases the GIL).

    Writes compressed data to a temp file and returns metadata needed
    to assemble the final ZIP archive.
    """
    crc = 0
    file_size = 0
    compressed_size = 0

    compressor = zlib.compressobj(
        zlib.Z_DEFAULT_COMPRESSION, zlib.DEFLATED, -15
    )

    fd, tmp_path = tempfile.mkstemp(dir=tmp_dir, suffix=".zpart")
    try:
        with os.fdopen(fd, "wb") as tmp_f, open(file_path, "rb") as src:
            while True:
                chunk = src.read(_CHUNK_SIZE)
                if not chunk:
                    break
                crc = zlib.crc32(chunk, crc)
                file_size += len(chunk)
                compressed = compressor.compress(chunk)
                if compressed:
                    tmp_f.write(compressed)
                    compressed_size += len(compressed)
            # Flush remaining
            tail = compressor.flush(zlib.Z_FINISH)
            if tail:
                tmp_f.write(tail)
                compressed_size += len(tail)
    except BaseException:
        # Clean up temp file on error
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise

    return {
        "arcname": arcname,
        "tmp_path": tmp_path,
        "crc32": crc & 0xFFFFFFFF,
        "file_size": file_size,
        "compressed_size": compressed_size,
    }


def _extract_batch_worker(
    zip_path: str,
    entry_names: list[str],
    dest_dir: str,
) -> list[dict]:
    """Extract a batch of files from a ZIP archive (thread-safe).

    Each worker opens its own ZipFile handle once and extracts all
    assigned entries.  This avoids re-reading the central directory
    for every file.  zlib decompression releases the GIL.
    """
    results = []
    with zipfile.ZipFile(zip_path, "r") as zf:
        for entry_name in entry_names:
            target = Path(dest_dir) / entry_name
            target.parent.mkdir(parents=True, exist_ok=True)

            file_size = 0
            with zf.open(entry_name) as src, open(target, "wb") as dst:
                while True:
                    chunk = src.read(_CHUNK_SIZE)
                    if not chunk:
                        break
                    dst.write(chunk)
                    file_size += len(chunk)

            results.append({"name": entry_name, "size": file_size})
    return results


# Files the Qubic node needs per epoch
CRITICAL_FILES = ["spectrum", "universe"]

# Snapshot directory files written by saveAllNodeStates()
SNAPSHOT_DIR_FILES = [
    "system.snp",
    "snapshotNodeMiningState",
    "snapshotSpectrumDigest",
    "snapshotUniverseDigest",
    "snapshotComputerDigest",
    "snapshotMinerSolutionFlag",
]


class StateManager:
    """Manages local state files for the Qubic node."""

    def __init__(self, data_dir: Path, downloader: BaseDownloader) -> None:
        self._data_dir = data_dir
        self._downloader = downloader

    @property
    def data_dir(self) -> Path:
        return self._data_dir

    def get_local_epoch(self) -> Optional[int]:
        """Detect the epoch from existing spectrum files."""
        pattern = str(self._data_dir / "spectrum.*")
        files = glob.glob(pattern)
        if not files:
            return None
        # Get the highest epoch number
        epochs = []
        for f in files:
            ext = Path(f).suffix.lstrip(".")
            try:
                epochs.append(int(ext))
            except ValueError:
                continue
        return max(epochs) if epochs else None

    def has_valid_state_files(self, epoch: int) -> bool:
        """Check if critical state files exist for the given epoch."""
        for name in CRITICAL_FILES:
            path = self._data_dir / f"{name}.{epoch}"
            if not path.exists() or path.stat().st_size == 0:
                logger.debug(f"Missing or empty: {path}")
                return False
        return True

    def get_snapshot_directory(self, epoch: int) -> Optional[Path]:
        """Return the snapshot directory path, or None if not found."""
        for name in [f"ep{epoch}", "ep"]:
            snap_dir = self._data_dir / name
            if snap_dir.is_dir():
                return snap_dir
        return None

    def has_snapshot_directory(self, epoch: int) -> bool:
        """Check if snapshot state directory exists with required files."""
        snap_dir = self.get_snapshot_directory(epoch)
        if snap_dir is None:
            return False
        for name in SNAPSHOT_DIR_FILES:
            if not (snap_dir / name).exists():
                return False
        return True

    async def download_epoch_files(
        self, epoch: int, url: str
    ) -> bool:
        """Download and extract epoch files from a URL."""
        zip_path = self._data_dir / "epoch_files.zip"
        try:
            await self._downloader.download(url, zip_path)
            self._extract_and_rename(zip_path, epoch)
            return True
        except Exception as e:
            logger.error(f"Failed to download epoch files: {e}")
            return False
        finally:
            if zip_path.exists():
                zip_path.unlink()

    async def download_snapshot(
        self, snapshot_meta: SnapshotMeta
    ) -> bool:
        """Download and extract a snapshot.

        If ``snapshot_meta.chunks`` is populated we stream each chunk into
        ``tar -I "zstd -T0"``; otherwise we fall back to the single-file
        archive path.
        """
        if snapshot_meta.chunks:
            try:
                await self._download_and_extract_chunked(snapshot_meta)
                return True
            except Exception as e:
                logger.error(f"Failed to download chunked snapshot: {e}")
                return False

        url = snapshot_meta.url
        if url.endswith(".tar.zst") or ".tar.zst" in url:
            archive_path = self._data_dir / "snapshot.tar.zst"
        else:
            archive_path = self._data_dir / "snapshot.zip"

        try:
            await self._downloader.download(url, archive_path)
            self._extract_archive(archive_path, snapshot_meta.epoch)
            return True
        except Exception as e:
            logger.error(f"Failed to download snapshot: {e}")
            return False
        finally:
            if archive_path.exists():
                archive_path.unlink()

    async def _download_and_extract_chunked(
        self, snapshot_meta: SnapshotMeta
    ) -> None:
        """Download chunks one at a time and stream them into tar -I zstd -xf -.

        Each chunk is written to disk briefly (to verify checksum) then piped
        into tar stdin and deleted. Peak extra disk usage is one chunk.
        """
        import asyncio
        import subprocess
        import time

        tmp_dir = self._data_dir / ".snap-chunks"
        tmp_dir.mkdir(parents=True, exist_ok=True)

        total_size = sum(c.size for c in snapshot_meta.chunks)
        logger.info(
            f"Downloading chunked snapshot: {len(snapshot_meta.chunks)} chunks, "
            f"{total_size / (1024**3):.2f} GB"
        )
        start = time.monotonic()

        proc = await asyncio.create_subprocess_exec(
            "tar", "-I", "zstd -T0",
            "-xf", "-",
            "-C", str(self._data_dir),
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        assert proc.stdin is not None

        downloaded_bytes = 0
        try:
            for i, chunk_info in enumerate(snapshot_meta.chunks):
                tmp_path = tmp_dir / chunk_info.filename
                logger.info(
                    f"Downloading chunk {i + 1}/{len(snapshot_meta.chunks)}: "
                    f"{chunk_info.filename} "
                    f"({chunk_info.size / (1024**2):.0f} MB)"
                )
                await self._downloader.download(chunk_info.url, tmp_path)

                actual_size = tmp_path.stat().st_size
                if chunk_info.size and actual_size != chunk_info.size:
                    raise RuntimeError(
                        f"chunk {chunk_info.filename} size mismatch: "
                        f"expected {chunk_info.size}, got {actual_size}"
                    )
                if chunk_info.checksum:
                    actual_sum = await asyncio.to_thread(
                        self.compute_checksum, tmp_path
                    )
                    if actual_sum != chunk_info.checksum:
                        raise RuntimeError(
                            f"chunk {chunk_info.filename} checksum mismatch"
                        )

                with open(tmp_path, "rb") as f:
                    while True:
                        buf = f.read(_CHUNK_SIZE)
                        if not buf:
                            break
                        proc.stdin.write(buf)
                        await proc.stdin.drain()
                tmp_path.unlink()

                downloaded_bytes += actual_size
                elapsed = time.monotonic() - start
                speed = (downloaded_bytes / elapsed) / (1024 * 1024) if elapsed > 0 else 0
                logger.info(
                    f"  chunk {i + 1}/{len(snapshot_meta.chunks)} done "
                    f"({downloaded_bytes / (1024**3):.2f}/{total_size / (1024**3):.2f} GB, "
                    f"{speed:.1f} MB/s)"
                )

            proc.stdin.close()
            await proc.wait()
            if proc.returncode != 0:
                stderr = (await proc.stderr.read()).decode().strip()
                raise RuntimeError(
                    f"tar extraction failed (exit {proc.returncode}): {stderr}"
                )
        except BaseException:
            if proc.returncode is None:
                try:
                    proc.kill()
                except ProcessLookupError:
                    pass
                await proc.wait()
            raise
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)

        duration = time.monotonic() - start
        logger.info(
            f"Chunked snapshot extracted: {downloaded_bytes / (1024**3):.2f} GB "
            f"in {duration:.0f}s"
        )

        self._rename_extracted_files(snapshot_meta.epoch)

    def _extract_archive(self, archive_path: Path, epoch: int) -> None:
        """Extract archive (auto-detect ZIP or tar.zst) and rename files."""
        if archive_path.suffix == ".zst" or archive_path.name.endswith(".tar.zst"):
            self._extract_tar_zst(archive_path, epoch)
        else:
            self._extract_zip(archive_path, epoch)

    def _extract_tar_zst(self, archive_path: Path, epoch: int) -> None:
        """Extract tar.zst archive using system tar+zstd."""
        import subprocess
        import time

        logger.info(f"Extracting tar.zst: {archive_path} to {self._data_dir}")
        start_time = time.monotonic()

        cmd = [
            "tar",
            "-I", "zstd -T0",  # Multi-threaded decompression
            "-xf", str(archive_path),
            "-C", str(self._data_dir),
        ]

        try:
            # Run extraction without verbose mode for speed
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
            )

            if result.returncode != 0:
                raise RuntimeError(
                    f"tar extraction failed (exit {result.returncode}): "
                    f"{result.stderr.strip()}"
                )

            duration = time.monotonic() - start_time
            archive_size = archive_path.stat().st_size / (1024**3)
            speed = archive_size / duration if duration > 0 else 0
            logger.info(
                f"Extracted {archive_size:.2f} GB in {duration:.0f}s "
                f"({speed:.2f} GB/s)"
            )

        except FileNotFoundError:
            raise RuntimeError("zstd not found. Install zstd package.")

        # Rename files to match epoch
        self._rename_extracted_files(epoch)

    def _extract_zip(self, zip_path: Path, epoch: int) -> None:
        """Extract zip in parallel and rename files to match the epoch."""
        logger.info(f"Extracting ZIP: {zip_path} to {self._data_dir}")

        with zipfile.ZipFile(zip_path, "r") as zf:
            entries = [i.filename for i in zf.infolist() if not i.is_dir()]

        workers = min(len(entries) or 1, os.cpu_count() or 4)
        logger.info(
            f"Extracting {len(entries)} files with {workers} workers"
        )

        # Split entries into batches so each worker opens the zip once
        batch_size = max(1, (len(entries) + workers - 1) // workers)
        batches = [
            entries[i : i + batch_size]
            for i in range(0, len(entries), batch_size)
        ]
        logger.info(
            f"Split into {len(batches)} batches of ~{batch_size} files"
        )

        with ThreadPoolExecutor(max_workers=workers) as pool:
            futures = {}
            for batch in batches:
                future = pool.submit(
                    _extract_batch_worker,
                    str(zip_path),
                    batch,
                    str(self._data_dir),
                )
                futures[future] = len(batch)

            done_files = 0
            for future in as_completed(futures):
                try:
                    results = future.result()
                    done_files += len(results)
                    logger.info(
                        f"Extracted batch ({len(results)} files, "
                        f"{done_files}/{len(entries)} total)"
                    )
                except Exception as exc:
                    logger.error(f"Batch extraction failed: {exc}")
                    raise

        # Rename files to match epoch
        self._rename_extracted_files(epoch)

    def _rename_extracted_files(self, epoch: int) -> None:
        """Rename files with .000 extension (or wrong epoch) to current epoch."""
        for path in self._data_dir.iterdir():
            if not path.is_file():
                continue
            name = path.name
            stem = path.stem
            ext = path.suffix.lstrip(".")

            # Only process known file types
            is_state_file = stem.startswith(("spectrum", "universe", "contract"))
            if not is_state_file:
                continue

            # Skip if already correct epoch
            if ext == str(epoch):
                continue

            # Rename .000 or other epoch to current epoch
            try:
                int(ext)  # Ensure extension is numeric
                new_path = self._data_dir / f"{stem}.{epoch}"
                path.rename(new_path)
                logger.info(f"Renamed {name} -> {new_path.name}")
            except ValueError:
                continue

    def list_state_files(self, epoch: int) -> list[Path]:
        """List all state files for a given epoch."""
        files = []
        for pattern in [
            f"spectrum.{epoch}",
            f"universe.{epoch}",
            f"contract*.{epoch}",
            f"score.{epoch}",
            f"custom_mining_cache.{epoch}",
            "system",
        ]:
            files.extend(self._data_dir.glob(pattern))
        return sorted(files)

    def list_snapshot_files(self, epoch: int) -> list[Path]:
        """List all files in the snapshot directory for packaging."""
        files = []
        # Main state files
        files.extend(self.list_state_files(epoch))
        # Snapshot directory
        for snap_dir_name in [f"ep{epoch}", "ep"]:
            snap_dir = self._data_dir / snap_dir_name
            if snap_dir.is_dir():
                for f in snap_dir.rglob("*"):
                    if f.is_file():
                        files.append(f)
                break
        # Page files from SwapVirtualMemory (.pg files in subdirectories)
        # Only include dirs whose trailing number matches the epoch
        # e.g. td00data199, tickdata199 for epoch 199
        for sub_dir in sorted(self._data_dir.iterdir()):
            if not sub_dir.is_dir():
                continue
            if sub_dir.name.startswith("ep"):
                continue  # already handled above
            match = re.search(r"(\d+)$", sub_dir.name)
            if not match or int(match.group(1)) != epoch:
                continue
            pg_files = sorted(sub_dir.glob("*.pg"))
            if pg_files:
                files.extend(pg_files)
        return files

    @staticmethod
    def _build_zip_from_entries(
        entries: list[dict], archive_path: Path
    ) -> None:
        """Assemble a ZIP file from pre-compressed entries.

        Each entry dict has: arcname, tmp_path, crc32, file_size,
        compressed_size.  Uses ZIP64 unconditionally for simplicity.
        """
        central_dir_entries: list[bytes] = []
        with open(archive_path, "wb") as out:
            for entry in entries:
                arcname_bytes = entry["arcname"].encode("utf-8")
                offset = out.tell()

                # ZIP64 extra field for local header:
                #   Header ID (0x0001) + Data Size (28) +
                #   Original Size (8) + Compressed Size (8) + Offset (8)
                # We include offset in local extra even though it's not
                # strictly required — keeps it consistent with central dir.
                zip64_extra = struct.pack(
                    "<HH QQQ",
                    0x0001,
                    24,
                    entry["file_size"],
                    entry["compressed_size"],
                    offset,
                )

                # Local file header
                local_header = struct.pack(
                    "<I HHHHH III HH",
                    _LOCAL_FILE_HEADER_SIG,
                    45,                       # version needed (4.5 for ZIP64)
                    0,                        # general purpose bit flag
                    8,                        # compression method (deflate)
                    0,                        # last mod time
                    0,                        # last mod date
                    entry["crc32"],
                    0xFFFFFFFF,               # compressed size (ZIP64)
                    0xFFFFFFFF,               # uncompressed size (ZIP64)
                    len(arcname_bytes),
                    len(zip64_extra),
                )
                out.write(local_header)
                out.write(arcname_bytes)
                out.write(zip64_extra)

                # Copy pre-compressed data from temp file
                with open(entry["tmp_path"], "rb") as tmp_f:
                    while True:
                        chunk = tmp_f.read(_CHUNK_SIZE)
                        if not chunk:
                            break
                        out.write(chunk)

                # Central directory entry
                cd_zip64_extra = struct.pack(
                    "<HH QQQ",
                    0x0001,
                    24,
                    entry["file_size"],
                    entry["compressed_size"],
                    offset,
                )
                cd_entry = struct.pack(
                    "<I HHHH HH I II HH HHH II",
                    _CENTRAL_DIR_SIG,
                    45,                       # version made by
                    45,                       # version needed
                    0,                        # general purpose bit flag
                    8,                        # compression method
                    0,                        # last mod time
                    0,                        # last mod date
                    entry["crc32"],
                    0xFFFFFFFF,               # compressed size (ZIP64)
                    0xFFFFFFFF,               # uncompressed size (ZIP64)
                    len(arcname_bytes),
                    len(cd_zip64_extra),
                    0,                        # file comment length
                    0,                        # disk number start
                    0,                        # internal file attributes
                    0,                        # external file attributes
                    0xFFFFFFFF,               # local header offset (ZIP64)
                )
                central_dir_entries.append(
                    cd_entry + arcname_bytes + cd_zip64_extra
                )

            # Write central directory
            cd_offset = out.tell()
            for cd in central_dir_entries:
                out.write(cd)
            cd_size = out.tell() - cd_offset
            num_entries = len(central_dir_entries)

            # ZIP64 end of central directory record
            out.write(struct.pack(
                "<I Q HH II QQ QQ",
                _ZIP64_END_SIG,
                44,                           # size of remaining record
                45,                           # version made by
                45,                           # version needed
                0,                            # disk number
                0,                            # disk with central dir
                num_entries,                  # entries on this disk
                num_entries,                  # total entries
                cd_size,
                cd_offset,
            ))

            # ZIP64 end of central directory locator
            zip64_end_offset = cd_offset + cd_size
            out.write(struct.pack(
                "<I I Q I",
                _ZIP64_LOCATOR_SIG,
                0,                            # disk with ZIP64 EOCD
                zip64_end_offset,
                1,                            # total disks
            ))

            # Standard end of central directory record
            out.write(struct.pack(
                "<I HH HH II H",
                _END_CENTRAL_DIR_SIG,
                0xFFFF if num_entries > 0xFFFF else 0,
                0xFFFF if num_entries > 0xFFFF else 0,
                min(num_entries, 0xFFFF),
                min(num_entries, 0xFFFF),
                min(cd_size, 0xFFFFFFFF),
                min(cd_offset, 0xFFFFFFFF),
                0,
            ))

    def package_snapshot(
        self,
        epoch: int,
        dest: Path,
        tick: int,
        compression: str = "tar.zst",
    ) -> Path:
        """Package state files into a snapshot archive.

        Uses tar+zstd for fast multi-threaded compression by default.
        Falls back to parallel ZIP if compression="zip" is specified.

        Args:
            epoch: Current epoch number.
            dest: Directory where the archive will be written.
            tick: Current tick number for naming the archive.
            compression: ``"zip"`` (default) or ``"tar.zst"`` for faster
                  multi-threaded zstd compression.
        """
        files = self.list_snapshot_files(epoch)
        if not files:
            raise FileNotFoundError(
                f"No state files found for epoch {epoch}"
            )

        # Determine archive extension based on compression
        if compression == "tar.zst":
            ext = "tar.zst"
        else:
            ext = "zip"

        archive_name = f"ep{epoch}-t{tick}-snap.{ext}"

        if compression == "tar.zst":
            return self._package_tar_zst(files, dest / archive_name)

        if compression != "zip":
            raise ValueError(f"Unsupported compression: {compression}")

        archive_path = dest / archive_name
        workers = min(len(files), os.cpu_count() or 4)
        logger.info(
            f"Compressing {len(files)} files with {workers} workers"
        )

        tmp_dir = tempfile.mkdtemp(prefix="qsnap_")
        try:
            # Phase 1: Compress files in parallel
            futures = {}
            with ThreadPoolExecutor(max_workers=workers) as pool:
                for f in files:
                    arcname = str(f.relative_to(self._data_dir))
                    future = pool.submit(
                        _compress_file_worker, str(f), arcname, tmp_dir
                    )
                    futures[future] = arcname

                entries = []
                done_count = 0
                for future in as_completed(futures):
                    done_count += 1
                    arcname = futures[future]
                    try:
                        entry = future.result()
                        entries.append(entry)
                        logger.debug(
                            f"Compressed [{done_count}/{len(files)}] "
                            f"{arcname} "
                            f"({entry['file_size']} -> {entry['compressed_size']})"
                        )
                    except Exception as exc:
                        logger.error(
                            f"Failed to compress {arcname}: {exc}"
                        )
                        raise

            # Preserve original file order in the archive
            order = {
                str(f.relative_to(self._data_dir)): i
                for i, f in enumerate(files)
            }
            entries.sort(key=lambda e: order.get(e["arcname"], 0))

            # Phase 2: Assemble ZIP from pre-compressed data
            self._build_zip_from_entries(entries, archive_path)
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)

        size = archive_path.stat().st_size
        logger.info(
            f"Packaged snapshot: {archive_path} "
            f"({size} bytes, {len(files)} files, {workers} workers)"
        )
        return archive_path

    def _package_tar_zst(self, files: list[Path], archive_path: Path) -> Path:
        """Package files using tar with multi-threaded zstd compression.

        Uses ``tar -I "zstd -T0"`` for native multi-threaded compression
        which is significantly faster than Python's zlib.

        Args:
            files: List of files to include in the archive.
            archive_path: Destination path for the archive.

        Returns:
            Path to the created archive.
        """
        import subprocess
        import time

        # Build list of files relative to data_dir
        file_args = []
        total_size = 0
        for f in files:
            rel_path = f.relative_to(self._data_dir)
            file_args.append(str(rel_path))
            try:
                total_size += f.stat().st_size
            except OSError:
                pass

        logger.info(
            f"Creating tar.zst archive: {len(files)} files, "
            f"{total_size / (1024**3):.1f} GB to compress"
        )

        # Use tar with zstd compression and verbose output
        # -I "zstd -T0": use zstd with all CPU threads
        # -v: verbose (outputs each file name to stderr)
        # -c: create archive
        # -f: output file
        # -C: change to data_dir before adding files
        cmd = [
            "tar",
            "-I", "zstd -T0",
            "-vcf", str(archive_path),
            "-C", str(self._data_dir),
        ] + file_args

        try:
            start_time = time.monotonic()
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            # Read stderr (tar -v outputs to stderr) and log progress
            files_processed = 0
            last_log_time = start_time
            log_interval = 10  # Log every 10 seconds

            for line in proc.stderr:
                files_processed += 1
                now = time.monotonic()
                if now - last_log_time >= log_interval:
                    elapsed = now - start_time
                    pct = (files_processed / len(files)) * 100
                    logger.info(
                        f"Compressing: {files_processed}/{len(files)} files "
                        f"({pct:.0f}%) - {elapsed:.0f}s elapsed"
                    )
                    last_log_time = now

            proc.wait()
            if proc.returncode != 0:
                raise subprocess.CalledProcessError(proc.returncode, cmd)

            duration = time.monotonic() - start_time

        except subprocess.CalledProcessError as e:
            logger.error(f"tar+zstd failed with exit code {e.returncode}")
            raise RuntimeError(f"Failed to create tar.zst archive: exit {e.returncode}")
        except FileNotFoundError:
            raise RuntimeError(
                "zstd not found. Install zstd package or use compression='zip'"
            )

        compressed_size = archive_path.stat().st_size
        ratio = total_size / compressed_size if compressed_size > 0 else 0
        logger.info(
            f"Packaged snapshot: {archive_path.name} "
            f"({compressed_size / (1024**3):.2f} GB, "
            f"{ratio:.1f}x compression, {duration:.0f}s)"
        )
        return archive_path

    async def package_snapshot_chunked_stream(
        self,
        epoch: int,
        dest: Path,
        tick: int,
        chunk_size_mb: int = 2048,
    ):
        """Package files as tar.zst chunks, yielding each chunk as it is finalised.

        Yields tuples of ``(chunk_path, total_uncompressed_size)`` (the second
        value is the same for every chunk — the uncompressed size known from
        listing files).

        Uses the same ``tar | zstd | split`` pipeline as
        :meth:`package_snapshot_chunked` but monitors the staging directory so
        chunks can be handed off to an uploader the moment they are complete.
        A chunk is considered finalised when the next-indexed chunk appears
        (split fills sequentially); the highest-indexed chunk is finalised
        when ``split`` exits.
        """
        import asyncio
        import time

        files = self.list_snapshot_files(epoch)
        if not files:
            raise FileNotFoundError(
                f"No state files found for epoch {epoch}"
            )

        file_args = []
        total_size = 0
        for f in files:
            rel_path = f.relative_to(self._data_dir)
            file_args.append(str(rel_path))
            try:
                total_size += f.stat().st_size
            except OSError:
                pass

        base_name = f"ep{epoch}-t{tick}-snap.tar.zst."
        chunk_prefix = dest / base_name
        filelist_path = dest / f".filelist-t{tick}.txt"
        filelist_path.write_text("\n".join(file_args))

        logger.info(
            f"Streaming chunked tar.zst: {len(files)} files, "
            f"{total_size / (1024**3):.1f} GB -> {chunk_size_mb}MB chunks"
        )
        start_time = time.monotonic()

        # Run the whole pipeline inside a single bash process. asyncio's
        # create_subprocess_exec can't chain stdin/stdout between asyncio
        # subprocesses (StreamReader has no fileno), so we let the shell
        # build the pipe. pipefail ensures bash's exit code reflects a
        # failure in any stage, not just the last one.
        import shlex
        pipeline = (
            f"tar -cf - -C {shlex.quote(str(self._data_dir))} "
            f"-T {shlex.quote(str(filelist_path))} "
            f"| zstd -T0 - "
            f"| split -b {chunk_size_mb}M -d -a 2 - "
            f"{shlex.quote(str(chunk_prefix))}"
        )
        proc = await asyncio.create_subprocess_exec(
            "bash", "-o", "pipefail", "-c", pipeline,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )

        emitted: set[str] = set()
        try:
            while True:
                existing = sorted(
                    p for p in dest.glob(f"{base_name}*")
                    if p.name not in emitted
                )
                if proc.returncode is not None:
                    for p in existing:
                        emitted.add(p.name)
                        yield p, total_size
                    break

                # Pipeline still running: every chunk except the highest-index
                # one is complete (split fills sequentially).
                if len(existing) >= 2:
                    for p in existing[:-1]:
                        emitted.add(p.name)
                        yield p, total_size
                await asyncio.sleep(0.5)

            # Reap the process and surface errors.
            await proc.wait()
            if proc.returncode != 0:
                stderr = (await proc.stderr.read()).decode().strip()
                raise RuntimeError(
                    f"packaging pipeline failed (exit {proc.returncode}): "
                    f"{stderr}"
                )
        except BaseException:
            if proc.returncode is None:
                try:
                    proc.kill()
                except ProcessLookupError:
                    pass
                try:
                    await proc.wait()
                except Exception:
                    pass
            raise
        finally:
            try:
                filelist_path.unlink()
            except OSError:
                pass

        duration = time.monotonic() - start_time
        logger.info(
            f"Streaming packaging complete: {len(emitted)} chunks "
            f"in {duration:.0f}s"
        )

    def package_snapshot_chunked(
        self,
        epoch: int,
        dest: Path,
        tick: int,
        chunk_size_mb: int = 2048,
    ) -> tuple[list[Path], int]:
        """Package state files directly into tar.zst chunks using streaming.

        Uses ``tar | zstd | split`` pipeline to create chunks without
        creating an intermediate full archive. This saves disk space and
        time for large snapshots (16GB+).

        Args:
            epoch: Current epoch number.
            dest: Directory where chunk files will be written.
            tick: Current tick number for naming.
            chunk_size_mb: Size of each chunk in MB (default 2048 = 2GB).

        Returns:
            Tuple of (list of chunk file paths, total uncompressed size).
        """
        import subprocess
        import time

        files = self.list_snapshot_files(epoch)
        if not files:
            raise FileNotFoundError(
                f"No state files found for epoch {epoch}"
            )

        # Build list of files relative to data_dir
        file_args = []
        total_size = 0
        for f in files:
            rel_path = f.relative_to(self._data_dir)
            file_args.append(str(rel_path))
            try:
                total_size += f.stat().st_size
            except OSError:
                pass

        # Base name for chunks: ep199-t43669270-snap.tar.zst.
        # split will append numeric suffixes: .00, .01, .02, etc.
        base_name = f"ep{epoch}-t{tick}-snap.tar.zst."
        chunk_prefix = dest / base_name

        logger.info(
            f"Creating chunked tar.zst: {len(files)} files, "
            f"{total_size / (1024**3):.1f} GB -> {chunk_size_mb}MB chunks"
        )

        # Pipeline: tar | zstd | split
        # tar -cf - : create archive to stdout
        # zstd -T0  : compress with all threads, output to stdout
        # split -b  : split into fixed-size chunks with numeric suffixes
        tar_cmd = [
            "tar", "-cf", "-",
            "-C", str(self._data_dir),
        ] + file_args

        zstd_cmd = ["zstd", "-T0", "-"]

        split_cmd = [
            "split",
            "-b", f"{chunk_size_mb}M",
            "-d",  # numeric suffixes: 00, 01, 02
            "-a", "2",  # 2-digit suffix
            "-",  # read from stdin
            str(chunk_prefix),  # output prefix
        ]

        try:
            start_time = time.monotonic()

            # Create pipeline: tar | zstd | split
            tar_proc = subprocess.Popen(
                tar_cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            zstd_proc = subprocess.Popen(
                zstd_cmd,
                stdin=tar_proc.stdout,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            # Close tar stdout in parent so zstd gets EOF
            tar_proc.stdout.close()

            split_proc = subprocess.Popen(
                split_cmd,
                stdin=zstd_proc.stdout,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            # Close zstd stdout in parent
            zstd_proc.stdout.close()

            # Log progress periodically while waiting
            last_log_time = start_time
            log_interval = 30  # Log every 30 seconds

            while split_proc.poll() is None:
                time.sleep(1)
                now = time.monotonic()
                if now - last_log_time >= log_interval:
                    elapsed = now - start_time
                    # Count existing chunks to show progress
                    existing_chunks = sorted(dest.glob(f"{base_name}*"))
                    chunk_bytes = sum(c.stat().st_size for c in existing_chunks)
                    logger.info(
                        f"Compressing: {len(existing_chunks)} chunks "
                        f"({chunk_bytes / (1024**3):.2f} GB) - {elapsed:.0f}s elapsed"
                    )
                    last_log_time = now

            # Wait for all processes to complete
            split_proc.wait()
            zstd_proc.wait()
            tar_proc.wait()

            # Check for errors
            if tar_proc.returncode != 0:
                stderr = tar_proc.stderr.read().decode() if tar_proc.stderr else ""
                raise RuntimeError(f"tar failed (exit {tar_proc.returncode}): {stderr}")
            if zstd_proc.returncode != 0:
                stderr = zstd_proc.stderr.read().decode() if zstd_proc.stderr else ""
                raise RuntimeError(f"zstd failed (exit {zstd_proc.returncode}): {stderr}")
            if split_proc.returncode != 0:
                stderr = split_proc.stderr.read().decode() if split_proc.stderr else ""
                raise RuntimeError(f"split failed (exit {split_proc.returncode}): {stderr}")

            duration = time.monotonic() - start_time

        except FileNotFoundError as e:
            raise RuntimeError(
                f"Required command not found: {e}. Install zstd package."
            )

        # Collect created chunk files
        chunk_files = sorted(dest.glob(f"{base_name}*"))
        if not chunk_files:
            raise RuntimeError("No chunk files created")

        compressed_size = sum(c.stat().st_size for c in chunk_files)
        ratio = total_size / compressed_size if compressed_size > 0 else 0

        logger.info(
            f"Created {len(chunk_files)} chunks: "
            f"{compressed_size / (1024**3):.2f} GB compressed "
            f"({ratio:.1f}x ratio, {duration:.0f}s)"
        )

        return chunk_files, total_size

    def delete_epoch_files(self, epoch: int) -> None:
        """Remove all state files for a specific epoch.

        This is used for STATE_INCOMPATIBLE recovery: when local state
        files are incompatible with the new binary (e.g. contract state
        layout changed), delete them so the orchestrator re-downloads
        fresh files on next startup.
        """
        deleted = 0

        # Delete epoch-numbered state files (spectrum.205, contract0001.205, etc.)
        for path in list(self._data_dir.iterdir()):
            if not path.is_file():
                continue
            ext = path.suffix.lstrip(".")
            try:
                file_epoch = int(ext)
            except ValueError:
                continue
            if file_epoch == epoch:
                logger.info(f"Deleting state file: {path.name}")
                path.unlink()
                deleted += 1

        # Delete snapshot directory (ep205/)
        for name in [f"ep{epoch}", "ep"]:
            d = self._data_dir / name
            if d.is_dir():
                logger.info(f"Deleting snapshot dir: {d.name}")
                shutil.rmtree(d)
                deleted += 1

        # Delete page file directories for this epoch (td00data205/, etc.)
        for d in list(self._data_dir.iterdir()):
            if not d.is_dir() or d.name.startswith("ep"):
                continue
            match = re.search(r"(\d+)$", d.name)
            if match and int(match.group(1)) == epoch:
                logger.info(f"Deleting page dir: {d.name}")
                shutil.rmtree(d)
                deleted += 1

        logger.info(f"Deleted {deleted} items for epoch {epoch}")

    def cleanup_old_epochs(self, current_epoch: int, keep: int = 0) -> None:
        """Remove state files, snapshot dirs, and page dirs from old epochs."""
        for path in self._data_dir.iterdir():
            if not path.is_file():
                continue
            ext = path.suffix.lstrip(".")
            try:
                file_epoch = int(ext)
            except ValueError:
                continue
            if file_epoch < current_epoch - keep:
                logger.info(f"Cleaning up old file: {path.name}")
                path.unlink()

        # Clean up old ep directories
        for d in self._data_dir.iterdir():
            if d.is_dir() and d.name.startswith("ep"):
                try:
                    dir_epoch = int(d.name[2:])
                    if dir_epoch < current_epoch - keep:
                        logger.info(f"Cleaning up old snapshot dir: {d.name}")
                        shutil.rmtree(d)
                except ValueError:
                    continue

        # Clean up old page file directories (.pg swap files)
        for d in self._data_dir.iterdir():
            if not d.is_dir() or d.name.startswith("ep"):
                continue
            if not any(d.glob("*.pg")):
                continue
            # Extract trailing epoch number (e.g. "td00data198" -> 198)
            match = re.search(r"(\d+)$", d.name)
            if match:
                dir_epoch = int(match.group(1))
                if dir_epoch < current_epoch - keep:
                    logger.info(f"Cleaning up old page dir: {d.name}")
                    shutil.rmtree(d)

    @staticmethod
    def compute_checksum(file_path: Path) -> str:
        """Compute SHA-256 checksum of a file."""
        sha256 = hashlib.sha256()
        with open(file_path, "rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                sha256.update(chunk)
        return sha256.hexdigest()
