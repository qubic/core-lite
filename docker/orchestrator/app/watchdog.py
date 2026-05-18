from __future__ import annotations

import asyncio
import logging
import time
from collections.abc import Awaitable, Callable
from typing import Optional

from app.alerting import AlertManager
from app.config import WatchdogConfig, WipeWindowConfig
from app.epoch_service import EpochService
from app.models import NodeHealth, NodeState, TickInfo
from app.node_client import NodeClient
from app.process_manager import ProcessManager
from app.wipe_window import is_in_window, window_id

logger = logging.getLogger(__name__)

# How often to send F4 (peer reset) while the node is stuck/misaligned.
_PEER_RESET_INTERVAL = 30


class Watchdog:
    """Monitors node health and triggers restarts when necessary."""

    def __init__(
        self,
        config: WatchdogConfig,
        node_client: NodeClient,
        process_manager: ProcessManager,
        alert_manager: AlertManager,
        qubic_args: list[str],
        epoch_service: EpochService | None = None,
        local_version: tuple[int, int] | None = None,
        on_state_incompatible: Optional[Callable[[], Awaitable[None]]] = None,
        wipe_window_config: Optional[WipeWindowConfig] = None,
        on_wipe_window_recovery: Optional[Callable[[], Awaitable[None]]] = None,
    ) -> None:
        self._config = config
        self._node_client = node_client
        self._process_manager = process_manager
        self._alert_manager = alert_manager
        self._qubic_args = qubic_args
        self._epoch_service = epoch_service
        self._local_version = local_version
        self._on_state_incompatible = on_state_incompatible
        self._wipe_window_config = wipe_window_config
        self._on_wipe_window_recovery = on_wipe_window_recovery
        # Per-window state: counts wipes performed in the current week's
        # window so we can cap retries and avoid infinite wipe loops.
        self._wipe_window_id: Optional[str] = None
        self._wipe_window_count: int = 0
        self._state = NodeState(health=NodeHealth.STARTING)
        self._state.last_tick_change_time = time.monotonic()
        self._snapshot_save_start_time: float = 0.0
        self._last_epoch_api_check: float = 0.0
        self._consecutive_epoch_behind_polls: int = 0
        # Misalignment tracking (time-based, requires same tick)
        self._misalignment_start_time: float | None = None
        self._misalignment_start_tick: int | None = None
        # Peer reset (F4) tracking.  F4 is sent every
        # _PEER_RESET_INTERVAL seconds while the node is unhealthy.
        # After _peer_reset_window seconds of F4 attempts without
        # recovery, escalate to a full restart.
        self._first_peer_reset_time: float | None = None
        self._last_peer_reset_time: float = 0
        # STATE_INCOMPATIBLE detection: count rapid failures after restart
        self._rapid_fail_count: int = 0

    @property
    def state(self) -> NodeState:
        return self._state

    async def run(self, shutdown_event: asyncio.Event) -> None:
        """Main watchdog loop. Runs until shutdown_event is set."""
        if not self._config.enabled:
            logger.info("Watchdog disabled")
            return

        logger.info(
            f"Watchdog started (poll={self._config.poll_interval_seconds}s, "
            f"stuck_threshold={self._config.stuck_threshold_seconds}s)"
        )

        # Grace period after startup
        grace_end = time.monotonic() + self._config.startup_grace_seconds
        while not shutdown_event.is_set() and time.monotonic() < grace_end:
            try:
                await asyncio.wait_for(
                    shutdown_event.wait(),
                    timeout=self._config.poll_interval_seconds,
                )
                return  # Shutdown requested
            except asyncio.TimeoutError:
                # Check if node API is up during grace period
                if await self._node_client.is_alive():
                    self._state.health = NodeHealth.HEALTHY
                    self._state.last_tick_change_time = time.monotonic()
                    logger.info("Node API responding, ending grace period early")
                    break

        # Main monitoring loop
        while not shutdown_event.is_set():
            try:
                await asyncio.wait_for(
                    shutdown_event.wait(),
                    timeout=self._config.poll_interval_seconds,
                )
                return  # Shutdown requested
            except asyncio.TimeoutError:
                pass

            try:
                health = await self._poll_health()
                prev_health = self._state.health
                self._state.health = health

                if health != prev_health:
                    logger.info(
                        f"Health state changed: {prev_health.value} -> {health.value}"
                    )

                # Reset peer-reset and rapid-fail tracking when node recovers
                if health == NodeHealth.HEALTHY:
                    self._first_peer_reset_time = None
                    self._last_peer_reset_time = 0
                    self._rapid_fail_count = 0

                # Post-epoch-transition wipe window: takes precedence over
                # the normal unhealthy-handling path so we recover faster
                # than the slow STATE_INCOMPATIBLE crashloop heuristic.
                if await self._maybe_wipe_for_epoch_transition(
                    health, shutdown_event
                ):
                    continue

                if health not in (
                    NodeHealth.HEALTHY,
                    NodeHealth.STARTING,
                    NodeHealth.SAVING_SNAPSHOT,
                    NodeHealth.VERSION_INCOMPATIBLE,
                    NodeHealth.STATE_INCOMPATIBLE,
                ):
                    await self._handle_unhealthy(health, shutdown_event)

            except asyncio.CancelledError:
                raise
            except Exception as e:
                logger.error(f"Watchdog poll error: {e}", exc_info=True)

    async def _poll_health(self) -> NodeHealth:
        """Assess current node health."""
        # 1. Check if process is alive
        if not self._process_manager.is_running():
            # Don't report as crashed if shutdown was intentionally requested
            if self._process_manager.shutdown_requested:
                logger.info("Process exited after intentional shutdown request")
                return NodeHealth.HEALTHY  # Prevent restart
            return NodeHealth.CRASHED

        # 2. Try to get tick info
        try:
            tick_info = await self._node_client.get_tick_info()
        except Exception as e:
            # Do NOT transition to STUCK on API failures: a slow/blocked HTTP
            # endpoint does not mean the node is unhealthy (the main loop may
            # still be ticking).  Just log and keep the previous health state.
            self._state.consecutive_stuck_polls += 1
            logger.warning(
                f"get_tick_info failed "
                f"(consecutive={self._state.consecutive_stuck_polls}): "
                f"{type(e).__name__}: {e}"
            )
            if self._state.health == NodeHealth.STARTING:
                return NodeHealth.STARTING
            return self._state.health

        # 3. Check if saving snapshot - do NOT interfere
        if tick_info.is_saving_snapshot:
            if self._state.health != NodeHealth.SAVING_SNAPSHOT:
                self._snapshot_save_start_time = time.monotonic()
                logger.info("Node is saving snapshot, suspending health checks")
            # Check for save timeout
            elapsed = time.monotonic() - self._snapshot_save_start_time
            if elapsed > self._config.stuck_threshold_seconds * 2:
                logger.warning(
                    f"Snapshot save has been running for {elapsed:.0f}s"
                )
            return NodeHealth.SAVING_SNAPSHOT

        # 4. Check tick progression
        now = time.monotonic()
        if (
            self._state.last_tick_info is not None
            and tick_info.tick == self._state.last_tick_info.tick
        ):
            elapsed = now - self._state.last_tick_change_time
            if elapsed > self._config.stuck_threshold_seconds:
                self._state.consecutive_stuck_polls += 1
                if (
                    self._state.consecutive_stuck_polls
                    >= self._config.stuck_consecutive_polls
                ):
                    return NodeHealth.STUCK
        else:
            # Tick is advancing
            self._state.last_tick_change_time = now
            self._state.consecutive_stuck_polls = 0

        # 5. Check for misalignment (time-based, only on same tick)
        is_misaligned = (
            tick_info.misaligned_votes
            >= self._config.misaligned_threshold_votes
        )
        if is_misaligned:
            current_tick = tick_info.tick
            if self._misalignment_start_time is None:
                # Start tracking misalignment
                self._misalignment_start_time = now
                self._misalignment_start_tick = current_tick
                logger.debug(
                    f"Misalignment detected at tick {current_tick}, "
                    f"votes={tick_info.misaligned_votes}"
                )
            elif current_tick != self._misalignment_start_tick:
                # Tick advanced while misaligned - reset, node is progressing
                logger.debug(
                    f"Tick advanced during misalignment "
                    f"({self._misalignment_start_tick} -> {current_tick}), "
                    "resetting misalignment timer"
                )
                self._misalignment_start_time = now
                self._misalignment_start_tick = current_tick
            else:
                # Same tick, check if threshold exceeded
                elapsed = now - self._misalignment_start_time
                if elapsed >= self._config.misaligned_threshold_seconds:
                    logger.warning(
                        f"Misaligned for {elapsed:.0f}s on tick {current_tick}"
                    )
                    return NodeHealth.MISALIGNED
        else:
            # Not misaligned - reset tracking
            if self._misalignment_start_time is not None:
                logger.debug("Node aligned, resetting misalignment timer")
            self._misalignment_start_time = None
            self._misalignment_start_tick = None

        # 6. Detect epoch transitions
        if (
            self._state.last_tick_info is not None
            and tick_info.epoch != self._state.last_tick_info.epoch
        ):
            await self._alert_manager.send_alert(
                "info",
                "epoch_transition",
                {
                    "old_epoch": self._state.last_tick_info.epoch,
                    "new_epoch": tick_info.epoch,
                    "tick": tick_info.tick,
                },
            )
            # Path 1: version check on tick-info epoch transition
            if self._epoch_service and self._local_version:
                compatible = await self._check_epoch_version()
                if not compatible:
                    self._state.last_tick_info = tick_info
                    return NodeHealth.VERSION_INCOMPATIBLE

        # 7. Periodic epoch API poll — catches nodes stuck on an old epoch
        now_api = time.monotonic()
        if (
            self._epoch_service
            and now_api - self._last_epoch_api_check
            > self._config.epoch_api_poll_seconds
        ):
            self._last_epoch_api_check = now_api
            try:
                api_epoch_info = (
                    await self._epoch_service.get_current_epoch_info()
                )
                node_epoch = tick_info.epoch if tick_info else 0

                if api_epoch_info.epoch > node_epoch:
                    # Version check first (if possible)
                    if (
                        self._local_version
                        and api_epoch_info.min_version is not None
                        and not EpochService.is_version_compatible(
                            self._local_version,
                            api_epoch_info.min_version,
                        )
                    ):
                        min_ver = EpochService.format_version(
                            api_epoch_info.min_version
                        )
                        local_ver = EpochService.format_version(
                            self._local_version
                        )
                        logger.critical(
                            f"Version {local_ver} incompatible with "
                            f"epoch {api_epoch_info.epoch} "
                            f"(requires >= {min_ver}). "
                            "Stopping node, waiting for update."
                        )
                        await self._alert_manager.send_alert(
                            "critical",
                            "version_incompatible",
                            {
                                "local_version": local_ver,
                                "min_version": min_ver,
                                "epoch": api_epoch_info.epoch,
                            },
                        )
                        await self._process_manager.stop()
                        self._state.last_tick_info = tick_info
                        return NodeHealth.VERSION_INCOMPATIBLE

                    # Epoch-behind detection
                    self._consecutive_epoch_behind_polls += 1
                    logger.info(
                        f"Node epoch {node_epoch} behind API "
                        f"epoch {api_epoch_info.epoch} "
                        f"(poll {self._consecutive_epoch_behind_polls}/"
                        f"{self._config.epoch_behind_restart_polls})"
                    )
                    if (
                        self._consecutive_epoch_behind_polls
                        >= self._config.epoch_behind_restart_polls
                    ):
                        self._state.last_tick_info = tick_info
                        return NodeHealth.EPOCH_BEHIND
                else:
                    self._consecutive_epoch_behind_polls = 0
            except Exception:
                pass  # Fail-open

        self._state.last_tick_info = tick_info
        return NodeHealth.HEALTHY

    async def _maybe_wipe_for_epoch_transition(
        self,
        health: NodeHealth,
        shutdown_event: asyncio.Event,
    ) -> bool:
        """Aggressive wipe-and-resync during the weekly transition window.

        Returns True if a wipe was performed this poll (caller should
        skip normal unhealthy handling).  Conditions:

        * wipe-window is configured and enabled
        * we're currently inside the window (default Wed 12:10–16:10 UTC)
        * wipes-per-window cap not yet reached
        * one of:
            - local tick is below the current epoch's ``initialTick``
            - node is CRASHED
            - node is STUCK (existing detection)
        """
        if (
            self._wipe_window_config is None
            or self._on_wipe_window_recovery is None
            or not self._wipe_window_config.enabled
            or self._epoch_service is None
        ):
            return False

        if not is_in_window(self._wipe_window_config):
            return False

        # Reset per-window counter at week rollover.
        current_id = window_id(self._wipe_window_config)
        if self._wipe_window_id != current_id:
            self._wipe_window_id = current_id
            self._wipe_window_count = 0

        if (
            self._wipe_window_count
            >= self._wipe_window_config.max_wipes_per_window
        ):
            return False

        # Need API + tick info to decide
        try:
            api_info = await self._epoch_service.get_current_epoch_info()
        except Exception:
            return False  # fail-open, try again next poll

        tick_info = self._state.last_tick_info

        needs_wipe = False
        reason = ""
        if health == NodeHealth.CRASHED:
            needs_wipe = True
            reason = "node crashed inside wipe window"
        elif health == NodeHealth.STUCK:
            needs_wipe = True
            reason = "node stuck inside wipe window"
        elif (
            tick_info is not None
            and tick_info.tick < api_info.initial_tick
        ):
            needs_wipe = True
            reason = (
                f"local tick {tick_info.tick} < "
                f"api.initialTick {api_info.initial_tick}"
            )

        if not needs_wipe:
            return False

        logger.warning(
            f"Wipe-window trigger: {reason} "
            f"(attempt {self._wipe_window_count + 1}"
            f"/{self._wipe_window_config.max_wipes_per_window})"
        )
        await self._alert_manager.send_alert(
            "warning",
            "wipe_window_recovery",
            {
                "reason": reason,
                "attempt": self._wipe_window_count + 1,
                "window_id": current_id,
            },
        )
        self._wipe_window_count += 1
        self._state.health = NodeHealth.STARTING
        try:
            await self._on_wipe_window_recovery()
        except Exception as e:
            logger.error(
                f"Wipe-window recovery failed: {e}", exc_info=True
            )
            await self._alert_manager.send_alert(
                "critical",
                "wipe_window_recovery_failed",
                {"error": str(e), "attempt": self._wipe_window_count},
            )
        # Re-arm timers so the next poll has a fresh baseline
        self._state.last_tick_change_time = time.monotonic()
        self._misalignment_start_time = None
        self._misalignment_start_tick = None
        return True

    async def _handle_unhealthy(
        self,
        health: NodeHealth,
        shutdown_event: asyncio.Event,
    ) -> None:
        """Handle an unhealthy node state."""
        if health == NodeHealth.SAVING_SNAPSHOT:
            return  # Never restart during snapshot save

        if health == NodeHealth.CRASHED:
            exit_code = self._process_manager.get_return_code()
            await self._alert_manager.send_alert(
                "error",
                "node_crashed",
                {
                    "exit_code": exit_code,
                    "restart_count": self._state.restart_count,
                },
            )
            if not shutdown_event.is_set():
                await asyncio.sleep(
                    self._config.crash_restart_delay_seconds
                )
                await self._do_restart("crashed", shutdown_event)

        elif health in (
            NodeHealth.STUCK,
            NodeHealth.MISALIGNED,
            NodeHealth.EPOCH_BEHIND,
        ):
            if self._state.restart_count >= self._config.max_restarts:
                await self._alert_manager.send_alert(
                    "critical",
                    "max_restarts_exceeded",
                    {
                        "restart_count": self._state.restart_count,
                        "health": health.value,
                    },
                )
                logger.error(
                    f"Max restarts ({self._config.max_restarts}) exceeded. "
                    "Manual intervention required."
                )
                return

            now = time.monotonic()
            if (
                now - self._state.last_restart_time
                < self._config.restart_cooldown_seconds
            ):
                logger.info(
                    "Restart cooldown active, skipping restart"
                )
                return

            tick_info = self._state.last_tick_info

            # For stuck/misaligned: repeatedly send F4 (peer reset)
            # every _PEER_RESET_INTERVAL seconds.  Only escalate to a
            # full restart after F4 has been tried for at least as long
            # as the original detection threshold.  Skip for
            # epoch_behind since reconnecting won't help.
            if health in (NodeHealth.STUCK, NodeHealth.MISALIGNED):
                now_f4 = time.monotonic()

                if self._first_peer_reset_time is None:
                    # First F4 for this unhealthy episode
                    self._first_peer_reset_time = now_f4
                    self._last_peer_reset_time = 0  # force immediate send

                # How long to keep trying F4 before escalating
                peer_reset_window = (
                    self._config.stuck_threshold_seconds
                    if health == NodeHealth.STUCK
                    else self._config.misaligned_threshold_seconds
                )
                elapsed_since_first = (
                    now_f4 - self._first_peer_reset_time
                )

                if elapsed_since_first < peer_reset_window:
                    # Still within the F4 window — send if interval elapsed
                    if (
                        now_f4 - self._last_peer_reset_time
                        >= _PEER_RESET_INTERVAL
                    ):
                        self._last_peer_reset_time = now_f4
                        logger.info(
                            f"Node is {health.value}, "
                            f"sending peer reset (F4) "
                            f"[{elapsed_since_first:.0f}s "
                            f"into {peer_reset_window}s window]"
                        )
                        await self._process_manager.send_key("f4")
                    return

                # F4 window exhausted — fall through to restart
                logger.info(
                    f"Peer reset window ({peer_reset_window}s) "
                    "exhausted, escalating to restart"
                )

            # Check for rapid failure pattern (STATE_INCOMPATIBLE detection).
            # If the node keeps becoming stuck/crashed shortly after each
            # restart, the local state files are likely incompatible with the
            # binary (e.g. contract state layout changed across epochs).
            if (
                health in (NodeHealth.STUCK, NodeHealth.CRASHED)
                and self._state.restart_count > 0
            ):
                time_since_restart = (
                    time.monotonic() - self._state.last_restart_time
                )
                if (
                    time_since_restart
                    < self._config.rapid_fail_threshold_seconds
                ):
                    self._rapid_fail_count += 1
                    logger.warning(
                        f"Rapid failure detected "
                        f"({self._rapid_fail_count}/"
                        f"{self._config.rapid_fail_count_for_incompatible}): "
                        f"node became {health.value} "
                        f"{time_since_restart:.0f}s after restart"
                    )
                    if (
                        self._rapid_fail_count
                        >= self._config.rapid_fail_count_for_incompatible
                    ):
                        await self._handle_state_incompatible(
                            shutdown_event
                        )
                        return

            await self._alert_manager.send_alert(
                "warning",
                f"node_{health.value}",
                {
                    "tick": tick_info.tick if tick_info else 0,
                    "epoch": tick_info.epoch if tick_info else 0,
                    "restart_count": self._state.restart_count,
                },
            )
            if not shutdown_event.is_set():
                await self._do_restart(health.value, shutdown_event)

    async def _handle_state_incompatible(
        self, shutdown_event: asyncio.Event
    ) -> None:
        """Handle state incompatibility by cleaning local files and restarting.

        When the node repeatedly fails quickly after restart, local state
        files are likely incompatible with the current binary.  The
        recovery callback (provided by the orchestrator) stops the node,
        deletes the local epoch files, and restarts through the normal
        startup flow which re-downloads fresh files.
        """
        tick_info = self._state.last_tick_info
        epoch = tick_info.epoch if tick_info else 0

        logger.error(
            f"State files appear incompatible with current epoch {epoch} "
            f"(rapid_fail_count={self._rapid_fail_count})"
        )
        self._state.health = NodeHealth.STATE_INCOMPATIBLE

        await self._alert_manager.send_alert(
            "critical",
            "state_incompatible",
            {
                "epoch": epoch,
                "rapid_fail_count": self._rapid_fail_count,
                "restart_count": self._state.restart_count,
            },
        )

        if self._on_state_incompatible and not shutdown_event.is_set():
            await self._process_manager.stop()
            await self._on_state_incompatible()
            # Reset tracking after recovery
            self._rapid_fail_count = 0
            self._state.restart_count = 0
            self._state.health = NodeHealth.STARTING
            self._state.consecutive_stuck_polls = 0
            self._misalignment_start_time = None
            self._misalignment_start_tick = None
            self._first_peer_reset_time = None
            self._last_peer_reset_time = 0
            self._state.last_tick_change_time = time.monotonic()
        else:
            logger.error(
                "No state_incompatible handler configured, "
                "manual intervention required"
            )

    async def _do_restart(
        self, reason: str, shutdown_event: asyncio.Event
    ) -> None:
        """Restart the Qubic process."""
        if shutdown_event.is_set():
            return

        self._state.restart_count += 1
        self._state.last_restart_time = time.monotonic()
        logger.info(
            f"Restarting node (reason={reason}, "
            f"count={self._state.restart_count})"
        )

        await self._alert_manager.send_alert(
            "info",
            "node_restarted",
            {
                "reason": reason,
                "restart_count": self._state.restart_count,
            },
        )

        try:
            await self._process_manager.restart(self._qubic_args)
            self._state.health = NodeHealth.STARTING
            self._state.consecutive_stuck_polls = 0
            self._consecutive_epoch_behind_polls = 0
            self._misalignment_start_time = None
            self._misalignment_start_tick = None
            self._first_peer_reset_time = None
            self._last_peer_reset_time = 0
            self._state.last_tick_change_time = time.monotonic()
        except Exception as e:
            logger.error(f"Failed to restart node: {e}")

    async def _check_epoch_version(self) -> bool:
        """Re-fetch epoch info and check version compatibility.

        Returns ``True`` if compatible or no ``minVersion`` set,
        ``False`` if incompatible.  Fail-open on errors.
        """
        try:
            epoch_info = await self._epoch_service.get_current_epoch_info()
            if epoch_info.min_version is None:
                return True
            if EpochService.is_version_compatible(
                self._local_version, epoch_info.min_version
            ):
                return True

            min_ver = EpochService.format_version(epoch_info.min_version)
            local_ver = EpochService.format_version(self._local_version)
            logger.critical(
                f"Version {local_ver} incompatible with "
                f"epoch {epoch_info.epoch} "
                f"(requires >= {min_ver}). "
                "Stopping node, waiting for update."
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
            await self._process_manager.stop()
            return False
        except Exception as e:
            logger.warning(
                f"Version check failed (allowing node to continue): {e}"
            )
            return True  # Fail-open
