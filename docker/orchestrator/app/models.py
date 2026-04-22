from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Optional


class OrchestratorMode(str, Enum):
    SOURCE = "source"
    NORMAL = "normal"


class NodeHealth(str, Enum):
    UNKNOWN = "unknown"
    STARTING = "starting"
    HEALTHY = "healthy"
    STUCK = "stuck"
    MISALIGNED = "misaligned"
    CRASHED = "crashed"
    SAVING_SNAPSHOT = "saving_snapshot"
    VERSION_INCOMPATIBLE = "version_incompatible"
    EPOCH_BEHIND = "epoch_behind"
    STATE_INCOMPATIBLE = "state_incompatible"


@dataclass
class TickInfo:
    epoch: int
    tick: int
    initial_tick: int
    aligned_votes: int
    misaligned_votes: int
    main_aux_status: int
    is_saving_snapshot: bool

    @classmethod
    def from_json(cls, data: dict) -> TickInfo:
        return cls(
            epoch=data["epoch"],
            tick=data["tick"],
            initial_tick=data["initialTick"],
            aligned_votes=data["alignedVotes"],
            misaligned_votes=data["misalignedVotes"],
            main_aux_status=data["mainAuxStatus"],
            is_saving_snapshot=data.get("isSavingSnapshot", False),
        )


@dataclass
class EpochInfo:
    epoch: int
    initial_tick: int
    peers: list[str] = field(default_factory=list)
    min_version: Optional[tuple[int, int]] = None


@dataclass
class SnapshotChunkInfo:
    """One chunk of a chunked snapshot on the remote server."""
    filename: str
    size: int
    checksum: str  # sha256 hex
    url: str       # absolute URL for this chunk


@dataclass
class SnapshotMeta:
    epoch: int
    tick: int
    timestamp: str
    url: str                 # URL of the chunk directory (no trailing slash)
    checksum: str = ""
    size_bytes: int = 0
    chunks: list[SnapshotChunkInfo] = field(default_factory=list)


@dataclass
class NodeState:
    health: NodeHealth = NodeHealth.UNKNOWN
    last_tick_info: Optional[TickInfo] = None
    last_tick_change_time: float = 0.0
    consecutive_stuck_polls: int = 0
    process_pid: Optional[int] = None
    restart_count: int = 0
    last_restart_time: float = 0.0


@dataclass
class UploadResult:
    success: bool
    remote_url: Optional[str] = None
    error_message: Optional[str] = None
    bytes_uploaded: int = 0
    duration_seconds: float = 0.0
    # For chunked uploads: the remote folder holding the chunks and a list of
    # chunk descriptors (filename/size/checksum). Empty for non-chunked uploads.
    remote_dir: Optional[str] = None
    chunks: list[dict] = field(default_factory=list)
