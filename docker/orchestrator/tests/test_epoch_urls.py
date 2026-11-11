import asyncio
import zipfile

import pytest

from app.config import OrchestratorConfig
from app.main import Orchestrator, epoch_file_urls
from app.models import EpochInfo
from app.state_manager import StateManager


class TestEpochFileUrls:
    def test_default_fallback(self):
        config = OrchestratorConfig()
        assert epoch_file_urls(
            config.snapshot_service_url, config.epoch_files_fallback_url, 239
        ) == [
            "https://storage.qubic.li/network/239/ep239-full.zip",
            "https://dl.qubic.global/ep239.zip",
        ]

    def test_fallback_disabled(self):
        assert epoch_file_urls("https://storage.qubic.li/", "", 239) == [
            "https://storage.qubic.li/network/239/ep239-full.zip",
        ]

    def test_same_url_not_tried_twice(self):
        url = "https://storage.qubic.li/network/{epoch}/ep{epoch}-full.zip"
        assert epoch_file_urls("https://storage.qubic.li", url, 239) == [
            "https://storage.qubic.li/network/239/ep239-full.zip",
        ]

    def test_env_override(self, monkeypatch):
        monkeypatch.setenv("QUBIC_EPOCH_FILES_FALLBACK_URL", "https://mirror.example/e{epoch}.zip")
        assert OrchestratorConfig().epoch_files_fallback_url == "https://mirror.example/e{epoch}.zip"


class _FakeStateManager:
    def __init__(self, working_url):
        self.working_url = working_url
        self.tried = []
        self.valid = False

    def get_local_epoch(self):
        return None

    def has_valid_state_files(self, epoch):
        return self.valid

    async def download_epoch_files(self, epoch, url):
        self.tried.append(url)
        self.valid = url == self.working_url
        return self.valid


class _NoSnapshot:
    async def get_latest_snapshot_meta(self, epoch):
        return None


def _orchestrator(state_manager):
    orch = Orchestrator.__new__(Orchestrator)
    orch._config = OrchestratorConfig()
    orch._state_manager = state_manager
    orch._epoch_service = _NoSnapshot()
    orch._shutdown_event = asyncio.Event()
    return orch


@pytest.mark.asyncio
async def test_broken_primary_falls_back():
    state = _FakeStateManager("https://dl.qubic.global/ep239.zip")
    await _orchestrator(state)._ensure_state_files(EpochInfo(epoch=239, initial_tick=0))
    assert state.tried == [
        "https://storage.qubic.li/network/239/ep239-full.zip",
        "https://dl.qubic.global/ep239.zip",
    ]


@pytest.mark.asyncio
async def test_working_primary_skips_fallback():
    state = _FakeStateManager("https://storage.qubic.li/network/239/ep239-full.zip")
    await _orchestrator(state)._ensure_state_files(EpochInfo(epoch=239, initial_tick=0))
    assert state.tried == ["https://storage.qubic.li/network/239/ep239-full.zip"]


class _ZeroFilledPrimaryDownloader:
    """The primary serves a zero-filled file, as storage.qubic.li did for epoch 239."""

    def __init__(self):
        self.urls = []

    async def download(self, url, dest_path):
        self.urls.append(url)
        if "storage.qubic.li" in url:
            dest_path.write_bytes(b"\0" * 261120)
        else:
            with zipfile.ZipFile(dest_path, "w") as zf:
                zf.writestr("spectrum.000", "spectrum data")
                zf.writestr("universe.000", "universe data")
        return dest_path

    async def close(self):
        pass


@pytest.mark.asyncio
async def test_invalid_primary_archive_falls_back(tmp_data_dir):
    downloader = _ZeroFilledPrimaryDownloader()
    state = StateManager(tmp_data_dir, downloader)
    await _orchestrator(state)._ensure_state_files(EpochInfo(epoch=239, initial_tick=0))
    assert downloader.urls == [
        "https://storage.qubic.li/network/239/ep239-full.zip",
        "https://dl.qubic.global/ep239.zip",
    ]
    assert state.has_valid_state_files(239)
    assert not (tmp_data_dir / "epoch_files.zip").exists()
