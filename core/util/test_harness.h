#pragma once

#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace purpledb {
namespace test {

class TestRunner {
 public:
  using TestFunc = void (*)();
  struct TestCase {
    std::string suite;
    std::string name;
    TestFunc func;
  };

  static TestRunner& Instance() {
    static TestRunner runner;
    return runner;
  }

  void Register(const std::string& suite, const std::string& name, TestFunc func) {
    tests_.push_back({suite, name, func});
  }

  int RunAll(const std::string& filter = "") {
    int passed = 0;
    int failed = 0;
    std::vector<TestCase> to_run;
    for (const auto& t : tests_) {
      std::string full_name = t.suite + "." + t.name;
      if (filter.empty() || full_name.find(filter) != std::string::npos ||
          t.suite.find(filter) != std::string::npos) {
        to_run.push_back(t);
      }
    }

    std::cout << "[==========] Running " << to_run.size() << " test(s)." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    for (const auto& t : to_run) {
      std::cout << "[ RUN      ] " << t.suite << "." << t.name << std::endl;
      current_failed_ = false;
      current_failure_msg_.clear();
      try {
        t.func();
      } catch (const std::exception& e) {
        current_failed_ = true;
        current_failure_msg_ = std::string("Unexpected exception: ") + e.what();
      } catch (...) {
        current_failed_ = true;
        current_failure_msg_ = "Unexpected non-standard exception.";
      }

      if (current_failed_) {
        std::cout << "[  FAILED  ] " << t.suite << "." << t.name << std::endl;
        if (!current_failure_msg_.empty()) {
          std::cout << "             " << current_failure_msg_ << std::endl;
        }
        failed++;
      } else {
        std::cout << "[       OK ] " << t.suite << "." << t.name << std::endl;
        passed++;
      }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end - start;
    std::cout << "[==========] " << to_run.size() << " test(s) ran ("
              << elapsed.count() << " ms total).\n";
    std::cout << "[  PASSED  ] " << passed << " test(s).\n";
    if (failed > 0) {
      std::cout << "[  FAILED  ] " << failed << " test(s).\n";
      return 1;
    }
    return 0;
  }

  void Fail(const std::string& msg) {
    current_failed_ = true;
    current_failure_msg_ = msg;
  }

  bool Failed() const { return current_failed_; }

 private:
  std::vector<TestCase> tests_;
  bool current_failed_{false};
  std::string current_failure_msg_;
};

struct TestRegistrar {
  TestRegistrar(const std::string& suite, const std::string& name, TestRunner::TestFunc func) {
    TestRunner::Instance().Register(suite, name, func);
  }
};

}  // namespace test
}  // namespace purpledb

#define TEST(suite, name) \
  void suite##_##name##_Test(); \
  static ::purpledb::test::TestRegistrar suite##_##name##_Registrar( \
      #suite, #name, suite##_##name##_Test); \
  void suite##_##name##_Test()

#define ASSERT_TRUE(c) \
  do { \
    if (!(c)) { \
      std::ostringstream ss; \
      ss << __FILE__ << ":" << __LINE__ << ": Assertion failed: " #c; \
      ::purpledb::test::TestRunner::Instance().Fail(ss.str()); \
      return; \
    } \
  } while (0)

#define ASSERT_FALSE(c) ASSERT_TRUE(!(c))

#define ASSERT_EQ(a, b) \
  do { \
    if (!((a) == (b))) { \
      std::ostringstream ss; \
      ss << __FILE__ << ":" << __LINE__ << ": Expected (" #a " == " #b "), but (" \
         << (a) << " != " << (b) << ")"; \
      ::purpledb::test::TestRunner::Instance().Fail(ss.str()); \
      return; \
    } \
  } while (0)

#define ASSERT_NE(a, b) \
  do { \
    if ((a) == (b)) { \
      std::ostringstream ss; \
      ss << __FILE__ << ":" << __LINE__ << ": Expected (" #a " != " #b "), but both equal " \
         << (a); \
      ::purpledb::test::TestRunner::Instance().Fail(ss.str()); \
      return; \
    } \
  } while (0)

#define ASSERT_LT(a, b) \
  do { \
    if (!((a) < (b))) { \
      std::ostringstream ss; \
      ss << __FILE__ << ":" << __LINE__ << ": Expected (" #a " < " #b "), but (" \
         << (a) << " >= " << (b) << ")"; \
      ::purpledb::test::TestRunner::Instance().Fail(ss.str()); \
      return; \
    } \
  } while (0)

#define ASSERT_LE(a, b) \
  do { \
    if (!((a) <= (b))) { \
      std::ostringstream ss; \
      ss << __FILE__ << ":" << __LINE__ << ": Expected (" #a " <= " #b "), but (" \
         << (a) << " > " << (b) << ")"; \
      ::purpledb::test::TestRunner::Instance().Fail(ss.str()); \
      return; \
    } \
  } while (0)

#define ASSERT_GT(a, b) \
  do { \
    if (!((a) > (b))) { \
      std::ostringstream ss; \
      ss << __FILE__ << ":" << __LINE__ << ": Expected (" #a " > " #b "), but (" \
         << (a) << " <= " << (b) << ")"; \
      ::purpledb::test::TestRunner::Instance().Fail(ss.str()); \
      return; \
    } \
  } while (0)

#define ASSERT_GE(a, b) \
  do { \
    if (!((a) >= (b))) { \
      std::ostringstream ss; \
      ss << __FILE__ << ":" << __LINE__ << ": Expected (" #a " >= " #b "), but (" \
         << (a) << " < " << (b) << ")"; \
      ::purpledb::test::TestRunner::Instance().Fail(ss.str()); \
      return; \
    } \
  } while (0)

#define ASSERT_NEAR(a, b, eps) \
  do { \
    double _diff = std::fabs(static_cast<double>(a) - static_cast<double>(b)); \
    if (_diff > static_cast<double>(eps)) { \
      std::ostringstream ss; \
      ss << __FILE__ << ":" << __LINE__ << ": Expected (" #a " approx " #b " within " #eps "), but diff was " \
         << _diff << " (" << (a) << " vs " << (b) << ")"; \
      ::purpledb::test::TestRunner::Instance().Fail(ss.str()); \
      return; \
    } \
  } while (0)

#define ASSERT_STATUS_OK(s) \
  do { \
    auto _status = (s); \
    if (!_status.ok()) { \
      std::ostringstream ss; \
      ss << __FILE__ << ":" << __LINE__ << ": Expected OK status but got: " \
         << _status.ToString(); \
      ::purpledb::test::TestRunner::Instance().Fail(ss.str()); \
      return; \
    } \
  } while (0)
