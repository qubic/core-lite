"""Pick the Qubic binary variant the CPU can run.

The image ships two builds of the same source (docker/orchestrator/Dockerfile):
QubicAVX512 (ENABLE_AVX512=ON) and QubicAVX2 (ENABLE_AVX512=OFF). The AVX-512
build uses every extension that cmake/CompilerSetup.cmake enables, so it is
only installed when the CPU reports all of them; otherwise it would stop with
an illegal instruction.
"""

from __future__ import annotations

import logging
from pathlib import Path

logger = logging.getLogger(__name__)

AVX512 = "avx512"
AVX2 = "avx2"
VARIANTS = (AVX512, AVX2)
BINARY_NAMES = {AVX512: "QubicAVX512", AVX2: "QubicAVX2"}

# /proc/cpuinfo names of the -m flags given for ENABLE_AVX512=ON in
# cmake/CompilerSetup.cmake (-mavx -mavx2 -mavx512f -mavx512cd -mavx512vl
# -mavx512bw -mavx512dq -mavx512vbmi -mavx512vbmi2 -mavx512vpopcntdq -mgfni).
AVX512_REQUIRED_FLAGS = frozenset({
    "avx", "avx2", "avx512f", "avx512cd", "avx512vl", "avx512bw", "avx512dq",
    "avx512vbmi", "avx512_vbmi2", "avx512_vpopcntdq", "gfni",
})


def read_cpu_flags(cpuinfo: Path = Path("/proc/cpuinfo")) -> frozenset[str]:
    """Flags of the first processor in /proc/cpuinfo, empty if unreadable."""
    try:
        with open(cpuinfo, encoding="utf-8", errors="replace") as f:
            for line in f:
                key, _, value = line.partition(":")
                if key.strip() == "flags":
                    return frozenset(value.split())
    except OSError as e:
        logger.warning(f"Cannot read {cpuinfo}: {e}")
    return frozenset()


def select_variant(flags: frozenset[str], requested: str = "auto") -> tuple[str, str]:
    """Return (variant, reason). ``requested`` is "auto", "avx512" or "avx2"."""
    requested = (requested or "auto").strip().lower()
    if requested in VARIANTS:
        return requested, f"forced by QUBIC_BINARY_VARIANT={requested}"
    if requested != "auto":
        logger.warning(f"Unknown QUBIC_BINARY_VARIANT {requested!r}, using auto")
    missing = sorted(AVX512_REQUIRED_FLAGS - flags)
    if not missing:
        return AVX512, "CPU has every AVX-512 extension the build uses"
    return AVX2, f"CPU lacks {', '.join(missing)}"
