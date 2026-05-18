"""Post-epoch-transition wipe window.

On non-seamless epoch transitions the network restarts from scratch with a
new initial tick, and lite nodes carrying state from the previous epoch
will fail to sync.  Recovery requires wiping the local data dir so the
node downloads a fresh snapshot for the new epoch.

This module computes a weekly observation window (default: 4 hours from
Wed 12:10 UTC) during which the watchdog applies an aggressive
"behind-the-network → wipe everything" policy.  Outside the window, the
usual STATE_INCOMPATIBLE rapid-failure path is the safety net.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone

from app.config import WipeWindowConfig


def most_recent_marker(
    now_utc: datetime, weekday: int, hour: int, minute: int
) -> datetime:
    """Return the most recent occurrence of ``weekday hour:minute UTC``.

    ``weekday`` is 0=Mon … 6=Sun.  The result is always strictly in the
    past (or exactly equal to ``now_utc``).
    """
    days_back = (now_utc.weekday() - weekday) % 7
    candidate = (now_utc - timedelta(days=days_back)).replace(
        hour=hour, minute=minute, second=0, microsecond=0
    )
    if candidate > now_utc:
        candidate -= timedelta(days=7)
    return candidate


def is_in_window(
    config: WipeWindowConfig, now_utc: datetime | None = None
) -> bool:
    """Return True if we are inside the post-transition wipe window."""
    if not config.enabled:
        return False
    if now_utc is None:
        now_utc = datetime.now(timezone.utc)
    marker = most_recent_marker(
        now_utc, config.weekday_utc, config.hour_utc, config.minute_utc
    )
    end = marker + timedelta(hours=config.duration_hours)
    return marker <= now_utc < end


def window_id(
    config: WipeWindowConfig, now_utc: datetime | None = None
) -> str:
    """Stable identifier for the current week's window.

    Used to reset per-window counters (e.g. ``max_wipes_per_window``) when
    a new week's window starts.  Returns the ISO date of the marker, e.g.
    ``"2026-05-13"`` for the window opened Wed 13 May 2026.
    """
    if now_utc is None:
        now_utc = datetime.now(timezone.utc)
    marker = most_recent_marker(
        now_utc, config.weekday_utc, config.hour_utc, config.minute_utc
    )
    return marker.date().isoformat()
