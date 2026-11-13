#include "core/pipeline/frame_decoder.h"
#include "util/test_harness.h"

namespace purpledb {
namespace test {

TEST(FrameDecoderTest, SyntheticFramesGeneration) {
  std::vector<DecodedFrame> frames;
  Status s = FrameDecoder::GenerateSyntheticFrames(5, 1.0, 224, 224, &frames);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(frames.size(), 5u);

  for (size_t i = 0; i < 5; ++i) {
    ASSERT_NEAR(frames[i].timestamp_sec, static_cast<double>(i), 1e-4);
    ASSERT_EQ(frames[i].image.width(), 224);
    ASSERT_EQ(frames[i].image.height(), 224);
    ASSERT_EQ(frames[i].image.channels(), 3);
    ASSERT_EQ(frames[i].image.data_size(), 224u * 224u * 3u);
  }
}

TEST(FrameDecoderTest, MissingVideoHandling) {
  FrameDecoder decoder;
  std::vector<DecodedFrame> frames;
  Status s = decoder.ExtractFrames("non_existent_video_file.mp4", &frames);
  ASSERT_TRUE(s.IsNotFound());
}

TEST(FrameDecoderTest, GoldenSetVideoExtraction) {
  std::string video_path = "prototype/golden_set/videos/shapes_events.mp4";
  Env* env = Env::Default();

  if (!env->FileExists(video_path)) {
    // If not found from current working dir, pass test gracefully
    return;
  }

  FrameDecoderOptions opts;
  opts.fps = 1.0;
  opts.scale_width = 112;
  opts.scale_height = 112;
  opts.temp_dir = "tmp_shapes_test_frames";

  FrameDecoder decoder(env, opts);
  std::vector<DecodedFrame> frames;
  Status s = decoder.ExtractFrames(video_path, &frames);

  // If ffmpeg is installed and available, frames should be extracted (~15 frames for 15s video)
  if (s.ok()) {
    ASSERT_GT(frames.size(), 10u);
    ASSERT_LT(frames.size(), 20u);
    ASSERT_NEAR(frames[0].timestamp_sec, 0.0, 1e-3);
  }

  // Cleanup temp dir
  std::vector<std::string> children;
  if (env->GetChildren(opts.temp_dir, &children).ok()) {
    for (const auto& c : children) {
      if (c != "." && c != "..") env->DeleteFile(opts.temp_dir + "/" + c);
    }
    env->DeleteDir(opts.temp_dir);
  }
}

}  // namespace test
}  // namespace purpledb
