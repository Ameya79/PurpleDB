#include "util/test_harness.h"

int main(int argc, char** argv) {
  std::string filter = (argc > 1) ? argv[1] : "";
  return ::purpledb::test::TestRunner::Instance().RunAll(filter);
}
