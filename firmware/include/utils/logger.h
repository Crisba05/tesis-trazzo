// include/utils/logger.h
// Log level macros — thin wrapper around Serial.printf with a compile-time
// filter. Reduces log noise in production without changing call sites.
//
// Usage:
//   LOG_INFO ("hb", "heartbeat OK");
//   LOG_WARN ("scan", "duplicate ignored: %s", code);
//   LOG_ERR  ("http", "POST failed %d: %s", status, body);
//   LOG_DEBUG("cache", "page +%d records (total=%d)", n, total);
//
// Level values (lower = more critical):
//   0 = OFF, 1 = ERR, 2 = WARN, 3 = INFO (default), 4 = DEBUG
//
// Override with -DTRAZZO_LOG_LEVEL=<n> in platformio.ini per env.
#pragma once

#include <Arduino.h>

#ifndef TRAZZO_LOG_LEVEL
#define TRAZZO_LOG_LEVEL 3   // INFO by default
#endif

#define LOG_LEVEL_OFF   0
#define LOG_LEVEL_ERR   1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_INFO  3
#define LOG_LEVEL_DEBUG 4

#define LOG_PRINT_(prefix, tag, fmt, ...) \
    do { Serial.printf("[" prefix "][%s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#if TRAZZO_LOG_LEVEL >= LOG_LEVEL_ERR
    #define LOG_ERR(tag, fmt, ...)  LOG_PRINT_("ERR",  tag, fmt, ##__VA_ARGS__)
#else
    #define LOG_ERR(tag, fmt, ...)  do {} while (0)
#endif

#if TRAZZO_LOG_LEVEL >= LOG_LEVEL_WARN
    #define LOG_WARN(tag, fmt, ...) LOG_PRINT_("WARN", tag, fmt, ##__VA_ARGS__)
#else
    #define LOG_WARN(tag, fmt, ...) do {} while (0)
#endif

#if TRAZZO_LOG_LEVEL >= LOG_LEVEL_INFO
    #define LOG_INFO(tag, fmt, ...) LOG_PRINT_("INFO", tag, fmt, ##__VA_ARGS__)
#else
    #define LOG_INFO(tag, fmt, ...) do {} while (0)
#endif

#if TRAZZO_LOG_LEVEL >= LOG_LEVEL_DEBUG
    #define LOG_DEBUG(tag, fmt, ...) LOG_PRINT_("DBG",  tag, fmt, ##__VA_ARGS__)
#else
    #define LOG_DEBUG(tag, fmt, ...) do {} while (0)
#endif
