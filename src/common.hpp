#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <vector>
#include <chrono>
#include <iostream>

namespace dl_turbo {

// DisplayLink USB Identifiers
constexpr uint16_t VENDOR_DISPLAYLINK = 0x17E9;

// Known DisplayLink Chip Product IDs
constexpr uint16_t PID_DL6950_THINKPAD_DOCK = 0x6015; // Ridge family (dual 4K60)
constexpr uint16_t PID_DL6950_GENERIC       = 0x6000;
constexpr uint16_t PID_DL5900_GENERIC       = 0x5000; // Navarro family
constexpr uint16_t PID_DL3900_GENERIC       = 0x4300; // Ella / Firefly family
constexpr uint16_t PID_DL195_GENERIC        = 0x0195; // DL-1x5 USB 2.0

// Pixel Formats (matching EVDI FourCC & DisplayLinkManager internal formats)
enum class PixelFormat : uint32_t {
    Unknown  = 0x00000000,
    XRGB8888 = 0x34325258, // 'XR24'
    ARGB8888 = 0x34325241, // 'AR24'
    BGRX8888 = 0x34324258, // 'XB24'
    ABGR8888 = 0x34324241, // 'AB24'
    YUV420   = 0x32315559, // 'YU12' (Planar YUV 4:2:0)
};

// Logging level
enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
};

inline LogLevel g_log_level = LogLevel::INFO;

inline void log_msg(LogLevel level, const char* prefix, const std::string& msg) {
    if (level < g_log_level) return;
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char time_buf[32];
    std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", std::localtime(&now));
    std::fprintf(stderr, "[%s] [%s] %s\n", time_buf, prefix, msg.c_str());
}

#define LOG_DEBUG(fmt, ...) do { \
    char buf[1024]; \
    std::snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); \
    dl_turbo::log_msg(dl_turbo::LogLevel::DEBUG, "DEBUG", buf); \
} while(0)

#define LOG_INFO(fmt, ...) do { \
    char buf[1024]; \
    std::snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); \
    dl_turbo::log_msg(dl_turbo::LogLevel::INFO, "INFO", buf); \
} while(0)

#define LOG_WARN(fmt, ...) do { \
    char buf[1024]; \
    std::snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); \
    dl_turbo::log_msg(dl_turbo::LogLevel::WARN, "WARN", buf); \
} while(0)

#define LOG_ERROR(fmt, ...) do { \
    char buf[1024]; \
    std::snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); \
    dl_turbo::log_msg(dl_turbo::LogLevel::ERROR, "ERROR", buf); \
} while(0)

} // namespace dl_turbo
