#include "util/test_harness.h"

int main() {
  return ::purpledb::test::TestRunner::Instance().RunAll();
}

