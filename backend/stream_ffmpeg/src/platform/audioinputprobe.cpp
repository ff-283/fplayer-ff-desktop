#include "audioinputprobe.h"

#if defined(_WIN32)
#include "windows/audioinputprobe.h"
#elif defined(__linux__)
#include "linux/audioinputprobe_linux.h"
#endif

namespace fplayer::platform_audio
{
	bool openAudioInputWithFallback(const QString& requestedSource,
	                                std::atomic<bool>& stopRequest,
	                                AVFormatContext*& outIfmt,
	                                QString& openedDeviceLabel,
	                                QString& detailLog)
	{
#if defined(_WIN32)
		return fplayer::windows_api::openDshowAudioInputWithFallback(requestedSource, stopRequest, outIfmt, openedDeviceLabel, detailLog);
#elif defined(__linux__)
		return fplayer::linux_api::openLinuxAudioInputWithFallback(requestedSource, stopRequest, outIfmt, openedDeviceLabel, detailLog);
#else
		(void)requestedSource;
		(void)stopRequest;
		outIfmt = nullptr;
		openedDeviceLabel.clear();
		detailLog = QStringLiteral("audio probe is unavailable on this platform");
		return false;
#endif
	}
}
