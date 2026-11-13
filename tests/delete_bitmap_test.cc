#include "core/vector/delete_bitmap.h"

#include <vector>
#include "util/test_harness.h"

namespace purpledb {
namespace test {

TEST(DeleteBitmapTest, BasicOperations) {
  DeleteBitmap bm;
  ASSERT_EQ(bm.DeletedCount(), 0u);
  ASSERT_EQ(bm.TotalCount(), 0u);
  ASSERT_FALSE(bm.IsDeleted(0));
  ASSERT_FALSE(bm.IsDeleted(42));

  // Mark vector 42 as deleted
  bm.MarkDeleted(42);
  ASSERT_TRUE(bm.IsDeleted(42));
  ASSERT_FALSE(bm.IsDeleted(41));
  ASSERT_FALSE(bm.IsDeleted(43));
  ASSERT_EQ(bm.DeletedCount(), 1u);
  ASSERT_EQ(bm.TotalCount(), 43u);

  // Redundant mark does not double count
  bm.MarkDeleted(42);
  ASSERT_EQ(bm.DeletedCount(), 1u);

  // Clear deleted
  bm.ClearDeleted(42);
  ASSERT_FALSE(bm.IsDeleted(42));
  ASSERT_EQ(bm.DeletedCount(), 0u);
}

TEST(DeleteBitmapTest, MultiWordSpanning) {
  DeleteBitmap bm;
  std::vector<uint64_t> ids = {0, 63, 64, 127, 128, 511, 1023, 4096, 65535};

  for (uint64_t id : ids) {
    ASSERT_FALSE(bm.IsDeleted(id));
    bm.MarkDeleted(id);
    ASSERT_TRUE(bm.IsDeleted(id));
  }

  ASSERT_EQ(bm.DeletedCount(), ids.size());
  ASSERT_EQ(bm.TotalCount(), 65536u);

  // Check random un-deleted IDs
  ASSERT_FALSE(bm.IsDeleted(1));
  ASSERT_FALSE(bm.IsDeleted(62));
  ASSERT_FALSE(bm.IsDeleted(65));
  ASSERT_FALSE(bm.IsDeleted(4095));

  // Deleted fraction
  bm.SetTotalCount(100000);
  ASSERT_NEAR(bm.DeletedFraction(), static_cast<double>(ids.size()) / 100000.0, 1e-6);
}

TEST(DeleteBitmapTest, SerializationAndCorruption) {
  DeleteBitmap bm;
  bm.SetTotalCount(1000);
  bm.MarkDeleted(10);
  bm.MarkDeleted(77);
  bm.MarkDeleted(500);
  bm.MarkDeleted(999);

  std::string serialized = bm.Serialize();
  ASSERT_GT(serialized.size(), 36u);

  DeleteBitmap recovered;
  Status s = recovered.Deserialize(serialized);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(recovered.TotalCount(), 1000u);
  ASSERT_EQ(recovered.DeletedCount(), 4u);
  ASSERT_TRUE(recovered.IsDeleted(10));
  ASSERT_TRUE(recovered.IsDeleted(77));
  ASSERT_TRUE(recovered.IsDeleted(500));
  ASSERT_TRUE(recovered.IsDeleted(999));
  ASSERT_FALSE(recovered.IsDeleted(0));
  ASSERT_FALSE(recovered.IsDeleted(11));

  // Corruption: flip bit in serialized buffer
  std::string corrupt = serialized;
  corrupt[15] ^= 0x55;
  DeleteBitmap corrupt_bm;
  Status s_corrupt = corrupt_bm.Deserialize(corrupt);
  ASSERT_FALSE(s_corrupt.ok());

  // Truncated buffer
  std::string truncated = serialized.substr(0, 20);
  DeleteBitmap trunc_bm;
  Status s_trunc = trunc_bm.Deserialize(truncated);
  ASSERT_FALSE(s_trunc.ok());
}

}  // namespace test
}  // namespace purpledb

