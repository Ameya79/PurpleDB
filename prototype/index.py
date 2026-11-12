"""In-memory vector store and cosine similarity search engine using NumPy.

Maintains normalized 512-dimensional vector embeddings and executes fast
dot-product cosine search with partial-sort top-k ranking.
"""

from dataclasses import dataclass, field
from typing import List, Optional, Dict, Any
import numpy as np
from PIL import Image

from prototype.moment_detector import Moment
from prototype.embedder import l2_normalize


@dataclass
class SearchResult:
    """Represents a ranked search match returned by a vector query."""

    video_id: str
    start_sec: float
    end_sec: float
    score: float
    thumbnail_image: Optional[Image.Image] = None
    thumbnail_path: Optional[str] = None
    metadata: Dict[str, Any] = field(default_factory=dict)

    @property
    def duration_sec(self) -> float:
        """Duration of matching moment in seconds."""
        return max(0.0, self.end_sec - self.start_sec)


class VectorIndex:
    """In-memory vector index storing moments and executing cosine similarity queries."""

    def __init__(self, dim: int = 512):
        self.dim = dim
        self._moments: List[Moment] = []
        self._matrix: Optional[np.ndarray] = None  # Shape (N, dim)

    def __len__(self) -> int:
        return len(self._moments)

    @property
    def count(self) -> int:
        return len(self._moments)

    def clear(self) -> None:
        """Clear all indexed moments and reset the vector matrix."""
        self._moments = []
        self._matrix = None

    def add_moments(self, moments: List[Moment]) -> None:
        """Add moments to the vector index and update the internal matrix.

        Args:
            moments: List of Moment instances to index.
        """
        if not moments:
            return

        new_vectors = []
        for m in moments:
            vec = np.asarray(m.embedding, dtype=np.float32).reshape(-1)
            if vec.shape[0] != self.dim:
                raise ValueError(
                    f"Vector dimension {vec.shape[0]} does not match index dimension {self.dim}"
                )
            # Ensure vector is L2 normalized
            vec = l2_normalize(vec)
            new_vectors.append(vec)
            self._moments.append(m)

        new_mat = np.vstack(new_vectors)
        if self._matrix is None:
            self._matrix = new_mat
        else:
            self._matrix = np.vstack([self._matrix, new_mat])

    def search(
        self,
        query_vector: np.ndarray,
        top_k: int = 5,
        video_id: Optional[str] = None,
    ) -> List[SearchResult]:
        """Search the index for the most similar moments using cosine similarity.

        Because stored vectors and query vectors are unit L2-normalized,
        cosine similarity is computed directly via the dot product (matrix multiplication).

        Args:
            query_vector: 1D or 2D vector of dimension (512,).
            top_k: Number of highest-scoring results to return.
            video_id: Optional filter to restrict search to a specific video.

        Returns:
            List of SearchResult objects sorted by descending cosine similarity score.
        """
        if self._matrix is None or len(self._moments) == 0:
            return []

        q = np.asarray(query_vector, dtype=np.float32).reshape(-1)
        if q.shape[0] != self.dim:
            raise ValueError(
                f"Query vector dimension {q.shape[0]} does not match index dimension {self.dim}"
            )

        # Ensure query is unit normalized
        q = l2_normalize(q)

        # Apply video_id filter if requested
        if video_id is not None:
            mask = [i for i, m in enumerate(self._moments) if m.video_id == video_id]
            if not mask:
                return []
            candidate_indices = np.array(mask, dtype=int)
            sub_matrix = self._matrix[candidate_indices]
            scores = sub_matrix @ q
            score_to_global = candidate_indices
        else:
            scores = self._matrix @ q
            score_to_global = np.arange(len(self._moments))

        n_candidates = len(scores)
        k = min(top_k, n_candidates)
        if k <= 0:
            return []

        # Ranking: partial partition if candidate set is large, else full argsort
        if n_candidates > k * 2 and n_candidates > 50:
            # O(N) selection of top k
            partitioned = np.argpartition(-scores, kth=k - 1)[:k]
            sorted_k = partitioned[np.argsort(-scores[partitioned])]
        else:
            sorted_k = np.argsort(-scores)[:k]

        results: List[SearchResult] = []
        for local_idx in sorted_k:
            global_idx = int(score_to_global[local_idx])
            moment = self._moments[global_idx]
            results.append(
                SearchResult(
                    video_id=moment.video_id,
                    start_sec=moment.start_sec,
                    end_sec=moment.end_sec,
                    score=float(scores[local_idx]),
                    thumbnail_image=moment.thumbnail_image,
                    metadata=dict(moment.metadata),
                )
            )

        return results
