"""Video frame extraction module using ffmpeg via subprocess.

Extracts frames at a specified frame rate (default 1 fps) and downscales them
to model input dimensions (default 224x224), returning timestamps and PIL Images.
Temporary files are written to an isolated system temp directory to prevent
OneDrive sync churn.
"""

import os
import subprocess
import tempfile
from typing import List, Tuple
from PIL import Image
import imageio_ffmpeg


def get_ffmpeg_path() -> str:
    """Resolve the path to the ffmpeg executable."""
    exe_path = imageio_ffmpeg.get_ffmpeg_exe()
    if not exe_path or not os.path.exists(exe_path):
        raise RuntimeError("FFmpeg executable not found via imageio_ffmpeg.")
    return exe_path


def sample_frames(
    video_path: str,
    fps: float = 1.0,
    scale: Tuple[int, int] = (224, 224),
) -> List[Tuple[float, Image.Image]]:
    """Sample frames from a video file at a fixed sampling rate and resolution.

    Args:
        video_path: Path to the input video file.
        fps: Sampling rate in frames per second (e.g. 1.0 = one frame per second).
        scale: Target (width, height) resolution for downsampling.

    Returns:
        List of tuples: (timestamp_sec, PIL.Image.Image in RGB).

    Raises:
        FileNotFoundError: If the video_path does not exist.
        RuntimeError: If ffmpeg fails during extraction.
    """
    if fps <= 0:
        raise ValueError(f"FPS must be positive, got {fps}")

    if not os.path.isfile(video_path):
        raise FileNotFoundError(f"Video file not found: {video_path}")

    ffmpeg_exe = get_ffmpeg_path()

    frames: List[Tuple[float, Image.Image]] = []

    # Use a system temp directory outside the workspace / OneDrive folder
    with tempfile.TemporaryDirectory(prefix="purpledb_frames_") as tmp_dir:
        output_pattern = os.path.join(tmp_dir, "frame_%06d.jpg")

        # ffmpeg filter: fps downsampling and bicubic resizing
        vf_filter = f"fps={fps},scale={scale[0]}:{scale[1]}"

        cmd = [
            ffmpeg_exe,
            "-y",  # overwrite output files
            "-i",
            os.path.abspath(video_path),
            "-vf",
            vf_filter,
            "-q:v",
            "2",  # high JPEG quality
            "-loglevel",
            "error",
            "-nostats",
            output_pattern,
        ]

        result = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        if result.returncode != 0:
            raise RuntimeError(
                f"FFmpeg execution failed (code {result.returncode}):\n{result.stderr}"
            )

        # Collect and sort generated frame files
        frame_files = sorted(
            f for f in os.listdir(tmp_dir) if f.startswith("frame_") and f.endswith(".jpg")
        )

        # Map each extracted frame index to its physical timestamp in seconds
        interval_sec = 1.0 / fps
        for idx, filename in enumerate(frame_files):
            timestamp_sec = round(idx * interval_sec, 4)
            file_path = os.path.join(tmp_dir, filename)

            # Open, ensure RGB conversion, and copy to detach from open file handle
            with Image.open(file_path) as img:
                rgb_img = img.convert("RGB")
                # Force loading pixel data before closing file handle (critical for Windows)
                rgb_img.load()
                frames.append((timestamp_sec, rgb_img))

    return frames


def get_video_duration(video_path: str) -> float:
    """Estimate video duration in seconds using ffmpeg probe output."""
    if not os.path.isfile(video_path):
        raise FileNotFoundError(f"Video file not found: {video_path}")

    ffmpeg_exe = get_ffmpeg_path()
    cmd = [
        ffmpeg_exe,
        "-i",
        os.path.abspath(video_path),
        "-hide_banner",
    ]

    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    # Search for "Duration: 00:00:15.00" in stderr
    for line in result.stderr.splitlines():
        if "Duration:" in line:
            parts = line.split("Duration:")[1].split(",")[0].strip()
            time_parts = parts.split(":")
            if len(time_parts) == 3:
                hours = float(time_parts[0])
                mins = float(time_parts[1])
                secs = float(time_parts[2])
                return hours * 3600.0 + mins * 60.0 + secs

    return 0.0
