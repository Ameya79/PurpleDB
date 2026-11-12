"""Unit tests for PurpleDB M0 Prototype modules."""

import os
import tempfile
import pytest
import numpy as np
from PIL import Image

from prototype.frame_sampler import sample_frames, get_ffmpeg_path, get_video_duration
from prototype.embedder import FastEmbedClipEmbedder, l2_normalize
from prototype.moment_detector import Moment, detect_moments
from prototype.index import VectorIndex, SearchResult
from prototype.pipeline import VideoSearchPipeline, compute_video_id


@pytest.fixture(scope="module")
def embedder():
    """Shared CLIP embedder instance for test suite."""
    return FastEmbedClipEmbedder()


@pytest.fixture(scope="module")
def sample_test_video():
    """Generate a tiny 3-second synthetic MP4 video using ffmpeg lavfi for testing."""
    import subprocess
    ffmpeg_exe = get_ffmpeg_path()
    with tempfile.NamedTemporaryFile(suffix=".mp4", delete=False) as f:
        video_path = f.name

    try:
        # Generate 3 seconds of solid red video
        cmd = [
            ffmpeg_exe,
            "-y",
            "-f",
            "lavfi",
            "-i",
            "color=c=red:s=320x240:d=3:r=25",
            "-pix_fmt",
            "yuv420p",
            "-loglevel",
            "error",
            video_path,
        ]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert res.returncode == 0, f"FFmpeg failed: {res.stderr.decode()}"
        yield video_path
    finally:
        if os.path.exists(video_path):
            os.remove(video_path)


# ============================================================================
# 1. Frame Sampler Tests
# ============================================================================

def test_frame_sampler_missing_file():
    with pytest.raises(FileNotFoundError):
        sample_frames("non_existent_video_file_xyz.mp4")


def test_frame_sampler_invalid_fps():
    with pytest.raises(ValueError):
        sample_frames("dummy.mp4", fps=-1.0)


def test_frame_sampler_synthetic_video(sample_test_video):
    frames = sample_frames(sample_test_video, fps=1.0, scale=(224, 224))
    # 3 second video at 1 fps should yield ~3 frames
    assert len(frames) in [3, 4]
    for ts, img in frames:
        assert isinstance(ts, float)
        assert ts >= 0.0
        assert isinstance(img, Image.Image)
        assert img.size == (224, 224)
        assert img.mode == "RGB"


def test_get_video_duration(sample_test_video):
    duration = get_video_duration(sample_test_video)
    assert 2.8 <= duration <= 3.2


# ============================================================================
# 2. Embedder Tests
# ============================================================================

def test_l2_normalize():
    v = np.array([3.0, 4.0], dtype=np.float32)
    normed = l2_normalize(v)
    assert np.isclose(np.linalg.norm(normed), 1.0)

    # 2D matrix
    mat = np.array([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]], dtype=np.float32)
    normed_mat = l2_normalize(mat)
    norms = np.linalg.norm(normed_mat, axis=1)
    assert np.allclose(norms, 1.0)


def test_embedder_images(embedder):
    img1 = Image.new("RGB", (224, 224), color="blue")
    img2 = Image.new("RGB", (224, 224), color="red")
    embs = embedder.embed_images([img1, img2])

    assert embs.shape == (2, 512)
    assert embs.dtype == np.float32
    norms = np.linalg.norm(embs, axis=1)
    assert np.allclose(norms, 1.0, atol=1e-5)


def test_embedder_text(embedder):
    texts = ["a blue ocean wave", "a red sports car"]
    embs = embedder.embed_text(texts)

    assert embs.shape == (2, 512)
    assert embs.dtype == np.float32
    norms = np.linalg.norm(embs, axis=1)
    assert np.allclose(norms, 1.0, atol=1e-5)


def test_embedder_empty_input(embedder):
    assert embedder.embed_images([]).shape == (0, 512)
    assert embedder.embed_text([]).shape == (0, 512)


def test_embedder_cross_modal_alignment(embedder):
    # Blue image should match "blue color" more than "red color"
    blue_img = Image.new("RGB", (224, 224), color="blue")
    img_emb = embedder.embed_images([blue_img])[0]

    txt_embs = embedder.embed_text(["solid blue color", "solid red color"])
    blue_sim = float(np.dot(img_emb, txt_embs[0]))
    red_sim = float(np.dot(img_emb, txt_embs[1]))

    assert blue_sim > red_sim, f"Expected blue sim ({blue_sim}) > red sim ({red_sim})"


