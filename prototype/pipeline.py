"""End-to-end video ingest and search pipeline.

Coordinates content hashing (SHA-256), frame extraction, CLIP embedding,
anchor-based moment detection, vector indexing, and multimodal search.
"""

import hashlib
import os
from typing import List, Optional, Union, Dict, Any
from PIL import Image

from prototype.frame_sampler import sample_frames, get_video_duration
from prototype.embedder import BaseEmbedder, create_default_embedder
from prototype.moment_detector import Moment, detect_moments
from prototype.index import VectorIndex, SearchResult


def compute_video_id(video_path: str, chunk_size: int = 1024 * 1024) -> str:
    """Compute deterministic SHA-256 content hash for a video file.

    Args:
        video_path: Path to the video file.
        chunk_size: Read buffer size in bytes.

    Returns:
        Hexadecimal SHA-256 digest string.
    """
    if not os.path.isfile(video_path):
        raise FileNotFoundError(f"Video file not found: {video_path}")

    hasher = hashlib.sha256()
    with open(video_path, "rb") as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            hasher.update(chunk)
    return hasher.hexdigest()


class VideoSearchPipeline:
    """End-to-end pipeline for multimodal video ingestion and retrieval."""

    def __init__(
        self,
        embedder: Optional[BaseEmbedder] = None,
        similarity_threshold: float = 0.90,
        fps: float = 1.0,
        thumbnail_dir: Optional[str] = None,
    ):
        """Initialize the video search pipeline.

        Args:
            embedder: Multimodal embedder instance (defaults to fastembed ONNX CLIP).
            similarity_threshold: Cosine threshold for moment boundary clustering.
            fps: Frame sampling rate in frames per second.
            thumbnail_dir: Optional filesystem path to persist representative thumbnails.
        """
        self.embedder = embedder or create_default_embedder()
        self.similarity_threshold = similarity_threshold
        self.fps = fps
        self.thumbnail_dir = thumbnail_dir
        self.index = VectorIndex(dim=512)
        self.indexed_videos: Dict[str, Dict[str, Any]] = {}

        if self.thumbnail_dir:
            os.makedirs(self.thumbnail_dir, exist_ok=True)

    def index_video(
        self,
        video_path: str,
        video_id: Optional[str] = None,
        metadata: Optional[Dict[str, Any]] = None,
    ) -> List[Moment]:
        """Ingest and index a video file.

        1. Computes SHA-256 video_id for idempotent content addressing.
        2. Samples frames at configured fps downscaled to 224x224.
        3. Encodes frames into 512-d normalized CLIP embeddings.
        4. Detects coherent moments via anchor-based clustering.
        5. Inserts moments into the vector index.

        Args:
            video_path: Path to the target video file.
            video_id: Optional explicit video ID (defaults to SHA-256 hash).
            metadata: Optional additional metadata dict to attach to moments.

        Returns:
            List of detected and indexed Moment instances.
        """
        if not os.path.isfile(video_path):
            raise FileNotFoundError(f"Video file not found: {video_path}")

        actual_video_id = video_id or compute_video_id(video_path)

        # 1. Sample frames
        sampled_frames = sample_frames(video_path, fps=self.fps)
        if not sampled_frames:
            return []

        # 2. Embed frames
        images = [f[1] for f in sampled_frames]
        embeddings = self.embedder.embed_images(images)

        # 3. Detect moments
        moments = detect_moments(
            frames=sampled_frames,
            embeddings=embeddings,
            video_id=actual_video_id,
            threshold=self.similarity_threshold,
        )

        # Attach custom metadata and optionally persist thumbnails
        for idx, m in enumerate(moments):
            if metadata:
                m.metadata.update(metadata)
            m.metadata["video_path"] = video_path

            if self.thumbnail_dir and m.thumbnail_image:
                thumb_filename = f"{actual_video_id[:12]}_moment_{idx:04d}_{m.start_sec:.1f}s.jpg"
                thumb_path = os.path.join(self.thumbnail_dir, thumb_filename)
                m.thumbnail_image.save(thumb_path, format="JPEG", quality=85)
                m.metadata["thumbnail_path"] = thumb_path

        # 4. Add to vector index
        self.index.add_moments(moments)

        # Record video catalog entry
        self.indexed_videos[actual_video_id] = {
            "video_id": actual_video_id,
            "video_path": video_path,
            "duration_sec": get_video_duration(video_path),
            "moments_count": len(moments),
            "frames_sampled": len(sampled_frames),
        }

        return moments

    def search_text(
        self,
        query: str,
        top_k: int = 5,
        video_id: Optional[str] = None,
    ) -> List[SearchResult]:
        """Search indexed moments using a natural language text query."""
        if not query.strip():
            return []

        query_embeddings = self.embedder.embed_text([query])
        if len(query_embeddings) == 0:
            return []

        q_vec = query_embeddings[0]
        return self.index.search(query_vector=q_vec, top_k=top_k, video_id=video_id)

    def search_image(
        self,
        image_or_path: Union[str, Image.Image],
        top_k: int = 5,
        video_id: Optional[str] = None,
    ) -> List[SearchResult]:
        """Search indexed moments using an example query image."""
        if isinstance(image_or_path, str):
            if not os.path.isfile(image_or_path):
                raise FileNotFoundError(f"Query image not found: {image_or_path}")
            with Image.open(image_or_path) as img:
                pil_img = img.convert("RGB")
        else:
            pil_img = image_or_path.convert("RGB")

        query_embeddings = self.embedder.embed_images([pil_img])
        if len(query_embeddings) == 0:
            return []

        q_vec = query_embeddings[0]
        return self.index.search(query_vector=q_vec, top_k=top_k, video_id=video_id)
