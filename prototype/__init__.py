"""PurpleDB Prototype - Milestone M0

A lightweight proof-of-concept for video ingestion, frame sampling,
multimodal CLIP embedding, moment detection, and in-memory vector search.
"""

from prototype.frame_sampler import sample_frames
from prototype.embedder import ClipEmbedder, create_default_embedder
from prototype.moment_detector import Moment, detect_moments
from prototype.index import VectorIndex, SearchResult
from prototype.pipeline import VideoSearchPipeline

__all__ = [
    "sample_frames",
    "ClipEmbedder",
    "create_default_embedder",
    "Moment",
    "detect_moments",
    "VectorIndex",
    "SearchResult",
    "VideoSearchPipeline",
]
