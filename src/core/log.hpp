// Minimal logging facade so the core stays free of AML/Android includes.
// The mod installs a sink in main.cpp (AML logger). Tests install a stdout sink.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>

namespace usa1to1 {
namespace core {

enum class LogLevel { Debug, Info, Warn, Error };

using LogSink = std::function<void(LogLevel, const std::string &)>;

class Log {
  public:
    static void SetSink(LogSink sink) {
        std::lock_guard<std::mutex> lock(Mutex());
        Sink() = std::move(sink);
    }

    static void SetTag(const std::string &tag) {
        std::lock_guard<std::mutex> lock(Mutex());
        Tag() = tag;
    }

    static void Print(LogLevel level, const char *fmt, ...) {
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        Emit(level, buf);
    }

    static void Emit(LogLevel level, const std::string &msg) {
        LogSink sink;
        {
            std::lock_guard<std::mutex> lock(Mutex());
            sink = Sink();
        }
        if (!sink)
            return;
        std::string line = Tag();
        if (!line.empty()) {
            line += ": ";
        }
        line += msg;
        sink(level, line);
    }

  private:
    static LogSink &Sink() {
        static LogSink sink;
        return sink;
    }
    static std::string &Tag() {
        static std::string tag = "USA1to1";
        return tag;
    }
    static std::mutex &Mutex() {
        static std::mutex m;
        return m;
    }
};

} // namespace core
} // namespace usa1to1

#define USA_LOG_I(...) ::usa1to1::core::Log::Print(::usa1to1::core::LogLevel::Info, __VA_ARGS__)
#define USA_LOG_W(...) ::usa1to1::core::Log::Print(::usa1to1::core::LogLevel::Warn, __VA_ARGS__)
#define USA_LOG_E(...) ::usa1to1::core::Log::Print(::usa1to1::core::LogLevel::Error, __VA_ARGS__)
#define USA_LOG_D(...) ::usa1to1::core::Log::Print(::usa1to1::core::LogLevel::Debug, __VA_ARGS__)
