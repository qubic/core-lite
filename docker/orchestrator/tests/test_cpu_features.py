"""Binary variant selection (app/cpu_features.py) and its install (Orchestrator._install_binary)."""

from __future__ import annotations

import os

import pytest

import app.main as main_module
from app.config import OrchestratorConfig
from app.cpu_features import AVX512_REQUIRED_FLAGS, read_cpu_flags, select_variant
from app.main import Orchestrator

ZEN4 = AVX512_REQUIRED_FLAGS | {"sse4_2", "bmi2", "sha_ni"}
ZEN2 = frozenset({"sse4_2", "avx", "avx2", "bmi2", "sha_ni"})


def test_reads_flags_of_the_first_processor(tmp_path):
    cpuinfo = tmp_path / "cpuinfo"
    cpuinfo.write_text(
        "processor\t: 0\nmodel name\t: AMD EPYC\nflags\t\t: fpu avx avx2 gfni\n\n"
        "processor\t: 1\nflags\t\t: fpu\n"
    )
    assert read_cpu_flags(cpuinfo) == {"fpu", "avx", "avx2", "gfni"}


def test_unreadable_cpuinfo_gives_no_flags(tmp_path):
    assert read_cpu_flags(tmp_path / "absent") == frozenset()


def test_full_avx512_cpu_gets_avx512():
    assert select_variant(ZEN4)[0] == "avx512"


def test_avx2_only_cpu_gets_avx2():
    variant, reason = select_variant(ZEN2)
    assert variant == "avx2"
    assert "avx512f" in reason


@pytest.mark.parametrize("missing", sorted(AVX512_REQUIRED_FLAGS))
def test_one_missing_extension_falls_back_to_avx2(missing):
    # e.g. Intel Skylake-X has avx512f but no vbmi / vbmi2 / gfni
    assert select_variant(ZEN4 - {missing})[0] == "avx2"


def test_no_flags_falls_back_to_avx2():
    assert select_variant(frozenset())[0] == "avx2"


@pytest.mark.parametrize("requested", ["avx2", "AVX512"])
def test_forced_variant(requested):
    assert select_variant(ZEN2 if requested == "AVX512" else ZEN4, requested)[0] == requested.lower()


def test_unknown_request_means_auto():
    assert select_variant(ZEN2, "avx10")[0] == "avx2"


# ------------------------------------------------------------------ install


@pytest.fixture
def staged(tmp_path):
    staging = tmp_path / "staging"
    staging.mkdir()
    (staging / "QubicAVX512").write_bytes(b"avx512-binary")
    (staging / "QubicAVX2").write_bytes(b"avx2-binary")
    (staging / "version.txt").write_text("1.313")
    (staging / "epoch.txt").write_text("241")
    return staging


def _install(tmp_data_dir, staging, monkeypatch, flags, variant="auto"):
    monkeypatch.setattr(main_module, "read_cpu_flags", lambda: flags)
    config = OrchestratorConfig(
        data_dir=str(tmp_data_dir), binary_staging_dir=str(staging), binary_variant=variant
    )
    Orchestrator(config)._install_binary()


def test_installs_the_variant_of_the_cpu(tmp_data_dir, staged, monkeypatch):
    _install(tmp_data_dir, staged, monkeypatch, ZEN2)
    assert (tmp_data_dir / "Qubic").read_bytes() == b"avx2-binary"
    assert (tmp_data_dir / "binary-variant.txt").read_text() == "avx2"
    assert (tmp_data_dir / "version.txt").read_text() == "1.313"
    assert (tmp_data_dir / "epoch.txt").read_text() == "241"


def test_same_version_other_cpu_reinstalls(tmp_data_dir, staged, monkeypatch):
    # The volume moved to a host with AVX-512 (or the reverse): same version, other variant.
    _install(tmp_data_dir, staged, monkeypatch, ZEN2)
    _install(tmp_data_dir, staged, monkeypatch, ZEN4)
    assert (tmp_data_dir / "Qubic").read_bytes() == b"avx512-binary"
    assert (tmp_data_dir / "binary-variant.txt").read_text() == "avx512"


def test_up_to_date_volume_is_left_alone(tmp_data_dir, staged, monkeypatch):
    _install(tmp_data_dir, staged, monkeypatch, ZEN4)
    (tmp_data_dir / "Qubic").write_bytes(b"patched-by-hand")
    staged_mtime = (staged / "QubicAVX512").stat().st_mtime
    os.utime(tmp_data_dir / "Qubic", (staged_mtime, staged_mtime))
    _install(tmp_data_dir, staged, monkeypatch, ZEN4)
    assert (tmp_data_dir / "Qubic").read_bytes() == b"patched-by-hand"


def test_volume_from_a_single_binary_image_is_upgraded(tmp_data_dir, staged, monkeypatch):
    # Before this change the volume held one AVX2 build and no binary-variant.txt.
    (tmp_data_dir / "Qubic").write_bytes(b"old-single-binary")
    (tmp_data_dir / "version.txt").write_text("1.313")
    _install(tmp_data_dir, staged, monkeypatch, ZEN4)
    assert (tmp_data_dir / "Qubic").read_bytes() == b"avx512-binary"


def test_image_with_a_single_binary_still_installs(tmp_data_dir, tmp_path, monkeypatch):
    staging = tmp_path / "old-staging"
    staging.mkdir()
    (staging / "Qubic").write_bytes(b"single")
    (staging / "version.txt").write_text("1.313")
    _install(tmp_data_dir, staging, monkeypatch, ZEN4)
    assert (tmp_data_dir / "Qubic").read_bytes() == b"single"
    assert (tmp_data_dir / "binary-variant.txt").read_text() == "single"
