from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from app.config import SourceConfig

from app.uploaders.base import BaseUploader


def create_uploader(config: SourceConfig) -> BaseUploader:
    if config.uploader_type == "scp":
        from app.uploaders.chunked_scp import ChunkedScpUploader

        return ChunkedScpUploader(
            host=config.scp_host,
            user=config.scp_user,
            port=config.scp_port,
            dest_path=config.scp_dest_path,
            key_file=config.scp_key_file,
            timeout=config.upload_timeout_seconds,
            chunk_size_mb=config.scp_chunk_size_mb,
            chunk_timeout=config.scp_chunk_timeout,
            parallel_chunks=config.scp_parallel_chunks,
            min_chunk_size_gb=config.scp_min_chunk_size_gb,
            chunk_retry_count=config.upload_retry_count,
            chunk_retry_delay=config.upload_retry_delay_seconds,
        )
    elif config.uploader_type == "rsync":
        from app.uploaders.rsync import RsyncUploader

        return RsyncUploader(
            host=config.scp_host,
            user=config.scp_user,
            port=config.scp_port,
            dest_path=config.scp_dest_path,
            key_file=config.scp_key_file,
            timeout=config.rsync_timeout,
            bandwidth_limit=config.rsync_bandwidth_limit,
            compress=config.rsync_compress,
        )
    elif config.uploader_type == "http_rest":
        from app.uploaders.http_rest import HttpRestUploader

        return HttpRestUploader(
            upload_url=config.http_upload_url,
            auth_token=config.http_auth_token,
            timeout=config.upload_timeout_seconds,
        )
    elif config.uploader_type == "s3":
        from app.uploaders.s3 import S3Uploader

        return S3Uploader(
            bucket=config.s3_bucket,
            prefix=config.s3_prefix,
            endpoint_url=config.s3_endpoint_url,
            region=config.s3_region,
            access_key=config.s3_access_key,
            secret_key=config.s3_secret_key,
        )
    elif config.uploader_type == "local_fs":
        from app.uploaders.local_fs import LocalFsUploader

        return LocalFsUploader(dest_dir=config.local_fs_dest_dir)
    else:
        raise ValueError(f"Unknown uploader type: {config.uploader_type}")
