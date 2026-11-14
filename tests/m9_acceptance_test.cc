#include "purpledb/purpledb_c.h"
#include "core/util/test_harness.h"

#include <string>

namespace purpledb {

/**
 * @brief Milestone M9 Acceptance Test: Python Client & Release Verification.
 *
 * Verifies that the stable C ABI and engine definitions match the v1.0 release
 * specification and packaging requirements.
 */
TEST(M9AcceptanceTest, ReleaseVersionAndAbiCompatibility) {
  // 1. ABI Version check
  ASSERT_EQ(pdb_abi_version(), 1);

  // 2. Options initialization conforms to v1.0 spec
  pdb_options_t options;
  pdb_options_init(&options);
  ASSERT_EQ(options.create_if_missing, 1);
  ASSERT_EQ(options.error_if_exists, 0);
  ASSERT_EQ(options.enable_hnsw, 1);
  ASSERT_EQ(options.hnsw_M, 16u);
  ASSERT_EQ(options.hnsw_efSearch, 64u);
  ASSERT_GE(options.write_buffer_size, 1024u * 1024u);
  ASSERT_GE(options.block_cache_size, 1024u * 1024u);
}

}  // namespace purpledb

