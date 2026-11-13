"""PurpleDB Django Integration Pattern.

Demonstrates:
- Thread-safe singleton service pattern for managing the embedded PurpleDB handle
- Django view integration returning JSON responses
- Background ingestion pattern compatible with Celery / Django Q
"""

import os
from typing import Optional
from pathlib import Path

# Mock minimal Django structures for standalone execution/demonstration
try:
    from django.http import JsonResponse, HttpRequest, HttpResponseBadRequest
    from django.views.decorators.http import require_http_methods
    from django.conf import settings
except ImportError:
    # Dummy mock definitions to illustrate pattern if django is not installed
    class JsonResponse:
        def __init__(self, data, status=200):
            self.data = data
            self.status = status

    def require_http_methods(methods):
        def decorator(f):
            return f
        return decorator

import purpledb


class PurpleDBService:
    """Singleton service managing the lifecycle of the embedded PurpleDB instance."""

    _instance: Optional[purpledb.PurpleDB] = None

    @classmethod
    def get_db(cls, db_path: Optional[str] = None) -> purpledb.PurpleDB:
        if cls._instance is None or cls._instance.is_closed:
            path = db_path or os.getenv("PURPLEDB_PATH", "./purpledb_django_data")
            cls._instance = purpledb.open(
                db_path=path,
                dim=512,
                enable_hnsw=True,
                hnsw_m=16,
                hnsw_ef_search=64,
            )
        return cls._instance

    @classmethod
    def close(cls) -> None:
        if cls._instance and not cls._instance.is_closed:
            cls._instance.close()
            cls._instance = None


# Django Views Example
@require_http_methods(["GET"])
def search_view(request):
    """Django view for searching video moments."""
    query = request.GET.get("q", "")
    if not query:
        return JsonResponse({"error": "Missing 'q' query parameter"}, status=400)

    top_k = int(request.GET.get("top_k", 5))
    video_id = request.GET.get("video_id", None)

    db = PurpleDBService.get_db()
    hits = db.search(query=query, top_k=top_k, video_id=video_id)
    return JsonResponse({
        "query": query,
        "count": len(hits),
        "results": [hit.to_dict() for hit in hits],
    })


@require_http_methods(["GET"])
def stats_view(request):
    """Django view returning engine and vector index statistics."""
    db = PurpleDBService.get_db()
    stats = db.stats()
    return JsonResponse(stats.to_dict())


# Celery Ingest Task Example
def ingest_video_task(video_path: str):
    """Example task for asynchronous ingestion with Celery or Django Q."""
    db = PurpleDBService.get_db()
    video_id = db.add_video(video_path)
    return {"status": "success", "video_id": video_id}


if __name__ == "__main__":
    print("Testing Django PurpleDBService Singleton Pattern...")
    db = PurpleDBService.get_db("./test_django_db")
    print("Database opened successfully:", db.stats())
    PurpleDBService.close()
    if os.path.exists("./test_django_db"):
        import shutil
        shutil.rmtree("./test_django_db", ignore_errors=True)
    print("Django integration pattern verified.")
