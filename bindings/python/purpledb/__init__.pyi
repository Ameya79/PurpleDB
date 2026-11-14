"""Type stubs for PurpleDB."""

from typing import Any, Dict, List, Optional, Union
from pathlib import Path

__version__: str

class MomentHit:
    video_id: str
    moment_id: int
    start_sec: float
    end_sec: float
    score: float
    thumbnail_path: str
    caption: str
    @property
    def duration(self) -> float: ...
    def to_dict(self) -> Dict[str, Any]: ...
    def __repr__(self) -> str: ...

class DatabaseStats:
    num_videos: int
    num_moments: int
    num_vectors: int
    disk_size_bytes: int
    def to_dict(self) -> Dict[str, Any]: ...
    def __repr__(self) -> str: ...

class PurpleDBEngine:
    def __init__(
        self,
        db_path: str,
        create_if_missing: bool = ...,
        error_if_exists: bool = ...,
        dim: int = ...,
        enable_hnsw: bool = ...,
        hnsw_m: int = ...,
        hnsw_ef_search: int = ...,
        fps: float = ...,
    ) -> None: ...
    def add_video(self, video_path: str) -> str: ...
    def add_synthetic_video(
        self,
        video_id: str,
        count: int = ...,
        fps: float = ...,
        width: int = ...,
        height: int = ...,
    ) -> int: ...
    def search(
        self,
        query: str,
        top_k: int = ...,
        video_id: str = ...,
        start_sec: float = ...,
        end_sec: float = ...,
    ) -> List[MomentHit]: ...
    def search_by_image(
        self,
        rgb_buffer: Any,
        width: int,
        height: int,
        channels: int = ...,
        top_k: int = ...,
        video_id: str = ...,
        start_sec: float = ...,
        end_sec: float = ...,
    ) -> List[MomentHit]: ...
    def delete_video(self, video_id: str) -> None: ...
    def stats(self) -> DatabaseStats: ...
    def sync(self) -> None: ...
    def close(self) -> None: ...
    def is_closed(self) -> bool: ...
    def __enter__(self) -> PurpleDBEngine: ...
    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None: ...

class PurpleDB:
    def __init__(
        self,
        db_path: Union[str, Path],
        create_if_missing: bool = ...,
        error_if_exists: bool = ...,
        dim: int = ...,
        enable_hnsw: bool = ...,
        hnsw_m: int = ...,
        hnsw_ef_search: int = ...,
        fps: float = ...,
    ) -> None: ...
    def add_video(self, video_path: Union[str, Path]) -> str: ...
    def add_synthetic_video(
        self,
        video_id: str,
        count: int = ...,
        fps: float = ...,
        width: int = ...,
        height: int = ...,
    ) -> int: ...
    def search(
        self,
        query: str,
        top_k: int = ...,
        video_id: Optional[str] = ...,
        start_sec: float = ...,
        end_sec: float = ...,
    ) -> List[MomentHit]: ...
    def search_by_image(
        self,
        rgb_data: Any,
        width: int,
        height: int,
        channels: int = ...,
        top_k: int = ...,
        video_id: Optional[str] = ...,
        start_sec: float = ...,
        end_sec: float = ...,
    ) -> List[MomentHit]: ...
    def delete_video(self, video_id: str) -> None: ...
    def stats(self) -> DatabaseStats: ...
    def sync(self) -> None: ...
    def close(self) -> None: ...
    @property
    def is_closed(self) -> bool: ...
    def __enter__(self) -> PurpleDB: ...
    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None: ...

def open(
    db_path: Union[str, Path],
    create_if_missing: bool = ...,
    error_if_exists: bool = ...,
    dim: int = ...,
    enable_hnsw: bool = ...,
    hnsw_m: int = ...,
    hnsw_ef_search: int = ...,
    fps: float = ...,
) -> PurpleDB: ...

