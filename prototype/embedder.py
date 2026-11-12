"""Multimodal CLIP embedding module.

Provides a unified interface for generating 512-dimensional L2-normalized
embeddings for images and text using fastembed ONNX Runtime CLIP (with optional
fallback support for open_clip if installed).
"""

import os
from typing import List, Union, Optional
import numpy as np
from PIL import Image

# Suppress HuggingFace symlink warnings on Windows
os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")


def l2_normalize(vectors: np.ndarray) -> np.ndarray:
    """Normalize vectors to unit L2 norm along the last axis.

    Args:
        vectors: NumPy array of shape (N, D) or (D,).

    Returns:
        L2-normalized float32 NumPy array of same shape.
    """
    vectors = np.asarray(vectors, dtype=np.float32)
    if vectors.ndim == 1:
        norm = np.linalg.norm(vectors)
        if norm > 0:
            return vectors / norm
        return vectors
    norms = np.linalg.norm(vectors, axis=1, keepdims=True)
    norms = np.where(norms == 0, 1e-12, norms)
    return (vectors / norms).astype(np.float32)


class BaseEmbedder:
    """Abstract protocol for multimodal embedders."""

    def embed_images(self, images: List[Image.Image]) -> np.ndarray:
        raise NotImplementedError

    def embed_text(self, texts: List[str]) -> np.ndarray:
        raise NotImplementedError


class FastEmbedClipEmbedder(BaseEmbedder):
    """CLIP ViT-B/32 multimodal embedder powered by fastembed and ONNX Runtime."""

    def __init__(
        self,
        vision_model_name: str = "Qdrant/clip-ViT-B-32-vision",
        text_model_name: str = "Qdrant/clip-ViT-B-32-text",
        cache_dir: Optional[str] = None,
    ):
        from fastembed import ImageEmbedding, TextEmbedding

        self.vision_model_name = vision_model_name
        self.text_model_name = text_model_name
        self._image_model = ImageEmbedding(model_name=vision_model_name, cache_dir=cache_dir)
        self._text_model = TextEmbedding(model_name=text_model_name, cache_dir=cache_dir)
        self.dim = 512

    def embed_images(self, images: List[Image.Image]) -> np.ndarray:
        """Generate 512-d normalized embeddings for a list of PIL Images."""
        if not images:
            return np.empty((0, self.dim), dtype=np.float32)

        # Ensure all images are RGB PIL Images
        rgb_images = [img.convert("RGB") if img.mode != "RGB" else img for img in images]
        raw_embeddings = list(self._image_model.embed(rgb_images))
        matrix = np.array(raw_embeddings, dtype=np.float32)
        return l2_normalize(matrix)

    def embed_text(self, texts: List[str]) -> np.ndarray:
        """Generate 512-d normalized embeddings for a list of text queries."""
        if not texts:
            return np.empty((0, self.dim), dtype=np.float32)

        raw_embeddings = list(self._text_model.embed(texts))
        matrix = np.array(raw_embeddings, dtype=np.float32)
        return l2_normalize(matrix)


class OpenClipEmbedder(BaseEmbedder):
    """Optional CLIP embedder using open_clip_torch if installed."""

    def __init__(
        self,
        model_name: str = "ViT-B-32",
        pretrained: str = "laion2b_s34b_b79k",
        device: str = "cpu",
    ):
        import open_clip  # type: ignore
        import torch  # type: ignore

        self.device = device
        self.model, _, self.preprocess = open_clip.create_model_and_transforms(
            model_name, pretrained=pretrained, device=device
        )
        self.tokenizer = open_clip.get_tokenizer(model_name)
        self.dim = 512

    def embed_images(self, images: List[Image.Image]) -> np.ndarray:
        import torch  # type: ignore

        if not images:
            return np.empty((0, self.dim), dtype=np.float32)

        tensors = torch.stack([self.preprocess(img.convert("RGB")) for img in images]).to(
            self.device
        )
        with torch.no_grad():
            features = self.model.encode_image(tensors)
            features = features / features.norm(dim=-1, keepdim=True)
            return features.cpu().numpy().astype(np.float32)

    def embed_text(self, texts: List[str]) -> np.ndarray:
        import torch  # type: ignore

        if not texts:
            return np.empty((0, self.dim), dtype=np.float32)

        tokens = self.tokenizer(texts).to(self.device)
        with torch.no_grad():
            features = self.model.encode_text(tokens)
            features = features / features.norm(dim=-1, keepdim=True)
            return features.cpu().numpy().astype(np.float32)


# Alias default embedder to FastEmbedClipEmbedder
ClipEmbedder = FastEmbedClipEmbedder


def create_default_embedder(prefer_backend: str = "fastembed") -> BaseEmbedder:
    """Factory creating the best available CLIP embedder."""
    if prefer_backend == "open_clip":
        try:
            return OpenClipEmbedder()
        except ImportError:
            return FastEmbedClipEmbedder()
    return FastEmbedClipEmbedder()
