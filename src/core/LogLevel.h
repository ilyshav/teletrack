#pragma once

#include <stdint.h>

enum class LogLevel : uint8_t {
  Info = 0,
  Warn = 1,
  Error = 2,
};

inline const char* logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Info:  return "INF";
    case LogLevel::Warn:  return "WRN";
    case LogLevel::Error: return "ERR";
  }
  return "???";
}
