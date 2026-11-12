"""Golden set evaluation runner and parameter sensitivity sweep for PurpleDB M0.

Evaluates multimodal retrieval performance on synthetic benchmark videos,
measuring Hit Rate @ k=5, MRR@5, Precision@1, and parameter sensitivity across
clustering thresholds and frame sampling rates. Emits a Go/No-Go assessment.
"""

import json
import os
import sys
import time
from dataclasses import dataclass, field
from typing import Dict, Any, List, Optional, Tuple

# Ensure project root in sys.path
current_dir = os.path.dirname(os.path.abspath(__file__))
project_root = os.path.abspath(os.path.join(current_dir, ".."))
if project_root not in sys.path:
    sys.path.insert(0, project_root)

from prototype.pipeline import VideoSearchPipeline
from prototype.embedder import BaseEmbedder, create_default_embedder
from prototype.index import SearchResult


@dataclass
class HitCheck:
    """Detailed hit diagnostics for a single retrieval candidate."""

    is_hit: bool
    overlap_sec: float
    iou: float
    midpoint_diff_sec: float
    matched_video: bool


@dataclass
class QueryEvalResult:
    """Evaluation result for an individual benchmark query."""

    query_id: str
    query_text: str
    target_video_id: str
    expected_interval: Tuple[float, float]
    top_k_results: List[SearchResult]
    hit_at_k: bool
    reciprocal_rank: float
    best_rank: Optional[int]


@dataclass
class BenchmarkMetrics:
    """Aggregate benchmark metrics and Go/No-Go assessment."""

    total_queries: int
    hits_at_k: int
    hit_rate_at_k: float
    mrr_at_k: float
    precision_at_1: float
    threshold: float
    fps: float
    total_moments_indexed: int
    eval_time_sec: float
    query_results: List[QueryEvalResult] = field(default_factory=list)
    decision: str = "PENDING"
    decision_rationale: str = ""


def check_hit(
    result: SearchResult,
    query_def: Dict[str, Any],
    min_overlap: float = 1.0,
    min_iou: float = 0.30,
    max_midpoint_diff: float = 2.0,
) -> HitCheck:
    """Determine whether a search result matches ground-truth target video and interval.

    A result candidate is deemed a Hit if:
    1. Video ID matches target video ID, AND
    2. Any of the following temporal criteria is met:
       a) Temporal overlap >= min_overlap (default 1.0 sec)
       b) Temporal Intersection-over-Union (tIoU) >= min_iou (default 0.30)
       c) Result midpoint falls within tolerance of expected interval midpoint
    """
    if result.video_id != query_def["target_video_id"]:
        return HitCheck(
            is_hit=False,
            overlap_sec=0.0,
            iou=0.0,
            midpoint_diff_sec=999.0,
            matched_video=False,
        )

    exp_start = float(query_def["expected_interval"]["start_sec"])
    exp_end = float(query_def["expected_interval"]["end_sec"])
    res_start = float(result.start_sec)
    res_end = float(result.end_sec)

    # Calculate temporal overlap
    overlap_start = max(res_start, exp_start)
    overlap_end = min(res_end, exp_end)
    overlap = max(0.0, overlap_end - overlap_start)

    # Calculate temporal Union
    union_start = min(res_start, exp_start)
    union_end = max(res_end, exp_end)
    union = max(1e-6, union_end - union_start)
    iou = overlap / union

    # Calculate midpoint difference
    exp_mid = (exp_start + exp_end) / 2.0
    res_mid = (res_start + res_end) / 2.0
    mid_diff = abs(res_mid - exp_mid)

    is_hit = (overlap >= min_overlap) or (iou >= min_iou) or (mid_diff <= max_midpoint_diff)

    return HitCheck(
        is_hit=is_hit,
        overlap_sec=overlap,
        iou=iou,
        midpoint_diff_sec=mid_diff,
        matched_video=True,
    )