# ============================================================================
# 3. Moment Detector Tests
# ============================================================================

def test_detect_moments_empty():
    assert detect_moments([], np.empty((0, 512))) == []


def test_detect_moments_dimension_mismatch():
    frames = [(0.0, Image.new("RGB", (10, 10)))]
    embs = np.zeros((2, 512), dtype=np.float32)
    with pytest.raises(ValueError):
        detect_moments(frames, embs)


def test_detect_moments_single_cluster():
    # 5 frames with identical embeddings should collapse to 1 moment
    frames = [(float(i), Image.new("RGB", (10, 10), color="green")) for i in range(5)]
    single_vec = l2_normalize(np.ones(512, dtype=np.float32))
    embs = np.tile(single_vec, (5, 1))

    moments = detect_moments(frames, embs, video_id="vid_test", threshold=0.90)
    assert len(moments) == 1
    m = moments[0]
    assert m.video_id == "vid_test"
    assert m.start_sec == 0.0
    assert m.end_sec == 5.0
    assert m.metadata["frame_count"] == 5
    assert np.isclose(np.linalg.norm(m.embedding), 1.0)


def test_detect_moments_multi_cluster():
    # Frames 0, 1, 2 are identical vector A; frames 3, 4 are orthogonal vector B
    frames = [(float(i), Image.new("RGB", (10, 10))) for i in range(5)]
    vec_a = np.zeros(512, dtype=np.float32)
    vec_a[0] = 1.0
    vec_b = np.zeros(512, dtype=np.float32)
    vec_b[1] = 1.0

    embs = np.array([vec_a, vec_a, vec_a, vec_b, vec_b], dtype=np.float32)

    moments = detect_moments(frames, embs, video_id="vid_split", threshold=0.90)
    assert len(moments) == 2
    assert moments[0].start_sec == 0.0
    assert moments[0].end_sec == 3.0
    assert moments[0].metadata["frame_count"] == 3

    assert moments[1].start_sec == 3.0
    assert moments[1].end_sec == 5.0
    assert moments[1].metadata["frame_count"] == 2


# ============================================================================
# 4. Vector Index Tests
# ============================================================================

def test_vector_index_empty():
    idx = VectorIndex(dim=512)
    assert len(idx) == 0
    assert idx.count == 0
    assert idx.search(np.ones(512, dtype=np.float32)) == []


def test_vector_index_search():
    idx = VectorIndex(dim=512)

    v1 = np.zeros(512, dtype=np.float32)
    v1[0] = 1.0
    m1 = Moment("vid1", 0.0, 5.0, v1)

    v2 = np.zeros(512, dtype=np.float32)
    v2[1] = 1.0
    m2 = Moment("vid2", 0.0, 5.0, v2)

    idx.add_moments([m1, m2])
    assert len(idx) == 2

    # Query matching v1 exactly
    results = idx.search(v1, top_k=2)
    assert len(results) == 2
    assert results[0].video_id == "vid1"
    assert np.isclose(results[0].score, 1.0)
    assert results[1].video_id == "vid2"
    assert np.isclose(results[1].score, 0.0)


def test_vector_index_filter_by_video_id():
    idx = VectorIndex(dim=512)
    v = l2_normalize(np.ones(512, dtype=np.float32))
    idx.add_moments([
        Moment("vid_A", 0.0, 5.0, v),
        Moment("vid_B", 0.0, 5.0, v),
    ])

    results = idx.search(v, video_id="vid_B", top_k=5)
    assert len(results) == 1
    assert results[0].video_id == "vid_B"


# ============================================================================
# 5. Pipeline Tests
# ============================================================================

def test_pipeline_content_id(sample_test_video):
    vid_id1 = compute_video_id(sample_test_video)
    vid_id2 = compute_video_id(sample_test_video)
    assert vid_id1 == vid_id2
    assert len(vid_id1) == 64  # SHA-256 length


def test_pipeline_e2e(embedder, sample_test_video):
    pipeline = VideoSearchPipeline(embedder=embedder, similarity_threshold=0.90, fps=1.0)
    moments = pipeline.index_video(sample_test_video)

    assert len(moments) >= 1
    assert pipeline.index.count == len(moments)

    # Search with text
    results = pipeline.search_text("bright solid red screen", top_k=5)
    assert len(results) > 0
    assert results[0].score > 0.0
