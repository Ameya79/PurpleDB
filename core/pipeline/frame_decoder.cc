#include "core/pipeline/frame_decoder.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace purpledb {

FrameDecoder::FrameDecoder(Env* env, const FrameDecoderOptions& options)
    : env_(env ? env : Env::Default()), options_(options) {}

std::string FrameDecoder::ResolveFFmpegPath() const {
  if (!options_.ffmpeg_binary.empty() && env_->FileExists(options_.ffmpeg_binary)) {
    return options_.ffmpeg_binary;
  }

  // Check environment variable
  const char* env_path = std::getenv("PURPLEDB_FFMPEG_PATH");
  if (env_path && env_->FileExists(env_path)) {
    return std::string(env_path);
  }

  // Check known locations
  std::vector<std::string> candidates = {
    "ffmpeg",
    "ffmpeg.exe",
    "/usr/bin/ffmpeg",
    "/usr/local/bin/ffmpeg",
    "/opt/homebrew/bin/ffmpeg",
    // Python imageio_ffmpeg common Windows installation path
    "C:/Users/vinay/AppData/Local/Python/pythoncore-3.14-64/Lib/site-packages/imageio_ffmpeg/binaries/ffmpeg-win-x86_64-v9.0.2.exe"
  };

  for (const auto& candidate : candidates) {
    if (env_->FileExists(candidate)) {
      return candidate;
    }
  }

  // Default to PATH lookup
#ifdef _WIN32
  return "ffmpeg.exe";
#else
  return "ffmpeg";
#endif
}

Status FrameDecoder::ExtractFrames(const std::string& video_path,
                                   std::vector<DecodedFrame>* out_frames) const {
  if (!out_frames) {
    return Status::InvalidArgument("out_frames cannot be nullptr");
  }
  out_frames->clear();

  if (!env_->FileExists(video_path)) {
    return Status::NotFound("Video file not found: " + video_path);
  }

  if (video_path.size() >= 6 && video_path.substr(video_path.size() - 6) == ".synth") {
    return GenerateSyntheticFrames(10, options_.fps, options_.scale_width, options_.scale_height, out_frames);
  }

  std::string ffmpeg_bin = ResolveFFmpegPath();

  // Create isolated temporary directory
  std::string tmp_dir = options_.temp_dir;
  if (tmp_dir.empty()) {
    tmp_dir = "tmp_purpledb_frames_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
  }
  env_->CreateDir(tmp_dir);

  // Construct FFmpeg command
  std::ostringstream cmd;
  cmd << "\"" << ffmpeg_bin << "\" -y -i \"" << video_path << "\" "
      << "-vf \"fps=" << options_.fps
      << ",scale=" << options_.scale_width << ":" << options_.scale_height << "\" "
      << "-q:v 2 -loglevel error -nostats \""
      << tmp_dir << "/frame_%06d.jpg\"";

  int ret = std::system(cmd.str().c_str());
  if (ret != 0) {
    // Attempt fallback: if external ffmpeg failed, try synthetic generation for testing
    // Clean up temp dir
    std::vector<std::string> children;
    env_->GetChildren(tmp_dir, &children);
    for (const auto& c : children) {
      if (c != "." && c != "..") env_->DeleteFile(tmp_dir + "/" + c);
    }
    env_->DeleteDir(tmp_dir);
    return Status::IOError("FFmpeg command failed with return code: " + std::to_string(ret));
  }

  // Read generated frame files
  std::vector<std::string> children;
  Status s = env_->GetChildren(tmp_dir, &children);
  if (!s.ok()) {
    env_->DeleteDir(tmp_dir);
    return s;
  }

  std::vector<std::string> frame_files;
  for (const auto& file : children) {
    if (file.find("frame_") == 0 && file.find(".jpg") != std::string::npos) {
      frame_files.push_back(file);
    }
  }
  std::sort(frame_files.begin(), frame_files.end());

  double interval = 1.0 / (options_.fps > 0 ? options_.fps : 1.0);
  for (size_t i = 0; i < frame_files.size(); ++i) {
    std::string full_path = tmp_dir + "/" + frame_files[i];
    DecodedFrame df;
    df.timestamp_sec = static_cast<double>(i) * interval;
    df.image = ImageRef::FromFile(full_path);
    out_frames->push_back(std::move(df));
  }

  return Status::OK();
}

Status FrameDecoder::GenerateSyntheticFrames(int count,
                                             double fps,
                                             int width,
                                             int height,
                                             std::vector<DecodedFrame>* out_frames) {
  if (!out_frames) {
    return Status::InvalidArgument("out_frames cannot be nullptr");
  }
  out_frames->clear();

  double interval = 1.0 / (fps > 0 ? fps : 1.0);
  size_t pixel_bytes = static_cast<size_t>(width * height * 3);

  for (int i = 0; i < count; ++i) {
    std::vector<uint8_t> rgb(pixel_bytes);
    // Draw synthetic chromatic gradient based on frame index
    uint8_t r = static_cast<uint8_t>((i * 50) % 256);
    uint8_t g = static_cast<uint8_t>((i * 30 + 100) % 256);
    uint8_t b = static_cast<uint8_t>((255 - i * 40) % 256);

    for (size_t p = 0; p < pixel_bytes; p += 3) {
      rgb[p + 0] = r;
      rgb[p + 1] = g;
      rgb[p + 2] = b;
    }

    DecodedFrame df;
    df.timestamp_sec = static_cast<double>(i) * interval;
    df.image = ImageRef::FromRgbMove(std::move(rgb), width, height, 3);
    out_frames->push_back(std::move(df));
  }

  return Status::OK();
}

}  // namespace purpledb
