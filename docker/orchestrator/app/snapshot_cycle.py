from __future__ import annotations

import asyncio
import json
import logging
import re
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from app.alerting import AlertManager
from app.config import SourceConfig
from app.models import NodeHealth, TickInfo
from app.node_client import NodeClient
from app.state_manager import StateManager
from app.uploaders.base import BaseUploader, RemotePackagingUploader
from app.uploaders.chunked_scp import ChunkedScpUploader

logger = logging.getLogger(__name__)

# Lock coordination threshold: skip cycle if another source
# uploaded a snapshot within this many ticks of our local tick.
LOCK_TICK_THRESHOLD = 100
# Match chunked-snapshot folders in an epoch directory (new layout).
_SNAPSHOT_DIR_RE = re.compile(r"^snap-t(\d+)$")
# Match legacy single-file snapshots (.zip / .tar.zst). Still recognised by
# cleanup so pre-existing files on the remote get swept up eventually.
_SNAPSHOT_FILE_RE = re.compile(r"^ep(\d+)-t(\d+)-snap\.(zip|tar\.zst)$")
# Format tag written into the sidecar/index so downloaders know what to expect.
SNAPSHOT_FORMAT_CHUNKED = "chunked-tar-zst"
SNAPSHOT_FORMAT_SINGLE = "single-file"
SNAPSHOT_FORMAT_VERSION = 1


