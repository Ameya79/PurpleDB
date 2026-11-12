"""Moment boundary detection and clustering module.

Groups consecutive sampled video frames into temporal moments based on
cosine similarity against an anchor frame embedding. Selects the most
representative frame (medoid) as the thumbnail and computes the normalized
moment embedding.
"""

from dataclasses import dataclass, field
from typing import List, Optional, Tuple, Dict, Any
import numpy as np
from PIL import Image


@dataclass
class Moment:
    """Represents a coherent temporal moment in a video."""

    video_id: str
    start_sec: float
    end_sec: float
    embedding: np.ndarray  # (512,) float32 unit L2-normalized vector
    thumbnail_image: Optional[Image.Image] = None
    metadata: Dict[str, Any] = field(default_factory=dict)

    @property
    def duration_sec(self) -> float:
        """Duration of this moment in seconds."""
        return max(0.0, self.end_sec - self.start_sec)


def detect_moments(
    frames: List[Tuple[float, Image.Image]],
    embeddings: np.ndarray,
    video_id: str = "",
    threshold: float = 0.90,
) -> List[Moment]:
    """Detect temporal moments by clustering consecutive frames.

    Uses an anchor-based boundary detection algorithm:
    1. The first frame of a moment establishes the anchor embedding.
    2. Subsequent frames are compared to the anchor using cosine similarity.
    3. As long as similarity >= threshold, frames are grouped into the active moment.
    4. When similarity drops below threshold, the active moment closes and a new
       moment is initialized.
    5. A representative thumbnail is selected as the medoid frame closest to the
       moment's centroid embedding.

    Args:
        frames: List of (timestamp_sec, PIL.Image) tuples.
        embeddings: Normalized 2D numpy array of shape (N, 512).
        video_id: Content-addressed or assigned ID for the video.
        threshold: Cosine similarity cutoff to initiate a new moment (default 0.90).

    Returns:
        List of Moment instances.
    """
    n_frames = len(frames)
    if n_frames == 0:
        return []

    if len(embeddings) != n_frames:
        raise ValueError(
            f"Frames count ({n_frames}) does not match embeddings count ({len(embeddings)})"
        )

    # Determine typical frame interval
    if n_frames > 1:
        default_interval = float(np.median([
            frames[i][0] - frames[i - 1][0] for i in range(1, n_frames)
        ]))
        if default_interval <= 0:
            default_interval = 1.0
    else:
        default_interval = 1.0

    moments: List[Moment] = []

    # Active cluster state
    accumulated_indices: List[int] = [0]
    anchor_idx: int = 0
    anchor_emb: np.ndarray = embeddings[0]

    def _finalize_moment(cluster_indices: List[int], start_time: float, end_time: float) -> Moment:
        """Build Moment object from accumulated indices."""
        cluster_embs = embeddings[cluster_indices]

        # Centroid embedding
        centroid = np.mean(cluster_embs, axis=0)
        norm = np.linalg.norm(centroid)
        if norm > 0:
            normalized_moment_emb = (centroid / norm).astype(np.float32)
        else:
            normalized_moment_emb = cluster_embs[0].astype(np.float32)

        # Select medoid thumbnail (frame whose embedding is closest to centroid)
        dots = np.dot(cluster_embs, normalized_moment_emb)
        best_local_idx = int(np.argmax(dots))
        best_global_idx = cluster_indices[best_local_idx]
        thumbnail = frames[best_global_idx][1].copy()

        return Moment(
            video_id=video_id,
            start_sec=round(start_time, 4),
            end_sec=round(end_time, 4),
            embedding=normalized_moment_emb,
            thumbnail_image=thumbnail,
            metadata={
                "frame_count": len(cluster_indices),
                "medoid_timestamp_sec": frames[best_global_idx][0],
                "anchor_timestamp_sec": frames[cluster_indices[0]][0],
            },
        )

    for i in range(1, n_frames):
        current_emb = embeddings[i]
        # Since embeddings are L2-normalized, cosine similarity is the dot product
        similarity = float(np.dot(current_emb, anchor_emb))

        if similarity >= threshold:
            accumulated_indices.append(i)
        else:
            # Boundary detected: close current moment
            start_t = frames[accumulated_indices[0]][0]
            end_t = frames[i][0]
            moments.append(_finalize_moment(accumulated_indices, start_t, end_t))

            # Start new moment with frame i as new anchor
            accumulated_indices = [i]
            anchor_idx = i
            anchor_emb = current_emb

    # Finalize trailing moment
    if accumulated_indices:
        start_t = frames[accumulated_indices[0]][0]
        end_t = frames[accumulated_indices[-1]][0] + default_interval
        moments.append(_finalize_moment(accumulated_indices, start_t, end_t))

    return moments
