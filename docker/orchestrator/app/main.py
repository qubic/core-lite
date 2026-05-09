from __future__ import annotations

import asyncio
import logging
import secrets
import shutil
import signal
import time
from pathlib import Path
from typing import Optional

import aiohttp

from app.alerting import AlertManager
from app.cleanup import Cleanup
from app.config import OrchestratorConfig, load_config
from app.downloaders import create_downloader
from app.epoch_service import EpochService
from app.local_snapshot_saver import LocalSnapshotSaver
from app.logging_config import setup_logging
from app.management_api import ManagementAPI
from app.models import EpochInfo, NodeHealth, OrchestratorMode
from app.node_client import NodeClient
from app.process_manager import ProcessManager
from app.snapshot_cycle import SnapshotCycle
from app.state_manager import StateManager
from app.uploaders import create_uploader
from app.watchdog import Watchdog

logger = logging.getLogger(__name__)


class Orchestrator:
    """Main orchestrator - runs as PID 1 in the Docker container."""

    def __init__(self, config: OrchestratorConfig) -> None:
        self._config = config
        self._shutdown_event = asyncio.Event()
        self._data_dir = Path(config.data_dir)
        self._tasks: list[asyncio.Task] = []
        self._start_time = time.monotonic()

        # Components (initialized in startup)
        self._node_client: Optional[NodeClient] = None
        self._epoch_service: Optional[EpochService] = None
        self._state_manager: Optional[StateManager] = None
        self._process_manager: Optional[ProcessManager] = None
        self._alert_manager: Optional[AlertManager] = None
        self._watchdog: Optional[Watchdog] = None
        self._snapshot_cycle: Optional[SnapshotCycle] = None
        self._local_snapshot_saver: Optional[LocalSnapshotSaver] = None
        self._management_api: Optional[ManagementAPI] = None
        self._local_version: tuple[int, int] | None = None

    async def run(self) -> None:
        """Main entry point for the orchestrator."""
        self._setup_signal_handlers()

        logger.info("=" * 60)
        logger.info("Qubic Docker Orchestrator starting")
        logger.info(f"Mode: {self._config.mode.value}")
        logger.info(f"Data dir: {self._config.data_dir}")
        logger.info("=" * 60)

        try:
            # Initialize components
            self._init_components()

            # Ensure the volume has the binary from the current image
            self._install_binary()

            # Read local binary version
            self._local_version = EpochService.read_local_version(
                str(self._data_dir / "version.txt")
            )
            if self._local_version:
                logger.info(
                    f"Node version: "
                    f"{EpochService.format_version(self._local_version)}"
                )

            # Startup sequence
            epoch_info = await self._discover_epoch()

            # Version compatibility check (only when API provides minVersion)
            if epoch_info.min_version is not None:
                if not EpochService.is_version_compatible(
                    self._local_version, epoch_info.min_version
                ):
                    await self._wait_for_compatible_version(epoch_info)
                    return  # Shutdown requested while waiting

            await self._ensure_state_files(epoch_info)

            # Build CLI args and start the node
            qubic_args = self._build_qubic_args(epoch_info)
            await self._process_manager.start(qubic_args)

            # Wait for the node HTTP API to become available
            await self._wait_for_node_api()

            # Launch background tasks
            self._launch_background_tasks(qubic_args)

            # Main loop: wait for shutdown or child exit
            await self._main_loop()

        except asyncio.CancelledError:
            pass
        except Exception as e:
            logger.error(f"Orchestrator fatal error: {e}", exc_info=True)
        finally:
            await self._shutdown()

    def _setup_signal_handlers(self) -> None:
        loop = asyncio.get_event_loop()
        for sig in (signal.SIGTERM, signal.SIGINT):
            loop.add_signal_handler(
                sig, self._handle_shutdown_signal, sig
            )
        # NOTE: We intentionally do NOT install a SIGCHLD handler or call
        # os.waitpid(-1).  Either approach races with asyncio's subprocess
        # child watcher, consuming exit statuses of SSH/SCP child processes
        # before asyncio can read them (causing spurious returncode=255).
        # If zombie accumulation from orphaned grandchildren becomes an
        # issue, use Docker's --init flag to add tini as PID 1.

    def _handle_shutdown_signal(self, sig: signal.Signals) -> None:
        logger.info(f"Received {sig.name}, initiating graceful shutdown")
        self._shutdown_event.set()

    def _init_components(self) -> None:
        """Initialize all orchestrator components."""
        # Generate random HTTP passcode if not configured
        # Format: 4 random numbers separated by dashes (e.g., "123-456-789-012")
        if not self._config.http_passcode:
            passcode = "-".join(str(secrets.randbelow(10**12)) for _ in range(4))
            # Update config so build_qubic_args() will include it
            object.__setattr__(self._config, "http_passcode", passcode)
            logger.info("Generated random HTTP passcode for node API authentication")

        self._node_client = NodeClient(
            passcode=self._config.http_passcode,
        )
        self._alert_manager = AlertManager(self._config.alerting)

        compiled_epoch = EpochService.read_compiled_epoch(
            str(self._data_dir / "epoch.txt")
        )
        if self._config.compiled_epoch is not None:
            compiled_epoch = self._config.compiled_epoch

        self._epoch_service = EpochService(
            epoch_api_url=self._config.epoch_api_url,
            snapshot_service_url=self._config.snapshot_service_url,
            compiled_epoch=compiled_epoch,
        )

        downloader = create_downloader(self._config.downloader)
        self._state_manager = StateManager(
            self._data_dir,
            downloader,
            download_concurrency=self._config.downloader.snapshot_download_concurrency,
        )

        self._process_manager = ProcessManager(
            binary_path=Path(self._config.binary_path),
            node_client=self._node_client,
            working_dir=self._data_dir,
        )

    def _install_binary(self) -> None:
        """Copy binary and metadata from the image staging dir to the data volume.

        Docker only populates a named volume on first use.  When Watchtower
        (or any updater) pulls a new image, the old binary persists on the
        volume.  This method ensures the volume always has the binary that
        shipped with the current image.

        The copy is skipped when the volume already has a binary whose
        version is equal to or newer than the staged one.
        """
        staging_dir = Path(self._config.binary_staging_dir)
        if not staging_dir.is_dir():
            logger.debug(
                f"No staging directory at {staging_dir}, "
                "skipping binary install"
            )
            return

        staged_ver = EpochService.read_local_version(
            str(staging_dir / "version.txt")
        )
        local_ver = EpochService.read_local_version(
            str(self._data_dir / "version.txt")
        )

        # Compare version and file modification time — a recompiled binary
        # with the same version but a newer mtime should still be installed.
        staged_bin = staging_dir / "Qubic"
        local_bin = self._data_dir / "Qubic"
        staged_newer = False
        if staged_bin.exists() and local_bin.exists():
            staged_newer = staged_bin.stat().st_mtime > local_bin.stat().st_mtime

        if staged_ver and local_ver and local_ver >= staged_ver and not staged_newer:
            logger.info(
                f"Binary on volume "
                f"({EpochService.format_version(local_ver)}) is "
                f"up-to-date with staged "
                f"({EpochService.format_version(staged_ver)}), "
                "skipping install"
            )
            return

        staged_fmt = (
            EpochService.format_version(staged_ver) if staged_ver else "?"
        )
        local_fmt = (
            EpochService.format_version(local_ver) if local_ver else "none"
        )
        logger.info(
            f"Updating binary on volume: {local_fmt} -> {staged_fmt}"
        )

        for name in ("Qubic", "epoch.txt", "version.txt"):
            src = staging_dir / name
            if not src.exists():
                continue
            dst = self._data_dir / name
            shutil.copy2(src, dst)
            if name == "Qubic":
                dst.chmod(0o755)
            logger.info(f"Installed {name} from image staging dir")

    @staticmethod
    async def _get_public_ip() -> Optional[str]:
        """Resolve the node's public IP via an external service."""
        services = [
            "https://api.ipify.org",
            "https://ifconfig.me/ip",
            "https://icanhazip.com",
        ]
        timeout = aiohttp.ClientTimeout(total=5)
        async with aiohttp.ClientSession(timeout=timeout) as session:
            for url in services:
                try:
                    async with session.get(url) as resp:
                        if resp.status == 200:
                            ip = (await resp.text()).strip()
                            if ip:
                                return ip
                except Exception:
                    continue
        return None

    async def _discover_epoch(self) -> EpochInfo:
        """Step 3: Query epoch info API or fall back to compiled epoch."""
        logger.info("Discovering current epoch information...")
        fallback_peers = self._config.get_peers_list()

        epoch_info = await self._epoch_service.get_epoch_info_or_fallback(
            fallback_peers=fallback_peers,
        )

        logger.info(
            f"Target epoch: {epoch_info.epoch}, "
            f"initialTick: {epoch_info.initial_tick}, "
            f"peers: {len(epoch_info.peers)}"
        )

        # Merge peers from API with configured peers
        configured_peers = set(self._config.get_peers_list())
        api_peers = set(epoch_info.peers)
        all_peers = configured_peers | api_peers

        # Add the node's own public IP so it can find itself in the network
        public_ip = await self._get_public_ip()
        if public_ip:
            logger.info(f"Public IP: {public_ip}")
            all_peers.add(public_ip)
        else:
            logger.warning("Could not determine public IP")

        epoch_info.peers = list(all_peers)

        return epoch_info

    async def _wait_for_compatible_version(
        self, epoch_info: EpochInfo
    ) -> None:
        """Block until shutdown when the local binary is incompatible.

        Logs a critical message, sends an alert, starts the management
        API (so operators can query ``/status``), and waits for SIGTERM
        (typically sent by Watchtower after pulling a new image).
        """
        min_ver = EpochService.format_version(epoch_info.min_version)
        local_ver = (
            EpochService.format_version(self._local_version)
            if self._local_version
            else "unknown"
        )
        logger.critical(
            f"Version {local_ver} incompatible with epoch "
            f"{epoch_info.epoch} (requires >= {min_ver}). "
            "Waiting for container update."
        )
        await self._alert_manager.send_alert(
            "critical",
            "version_incompatible",
            {
                "local_version": local_ver,
                "min_version": min_ver,
                "epoch": epoch_info.epoch,
            },
        )

        # Start management API so /status is reachable
        self._management_api = ManagementAPI(
            config=self._config,
            process_manager=self._process_manager,
            watchdog=None,
            snapshot_cycle=None,
            qubic_args=[],
            start_time=self._start_time,
            local_version=self._local_version,
            version_health=NodeHealth.VERSION_INCOMPATIBLE,
        )
        self._tasks.append(
            asyncio.create_task(
                self._run_management_api(),
                name="management_api",
            )
        )

        # Wait loop — log periodically until SIGTERM
        while not self._shutdown_event.is_set():
            try:
                await asyncio.wait_for(
                    self._shutdown_event.wait(), timeout=300
                )
            except asyncio.TimeoutError:
                logger.info(
                    f"Still waiting for update (current: {local_ver}, "
                    f"required: >= {min_ver})"
                )

    async def _ensure_state_files(self, epoch_info: EpochInfo) -> None:
        """Steps 4-6: Check local state, download if needed.

        Retries both snapshot and epoch file downloads in a loop until
        files are available locally.  This handles the case where a new
        epoch just started and no remote files have been published yet.
        """
        epoch = epoch_info.epoch
        local_epoch = self._state_manager.get_local_epoch()

        logger.info(
            f"Local epoch: {local_epoch}, target epoch: {epoch}"
        )

        # Check if local state is usable
        if (
            not self._config.force_download
            and local_epoch == epoch
            and self._state_manager.has_valid_state_files(epoch)
        ):
            logger.info("Local state files are current, skipping download")
            return

        if self._config.force_download:
            logger.info("Force download enabled")

        epoch_url = (
            f"{self._config.snapshot_service_url}"
            f"/network/{epoch}/ep{epoch}-full.zip"
        )

        # Retry loop: try snapshot first, then epoch files
        while not self._shutdown_event.is_set():
            try:
                # Try snapshot
                logger.info("Checking snapshot service for available snapshots...")
                snapshot_meta = (
                    await self._epoch_service.get_latest_snapshot_meta(epoch)
                )
                if snapshot_meta is not None:
                    logger.info(f"Snapshot available: {snapshot_meta.url}")
                    success = await self._state_manager.download_snapshot(
                        snapshot_meta
                    )
                    if success and self._state_manager.has_valid_state_files(
                        epoch
                    ):
                        logger.info("Snapshot downloaded and validated")
                        return
                    logger.warning(
                        "Snapshot download failed, trying epoch files"
                    )

                # Try epoch files
                logger.info(f"Downloading epoch files from: {epoch_url}")
                success = await self._state_manager.download_epoch_files(
                    epoch, epoch_url
                )
                if success and self._state_manager.has_valid_state_files(epoch):
                    logger.info("Epoch files downloaded and validated")
                    return
            except Exception as e:
                logger.warning(f"Download attempt failed: {e}")

            logger.info(
                f"Epoch {epoch} files not yet available locally or "
                "remotely, retrying in 60s..."
            )
            try:
                await asyncio.wait_for(
                    self._shutdown_event.wait(), timeout=60
                )
                return  # Shutdown requested
            except asyncio.TimeoutError:
                pass

    async def _handle_state_incompatible(self) -> None:
        """Recovery handler for STATE_INCOMPATIBLE.

        Called by the watchdog when local state files are incompatible
        with the current binary (e.g. contract state layout changed
        across epochs).  Deletes local epoch files so the normal startup
        flow re-downloads fresh ones.
        """
        epoch = self._state_manager.get_local_epoch()
        if epoch is not None:
            logger.warning(
                f"Removing local state files for epoch {epoch} "
                "(state incompatible)"
            )
            self._state_manager.delete_epoch_files(epoch)

        # Re-discover epoch and download fresh files
        epoch_info = await self._discover_epoch()
        await self._ensure_state_files(epoch_info)

        # Rebuild args and restart
        qubic_args = self._build_qubic_args(epoch_info)
        self._watchdog._qubic_args = qubic_args
        await self._process_manager.start(qubic_args)
        await self._wait_for_node_api()

    def _build_qubic_args(self, epoch_info: EpochInfo) -> list[str]:
        """Step 7: Build CLI arguments for the Qubic binary."""
        args = self._config.build_qubic_args()

        # If the config didn't specify peers but the API gave us some, use those
        if not self._config.peers and epoch_info.peers:
            args.extend(["--peers", ",".join(epoch_info.peers)])

        logger.info(f"Qubic args: {' '.join(args)}")
        return args

    async def _wait_for_node_api(self) -> None:
        """Step 8: Wait for the Qubic HTTP API to become available."""
        logger.info("Waiting for Qubic HTTP API to become available...")
        deadline = 300  # seconds
        elapsed = 0
        poll_interval = 2

        while elapsed < deadline and not self._shutdown_event.is_set():
            if await self._node_client.is_alive():
                tick_info = await self._node_client.get_tick_info()
                logger.info(
                    f"Node API is up - epoch={tick_info.epoch}, "
                    f"tick={tick_info.tick}"
                )
                return

            if not self._process_manager.is_running():
                exit_code = self._process_manager.get_return_code()
                raise RuntimeError(
                    f"Qubic process exited during startup "
                    f"(code {exit_code})"
                )

            await asyncio.sleep(poll_interval)
            elapsed += poll_interval

        if not self._shutdown_event.is_set():
            raise RuntimeError(
                f"Qubic HTTP API did not become available within {deadline}s"
            )

    def _launch_background_tasks(self, qubic_args: list[str]) -> None:
        """Steps 9-10: Launch watchdog and snapshot cycle."""
        # Watchdog (always)
        self._watchdog = Watchdog(
            config=self._config.watchdog,
            node_client=self._node_client,
            process_manager=self._process_manager,
            alert_manager=self._alert_manager,
            qubic_args=qubic_args,
            epoch_service=self._epoch_service,
            local_version=self._local_version,
            on_state_incompatible=self._handle_state_incompatible,
        )
        self._tasks.append(
            asyncio.create_task(
                self._watchdog.run(self._shutdown_event),
                name="watchdog",
            )
        )

        # Snapshot cycle (source mode only)
        if self._config.mode == OrchestratorMode.SOURCE:
            uploader = create_uploader(self._config.source)
            self._snapshot_cycle = SnapshotCycle(
                config=self._config.source,
                node_client=self._node_client,
                state_manager=self._state_manager,
                uploader=uploader,
                alert_manager=self._alert_manager,
                data_dir=self._data_dir,
            )
            self._tasks.append(
                asyncio.create_task(
                    self._snapshot_cycle.run(self._shutdown_event),
                    name="snapshot_cycle",
                )
            )
            logger.info("Source mode: snapshot cycle enabled")

        # Cleanup task (periodic removal of old state files)
        if self._config.cleanup.enabled:
            cleanup = Cleanup(
                state_manager=self._state_manager,
                interval_seconds=self._config.cleanup.interval_seconds,
                keep_epochs=self._config.cleanup.keep_epochs,
            )
            self._tasks.append(
                asyncio.create_task(
                    cleanup.run(self._shutdown_event),
                    name="cleanup",
                )
            )

        # Local snapshot saver (periodic F8-style saves)
        if self._config.local_snapshot.enabled:
            self._local_snapshot_saver = LocalSnapshotSaver(
                config=self._config.local_snapshot,
                node_client=self._node_client,
                watchdog=self._watchdog,
            )
            self._tasks.append(
                asyncio.create_task(
                    self._local_snapshot_saver.run(self._shutdown_event),
                    name="local_snapshot_saver",
                )
            )
            logger.info("Local snapshot saver enabled")

        # Management API (always)
        self._management_api = ManagementAPI(
            config=self._config,
            process_manager=self._process_manager,
            watchdog=self._watchdog,
            snapshot_cycle=self._snapshot_cycle,
            qubic_args=qubic_args,
            start_time=self._start_time,
            local_version=self._local_version,
        )
        self._tasks.append(
            asyncio.create_task(
                self._run_management_api(),
                name="management_api",
            )
        )

        logger.info("Background tasks launched")

    async def _run_management_api(self) -> None:
        """Run the management API server until shutdown."""
        try:
            await self._management_api.start()
            await self._shutdown_event.wait()
        except asyncio.CancelledError:
            pass
        finally:
            await self._management_api.stop()

    async def _main_loop(self) -> None:
        """Step 11: Wait for shutdown signal or unexpected child exit."""
        while not self._shutdown_event.is_set():
            # Check if the process died unexpectedly (watchdog handles restart,
            # but we also monitor here for the case where watchdog isn't running)
            if (
                not self._process_manager.is_running()
                and not self._shutdown_event.is_set()
            ):
                # Let watchdog handle it if enabled
                if not self._config.watchdog.enabled:
                    logger.error(
                        "Qubic process exited and watchdog is disabled"
                    )
                    break

            try:
                await asyncio.wait_for(
                    self._shutdown_event.wait(), timeout=5.0
                )
            except asyncio.TimeoutError:
                pass

    async def _shutdown(self) -> None:
        """Graceful shutdown sequence."""
        logger.info("Shutting down orchestrator...")

        # Save node state before stopping anything
        if (
            self._local_snapshot_saver
            and self._process_manager
            and self._process_manager.is_running()
        ):
            logger.info("Saving node state before shutdown...")
            await self._local_snapshot_saver.save_and_wait()

        self._shutdown_event.set()

        # Cancel background tasks
        for task in self._tasks:
            task.cancel()
        if self._tasks:
            await asyncio.gather(*self._tasks, return_exceptions=True)

        # Stop the Qubic process
        if self._process_manager and self._process_manager.is_running():
            exit_code = await self._process_manager.stop(timeout=150.0)
            logger.info(f"Qubic process exited with code {exit_code}")

        # Cleanup HTTP sessions
        if self._node_client:
            await self._node_client.close()
        if self._epoch_service:
            await self._epoch_service.close()
        if self._alert_manager:
            await self._alert_manager.close()

        logger.info("Orchestrator shutdown complete")


def run_orchestrator() -> None:
    """Load config and run the orchestrator."""
    config = load_config()
    setup_logging(level=config.log_level, fmt=config.log_format)

    orchestrator = Orchestrator(config)
    asyncio.run(orchestrator.run())
