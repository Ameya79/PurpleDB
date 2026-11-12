"""Synthetic golden set dataset generator for PurpleDB Milestone M0.

Programmatically generates 6 diverse benchmark videos using ffmpeg lavfi
filtergraphs and emits annotations.json containing ground-truth temporal intervals
and natural language queries.
"""

import json
import os
import sys
import subprocess
from typing import Dict, Any, List

# Ensure parent directory is in sys.path when script is executed directly
current_dir = os.path.dirname(os.path.abspath(__file__))
project_root = os.path.abspath(os.path.join(current_dir, "../.."))
if project_root not in sys.path:
    sys.path.insert(0, project_root)

from prototype.frame_sampler import get_ffmpeg_path


def generate_benchmark_dataset(
    output_dir: str = os.path.dirname(__file__),
) -> Dict[str, Any]:
    """Generate synthetic videos and annotations.json in the specified directory.

    Args:
        output_dir: Directory where 'videos/' and 'annotations.json' will be written.

    Returns:
        The annotations dictionary.
    """
    videos_dir = os.path.join(output_dir, "videos")
    os.makedirs(videos_dir, exist_ok=True)
    ffmpeg_exe = get_ffmpeg_path()

    # Video 1: Geometric Shapes & Primary Colors (15 seconds)
    # 0-5s: Blue with white center box
    # 5-10s: Red with green center box
    # 10-15s: Yellow with black center box
    vid1_path = os.path.join(videos_dir, "shapes_events.mp4")
    cmd1 = [
        ffmpeg_exe,
        "-y",
        "-f",
        "lavfi",
        "-i",
        "color=c=blue:s=320x240:d=5,drawbox=x=120:y=80:w=80:h=80:color=white@1:t=fill",
        "-f",
        "lavfi",
        "-i",
        "color=c=red:s=320x240:d=5,drawbox=x=120:y=80:w=80:h=80:color=green@1:t=fill",
        "-f",
        "lavfi",
        "-i",
        "color=c=yellow:s=320x240:d=5,drawbox=x=120:y=80:w=80:h=80:color=black@1:t=fill",
        "-filter_complex",
        "[0:v][1:v][2:v]concat=n=3:v=1:a=0[outv]",
        "-map",
        "[outv]",
        "-pix_fmt",
        "yuv420p",
        vid1_path,
    ]

    # Video 2: Simulated Nature / Scenic Landscapes (15 seconds)
    # 0-5s: Warm coral sunset sky
    # 5-10s: Deep forest green canopy
    # 10-15s: Deep ocean navy blue
    vid2_path = os.path.join(videos_dir, "scenic_events.mp4")
    cmd2 = [
        ffmpeg_exe,
        "-y",
        "-f",
        "lavfi",
        "-i",
        "color=c=coral:s=320x240:d=5",
        "-f",
        "lavfi",
        "-i",
        "color=c=darkgreen:s=320x240:d=5",
        "-f",
        "lavfi",
        "-i",
        "color=c=navy:s=320x240:d=5",
        "-filter_complex",
        "[0:v][1:v][2:v]concat=n=3:v=1:a=0[outv]",
        "-map",
        "[outv]",
        "-pix_fmt",
        "yuv420p",
        vid2_path,
    ]

    # Video 3: Everyday Objects & Activities (15 seconds)
    # 0-5s: Asphalt grey road with red car shape
    # 5-10s: Warm wood kitchen table with white mug shape
    # 10-15s: Green grass lawn with black silhouette
    vid3_path = os.path.join(videos_dir, "activity_events.mp4")
    cmd3 = [
        ffmpeg_exe,
        "-y",
        "-f",
        "lavfi",
        "-i",
        "color=c=dimgray:s=320x240:d=5,drawbox=x=80:y=120:w=120:h=45:color=red@1:t=fill",
        "-f",
        "lavfi",
        "-i",
        "color=c=burlywood:s=320x240:d=5,drawbox=x=130:y=90:w=50:h=60:color=white@1:t=fill",
        "-f",
        "lavfi",
        "-i",
        "color=c=forestgreen:s=320x240:d=5,drawbox=x=145:y=60:w=30:h=90:color=black@1:t=fill",
        "-filter_complex",
        "[0:v][1:v][2:v]concat=n=3:v=1:a=0[outv]",
        "-map",
        "[outv]",
        "-pix_fmt",
        "yuv420p",
        vid3_path,
    ]

    # Video 4: Monochrome & Grayscale Contrast (12 seconds)
    # 0-4s: Pitch dark black screen
    # 4-8s: Clean white screen
    # 8-12s: Neutral middle gray screen
    vid4_path = os.path.join(videos_dir, "monochrome_contrast.mp4")
    cmd4 = [
        ffmpeg_exe,
        "-y",
        "-f",
        "lavfi",
        "-i",
        "color=c=black:s=320x240:d=4",
        "-f",
        "lavfi",
        "-i",
        "color=c=white:s=320x240:d=4",
        "-f",
        "lavfi",
        "-i",
        "color=c=gray:s=320x240:d=4",
        "-filter_complex",
        "[0:v][1:v][2:v]concat=n=3:v=1:a=0[outv]",
        "-map",
        "[outv]",
        "-pix_fmt",
        "yuv420p",
        vid4_path,
    ]

    # Video 5: Vibrant Palette / Neon Tones (12 seconds)
    # 0-4s: Rich royal purple screen
    # 4-8s: Bright cyan turquoise screen
    # 8-12s: Bright sunny golden yellow screen
    vid5_path = os.path.join(videos_dir, "vibrant_palette.mp4")
    cmd5 = [
        ffmpeg_exe,
        "-y",
        "-f",
        "lavfi",
        "-i",
        "color=c=purple:s=320x240:d=4",
        "-f",
        "lavfi",
        "-i",
        "color=c=cyan:s=320x240:d=4",
        "-f",
        "lavfi",
        "-i",
        "color=c=gold:s=320x240:d=4",
        "-filter_complex",
        "[0:v][1:v][2:v]concat=n=3:v=1:a=0[outv]",
        "-map",
        "[outv]",
        "-pix_fmt",
        "yuv420p",
        vid5_path,
    ]

    # Video 6: Industrial & Architectural Materials (12 seconds)
    # 0-4s: Metallic silver aluminum screen
    # 4-8s: Brick red masonry screen
    # 8-12s: Midnight slate blue evening screen
    vid6_path = os.path.join(videos_dir, "industrial_materials.mp4")
    cmd6 = [
        ffmpeg_exe,
        "-y",
        "-f",
        "lavfi",
        "-i",
        "color=c=silver:s=320x240:d=4",
        "-f",
        "lavfi",
        "-i",
        "color=c=firebrick:s=320x240:d=4",
        "-f",
        "lavfi",
        "-i",
        "color=c=darkslateblue:s=320x240:d=4",
        "-filter_complex",
        "[0:v][1:v][2:v]concat=n=3:v=1:a=0[outv]",
        "-map",
        "[outv]",
        "-pix_fmt",
        "yuv420p",
        vid6_path,
    ]

    # Execute ffmpeg commands to build videos
    commands = [
        ("vid_shapes", cmd1, vid1_path),
        ("vid_scenic", cmd2, vid2_path),
        ("vid_activity", cmd3, vid3_path),
        ("vid_monochrome", cmd4, vid4_path),
        ("vid_vibrant", cmd5, vid5_path),
        ("vid_industrial", cmd6, vid6_path),
    ]

    for name, cmd, vpath in commands:
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"Failed to generate {name} ({vpath}):\n{res.stderr}")

    # Build annotations specification
    annotations: Dict[str, Any] = {
        "version": "1.0",
        "dataset_name": "PurpleDB M0 Synthetic Golden Benchmark",
        "description": "Deterministic benchmark suite generated via ffmpeg lavfi for retrieval evaluation",
        "videos": [
            {
                "video_id": "vid_shapes",
                "filename": "shapes_events.mp4",
                "duration_sec": 15.0,
                "segments": [
                    {"start_sec": 0.0, "end_sec": 5.0, "concept": "blue screen with white square"},
                    {"start_sec": 5.0, "end_sec": 10.0, "concept": "bright red screen with green square"},
                    {"start_sec": 10.0, "end_sec": 15.0, "concept": "yellow screen with black square"},
                ],
            },
            {
                "video_id": "vid_scenic",
                "filename": "scenic_events.mp4",
                "duration_sec": 15.0,
                "segments": [
                    {"start_sec": 0.0, "end_sec": 5.0, "concept": "coral orange sunset sky"},
                    {"start_sec": 5.0, "end_sec": 10.0, "concept": "dark forest green trees foliage"},
                    {"start_sec": 10.0, "end_sec": 15.0, "concept": "deep navy blue ocean waters"},
                ],
            },
            {
                "video_id": "vid_activity",
                "filename": "activity_events.mp4",
                "duration_sec": 15.0,
                "segments": [
                    {"start_sec": 0.0, "end_sec": 5.0, "concept": "red vehicle car on asphalt grey road"},
                    {"start_sec": 5.0, "end_sec": 10.0, "concept": "white ceramic coffee cup on wooden table"},
                    {"start_sec": 10.0, "end_sec": 15.0, "concept": "person silhouette standing on green lawn grass"},
                ],
            },
            {
                "video_id": "vid_monochrome",
                "filename": "monochrome_contrast.mp4",
                "duration_sec": 12.0,
                "segments": [
                    {"start_sec": 0.0, "end_sec": 4.0, "concept": "pitch dark black screen darkness"},
                    {"start_sec": 4.0, "end_sec": 8.0, "concept": "pure solid white bright screen"},
                    {"start_sec": 8.0, "end_sec": 12.0, "concept": "neutral gray background"},
                ],
            },
            {
                "video_id": "vid_vibrant",
                "filename": "vibrant_palette.mp4",
                "duration_sec": 12.0,
                "segments": [
                    {"start_sec": 0.0, "end_sec": 4.0, "concept": "rich royal purple violet background"},
                    {"start_sec": 4.0, "end_sec": 8.0, "concept": "bright cyan turquoise blue screen"},
                    {"start_sec": 8.0, "end_sec": 12.0, "concept": "bright golden yellow background"},
                ],
            },
            {
                "video_id": "vid_industrial",
                "filename": "industrial_materials.mp4",
                "duration_sec": 12.0,
                "segments": [
                    {"start_sec": 0.0, "end_sec": 4.0, "concept": "metallic silver aluminum surface"},
                    {"start_sec": 4.0, "end_sec": 8.0, "concept": "dark brick red masonry wall"},
                    {"start_sec": 8.0, "end_sec": 12.0, "concept": "midnight slate blue night"},
                ],
            },
        ],
        "queries": [
            # Queries for vid_shapes
            {
                "query_id": "q01",
                "query_text": "a blue screen with a white square in the center",
                "target_video_id": "vid_shapes",
                "expected_interval": {"start_sec": 0.0, "end_sec": 5.0},
                "tolerance_sec": 1.5,
            },
            {
                "query_id": "q02",
                "query_text": "bright red background with a green square",
                "target_video_id": "vid_shapes",
                "expected_interval": {"start_sec": 5.0, "end_sec": 10.0},
                "tolerance_sec": 1.5,
            },
            {
                "query_id": "q03",
                "query_text": "bright yellow screen with a black box",
                "target_video_id": "vid_shapes",
                "expected_interval": {"start_sec": 10.0, "end_sec": 15.0},
                "tolerance_sec": 1.5,
            },
            # Queries for vid_scenic
            {
                "query_id": "q04",
                "query_text": "warm coral sunset evening sky glow",
                "target_video_id": "vid_scenic",
                "expected_interval": {"start_sec": 0.0, "end_sec": 5.0},
                "tolerance_sec": 1.5,
            },
            {
                "query_id": "q05",
                "query_text": "deep dark green forest foliage and trees",
                "target_video_id": "vid_scenic",
                "expected_interval": {"start_sec": 5.0, "end_sec": 10.0},
                "tolerance_sec": 1.5,
            },
            {
                "query_id": "q06",
                "query_text": "deep dark navy blue ocean water",
                "target_video_id": "vid_scenic",
                "expected_interval": {"start_sec": 10.0, "end_sec": 15.0},
                "tolerance_sec": 1.5,
            },
            # Queries for vid_activity
            {
                "query_id": "q07",
                "query_text": "red car driving on asphalt grey road",
                "target_video_id": "vid_activity",
                "expected_interval": {"start_sec": 0.0, "end_sec": 5.0},
                "tolerance_sec": 1.5,
            },
            {
                "query_id": "q08",
                "query_text": "white coffee mug on wooden table",
                "target_video_id": "vid_activity",
                "expected_interval": {"start_sec": 5.0, "end_sec": 10.0},
                "tolerance_sec": 1.5,
            },
            {
                "query_id": "q09",
                "query_text": "person standing outdoors on green lawn grass",
                "target_video_id": "vid_activity",
                "expected_interval": {"start_sec": 10.0, "end_sec": 15.0},
                "tolerance_sec": 1.5,
            },
            # Queries for vid_monochrome
            {
                "query_id": "q10",
                "query_text": "completely pitch black dark screen",
                "target_video_id": "vid_monochrome",
                "expected_interval": {"start_sec": 0.0, "end_sec": 4.0},
                "tolerance_sec": 1.5,
            },
            {
                "query_id": "q11",
                "query_text": "pure solid white blank screen",
                "target_video_id": "vid_monochrome",
                "expected_interval": {"start_sec": 4.0, "end_sec": 8.0},
                "tolerance_sec": 1.5,
            },
            {
                "query_id": "q12",
                "query_text": "neutral medium gray background",
                "target_video_id": "vid_monochrome",
                "expected_interval": {"start_sec": 8.0, "end_sec": 12.0},
                "tolerance_sec": 1.5,
            },
            # Queries for vid_vibrant
            {
                "query_id": "q13",
                "query_text": "vibrant purple violet colored screen",
                "target_video_id": "vid_vibrant",
                "expected_interval": {"start_sec": 0.0, "end_sec": 4.0},
                "tolerance_sec": 1.5,
            },
            {
                "query_id": "q14",
                "query_text": "bright cyan turquoise blue background",
                "target_video_id": "vid_vibrant",
                "expected_interval": {"start_sec": 4.0, "end_sec": 8.0},
                "tolerance_sec": 1.5,
            },
            {
                "query_id": "q15",
                "query_text": "bright sunny golden yellow background",
                "target_video_id": "vid_vibrant",
                "expected_interval": {"start_sec": 8.0, "end_sec": 12.0},
                "tolerance_sec": 1.5,
            },
            # Queries for vid_industrial
            {
                "query_id": "q16",
                "query_text": "metallic silver aluminum metal surface",
                "target_video_id": "vid_industrial",
                "expected_interval": {"start_sec": 0.0, "end_sec": 4.0},
                "tolerance_sec": 1.5,
            },
            {
                "query_id": "q17",
                "query_text": "brick red masonry wall surface",
                "target_video_id": "vid_industrial",
                "expected_interval": {"start_sec": 4.0, "end_sec": 8.0},
                "tolerance_sec": 1.5,
            },
            {
                "query_id": "q18",
                "query_text": "midnight dark slate blue night",
                "target_video_id": "vid_industrial",
                "expected_interval": {"start_sec": 8.0, "end_sec": 12.0},
                "tolerance_sec": 1.5,
            },
        ],
    }

    annotations_file = os.path.join(output_dir, "annotations.json")
    with open(annotations_file, "w", encoding="utf-8") as f:
        json.dump(annotations, f, indent=2)

    return annotations


if __name__ == "__main__":
    current_dir = os.path.dirname(os.path.abspath(__file__))
    print(f"Generating synthetic benchmark dataset in {current_dir}...")
    data = generate_benchmark_dataset(current_dir)
    print(f"Generated {len(data['videos'])} videos and {len(data['queries'])} annotated queries.")
