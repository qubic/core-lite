from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any, ClassVar, Optional, Tuple

import yaml
from pydantic import BaseModel, Field
from pydantic.fields import FieldInfo
from pydantic_settings import (
    BaseSettings,
    PydanticBaseSettingsSource,
    SettingsConfigDict,
)

from app.models import OrchestratorMode


class YamlConfigSettingsSource(PydanticBaseSettingsSource):
    """Settings source that reads from a pre-loaded YAML dict.

    Used as a low-priority source so that ENV vars and CLI args
    override YAML values.
    """

    def __init__(
        self, settings_cls: type[BaseSettings], yaml_data: dict
    ) -> None:
        super().__init__(settings_cls)
        self._yaml_data = yaml_data

    def get_field_value(
        self, field: FieldInfo, field_name: str
    ) -> Tuple[Any, str, bool]:
        value = self._yaml_data.get(field_name)
        return value, field_name, self.field_is_complex(field)

    def __call__(self) -> dict[str, Any]:
        d: dict[str, Any] = {}
        for field_name, field_info in self.settings_cls.model_fields.items():
            value, key, is_complex = self.get_field_value(
                field_info, field_name
            )
            if value is not None:
                d[key] = value
        return d


class WatchdogConfig(BaseModel):
    enabled: bool = True
    poll_interval_seconds: int = 15
    startup_grace_seconds: int = 300
    stuck_threshold_seconds: int = 300
    stuck_consecutive_polls: int = 3
    misaligned_threshold_votes: int = 451
    misaligned_threshold_seconds: int = 150  # 2.5 minutes of misalignment on the same tick
    max_restarts: int = 5
    restart_cooldown_seconds: int = 600
    crash_restart_delay_seconds: int = 10
    epoch_api_poll_seconds: int = 300
    epoch_behind_restart_polls: int = 2
    rapid_fail_threshold_seconds: int = 120
    rapid_fail_count_for_incompatible: int = 2


class SourceConfig(BaseModel):
    snapshot_interval_seconds: int = 3600
    snapshot_wait_timeout_seconds: int = 600
    snapshot_poll_interval_seconds: int = 5
    upload_timeout_seconds: int = 1800
    upload_retry_count: int = 3
    upload_retry_delay_seconds: int = 30
    package_compression: str = "tar.zst"  # "zip" or "tar.zst" (faster, multi-threaded)
    snapshot_keep_count: int = 1     # keep N most recent snapshots, 0 = no cleanup
    uploader_type: str = "scp"

    # SCP/SSH uploader (default)
    scp_host: str = ""
    scp_user: str = ""
    scp_port: int = 22
    scp_dest_path: str = "/snapshots"
    scp_key_file: str = ""

    # Chunked upload settings (for large files)
    scp_chunk_size_mb: int = 512         # Split into 512MB chunks
    scp_chunk_timeout: int = 600        # 10 min timeout per chunk
    scp_parallel_chunks: int = 2        # Upload 2 chunks in parallel
    scp_min_chunk_size_gb: int = 2      # Only chunk files > 2GB

    # Rsync uploader (set uploader_type: "rsync")
    # Reuses scp_host, scp_user, scp_port, scp_dest_path, scp_key_file
    rsync_bandwidth_limit: int = 0      # KB/s, 0 = unlimited
    rsync_compress: bool = False        # rsync -z flag (off: binary data is incompressible)
    rsync_timeout: int = 1800           # rsync + remote zip timeout (seconds)

    # HTTP REST uploader
    http_upload_url: str = ""
    http_auth_token: str = ""

    # S3 uploader
    s3_bucket: str = ""
    s3_prefix: str = "snapshots/"
    s3_endpoint_url: str = ""
    s3_region: str = "us-east-1"
    s3_access_key: str = ""
    s3_secret_key: str = ""

    # Local FS uploader (testing)
    local_fs_dest_dir: str = "/tmp/snapshots"


class DownloaderConfig(BaseModel):
    type: str = "http"
    timeout_seconds: int = 3600
    retry_count: int = 5
    retry_delay_seconds: int = 60

    # S3 downloader
    s3_bucket: str = ""
    s3_prefix: str = ""
    s3_endpoint_url: str = ""
    s3_region: str = "us-east-1"
    s3_access_key: str = ""
    s3_secret_key: str = ""


class CleanupConfig(BaseModel):
    enabled: bool = True
    interval_seconds: int = 3600     # run cleanup every hour
    keep_epochs: int = 0             # keep N old epochs alongside the current one


class LocalSnapshotConfig(BaseModel):
    enabled: bool = True
    interval_seconds: int = 3600     # save local snapshot every hour (like pressing F8)


class AlertingConfig(BaseModel):
    enabled: bool = False
    webhook_url: str = ""
    rate_limit_seconds: int = 300
    include_node_info: bool = True


