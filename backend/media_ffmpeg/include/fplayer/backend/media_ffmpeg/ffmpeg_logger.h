#pragma once
// ponytail: shared FFmpeg log callback — was copy-pasted in cameraffmpeg.cpp + camerainfofetcher.cpp

#include <mutex>
#include <string>
#include <logger/logger.h>

extern "C" {
#include <libavutil/log.h>
}

inline std::mutex g_ffmpegLogMutex;

inline void ffmpegLoggerCallback(void*, int level, const char* fmt, va_list vargs)
{
    if (level > av_log_get_level()) return;

    static char message[2048];
    std::lock_guard<std::mutex> lock(g_ffmpegLogMutex);
    vsnprintf(message, sizeof(message), fmt, vargs);

    std::string str(message);
    if (!str.empty() && str.back() == '\n') str.pop_back();

    if (level <= AV_LOG_PANIC || level == AV_LOG_FATAL)
        LOG_CRITI("[ffmpeg]", str);
    else if (level <= AV_LOG_ERROR)
        LOG_ERROR("[ffmpeg]", str);
    else if (level <= AV_LOG_WARNING)
        LOG_WARN("[ffmpeg]", str);
    else if (level <= AV_LOG_INFO)
        LOG_INFO("[ffmpeg]", str);
    else if (level <= AV_LOG_VERBOSE || level <= AV_LOG_DEBUG)
        LOG_DEBUG("[ffmpeg]", str);
    else
        LOG_TRACE("[ffmpeg]", str);
}
