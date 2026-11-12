#include "util/logging.h"
#include "util/test_harness.h"

#include <atomic>
#include <thread>
#include <vector>

namespace purpledb {

struct CapturedLog {
  LogLevel level;
  std::string file;
  int line;
  std::string message;
};

TEST(LoggingTest, LevelFilteringAndSinkCapture) {
  std::vector<CapturedLog> logs;
  SetLogSink([&logs](LogLevel lvl, const char* file, int line, const std::string& msg) {
    logs.push_back({lvl, file, line, msg});
  });

  SetLogLevel(LogLevel::kWarn);

  PURPLE_LOG(Debug, "this should be ignored");
  PURPLE_LOG(Info, "this should also be ignored");
  PURPLE_LOG(Warn, "warning: %d items", 42);
  PURPLE_LOG(Error, "critical error in %s", "memtable");

  ASSERT_EQ(logs.size(), 2u);
  ASSERT_EQ(logs[0].level, LogLevel::kWarn);
  ASSERT_EQ(logs[0].message, "warning: 42 items");
  ASSERT_EQ(logs[1].level, LogLevel::kError);
  ASSERT_EQ(logs[1].message, "critical error in memtable");

  ResetLogSink();
  SetLogLevel(LogLevel::kInfo);
}

TEST(LoggingTest, LargeMessageDynamicBuffer) {
  std::string captured;
  SetLogSink([&captured](LogLevel, const char*, int, const std::string& msg) {
    captured = msg;
  });

  std::string huge(2500, 'x');
  PURPLE_LOG(Info, "Prefix: %s", huge.c_str());

  ASSERT_EQ(captured.size(), 2500u + 8u);
  ASSERT_EQ(captured.substr(0, 8), "Prefix: ");
  ASSERT_EQ(captured.substr(8), huge);

  ResetLogSink();
}

TEST(LoggingTest, ConcurrentLoggingThreadSafety) {
  std::atomic<int> log_count{0};
  SetLogSink([&log_count](LogLevel, const char*, int, const std::string&) {
    log_count.fetch_add(1, std::memory_order_relaxed);
  });

  SetLogLevel(LogLevel::kDebug);

  const int kThreads = 4;
  const int kLogsPerThread = 250;
  std::vector<std::thread> threads;

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t]() {
      for (int i = 0; i < kLogsPerThread; ++i) {
        PURPLE_LOG(Info, "Thread %d iteration %d", t, i);
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  ASSERT_EQ(log_count.load(), kThreads * kLogsPerThread);

  ResetLogSink();
  SetLogLevel(LogLevel::kInfo);
}

}  // namespace purpledb
