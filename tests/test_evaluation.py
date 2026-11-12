"""Tests for golden set evaluation metrics, hit calculations, and annotations."""

import json
import os
import pytest

from prototype.evaluate import check_hit, evaluate_golden_set, BenchmarkMetrics
from prototype.index import SearchResult
from prototype.embedder import FastEmbedClipEmbedder


@pytest.fixture(scope="module")
def annotations_path():
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base_dir, "prototype", "golden_set", "annotations.json")


@pytest.fixture(scope="module")
def videos_dir():
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base_dir, "prototype", "golden_set", "videos")


# ============================================================================
# 1. Hit Checking Logic Tests
# ============================================================================

def test_check_hit_exact_match():
    query_def = {
        "target_video_id": "vid1",
        "expected_interval": {"start_sec": 5.0, "end_sec": 10.0},
    }
    result = SearchResult(video_id="vid1", start_sec=5.0, end_sec=10.0, score=0.95)
    check = check_hit(result, query_def)

    assert check.is_hit is True
    assert check.matched_video is True
    assert check.overlap_sec == 5.0
    assert check.iou == 1.0


def test_check_hit_wrong_video():
    query_def = {
        "target_video_id": "vid1",
        "expected_interval": {"start_sec": 5.0, "end_sec": 10.0},
    }
    result = SearchResult(video_id="vid2", start_sec=5.0, end_sec=10.0, score=0.95)
    check = check_hit(result, query_def)

    assert check.is_hit is False
    assert check.matched_video is False


def test_check_hit_disjoint_interval():
    query_def = {
        "target_video_id": "vid1",
        "expected_interval": {"start_sec": 0.0, "end_sec": 4.0},
    }
    result = SearchResult(video_id="vid1", start_sec=10.0, end_sec=14.0, score=0.85)
    check = check_hit(result, query_def)

    assert check.is_hit is False
    assert check.overlap_sec == 0.0
    assert check.iou == 0.0


def test_check_hit_partial_overlap():
    query_def = {
        "target_video_id": "vid1",
        "expected_interval": {"start_sec": 2.0, "end_sec": 8.0},
    }
    # Result from 6.0 to 11.0 overlaps [6.0, 8.0] = 2.0s
    result = SearchResult(video_id="vid1", start_sec=6.0, end_sec=11.0, score=0.80)
    check = check_hit(result, query_def)

    assert check.is_hit is True
    assert check.overlap_sec == 2.0


# ============================================================================
# 2. Dataset Schema & Annotations Integrity Tests
# ============================================================================

def test_annotations_integrity(annotations_path):
    assert os.path.isfile(annotations_path), f"annotations.json missing at {annotations_path}"
    with open(annotations_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    assert "videos" in data
    assert "queries" in data
    assert len(data["videos"]) >= 5
    assert len(data["queries"]) >= 15

    video_ids = set()
    for v in data["videos"]:
        assert "video_id" in v
        assert "filename" in v
        assert "duration_sec" in v
        assert v["duration_sec"] > 0
        video_ids.add(v["video_id"])

    for q in data["queries"]:
        assert "query_id" in q
        assert "query_text" in q
        assert "target_video_id" in q
        assert q["target_video_id"] in video_ids, f"Unknown target video in query {q['query_id']}"
        exp = q["expected_interval"]
        assert exp["start_sec"] < exp["end_sec"]
        assert exp["start_sec"] >= 0.0


# ============================================================================
# 3. Golden Set Benchmark Evaluation Test
# ============================================================================

def test_golden_set_benchmark_execution(annotations_path, videos_dir):
    embedder = FastEmbedClipEmbedder()
    metrics = evaluate_golden_set(
        annotations_path=annotations_path,
        videos_dir=videos_dir,
        k=5,
        threshold=0.90,
        fps=1.0,
        embedder=embedder,
    )

    assert isinstance(metrics, BenchmarkMetrics)
    assert metrics.total_queries >= 15
    assert metrics.hits_at_k > 0
    # Must achieve >= 80% Hit Rate @ 5
    assert metrics.hit_rate_at_k >= 0.80, (
        f"Measured Hit Rate @ 5 was {metrics.hit_rate_at_k * 100:.1f}%, expected >= 80%"
    )
    assert metrics.mrr_at_k > 0.50
    assert metrics.decision == "GO"
