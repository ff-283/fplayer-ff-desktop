#pragma once

#include <QString>
#include <atomic>

extern "C"
{
#include <libavformat/avformat.h>
}

namespace fplayer::linux_api
{
	bool openLinuxAudioInputWithFallback(const QString& requestedSource,
	                                     std::atomic<bool>& stopRequest,
	                                     AVFormatContext*& outIfmt,
	                                     QString& openedDeviceLabel,
	                                     QString& detailLog);
}
