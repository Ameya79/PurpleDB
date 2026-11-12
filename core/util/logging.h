#pragma once

#include <cstdarg>
#include <functional>
#include <string>

namespace purpledb {

/**
 * @brief Thread-safe logging facility for PurpleDB.
 *
 * CONCEPT FOR LEARNERS:
 * Database engines require predictable, thread-safe logging that does not
 * interfere with runtime performance.
 * - Log levels (DEBUG, INFO, WARN, ERROR, FATAL) allow operators to silence
 *   verbose diagnostic messages in production.
 * - The PURPLE_LOG macro checks the log level BEFORE formatting strings,
 *   avoiding expensive snprintf or string allocations when messages are disabled.
 * - Custom sinks allow redirecting logs to files, monitoring pipelines, or
 *   capturing log output in automated unit tests.
 */

enum class LogLevel {
  kDebug = 0,
  kInfo = 1,
  kWarn = 2,
  kError = 3,
  kFatal = 4
};

void SetLogLevel(LogLevel level);
LogLevel GetLogLevel();

const char* LogLevelToString(LogLevel level);
std::ostream& operator<<(std::ostream& os, LogLevel level);

using LogSink = std::function<void(LogLevel level, const char* file, int line, const std::string& message)>;
void SetLogSink(LogSink sink);
void ResetLogSink();

void LogMessage(LogLevel level, const char* file, int line, const char* format, ...);

}  // namespace purpledb

#define PURPLE_LOG(level, ...) \
  do { \
    if (::purpledb::LogLevel::k##level >= ::purpledb::GetLogLevel()) { \
      ::purpledb::LogMessage(::purpledb::LogLevel::k##level, __FILE__, __LINE__, __VA_ARGS__); \
    } \
  } while (0)
