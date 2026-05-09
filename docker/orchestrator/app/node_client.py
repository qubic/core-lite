from __future__ import annotations

import logging
import time
from pathlib import Path

import aiohttp

from app.models import TickInfo

logger = logging.getLogger(__name__)

DEFAULT_NODE_URL = "http://127.0.0.1:41841"
REQUEST_TIMEOUT = aiohttp.ClientTimeout(total=10)
SLOW_CALL_THRESHOLD_MS = 1000


class NodeClient:
    """Async HTTP client for the local Qubic node API."""

    def __init__(self, base_url: str = DEFAULT_NODE_URL, passcode: str = "") -> None:
        self._base_url = base_url.rstrip("/")
        self._passcode = passcode
        self._session: aiohttp.ClientSession | None = None

    async def _get_session(self) -> aiohttp.ClientSession:
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession(timeout=REQUEST_TIMEOUT)
        return self._session

    async def close(self) -> None:
        if self._session and not self._session.closed:
            await self._session.close()

    def _passcode_params(self) -> dict[str, str]:
        if self._passcode:
            return {"passcode": self._passcode}
        return {}

    async def get_tick_info(self) -> TickInfo:
        session = await self._get_session()
        start = time.monotonic()
        try:
            async with session.get(f"{self._base_url}/tick-info") as resp:
                resp.raise_for_status()
                data = await resp.json()
                elapsed_ms = (time.monotonic() - start) * 1000
                if elapsed_ms >= SLOW_CALL_THRESHOLD_MS:
                    logger.info(
                        f"/tick-info slow: {elapsed_ms:.0f} ms "
                        f"(tick={data.get('tick')}, status={resp.status})"
                    )
                return TickInfo.from_json(data)
        except Exception as e:
            elapsed_ms = (time.monotonic() - start) * 1000
            logger.warning(
                f"/tick-info failed after {elapsed_ms:.0f} ms: "
                f"{type(e).__name__}: {e}"
            )
            raise

    async def get_latest_stats(self) -> dict:
        session = await self._get_session()
        start = time.monotonic()
        try:
            async with session.get(f"{self._base_url}/v1/latest-stats") as resp:
                resp.raise_for_status()
                data = await resp.json()
                elapsed_ms = (time.monotonic() - start) * 1000
                if elapsed_ms >= SLOW_CALL_THRESHOLD_MS:
                    logger.info(
                        f"/v1/latest-stats slow: {elapsed_ms:.0f} ms "
                        f"(status={resp.status})"
                    )
                return data
        except Exception as e:
            elapsed_ms = (time.monotonic() - start) * 1000
            logger.warning(
                f"/v1/latest-stats failed after {elapsed_ms:.0f} ms: "
                f"{type(e).__name__}: {e}"
            )
            raise

    async def request_save_snapshot(self) -> bool:
        session = await self._get_session()
        async with session.get(
            f"{self._base_url}/request-save-snapshot"
        ) as resp:
            resp.raise_for_status()
            data = await resp.json()
            return data.get("status") == "ok"

    async def shutdown(self) -> bool:
        session = await self._get_session()
        params = self._passcode_params()
        try:
            async with session.get(
                f"{self._base_url}/shutdown",
                params=params,
                timeout=aiohttp.ClientTimeout(total=5),
            ) as resp:
                if resp.status == 200:
                    logger.info("Shutdown request accepted by node")
                    return True
                else:
                    logger.warning(
                        f"Shutdown request failed: HTTP {resp.status}"
                    )
                    return False
        except (aiohttp.ClientError, TimeoutError):
            # Node may close connection immediately on shutdown
            logger.info("Shutdown request sent (connection closed by node)")
            return True

    async def download_spectrum(self, dest_path: Path, zip: bool = True) -> Path:
        return await self._download_file("spectrum", dest_path, zip)

    async def download_universe(self, dest_path: Path, zip: bool = True) -> Path:
        return await self._download_file("universe", dest_path, zip)

    async def _download_file(
        self, name: str, dest_path: Path, zip: bool
    ) -> Path:
        session = await self._get_session()
        params = {**self._passcode_params()}
        if zip:
            params["zip"] = "true"
        timeout = aiohttp.ClientTimeout(total=600)
        async with session.get(
            f"{self._base_url}/{name}",
            params=params,
            timeout=timeout,
        ) as resp:
            resp.raise_for_status()
            dest_path.parent.mkdir(parents=True, exist_ok=True)
            with open(dest_path, "wb") as f:
                async for chunk in resp.content.iter_chunked(1024 * 1024):
                    f.write(chunk)
        logger.info(f"Downloaded {name} to {dest_path}")
        return dest_path

    async def is_alive(self) -> bool:
        try:
            await self.get_tick_info()
            return True
        except Exception:
            return False