def evaluate_golden_set(
    annotations_path: str,
    videos_dir: Optional[str] = None,
    pipeline: Optional[VideoSearchPipeline] = None,
    k: int = 5,
    threshold: float = 0.90,
    fps: float = 1.0,
    embedder: Optional[BaseEmbedder] = None,
) -> BenchmarkMetrics:
    """Run golden set retrieval evaluation against annotations and videos.

    Args:
        annotations_path: Path to annotations.json.
        videos_dir: Path to directory containing the .mp4 video files.
        pipeline: Optional pre-configured VideoSearchPipeline.
        k: Cutoff rank for Hit Rate @ k and MRR@k (default 5).
        threshold: Moment clustering similarity threshold.
        fps: Video frame sampling rate.
        embedder: Multimodal embedder to use if pipeline is not provided.

    Returns:
        BenchmarkMetrics containing aggregate metrics and query-by-query breakdown.
    """
    if not os.path.isfile(annotations_path):
        raise FileNotFoundError(f"Annotations file not found: {annotations_path}")

    with open(annotations_path, "r", encoding="utf-8") as f:
        annotations = json.load(f)

    if videos_dir is None:
        videos_dir = os.path.join(os.path.dirname(annotations_path), "videos")

    start_time = time.time()

    # Initialize pipeline if not passed
    active_pipeline = pipeline or VideoSearchPipeline(
        embedder=embedder or create_default_embedder(),
        similarity_threshold=threshold,
        fps=fps,
    )

    # Index all videos defined in annotations
    for vid_meta in annotations["videos"]:
        vid_filename = vid_meta["filename"]
        vid_path = os.path.join(videos_dir, vid_filename)
        if not os.path.isfile(vid_path):
            raise FileNotFoundError(f"Benchmark video not found: {vid_path}")
        active_pipeline.index_video(video_path=vid_path, video_id=vid_meta["video_id"])

    total_moments = active_pipeline.index.count
    queries = annotations.get("queries", [])
    total_queries = len(queries)

    hits_at_k = 0
    reciprocal_ranks = []
    precision_1_hits = 0
    query_eval_results: List[QueryEvalResult] = []

    for q in queries:
        query_text = q["query_text"]
        target_video_id = q["target_video_id"]
        exp_start = float(q["expected_interval"]["start_sec"])
        exp_end = float(q["expected_interval"]["end_sec"])

        search_results = active_pipeline.search_text(query=query_text, top_k=k)

        first_hit_rank: Optional[int] = None
        for rank, res in enumerate(search_results, start=1):
            check = check_hit(res, q)
            if check.is_hit:
                first_hit_rank = rank
                break

        if first_hit_rank is not None:
            hits_at_k += 1
            rr = 1.0 / first_hit_rank
            if first_hit_rank == 1:
                precision_1_hits += 1
        else:
            rr = 0.0

        reciprocal_ranks.append(rr)

        query_eval_results.append(
            QueryEvalResult(
                query_id=q["query_id"],
                query_text=query_text,
                target_video_id=target_video_id,
                expected_interval=(exp_start, exp_end),
                top_k_results=search_results,
                hit_at_k=(first_hit_rank is not None),
                reciprocal_rank=rr,
                best_rank=first_hit_rank,
            )
        )

    eval_duration = time.time() - start_time
    hit_rate = hits_at_k / total_queries if total_queries > 0 else 0.0
    mrr = sum(reciprocal_ranks) / total_queries if total_queries > 0 else 0.0
    precision_1 = precision_1_hits / total_queries if total_queries > 0 else 0.0

    # Milestone M0 Go / No-Go Decision Logic (Target: Hit Rate @ 5 >= 0.80)
    if hit_rate >= 0.80:
        decision = "GO"
        decision_rationale = (
            f"Hit Rate @ k={k} of {hit_rate * 100:.1f}% meets/exceeds the 80% target. "
            f"Multimodal joint embedding and moment segmentation demonstrate high retrieval fidelity."
        )
    else:
        decision = "NO-GO"
        decision_rationale = (
            f"Hit Rate @ k={k} of {hit_rate * 100:.1f}% is below the 80% acceptance threshold. "
            f"Moment clustering parameters or model alignment require tuning before proceeding to M1."
        )

    return BenchmarkMetrics(
        total_queries=total_queries,
        hits_at_k=hits_at_k,
        hit_rate_at_k=round(hit_rate, 4),
        mrr_at_k=round(mrr, 4),
        precision_at_1=round(precision_1, 4),
        threshold=threshold,
        fps=fps,
        total_moments_indexed=total_moments,
        eval_time_sec=round(eval_duration, 3),
        query_results=query_eval_results,
        decision=decision,
        decision_rationale=decision_rationale,
    )


