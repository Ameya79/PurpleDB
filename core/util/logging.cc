#include "util/logging.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

namespace purpledb {

namespace {

std::mutex g_log_mutex;
LogLevel g_log_level = LogLevel::kInfo;
LogSink g_log_sink = nullptr;

const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo:  return "INFO ";
    case LogLevel::kWarn:  return "WARN ";
    case LogLevel::kError: return "ERROR";
    case LogLevel::kFatal: return "FATAL";
  }
  return "UNKNOWN";
}

std::string CurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf;
#if defined(_WIN32)
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif

  std::ostringstream ss;
  ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
     << '.' << std::setfill('0') << std::setw(3) << ms.count();
  return ss.str();
}

const char* StripPath(const char* file) {
  const char* p = file;
  const char* last = file;
  while (*p) {
    if (*p == '/' || *p == '\\') {
      last = p + 1;
    }
    p++;
  }
  return last;
}

}  // namespace

void SetLogLevel(LogLevel level) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  g_log_level = level;
}

LogLevel GetLogLevel() {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  return g_log_level;
}

const char* LogLevelToString(LogLevel level) {
  return LevelName(level);
}

std::ostream& operator<<(std::ostream& os, LogLevel level) {
  os << LevelName(level);
  return os;
}

void SetLogSink(LogSink sink) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  g_log_sink = std::move(sink);
}

void ResetLogSink() {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  g_log_sink = nullptr;
}

void LogMessage(LogLevel level, const char* file, int line, const char* format, ...) {
  char buf[1024];
  va_list args;
  va_start(args, format);
  int n = std::vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);

  std::string msg;
  if (n >= 0 && static_cast<size_t>(n) < sizeof(buf)) {
    msg = std::string(buf, n);
  } else {
    // Dynamic fallback for unusually large log messages
    std::vector<char> dyn(static_cast<size_t>(n + 1));
    va_start(args, format);
    std::vsnprintf(dyn.data(), dyn.size(), format, args);
    va_end(args);
    msg = std::string(dyn.data(), n);
  }

  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (g_log_sink) {
    g_log_sink(level, StripPath(file), line, msg);
    return;
  }

  std::string ts = CurrentTimestamp();
  std::fprintf(stderr, "[%s] [%s] [%s:%d] %s\n",
               ts.c_str(), LevelName(level), StripPath(file), line, msg.c_str());
  std::fflush(stderr);

  if (level == LogLevel::kFatal) {
    std::abort();
  }
}

}  // namespace purpledb
