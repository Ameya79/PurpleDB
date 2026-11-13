"""PurpleDB FastAPI Integration Example.

Demonstrates:
- Embedded database lifecycle with FastAPI lifespan
- Non-blocking search endpoints (pybind11 releases GIL)
- Background worker task for video ingestion
- Visual and text search endpoints
"""

from contextlib import asynccontextmanager
from typing import Any, Dict, List, Optional
from fastapi import FastAPI, HTTPException, BackgroundTasks, UploadFile, File, Query
from pydantic import BaseModel
import purpledb

# Global database instance
db: Optional[purpledb.PurpleDB] = None


@asynccontextmanager
async def lifespan(app: FastAPI):
    global db
    # Initialize database on startup
    db = purpledb.open("./purpledb_fastapi_data", dim=512, enable_hnsw=True)
    yield
    # Safely flush and close database on shutdown
    if db:
        db.close()


app = FastAPI(
    title="PurpleDB Semantic Video Search API",
    version="1.0.0",
    description="High-performance embedded video search microservice powered by PurpleDB.",
    lifespan=lifespan,
)


class SearchHitResponse(BaseModel):
    video_id: str
    moment_id: int
    start_sec: float
    end_sec: float
    duration: float
    score: float
    thumbnail_path: str
    caption: str


class DatabaseStatsResponse(BaseModel):
    num_videos: int
    num_moments: int
    num_vectors: int
    disk_size_bytes: int


def ingest_worker(video_path: str) -> None:
    """Background worker for video ingestion without blocking API responses."""
    assert db is not None
    try:
        video_id = db.add_video(video_path)
        print(f"Successfully ingested video: {video_id}")
    except Exception as exc:
        print(f"Failed to ingest video {video_path}: {exc}")


@app.post("/ingest", status_code=202)
async def ingest_video(video_path: str, background_tasks: BackgroundTasks) -> Dict[str, str]:
    """Schedules an asynchronous video ingestion job."""
    background_tasks.add_task(ingest_worker, video_path)
    return {"status": "accepted", "message": f"Ingestion queued for {video_path}"}


@app.get("/search", response_model=List[SearchHitResponse])
async def search_moments(
    q: str = Query(..., description="Natural language search query"),
    top_k: int = Query(5, ge=1, le=100),
    video_id: Optional[str] = Query(None, description="Optional video ID filter"),
    start_sec: float = Query(-1.0, description="Start timestamp bound in seconds"),
    end_sec: float = Query(-1.0, description="End timestamp bound in seconds"),
) -> List[Dict[str, Any]]:
    """Searches video moments matching natural language query."""
    if not db:
        raise HTTPException(status_code=503, detail="Database not initialized")

    hits = db.search(
        query=q,
        top_k=top_k,
        video_id=video_id,
        start_sec=start_sec,
        end_sec=end_sec,
    )
    return [h.to_dict() for h in hits]


@app.post("/search/image", response_model=List[SearchHitResponse])
async def search_by_image(
    file: UploadFile = File(...),
    width: int = Query(224),
    height: int = Query(224),
    top_k: int = Query(5, ge=1, le=100),
) -> List[Dict[str, Any]]:
    """Searches video moments matching an uploaded visual query frame."""
    if not db:
        raise HTTPException(status_code=503, detail="Database not initialized")

    image_bytes = await file.read()
    hits = db.search_by_image(
        rgb_data=image_bytes,
        width=width,
        height=height,
        top_k=top_k,
    )
    return [h.to_dict() for h in hits]


@app.get("/stats", response_model=DatabaseStatsResponse)
async def get_stats() -> Dict[str, Any]:
    """Returns database size, video count, and indexing statistics."""
    if not db:
        raise HTTPException(status_code=503, detail="Database not initialized")
    return db.stats().to_dict()


@app.delete("/videos/{video_id}")
async def delete_video(video_id: str) -> Dict[str, str]:
    """Deletes a video with cascading tombstone propagation."""
    if not db:
        raise HTTPException(status_code=503, detail="Database not initialized")
    db.delete_video(video_id)
    return {"status": "ok", "message": f"Video {video_id} deleted"}


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="127.0.0.1", port=8000)
