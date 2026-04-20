#pragma once

#include <QString>
#include <atomic>

extern "C"
{
#include <libavformat/avformat.h>
}

namespace fplayer::windows_api
{
	bool openDshowAudioInputWithFallback(const QString& requestedSource,
	                                     std::atomic<bool>& stopRequest,
	                                     AVFormatContext*& outIfmt,
	                                     QString& openedDeviceLabel,
	                                     QString& detailLog);
}
