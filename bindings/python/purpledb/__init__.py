"""PurpleDB: Embedded multimodal database for semantic video and image search."""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Any, List, Optional, Union

# On Windows Python 3.8+, register the package directory for dependent DLLs
_pkg_dir = Path(__file__).resolve().parent
if sys.platform == "win32" and hasattr(os, "add_dll_directory"):
    try:
        os.add_dll_directory(str(_pkg_dir))
    except Exception:
        pass

from ._purpledb import (
    DatabaseStats,
    MomentHit,
    PurpleDBEngine,
)

__version__ = "1.0.0"
__all__ = [
    "open",
    "PurpleDB",
    "MomentHit",
    "DatabaseStats",
    "__version__",
]


class PurpleDB:
    """PurpleDB multimodal database client."""

    def __init__(
        self,
        db_path: str,
        create_if_missing: bool = True,
        error_if_exists: bool = False,
        dim: int = 512,
        enable_hnsw: bool = True,
        hnsw_m: int = 16,
        hnsw_ef_search: int = 64,
        fps: float = 1.0,
    ) -> None:
        self._engine = PurpleDBEngine(
            db_path=str(db_path),
            create_if_missing=create_if_missing,
            error_if_exists=error_if_exists,
            dim=dim,
            enable_hnsw=enable_hnsw,
            hnsw_m=hnsw_m,
            hnsw_ef_search=hnsw_ef_search,
            fps=fps,
        )

    def add_video(self, video_path: Union[str, Path]) -> str:
        """Ingests a video file into the database and returns its content-addressed video_id.
        
        The video file is processed by an isolated ffmpeg worker, embedded into
        vector representations, clustered into moments, and durably written.
        """
        return self._engine.add_video(str(video_path))

    def add_synthetic_video(
        self,
        video_id: str,
        count: int = 10,
        fps: float = 1.0,
        width: int = 224,
        height: int = 224,
    ) -> int:
        """Generates and ingests synthetic frames for testing and offline verification."""
        return self._engine.add_synthetic_video(
            video_id=video_id, count=count, fps=fps, width=width, height=height
        )

    def search(
        self,
        query: str,
        top_k: int = 5,
        video_id: Optional[str] = None,
        start_sec: float = -1.0,
        end_sec: float = -1.0,
    ) -> List[MomentHit]:
        """Searches video moments using natural language query with optional temporal/video filtering.
        
        Args:
            query: Natural language search string (e.g. "red sports car drifting").
            top_k: Maximum number of ranked results to return.
            video_id: Optional video ID filter to restrict search to a specific video.
            start_sec: Optional start timestamp bound in seconds (-1 for unbounded).
            end_sec: Optional end timestamp bound in seconds (-1 for unbounded).
            
        Returns:
            List of scored MomentHit instances ranked by cosine similarity.
        """
        return self._engine.search(
            query=query,
            top_k=top_k,
            video_id=video_id or "",
            start_sec=start_sec,
            end_sec=end_sec,
        )

    def search_by_image(
        self,
        rgb_data: Any,
        width: int,
        height: int,
        channels: int = 3,
        top_k: int = 5,
        video_id: Optional[str] = None,
        start_sec: float = -1.0,
        end_sec: float = -1.0,
    ) -> List[MomentHit]:
        """Searches video moments matching visual example image buffer.
        
        Args:
            rgb_data: Contiguous byte buffer (bytes, bytearray, or numpy ndarray).
            width: Image width in pixels.
            height: Image height in pixels.
            channels: Number of color channels (default 3 for RGB).
            top_k: Maximum number of ranked results.
            video_id: Optional video ID filter.
            start_sec: Optional start timestamp bound in seconds.
            end_sec: Optional end timestamp bound in seconds.
        """
        return self._engine.search_by_image(
            rgb_buffer=rgb_data,
            width=width,
            height=height,
            channels=channels,
            top_k=top_k,
            video_id=video_id or "",
            start_sec=start_sec,
            end_sec=end_sec,
        )

    def delete_video(self, video_id: str) -> None:
        """Deletes a video and cascades O(1) tombstones across vectors and LSM metadata."""
        self._engine.delete_video(video_id)

    def stats(self) -> DatabaseStats:
        """Retrieves global database storage and index statistics."""
        return self._engine.stats()

    def sync(self) -> None:
        """Flushes MemTables and syncs WAL and vector segments to durable disk."""
        self._engine.sync()

    def close(self) -> None:
        """Flushes and closes database handles, releasing locks."""
        self._engine.close()

    @property
    def is_closed(self) -> bool:
        """True if the database handle has been closed."""
        return self._engine.is_closed()

    def __enter__(self) -> "PurpleDB":
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None:
        self.close()


def open(
    db_path: Union[str, Path],
    create_if_missing: bool = True,
    error_if_exists: bool = False,
    dim: int = 512,
    enable_hnsw: bool = True,
    hnsw_m: int = 16,
    hnsw_ef_search: int = 64,
    fps: float = 1.0,
) -> PurpleDB:
    """Opens or creates an embedded PurpleDB database instance.

    Usage:
        import purpledb

        with purpledb.open("./mydb") as db:
            db.add_video("drone_footage.mp4")
            results = db.search("drone flying above pine trees", top_k=5)
            for hit in results:
                print(hit.video_id, hit.start_sec, hit.end_sec, hit.score)
    """
    return PurpleDB(
        db_path=str(db_path),
        create_if_missing=create_if_missing,
        error_if_exists=error_if_exists,
        dim=dim,
        enable_hnsw=enable_hnsw,
        hnsw_m=hnsw_m,
        hnsw_ef_search=hnsw_ef_search,
        fps=fps,
    )
