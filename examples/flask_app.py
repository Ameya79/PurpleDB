"""PurpleDB Flask Integration Example.

Lightweight synchronous REST microservice using PurpleDB.
"""

from typing import Any, Dict
from flask import Flask, request, jsonify
import purpledb

app = Flask(__name__)

# Open embedded database
db = purpledb.open("./purpledb_flask_data", dim=512, enable_hnsw=True)


@app.route("/ingest", methods=["POST"])
def ingest_video():
    """Ingest a video file synchronously."""
    data = request.get_json(force=True) or {}
    video_path = data.get("video_path")
    if not video_path:
        return jsonify({"error": "video_path is required"}), 400

    try:
        video_id = db.add_video(video_path)
        return jsonify({"status": "ok", "video_id": video_id}), 201
    except Exception as exc:
        return jsonify({"error": str(exc)}), 500


@app.route("/search", methods=["GET"])
def search():
    """Natural language video search endpoint."""
    q = request.args.get("q", "")
    if not q:
        return jsonify({"error": "Query parameter 'q' is required"}), 400

    top_k = int(request.args.get("top_k", 5))
    video_id = request.args.get("video_id", None)
    start_sec = float(request.args.get("start_sec", -1.0))
    end_sec = float(request.args.get("end_sec", -1.0))

    try:
        hits = db.search(
            query=q,
            top_k=top_k,
            video_id=video_id,
            start_sec=start_sec,
            end_sec=end_sec,
        )
        return jsonify({"query": q, "results": [h.to_dict() for h in hits]}), 200
    except Exception as exc:
        return jsonify({"error": str(exc)}), 500


@app.route("/stats", methods=["GET"])
def stats():
    """Return database statistics."""
    try:
        st = db.stats()
        return jsonify(st.to_dict()), 200
    except Exception as exc:
        return jsonify({"error": str(exc)}), 500


@app.route("/videos/<video_id>", methods=["DELETE"])
def delete_video(video_id: str):
    """Delete video and cascades O(1) tombstones."""
    try:
        db.delete_video(video_id)
        return jsonify({"status": "ok", "message": f"Deleted {video_id}"}), 200
    except Exception as exc:
        return jsonify({"error": str(exc)}), 500


if __name__ == "__main__":
    app.run(host="127.0.0.1", port=5000)

