#pragma once

#include <QString>
#include <vector>

extern "C"
{
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

namespace fplayer::windows_api
{
	class WasapiLoopbackCapture
	{
	public:
		WasapiLoopbackCapture();
		~WasapiLoopbackCapture();

		WasapiLoopbackCapture(const WasapiLoopbackCapture&) = delete;
		WasapiLoopbackCapture& operator=(const WasapiLoopbackCapture&) = delete;

		bool init(QString& err);
		void close();
		bool readInterleaved(std::vector<uint8_t>& out, int& outSamples);

		AVSampleFormat sampleFmt() const;
		const AVChannelLayout& channelLayout() const;
		int sampleRate() const;

	private:
		struct Impl;
		Impl* m_impl;
	};
}
