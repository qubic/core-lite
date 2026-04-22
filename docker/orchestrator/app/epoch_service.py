from __future__ import annotations

import logging
from pathlib import Path
from typing import Optional

import aiohttp

from app.models import EpochInfo, SnapshotChunkInfo, SnapshotMeta

logger = logging.getLogger(__name__)

REQUEST_TIMEOUT = aiohttp.ClientTimeout(total=30)


class EpochService:
    """Client for external epoch info API and snapshot service."""

    def __init__(
        self,
        epoch_api_url: str,
        snapshot_service_url: str,
        compiled_epoch: Optional[int] = None,
    ) -> None:
        self._epoch_api_url = epoch_api_url.rstrip("/")
        self._snapshot_service_url = snapshot_service_url.rstrip("/")
        self._compiled_epoch = compiled_epoch
        self._session: aiohttp.ClientSession | None = None

    async def _get_session(self) -> aiohttp.ClientSession:
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession(timeout=REQUEST_TIMEOUT)
        return self._session

    async def close(self) -> None:
        if self._session and not self._session.closed:
            await self._session.close()

    async def get_current_epoch_info(self) -> EpochInfo:
        """Query the epoch info API for current epoch, initialTick, and peers.

        The configured URL is called directly (no path appended).
        Expected JSON: ``{"epoch": N, "initialTick": N, "peers": [...],
        "minVersion": "1.280"}``

        The ``minVersion`` field is optional.  When absent, no version
        restriction is applied.
        """
        session = await self._get_session()
        try:
            async with session.get(self._epoch_api_url) as resp:
                resp.raise_for_status()
                data = await resp.json()

                min_ver: tuple[int, int] | None = None
                raw = data.get("minVersion")
                if raw:
                    min_ver = EpochService.parse_version(str(raw))

                return EpochInfo(
                    epoch=data["epoch"],
                    initial_tick=data["initialTick"],
                    peers=data.get("peers", []),
                    min_version=min_ver,
                )
        except (aiohttp.ClientError, KeyError, TimeoutError) as e:
            logger.warning(f"Failed to get epoch info from API: {e}")
            raise

    async def get_epoch_info_or_fallback(
        self,
        fallback_peers: list[str] | None = None,
    ) -> EpochInfo:
        """Get epoch info from API, falling back to compiled epoch if unavailable."""
        try:
            return await self.get_current_epoch_info()
        except Exception:
            if self._compiled_epoch is not None:
                logger.warning(
                    f"Using compiled epoch {self._compiled_epoch} as fallback"
                )
                return EpochInfo(
                    epoch=self._compiled_epoch,
                    initial_tick=0,
                    peers=fallback_peers or [],
                    min_version=None,
                )
            raise

    async def get_latest_snapshot_meta(
        self, epoch: int
    ) -> Optional[SnapshotMeta]:
        """Fetch ``ep{epoch}-latest-snap.json`` and return a SnapshotMeta.

        Supports two formats:

        * ``chunked-tar-zst`` — the index has a ``dir`` and ``chunks`` list.
          ``SnapshotMeta.url`` is the chunk directory URL, and
          ``SnapshotMeta.chunks`` is populated with one entry per chunk.
        * ``single-file`` — the index has a ``file`` field. ``SnapshotMeta.url``
          is the archive URL and ``SnapshotMeta.chunks`` is empty.
        """
        session = await self._get_session()

        index_url = (
            f"{self._snapshot_service_url}/network/{epoch}/"
            f"ep{epoch}-latest-snap.json"
        )
        try:
            async with session.get(index_url) as resp:
                if resp.status != 200:
                    logger.debug(
                        f"No snapshot index for epoch {epoch} "
                        f"(HTTP {resp.status})"
                    )
                    return None
                data = await resp.json()
        except (aiohttp.ClientError, TimeoutError) as e:
            logger.debug(f"Index lookup failed for epoch {epoch}: {e}")
            return None
        except Exception as e:
            logger.warning(f"Unexpected index parse error for epoch {epoch}: {e}")
            return None

        fmt = data.get("format", "single-file")
        tick = data.get("tick", 0)
        epoch_from_index = data.get("epoch", epoch)
        epoch_base_url = f"{self._snapshot_service_url}/network/{epoch_from_index}"

        if fmt == "chunked-tar-zst":
            dir_name = data.get("dir") or f"snap-t{tick}"
            chunks_raw = data.get("chunks") or []
            chunks = [
                SnapshotChunkInfo(
                    filename=c["filename"],
                    size=int(c.get("size", 0)),
                    checksum=c.get("checksum", ""),
                    url=f"{epoch_base_url}/{dir_name}/{c['filename']}",
                )
                for c in chunks_raw
            ]
            return SnapshotMeta(
                epoch=epoch_from_index,
                tick=tick,
                timestamp=data.get("timestamp", ""),
                url=f"{epoch_base_url}/{dir_name}",
                checksum="",
                size_bytes=data.get("size_bytes", 0),
                chunks=chunks,
            )

        # single-file format
        filename = data.get("file") or f"ep{epoch_from_index}-t{tick}-snap.tar.zst"
        return SnapshotMeta(
            epoch=epoch_from_index,
            tick=tick,
            timestamp=data.get("timestamp", ""),
            url=f"{epoch_base_url}/{filename}",
            checksum=data.get("checksum", ""),
            size_bytes=data.get("size_bytes", 0),
        )

    async def check_snapshot_available(self, epoch: int) -> bool:
        meta = await self.get_latest_snapshot_meta(epoch)
        return meta is not None

    def get_compiled_epoch(self) -> Optional[int]:
        return self._compiled_epoch

    @staticmethod
    def read_compiled_epoch(epoch_file: str = "/qubic/epoch.txt") -> Optional[int]:
        """Read the compiled epoch number from the epoch.txt file."""
        try:
            path = Path(epoch_file)
            if path.exists():
                text = path.read_text().strip()
                return int(text) if text else None
        except (ValueError, OSError) as e:
            logger.warning(f"Failed to read compiled epoch from {epoch_file}: {e}")
        return None

    @staticmethod
    def read_local_version(
        version_file: str = "/qubic/version.txt",
    ) -> tuple[int, int] | None:
        """Read VERSION_A.VERSION_B from version.txt."""
        try:
            path = Path(version_file)
            if path.exists():
                text = path.read_text().strip()
                return EpochService.parse_version(text)
        except OSError as e:
            logger.warning(f"Failed to read version from {version_file}: {e}")
        return None

    @staticmethod
    def parse_version(version_str: str) -> tuple[int, int] | None:
        """Parse ``"1.276"`` → ``(1, 276)``.  Returns ``None`` on invalid input."""
        try:
            parts = version_str.strip().split(".")
            if len(parts) >= 2:
                return (int(parts[0]), int(parts[1]))
        except (ValueError, IndexError):
            pass
        return None

    @staticmethod
    def format_version(version: tuple[int, int]) -> str:
        """Format ``(1, 276)`` → ``"1.276"``."""
        return f"{version[0]}.{version[1]}"

    @staticmethod
    def is_version_compatible(
        local: tuple[int, int] | None,
        minimum: tuple[int, int] | None,
    ) -> bool:
        """Return ``True`` if *minimum* is ``None`` (no check) or *local* >= *minimum*.

        Returns ``False`` if *local* is ``None`` but *minimum* is set.
        """
        if minimum is None:
            return True
        if local is None:
            return False
        return local >= minimum
