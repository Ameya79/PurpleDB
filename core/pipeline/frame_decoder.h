#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include "core/embed/embedder.h"
#include "core/storage/env.h"
#include "purpledb/status.h"

namespace purpledb {

struct DecodedFrame {
  double timestamp_sec{0.0};
  ImageRef image;
};

struct FrameDecoderOptions {
  std::string ffmpeg_binary;       // If empty, auto-resolves from PATH / environment
  double fps{1.0};                 // Sample 1 frame per second by default
  int scale_width{224};            // Standard CLIP ViT-B/32 vision input width
  int scale_height{224};           // Standard CLIP ViT-B/32 vision input height
  std::string temp_dir;            // Temporary directory for extracted frames
};

/**
 * @brief Subprocess FFmpeg Video Frame Decoder.
 *
 * Invokes the external `ffmpeg` binary in an isolated subprocess to decode
 * and downscale frames.
 *
 * SYSTEMS RESILIENCE (ADR-031):
 * Decoding untrusted video codecs in an isolated subprocess guarantees that
 * corrupted bitstreams or codec segfaults cannot crash the database engine.
 */
class FrameDecoder {
 public:
  explicit FrameDecoder(Env* env = Env::Default(),
                        const FrameDecoderOptions& options = FrameDecoderOptions{});
  ~FrameDecoder() = default;

  // Resolve path to the ffmpeg executable on this system
  std::string ResolveFFmpegPath() const;

  // Extract frames from video at configured fps and resolution
  Status ExtractFrames(const std::string& video_path,
                       std::vector<DecodedFrame>* out_frames) const;

  // Synthetic frame generator for tests and offline validation
  static Status GenerateSyntheticFrames(int count,
                                        double fps,
                                        int width,
                                        int height,
                                        std::vector<DecodedFrame>* out_frames);

 private:
  Env* env_;
  FrameDecoderOptions options_;
};

}  // namespace purpledb