def run_parameter_sweep(
    annotations_path: str,
    videos_dir: Optional[str] = None,
    thresholds: Optional[List[float]] = None,
    fps_rates: Optional[List[float]] = None,
    k: int = 5,
    embedder: Optional[BaseEmbedder] = None,
) -> List[BenchmarkMetrics]:
    """Execute parameter sensitivity sweeps across thresholds and sampling rates."""
    if thresholds is None:
        thresholds = [0.85, 0.90, 0.95]
    if fps_rates is None:
        fps_rates = [0.5, 1.0, 2.0]

    shared_embedder = embedder or create_default_embedder()
    sweep_results: List[BenchmarkMetrics] = []

    for fps in fps_rates:
        for thresh in thresholds:
            metrics = evaluate_golden_set(
                annotations_path=annotations_path,
                videos_dir=videos_dir,
                k=k,
                threshold=thresh,
                fps=fps,
                embedder=shared_embedder,
            )
            sweep_results.append(metrics)

    return sweep_results


if __name__ == "__main__":
    annot_path = os.path.join(current_dir, "golden_set", "annotations.json")
    v_dir = os.path.join(current_dir, "golden_set", "videos")

    # If dataset has not been generated yet, generate it now
    if not os.path.exists(annot_path):
        from prototype.golden_set.generate_data import generate_benchmark_dataset
        print("Golden dataset not found. Generating now...")
        generate_benchmark_dataset(os.path.join(current_dir, "golden_set"))

    print("\n" + "=" * 70)
    print("PurpleDB Milestone M0 — Golden Set Evaluation Runner")
    print("=" * 70)

    # 1. Baseline Evaluation (Default threshold=0.90, fps=1.0)
    baseline = evaluate_golden_set(annot_path, v_dir, threshold=0.90, fps=1.0)

    print(f"\n[Baseline Configuration: fps={baseline.fps}, threshold={baseline.threshold}]")
    print(f"Total Queries Evaluated:    {baseline.total_queries}")
    print(f"Moments Indexed:            {baseline.total_moments_indexed}")
    print(f"Hit Rate @ k=5:             {baseline.hit_rate_at_k * 100:.1f}% ({baseline.hits_at_k}/{baseline.total_queries})")
    print(f"MRR @ k=5:                  {baseline.mrr_at_k:.4f}")
    print(f"Precision @ 1:              {baseline.precision_at_1 * 100:.1f}%")
    print(f"Evaluation Duration:        {baseline.eval_time_sec:.2f}s")
    print(f"Assessment Decision:        [{baseline.decision}]")
    print(f"Rationale:                  {baseline.decision_rationale}")

    print("\nDetailed Query Results:")
    print("-" * 70)
    for q_res in baseline.query_results:
        status = "HIT " if q_res.hit_at_k else "MISS"
        rank_str = f"Rank {q_res.best_rank}" if q_res.best_rank else "None  "
        print(f"[{status}] {q_res.query_id} ({rank_str}) | Target: {q_res.target_video_id} {q_res.expected_interval}s | \"{q_res.query_text}\"")

    # 2. Parameter Sensitivity Sweep
    print("\n" + "=" * 70)
    print("Parameter Sensitivity Sweep (Sampling Rates x Similarity Thresholds)")
    print("=" * 70)
    print(f"{'FPS':>5} | {'Threshold':>9} | {'Moments':>7} | {'Hit Rate@5':>10} | {'MRR@5':>7} | {'Prec@1':>7} | {'Decision':>8}")
    print("-" * 70)

    sweep = run_parameter_sweep(annot_path, v_dir, thresholds=[0.85, 0.90, 0.95], fps_rates=[0.5, 1.0, 2.0])
    for s in sweep:
        print(f"{s.fps:>5.1f} | {s.threshold:>9.2f} | {s.total_moments_indexed:>7} | {s.hit_rate_at_k * 100:>9.1f}% | {s.mrr_at_k:>7.4f} | {s.precision_at_1 * 100:>6.1f}% | {s.decision:>8}")
    print("=" * 70)
