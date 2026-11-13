#include "core/pipeline/moment_detector.h"
#include "core/vector/distance.h"
#include "util/test_harness.h"

#include <vector>

namespace purpledb {
namespace test {

TEST(MomentDetectorTest, EmptyAndSingleFrame) {
  MomentDetector detector;
  std::vector<Moment> moments;

  // 0 frames
  Status s = detector.DetectMoments("vid1", {}, {}, {}, 16, &moments);
  ASSERT_STATUS_OK(s);
  ASSERT_TRUE(moments.empty());

  // 1 frame
  std::vector<double> timestamps = {0.0};
  std::vector<ImageRef> frames = {ImageRef::FromFile("f0.jpg")};
  std::vector<float> emb(16, 0.0f);
  emb[0] = 1.0f;

  s = detector.DetectMoments("vid1", timestamps, frames, emb, 16, &moments);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(moments.size(), 1u);
  ASSERT_EQ(moments[0].video_id, "vid1");
  ASSERT_EQ(moments[0].start_sec, 0.0);
  ASSERT_EQ(moments[0].end_sec, 1.0);
  ASSERT_EQ(moments[0].medoid_frame_index, 0);
}

TEST(MomentDetectorTest, SceneTransitionBoundaryCut) {
  MomentDetector detector;
  detector.SetThreshold(0.90);

  const size_t dim = 8;
  // Scene A: 3 frames (timestamps 0.0, 1.0, 2.0) pointing along dimension 0
  // Scene B: 3 frames (timestamps 3.0, 4.0, 5.0) pointing along dimension 1
  std::vector<double> timestamps;
  std::vector<ImageRef> frames;
  std::vector<float> embeddings;

  for (size_t i = 0; i < 6; ++i) {
    timestamps.push_back(static_cast<double>(i));
    frames.push_back(ImageRef::FromFile("frame_" + std::to_string(i) + ".jpg"));

    std::vector<float> vec(dim, 0.0f);
    if (i < 3) {
      vec[0] = 1.0f;  // Scene A
    } else {
      vec[1] = 1.0f;  // Scene B (orthogonal to Scene A, similarity = 0.0 < 0.90)
    }
    embeddings.insert(embeddings.end(), vec.begin(), vec.end());
  }

  std::vector<Moment> moments;
  Status s = detector.DetectMoments("vid_scenes", timestamps, frames, embeddings, dim, &moments);
  ASSERT_STATUS_OK(s);

  // Must detect exactly 2 moments
  ASSERT_EQ(moments.size(), 2u);

  // Moment 0: 0.0 to 3.0 sec
  ASSERT_EQ(moments[0].moment_id, "m_0");
  ASSERT_EQ(moments[0].start_sec, 0.0);
  ASSERT_EQ(moments[0].end_sec, 3.0);
  ASSERT_NEAR(moments[0].embedding[0], 1.0f, 1e-4f);

  // Moment 1: 3.0 to 6.0 sec
  ASSERT_EQ(moments[1].moment_id, "m_1");
  ASSERT_EQ(moments[1].start_sec, 3.0);
  ASSERT_EQ(moments[1].end_sec, 6.0);
  ASSERT_NEAR(moments[1].embedding[1], 1.0f, 1e-4f);
}

TEST(MomentDetectorTest, MedoidThumbnailSelection) {
  MomentDetector detector;
  const size_t dim = 4;

  std::vector<double> timestamps = {0.0, 1.0, 2.0};
  std::vector<ImageRef> frames = {
    ImageRef::FromFile("f0.jpg"),
    ImageRef::FromFile("f1.jpg"),
    ImageRef::FromFile("f2.jpg")
  };

  // Three vectors in the same scene:
  // f0: [0.99, +0.03, 0, 0]
  // f1: [1.00,  0.00, 0, 0] -> exact center / medoid
  // f2: [0.99, -0.03, 0, 0]
  std::vector<float> f0 = {0.99f, 0.03f, 0.0f, 0.0f}; NormalizeVector(f0.data(), 4);
  std::vector<float> f1 = {1.00f, 0.00f, 0.0f, 0.0f}; NormalizeVector(f1.data(), 4);
  std::vector<float> f2 = {0.99f, -0.03f, 0.0f, 0.0f}; NormalizeVector(f2.data(), 4);

  std::vector<float> embeddings;
  embeddings.insert(embeddings.end(), f0.begin(), f0.end());
  embeddings.insert(embeddings.end(), f1.begin(), f1.end());
  embeddings.insert(embeddings.end(), f2.begin(), f2.end());

  std::vector<Moment> moments;
  Status s = detector.DetectMoments("vid", timestamps, frames, embeddings, dim, &moments);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(moments.size(), 1u);

  // f1 is closest to the mean centroid
  ASSERT_EQ(moments[0].medoid_frame_index, 1);
  ASSERT_EQ(moments[0].thumbnail.file_path(), "f1.jpg");
}

}  // namespace test
}  // namespace purpledb
