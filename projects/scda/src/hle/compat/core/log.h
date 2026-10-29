// Log macros of the SCDA-XBERecomp sources (hle/tgp), written to stderr with
// an [HLE] prefix like the rest of the runtime's diagnostics.
#pragma once
#include <cstdio>

#define HLE_LOG_(level, ...)                         \
    do {                                             \
        std::fprintf(stderr, "[HLE] " level " ");    \
        std::fprintf(stderr, __VA_ARGS__);           \
        std::fputc('\n', stderr);                    \
    } while (0)

#define LOGD(...) ((void)0)
#define LOGI(...) HLE_LOG_("I", __VA_ARGS__)
#define LOGW(...) HLE_LOG_("W", __VA_ARGS__)
#define LOGE(...) HLE_LOG_("E", __VA_ARGS__)
#define LOGR(...) HLE_LOG_("R", __VA_ARGS__)

// the first n occurrences of a message
#define LOG_FIRST(n, ...)                 \
    do {                                  \
        static int hle_log_first_ = 0;    \
        if (hle_log_first_ < (n)) {       \
            hle_log_first_++;             \
            HLE_LOG_("I", __VA_ARGS__);   \
        }                                 \
    } while (0)