class SnapshotCycle:
    """Source mode: periodically trigger, package, and upload snapshots.

    Uses lock-based coordination so multiple source nodes don't upload
    duplicate/overlapping snapshots.  Remote layout::

        Chunks:   {epoch}/snap-t{tick}/chunk.00, chunk.01, ...   (chunked tar.zst)
        Archive:  {epoch}/ep{epoch}-t{tick}-snap.{zip,tar.zst}   (single-file fallback)
        Sidecar:  {epoch}/ep{epoch}-t{tick}-snap.json
        Index:    {epoch}/ep{epoch}-latest-snap.json
        Lock:     {epoch}/snap.lock
    """

    def __init__(
        self,
        config: SourceConfig,
        node_client: NodeClient,
        state_manager: StateManager,
        uploader: BaseUploader,
        alert_manager: AlertManager,
        data_dir: Path,
    ) -> None:
        self._config = config
        self._node_client = node_client
        self._state_manager = state_manager
        self._uploader = uploader
        self._alert_manager = alert_manager
        self._data_dir = data_dir
        self._last_snapshot_epoch: Optional[int] = None
        self._node_id = uuid.uuid4().hex[:12]
        self._trigger_event = asyncio.Event()
        self._cycle_running = False

    # ------------------------------------------------------------------
    # Remote key helpers
    # ------------------------------------------------------------------
    def _archive_key(self, epoch: int, tick: int) -> str:
        ext = "tar.zst" if self._config.package_compression == "tar.zst" else "zip"
        return f"{epoch}/ep{epoch}-t{tick}-snap.{ext}"

    @staticmethod
    def _snapshot_dir_key(epoch: int, tick: int) -> str:
        return f"{epoch}/snap-t{tick}"

    @staticmethod
    def _sidecar_key(epoch: int, tick: int) -> str:
        return f"{epoch}/ep{epoch}-t{tick}-snap.json"

    @staticmethod
    def _index_key(epoch: int) -> str:
        return f"{epoch}/ep{epoch}-latest-snap.json"

    @staticmethod
    def _lock_key(epoch: int) -> str:
        return f"{epoch}/snap.lock"

    # ------------------------------------------------------------------
    # Public trigger interface
    # ------------------------------------------------------------------
    def trigger_immediate(self) -> bool:
        """Request an immediate snapshot cycle.

        Returns True if the trigger was accepted, False if a cycle
        is already running or a trigger is already pending.
        """
        if self._cycle_running or self._trigger_event.is_set():
            return False
        self._trigger_event.set()
        return True

    @property
    def is_cycle_running(self) -> bool:
        return self._cycle_running

    @property
    def is_trigger_pending(self) -> bool:
        return self._trigger_event.is_set()

    @property
    def last_snapshot_epoch(self) -> Optional[int]:
        return self._last_snapshot_epoch

    # ------------------------------------------------------------------
    # Main loop
    # ------------------------------------------------------------------
    async def run(self, shutdown_event: asyncio.Event) -> None:
        """Main snapshot cycle loop."""
        logger.info(
            f"Snapshot cycle started "
            f"(interval={self._config.snapshot_interval_seconds}s, "
            f"uploader={self._uploader.get_name()}, "
            f"node_id={self._node_id})"
        )

        while not shutdown_event.is_set():
            # Wait for shutdown, manual trigger, or interval timeout
            shutdown_task = asyncio.create_task(shutdown_event.wait())
            trigger_task = asyncio.create_task(self._trigger_event.wait())
            timeout_task = asyncio.create_task(
                asyncio.sleep(self._config.snapshot_interval_seconds)
            )

            try:
                done, pending = await asyncio.wait(
                    {shutdown_task, trigger_task, timeout_task},
                    return_when=asyncio.FIRST_COMPLETED,
                )
            except asyncio.CancelledError:
                for t in (shutdown_task, trigger_task, timeout_task):
                    t.cancel()
                raise

            for t in pending:
                t.cancel()

            if shutdown_task in done:
                return

            if trigger_task in done:
                self._trigger_event.clear()
                logger.info("Manual snapshot trigger received")

            try:
                self._cycle_running = True
                await self._execute_cycle()
            except asyncio.CancelledError:
                raise
            except Exception as e:
                logger.error(f"Snapshot cycle failed: {e}", exc_info=True)
                await self._alert_manager.send_alert(
                    "error",
                    "snapshot_cycle_failed",
                    {"error": str(e)},
                )
            finally:
                self._cycle_running = False

    # ------------------------------------------------------------------
    # Single cycle
    # ------------------------------------------------------------------
    async def _execute_cycle(self) -> None:
        """Execute a single snapshot cycle with lock coordination."""

        # Step 1: Pre-check — is the node healthy?
        tick_info = await self._pre_check()
        if tick_info is None:
            return

        epoch = tick_info.epoch
        local_tick = tick_info.tick

        logger.info(
            f"Starting snapshot cycle for epoch {epoch}, tick {local_tick}"
        )

        # Detect epoch transition since last snapshot
        if (
            self._last_snapshot_epoch is not None
            and epoch != self._last_snapshot_epoch
        ):
            logger.info(
                f"Epoch changed from {self._last_snapshot_epoch} to {epoch}, "
                "resetting snapshot state"
            )

        # Step 2: Check remote lock for this epoch
        skip = await self._check_remote_lock(epoch, local_tick)
        if skip:
            return

        # Step 3: Acquire lock
        lock_acquired = await self._acquire_lock(epoch, local_tick)
        if not lock_acquired:
            logger.warning("Failed to write lock file, skipping cycle")
            return

        try:
            # Step 4: Trigger snapshot save
            logger.info("Triggering snapshot save via HTTP API")
            success = await self._node_client.request_save_snapshot()
            if not success:
                logger.warning(
                    "Snapshot save request returned non-ok status"
                )
                return

            # Step 5: Wait for save to start
            started = await self._wait_for_save_start()
            if not started:
                logger.warning(
                    "Snapshot save did not start within timeout, skipping"
                )
                return

            # Step 6: Wait for save to complete
            completed = await self._wait_for_save_complete()
            if not completed:
                logger.error("Snapshot save timed out, skipping upload")
                await self._alert_manager.send_alert(
                    "warning",
                    "snapshot_save_timeout",
                    {"epoch": epoch, "tick": local_tick},
                )
                return

            logger.info("Snapshot save completed")

            # Re-read tick info — the tick may have advanced during save
            try:
                post_save_info = await self._node_client.get_tick_info()
                snap_tick = post_save_info.tick
            except Exception:
                snap_tick = local_tick

            # Step 7: Verify snapshot files
            if not self._state_manager.has_snapshot_directory(epoch):
                logger.error(
                    f"Snapshot directory for epoch {epoch} not found "
                    "after save"
                )
                return

            # Step 8+: Package and upload
            if isinstance(self._uploader, RemotePackagingUploader):
                upload_ok = await self._rsync_upload_path(epoch, snap_tick)
            else:
                upload_ok = await self._local_package_upload_path(
                    epoch, snap_tick
                )

            # Step 9: Clean up old remote snapshots
            if upload_ok:
                await self._cleanup_old_snapshots()

        finally:
            # Always release the lock
            await self._release_lock(epoch)

    # ------------------------------------------------------------------
    # Lock coordination
    # ------------------------------------------------------------------
    async def _check_remote_lock(
        self, epoch: int, local_tick: int
    ) -> bool:
        """Check remote lock and index. Returns True if we should skip."""
        # First check the index to see if a recent-enough snapshot exists
        try:
            index_bytes = await self._uploader.get_small_file(
                self._index_key(epoch)
            )
            if index_bytes is not None:
                index = json.loads(index_bytes)
                remote_tick = index.get("tick", 0)
                if local_tick < remote_tick + LOCK_TICK_THRESHOLD:
                    logger.info(
                        f"Recent snapshot exists at tick {remote_tick} "
                        f"(local={local_tick}, threshold={LOCK_TICK_THRESHOLD}), "
                        "skipping cycle"
                    )
                    return True
        except (json.JSONDecodeError, Exception) as e:
            logger.debug(f"Could not read index: {e}")

        # Check if another source currently holds the lock
        try:
            lock_bytes = await self._uploader.get_small_file(
                self._lock_key(epoch)
            )
            if lock_bytes is not None:
                lock_data = json.loads(lock_bytes)
                lock_tick = lock_data.get("tick", 0)
                lock_ts = lock_data.get("timestamp", "")

                # If the lock is from a tick close to ours, another source
                # is actively building a snapshot — skip.
                if local_tick < lock_tick + LOCK_TICK_THRESHOLD:
                    logger.info(
                        f"Lock held by {lock_data.get('node_id', '?')} "
                        f"at tick {lock_tick}, skipping cycle"
                    )
                    return True

                # Lock is from a much older tick — could be stale.
                # We'll overwrite it.
                logger.info(
                    f"Stale lock at tick {lock_tick} "
                    f"(local={local_tick}), proceeding"
                )
        except (json.JSONDecodeError, Exception) as e:
            logger.debug(f"Could not read lock: {e}")

        return False

    async def _acquire_lock(self, epoch: int, tick: int) -> bool:
        """Write a lock file to remote storage."""
        lock_data = {
            "tick": tick,
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "node_id": self._node_id,
        }
        return await self._uploader.put_small_file(
            self._lock_key(epoch),
            json.dumps(lock_data).encode(),
        )

    async def _release_lock(self, epoch: int) -> None:
        """Delete the lock file from remote storage."""
        try:
            await self._uploader.delete_file(self._lock_key(epoch))
        except Exception as e:
            logger.warning(f"Failed to release lock: {e}")

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------
    async def _pre_check(self) -> Optional[TickInfo]:
        """Check if the node is healthy enough for a snapshot."""
        try:
            tick_info = await self._node_client.get_tick_info()
        except Exception as e:
            logger.warning(f"Cannot reach node for pre-check: {e}")
            return None

        if tick_info.is_saving_snapshot:
            logger.info("Node is already saving a snapshot, skipping cycle")
            return None

        return tick_info

    async def _wait_for_save_start(self) -> bool:
        """Wait for isSavingSnapshot to become true."""
        timeout = 60
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            await asyncio.sleep(self._config.snapshot_poll_interval_seconds)
            try:
                tick_info = await self._node_client.get_tick_info()
                if tick_info.is_saving_snapshot:
                    return True
            except Exception:
                pass
        return False

    async def _wait_for_save_complete(self) -> bool:
        """Wait for isSavingSnapshot to become false after it was true."""
        deadline = (
            time.monotonic() + self._config.snapshot_wait_timeout_seconds
        )
        while time.monotonic() < deadline:
            await asyncio.sleep(self._config.snapshot_poll_interval_seconds)
            try:
                tick_info = await self._node_client.get_tick_info()
                if not tick_info.is_saving_snapshot:
                    return True
            except Exception:
                pass
        return False

    # ------------------------------------------------------------------
    # Upload paths
    # ------------------------------------------------------------------
    async def _local_package_upload_path(
        self, epoch: int, snap_tick: int
    ) -> bool:
        """Standard path: local packaging + upload."""
        staging_dir = self._data_dir / ".snapshot-staging"
        staging_dir.mkdir(exist_ok=True)

        # Marker file to prevent cleanup task from removing staging
        in_progress_marker = staging_dir / ".upload-in-progress"

        # Check if we should use streaming chunked compression
        use_streaming_chunks = (
            self._config.package_compression == "tar.zst"
            and isinstance(self._uploader, ChunkedScpUploader)
        )

        if use_streaming_chunks:
            return await self._chunked_package_upload_path(
                epoch, snap_tick, staging_dir, in_progress_marker
            )

        # Standard path: package into single archive, then upload
        try:
            archive_path = await asyncio.to_thread(
                self._state_manager.package_snapshot,
                epoch,
                staging_dir,
                tick=snap_tick,
                compression=self._config.package_compression,
            )
        except Exception as e:
            logger.error(f"Failed to package snapshot: {e}")
            return False

        # Upload archive
        checksum = await asyncio.to_thread(
            StateManager.compute_checksum, archive_path
        )
        now = datetime.now(timezone.utc).isoformat()
        metadata = {
            "epoch": epoch,
            "tick": snap_tick,
            "timestamp": now,
            "checksum": checksum,
            "size_bytes": archive_path.stat().st_size,
            "node_id": self._node_id,
            "format": SNAPSHOT_FORMAT_SINGLE,
            "format_version": SNAPSHOT_FORMAT_VERSION,
        }
        remote_key = self._archive_key(epoch, snap_tick)

        # Mark upload as in-progress to prevent cleanup interference
        try:
            in_progress_marker.touch()
        except OSError:
            pass

        try:
            upload_ok = await self._upload_with_retries(
                archive_path, metadata, remote_key
            )

            if upload_ok:
                await self._publish_metadata(epoch, snap_tick, metadata)
            else:
                await self._alert_manager.send_alert(
                    "error",
                    "snapshot_upload_failed",
                    {"epoch": epoch, "tick": snap_tick},
                )
        finally:
            # Remove in-progress marker
            try:
                in_progress_marker.unlink(missing_ok=True)
            except OSError:
                pass

        # Cleanup staging only after successful upload
        if upload_ok:
            try:
                if archive_path.exists():
                    archive_path.unlink()
            except OSError:
                pass

        return upload_ok

    async def _chunked_package_upload_path(
        self,
        epoch: int,
        snap_tick: int,
        staging_dir: Path,
        in_progress_marker: Path,
    ) -> bool:
        """Streaming path: pipe ``tar | zstd | split`` chunks straight into the
        uploader so packaging and upload run concurrently. Each chunk is
        retried and verified independently inside the uploader."""
        chunk_size_mb = self._config.scp_chunk_size_mb
        logger.info(
            f"Using streaming chunked compression "
            f"(tar | zstd | split -b {chunk_size_mb}M) with per-chunk upload"
        )

        now = datetime.now(timezone.utc).isoformat()
        base_metadata = {
            "epoch": epoch,
            "tick": snap_tick,
            "timestamp": now,
            "node_id": self._node_id,
        }

        try:
            in_progress_marker.touch()
        except OSError:
            pass

        chunk_stream = self._state_manager.package_snapshot_chunked_stream(
            epoch,
            staging_dir,
            tick=snap_tick,
            chunk_size_mb=chunk_size_mb,
        )

        total_uncompressed = 0

        async def _sized_stream():
            nonlocal total_uncompressed
            async for chunk_path, uncompressed in chunk_stream:
                total_uncompressed = uncompressed
                yield chunk_path

        try:
            try:
                result = await self._uploader.upload_chunks(
                    _sized_stream(), base_metadata, total_uncompressed
                )
            except Exception as e:
                logger.error(
                    f"Chunked upload pipeline failed: {e}", exc_info=True
                )
                result = None

            upload_ok = result is not None and result.success

            if upload_ok:
                logger.info(
                    f"Chunked upload successful: {result.remote_url} "
                    f"({result.bytes_uploaded} bytes, "
                    f"{result.duration_seconds:.1f}s)"
                )
                chunk_metadata = {
                    **base_metadata,
                    "format": SNAPSHOT_FORMAT_CHUNKED,
                    "format_version": SNAPSHOT_FORMAT_VERSION,
                    "dir": f"snap-t{snap_tick}",
                    "size_bytes": result.bytes_uploaded,
                    "uncompressed_size_bytes": total_uncompressed,
                    "chunks": result.chunks,
                }
                await self._publish_chunked_metadata(
                    epoch, snap_tick, chunk_metadata
                )
            else:
                err = result.error_message if result else "pipeline error"
                logger.error(f"Chunked upload failed: {err}")
                await self._alert_manager.send_alert(
                    "error",
                    "snapshot_upload_failed",
                    {"epoch": epoch, "tick": snap_tick, "error": err},
                )
        finally:
            try:
                in_progress_marker.unlink(missing_ok=True)
            except OSError:
                pass
            # Sweep any chunk files the uploader didn't get around to deleting
            # (e.g. on failure). Streaming uploader removes successful ones as
            # it goes.
            for leftover in staging_dir.glob(
                f"ep{epoch}-t{snap_tick}-snap.tar.zst.*"
            ):
                try:
                    leftover.unlink()
                except OSError:
                    pass

        return upload_ok

    async def _rsync_upload_path(
        self, epoch: int, snap_tick: int
    ) -> bool:
        """Rsync path: sync snapshot directory + ZIP on remote server."""
        snap_dir = self._state_manager.get_snapshot_directory(epoch)
        if snap_dir is None:
            logger.error(
                f"No snapshot directory found for epoch {epoch}"
            )
            return False

        upload_ok = await self._sync_and_package_with_retries(
            snap_dir, epoch, snap_tick
        )

        if upload_ok:
            # Get checksum and size from remote
            remote_key = self._archive_key(epoch, snap_tick)
            try:
                checksum, size_bytes = (
                    await self._uploader.get_remote_checksum(remote_key)
                )
            except Exception as e:
                logger.error(f"Failed to get remote checksum: {e}")
                checksum = ""
                size_bytes = 0

            now = datetime.now(timezone.utc).isoformat()
            metadata = {
                "epoch": epoch,
                "tick": snap_tick,
                "timestamp": now,
                "checksum": checksum,
                "size_bytes": size_bytes,
                "node_id": self._node_id,
                "format": SNAPSHOT_FORMAT_SINGLE,
                "format_version": SNAPSHOT_FORMAT_VERSION,
            }
            await self._publish_metadata(epoch, snap_tick, metadata)
        else:
            await self._alert_manager.send_alert(
                "error",
                "snapshot_upload_failed",
                {"epoch": epoch, "tick": snap_tick},
            )

        return upload_ok

    async def _publish_metadata(
        self, epoch: int, snap_tick: int, metadata: dict
    ) -> None:
        """Single-file snapshot: upload sidecar JSON, update index, send alert."""
        ext = "tar.zst" if self._config.package_compression == "tar.zst" else "zip"

        sidecar_key = self._sidecar_key(epoch, snap_tick)
        sidecar_content = json.dumps(metadata, indent=2).encode()
        await self._uploader.put_small_file(sidecar_key, sidecar_content)

        index_key = self._index_key(epoch)
        index_data = {
            "format": SNAPSHOT_FORMAT_SINGLE,
            "format_version": SNAPSHOT_FORMAT_VERSION,
            "epoch": epoch,
            "tick": snap_tick,
            "file": f"ep{epoch}-t{snap_tick}-snap.{ext}",
            "sidecar": f"ep{epoch}-t{snap_tick}-snap.json",
            "timestamp": metadata["timestamp"],
            "checksum": metadata.get("checksum", ""),
            "size_bytes": metadata.get("size_bytes", 0),
        }
        await self._uploader.put_small_file(
            index_key,
            json.dumps(index_data, indent=2).encode(),
        )

        self._last_snapshot_epoch = epoch
        await self._alert_manager.send_alert(
            "info",
            "snapshot_uploaded",
            metadata,
        )

    async def _publish_chunked_metadata(
        self, epoch: int, snap_tick: int, metadata: dict
    ) -> None:
        """Chunked snapshot: sidecar lists chunks, index points at the folder."""
        sidecar_key = self._sidecar_key(epoch, snap_tick)
        sidecar_content = json.dumps(metadata, indent=2).encode()
        await self._uploader.put_small_file(sidecar_key, sidecar_content)

        index_key = self._index_key(epoch)
        index_data = {
            "format": SNAPSHOT_FORMAT_CHUNKED,
            "format_version": SNAPSHOT_FORMAT_VERSION,
            "epoch": epoch,
            "tick": snap_tick,
            "dir": metadata["dir"],
            "sidecar": f"ep{epoch}-t{snap_tick}-snap.json",
            "timestamp": metadata["timestamp"],
            "size_bytes": metadata.get("size_bytes", 0),
            "uncompressed_size_bytes": metadata.get(
                "uncompressed_size_bytes", 0
            ),
            "chunks": metadata["chunks"],
        }
        await self._uploader.put_small_file(
            index_key,
            json.dumps(index_data, indent=2).encode(),
        )

        self._last_snapshot_epoch = epoch
        await self._alert_manager.send_alert(
            "info",
            "snapshot_uploaded",
            metadata,
        )

    async def _upload_with_retries(
        self,
        file_path: Path,
        metadata: dict,
        remote_key: str,
    ) -> bool:
        """Upload with retry logic."""
        for attempt in range(self._config.upload_retry_count + 1):
            result = await self._uploader.upload(
                file_path, metadata, remote_key
            )
            if result.success:
                logger.info(
                    f"Upload successful: {result.remote_url} "
                    f"({result.bytes_uploaded} bytes, "
                    f"{result.duration_seconds:.1f}s)"
                )
                return True

            logger.warning(
                f"Upload attempt {attempt + 1} failed: "
                f"{result.error_message}"
            )
            if attempt < self._config.upload_retry_count:
                await asyncio.sleep(
                    self._config.upload_retry_delay_seconds
                )

        return False

    async def _sync_and_package_with_retries(
        self,
        snap_dir: Path,
        epoch: int,
        tick: int,
    ) -> bool:
        """Rsync + remote package with retry logic."""
        for attempt in range(self._config.upload_retry_count + 1):
            result = await self._uploader.sync_and_package(
                snap_dir, epoch, tick
            )
            if result.success:
                logger.info(
                    f"Sync+package successful: {result.remote_url} "
                    f"({result.bytes_uploaded} bytes transferred, "
                    f"{result.duration_seconds:.1f}s)"
                )
                return True

            logger.warning(
                f"Sync+package attempt {attempt + 1} failed: "
                f"{result.error_message}"
            )
            if attempt < self._config.upload_retry_count:
                await asyncio.sleep(
                    self._config.upload_retry_delay_seconds
                )

        return False

    # ------------------------------------------------------------------
    # Remote snapshot cleanup
    # ------------------------------------------------------------------
    async def _cleanup_old_snapshots(self) -> None:
        """Remove old snapshots from remote, keeping only the N most recent.

        Recognises both the new chunked layout (snap-t{tick}/ folders) and
        legacy single-file snapshots (.tar.zst / .zip).
        """
        keep = self._config.snapshot_keep_count
        if keep <= 0:
            return

        # (epoch, tick, kind, payload) where kind is "dir" or "file" and
        # payload is either the folder name or the file extension.
        all_snapshots: list[tuple[int, int, str, str]] = []

        try:
            entries = await self._uploader.list_remote_dir("")
        except Exception as e:
            logger.debug(f"Cannot list remote root for cleanup: {e}")
            return

        epoch_nums: list[int] = []
        for name in entries:
            try:
                epoch_nums.append(int(name))
            except ValueError:
                continue

        for ep in epoch_nums:
            try:
                items = await self._uploader.list_remote_dir(str(ep))
            except Exception:
                continue

            for name in items:
                dir_match = _SNAPSHOT_DIR_RE.match(name)
                if dir_match:
                    all_snapshots.append(
                        (ep, int(dir_match.group(1)), "dir", name)
                    )
                    continue
                file_match = _SNAPSHOT_FILE_RE.match(name)
                if file_match:
                    all_snapshots.append(
                        (ep, int(file_match.group(2)), "file", file_match.group(3))
                    )

        if len(all_snapshots) <= keep:
            return

        # Most recent first. Collapse duplicates per (epoch, tick) so a single
        # snapshot with both a folder and a legacy file counts once toward `keep`.
        all_snapshots.sort(key=lambda x: (x[0], x[1]), reverse=True)
        seen: set[tuple[int, int]] = set()
        to_keep: set[tuple[int, int]] = set()
        to_delete: list[tuple[int, int, str, str]] = []
        for item in all_snapshots:
            key = (item[0], item[1])
            if key in seen:
                # Duplicate (e.g. snap-t{tick}/ + legacy tar.zst): if the kept
                # set already includes this snapshot, keep extras; otherwise delete.
                if key in to_keep:
                    continue
                to_delete.append(item)
                continue
            seen.add(key)
            if len(to_keep) < keep:
                to_keep.add(key)
            else:
                to_delete.append(item)

        kept_epochs = {k[0] for k in to_keep}
        deleted_count = 0

        for epoch, tick, kind, payload in to_delete:
            sidecar_key = f"{epoch}/ep{epoch}-t{tick}-snap.json"
            if kind == "dir":
                dir_key = f"{epoch}/{payload}"
                logger.info(f"Deleting old snapshot folder: {dir_key}")
                await self._uploader.delete_remote_dir(dir_key)
            else:
                archive_key = f"{epoch}/ep{epoch}-t{tick}-snap.{payload}"
                logger.info(f"Deleting old snapshot archive: {archive_key}")
                await self._uploader.delete_file(archive_key)
            await self._uploader.delete_file(sidecar_key)
            deleted_count += 1

        removed_epochs = {s[0] for s in to_delete} - kept_epochs
        for epoch in removed_epochs:
            logger.info(
                f"Cleaning snapshot metadata from epoch {epoch} "
                "(preserving epoch directory and files)"
            )
            await self._uploader.delete_file(self._index_key(epoch))
            await self._uploader.delete_file(self._lock_key(epoch))

        if deleted_count > 0:
            logger.info(
                f"Cleaned up {deleted_count} old snapshot(s), "
                f"keeping {keep} most recent"
            )