class OrchestratorConfig(BaseSettings):
    model_config = SettingsConfigDict(
        env_prefix="QUBIC_",
        env_nested_delimiter="__",
        case_sensitive=False,
    )

    # Class-level store for YAML data (set before construction).
    _yaml_config: ClassVar[dict] = {}

    @classmethod
    def settings_customise_sources(
        cls,
        settings_cls: type[BaseSettings],
        init_settings: PydanticBaseSettingsSource,
        env_settings: PydanticBaseSettingsSource,
        dotenv_settings: PydanticBaseSettingsSource,
        file_secret_settings: PydanticBaseSettingsSource,
    ) -> Tuple[PydanticBaseSettingsSource, ...]:
        """Priority: init kwargs (CLI) > env vars > YAML file > defaults."""
        yaml_source = YamlConfigSettingsSource(settings_cls, cls._yaml_config)
        return (init_settings, env_settings, yaml_source)

    # General
    mode: OrchestratorMode = OrchestratorMode.NORMAL
    data_dir: str = "/qubic"
    binary_path: str = "/qubic/Qubic"
    binary_staging_dir: str = "/opt/qubic-bin"
    log_level: str = "INFO"
    log_format: str = "json"

    # Network / Epoch
    epoch_api_url: str = "https://api.qubic.li/public/EpochInfo"
    snapshot_service_url: str = "https://storage.qubic.li"
    compiled_epoch: Optional[int] = None
    force_download: bool = False

    # Qubic binary CLI arguments
    peers: str = ""
    security_tick: int = 32
    threads: Optional[int] = None
    solution_threads: Optional[int] = None
    ticking_delay: Optional[int] = None
    operator_seed: Optional[str] = None
    operator: Optional[str] = None
    http_passcode: Optional[str] = None
    reader_passcode: Optional[str] = None
    operator_alias: Optional[str] = None
    node_mode: Optional[int] = None
    seeds: str = ""

    # Management API
    management_api_port: int = 8080
    management_api_host: str = "127.0.0.1"

    # Sub-configs
    watchdog: WatchdogConfig = Field(default_factory=WatchdogConfig)
    source: SourceConfig = Field(default_factory=SourceConfig)
    downloader: DownloaderConfig = Field(default_factory=DownloaderConfig)
    alerting: AlertingConfig = Field(default_factory=AlertingConfig)
    cleanup: CleanupConfig = Field(default_factory=CleanupConfig)
    local_snapshot: LocalSnapshotConfig = Field(default_factory=LocalSnapshotConfig)

    def get_peers_list(self) -> list[str]:
        if not self.peers:
            return []
        return [p.strip() for p in self.peers.split(",") if p.strip()]

    def get_seeds_list(self) -> list[str]:
        if not self.seeds:
            return []
        return [s.strip() for s in self.seeds.split(",") if s.strip()]

    def build_qubic_args(self) -> list[str]:
        args: list[str] = []
        peers = self.get_peers_list()
        if peers:
            args.extend(["--peers", ",".join(peers)])
        if self.security_tick:
            args.extend(["--security-tick", str(self.security_tick)])
        if self.threads is not None:
            args.extend(["--threads", str(self.threads)])
        if self.solution_threads is not None:
            args.extend(["--solution-threads", str(self.solution_threads)])
        if self.ticking_delay is not None:
            args.extend(["--ticking-delay", str(self.ticking_delay)])
        if self.operator_seed:
            args.extend(["--operator-seed", self.operator_seed])
        if self.operator:
            args.extend(["--operator", self.operator])
        if self.http_passcode:
            args.extend(["--http-passcode", self.http_passcode])
        if self.reader_passcode:
            args.extend(["--reader-passcode", self.reader_passcode])
        if self.operator_alias:
            args.extend(["--operator-alias", self.operator_alias])
        if self.node_mode is not None:
            args.extend(["--node-mode", str(self.node_mode)])
        seeds = self.get_seeds_list()
        if seeds:
            args.extend(["--seeds", ",".join(seeds)])
        return args


def _parse_cli_args() -> dict:
    parser = argparse.ArgumentParser(description="Qubic Docker Orchestrator")
    parser.add_argument("--config", type=str, help="Path to YAML config file")
    parser.add_argument("--mode", type=str, choices=["source", "normal"])
    parser.add_argument("--data-dir", type=str)
    parser.add_argument("--log-level", type=str)
    parser.add_argument("--peers", type=str)
    parser.add_argument("--security-tick", type=int)
    parser.add_argument("--operator-seed", type=str)
    parser.add_argument("--http-passcode", type=str)
    args, _ = parser.parse_known_args()
    result = {}
    if args.mode:
        result["mode"] = args.mode
    if args.data_dir:
        result["data_dir"] = args.data_dir
    if args.log_level:
        result["log_level"] = args.log_level
    if args.peers:
        result["peers"] = args.peers
    if args.security_tick is not None:
        result["security_tick"] = args.security_tick
    if args.operator_seed:
        result["operator_seed"] = args.operator_seed
    if args.http_passcode:
        result["http_passcode"] = args.http_passcode
    return result, getattr(args, "config", None)


def _load_yaml_config(path: str) -> dict:
    config_path = Path(path)
    if config_path.exists():
        with open(config_path) as f:
            return yaml.safe_load(f) or {}
    return {}


def load_config() -> OrchestratorConfig:
    """Load configuration with precedence: ENV > CLI > YAML > defaults.

    CLI overrides are passed as init kwargs (highest priority).
    ENV vars are read by pydantic-settings automatically.
    YAML values are loaded via a custom settings source (lowest priority).
    """
    cli_overrides, config_file = _parse_cli_args()

    yaml_config: dict = {}
    if config_file:
        yaml_config = _load_yaml_config(config_file)
    else:
        # Try default locations
        for candidate in [
            "/opt/orchestrator/config/config.yaml",
            "/opt/orchestrator/config/default.yaml",
            "./config/config.yaml",
            "./config/default.yaml",
        ]:
            if Path(candidate).exists():
                yaml_config = _load_yaml_config(candidate)
                break

    # Store YAML data for the custom settings source
    OrchestratorConfig._yaml_config = yaml_config

    # Only pass CLI overrides as init kwargs (highest priority).
    # ENV vars and YAML are handled by their respective sources.
    return OrchestratorConfig(**cli_overrides)
