#include "wasapiloopbackcapture.h"

#include <cstring>

#ifdef _WIN32
#include <audioclient.h>
#include <mmdeviceapi.h>
#endif

extern "C"
{
#include <libavutil/channel_layout.h>
}

namespace fplayer::windows_api
{
	struct WasapiLoopbackCapture::Impl
	{
#ifdef _WIN32
		IMMDeviceEnumerator* enumerator = nullptr;
		IMMDevice* device = nullptr;
		IAudioClient* client = nullptr;
		IAudioCaptureClient* capture = nullptr;
		WAVEFORMATEX* mixFmt = nullptr;
		bool comInited = false;
		bool started = false;
		AVSampleFormat sampleFmt = AV_SAMPLE_FMT_NONE;
		AVChannelLayout chLayout{};
		int sampleRate = 0;
		int bytesPerSample = 0;
		int channels = 0;
#else
		AVSampleFormat sampleFmt = AV_SAMPLE_FMT_NONE;
		AVChannelLayout chLayout{};
		int sampleRate = 0;
#endif
	};

	WasapiLoopbackCapture::WasapiLoopbackCapture() : m_impl(new Impl()) {}

	WasapiLoopbackCapture::~WasapiLoopbackCapture()
	{
		close();
		delete m_impl;
		m_impl = nullptr;
	}

	void WasapiLoopbackCapture::close()
	{
#ifdef _WIN32
		if (m_impl->started && m_impl->client)
		{
			m_impl->client->Stop();
		}
		m_impl->started = false;
		if (m_impl->capture)
		{
			m_impl->capture->Release();
			m_impl->capture = nullptr;
		}
		if (m_impl->client)
		{
			m_impl->client->Release();
			m_impl->client = nullptr;
		}
		if (m_impl->device)
		{
			m_impl->device->Release();
			m_impl->device = nullptr;
		}
		if (m_impl->enumerator)
		{
			m_impl->enumerator->Release();
			m_impl->enumerator = nullptr;
		}
		if (m_impl->mixFmt)
		{
			CoTaskMemFree(m_impl->mixFmt);
			m_impl->mixFmt = nullptr;
		}
		if (m_impl->comInited)
		{
			CoUninitialize();
			m_impl->comInited = false;
		}
		m_impl->bytesPerSample = 0;
		m_impl->channels = 0;
#endif
		av_channel_layout_uninit(&m_impl->chLayout);
		m_impl->sampleFmt = AV_SAMPLE_FMT_NONE;
		m_impl->sampleRate = 0;
	}

	bool WasapiLoopbackCapture::init(QString& err)
	{
#ifndef _WIN32
		err = QStringLiteral("WASAPI is only available on Windows");
		return false;
#else
		close();
		const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (SUCCEEDED(hrCo))
		{
			m_impl->comInited = true;
		}
		else if (hrCo != RPC_E_CHANGED_MODE)
		{
			err = QStringLiteral("CoInitializeEx failed: 0x%1").arg(static_cast<unsigned>(hrCo), 0, 16);
			return false;
		}

		HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
		                              reinterpret_cast<void**>(&m_impl->enumerator));
		if (FAILED(hr) || !m_impl->enumerator)
		{
			err = QStringLiteral("MMDeviceEnumerator failed: 0x%1").arg(static_cast<unsigned>(hr), 0, 16);
			return false;
		}
		hr = m_impl->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_impl->device);
		if (FAILED(hr) || !m_impl->device)
		{
			err = QStringLiteral("GetDefaultAudioEndpoint failed: 0x%1").arg(static_cast<unsigned>(hr), 0, 16);
			return false;
		}
		hr = m_impl->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_impl->client));
		if (FAILED(hr) || !m_impl->client)
		{
			err = QStringLiteral("IAudioClient activate failed: 0x%1").arg(static_cast<unsigned>(hr), 0, 16);
			return false;
		}
		hr = m_impl->client->GetMixFormat(&m_impl->mixFmt);
		if (FAILED(hr) || !m_impl->mixFmt)
		{
			err = QStringLiteral("GetMixFormat failed: 0x%1").arg(static_cast<unsigned>(hr), 0, 16);
			return false;
		}

		WORD bits = m_impl->mixFmt->wBitsPerSample;
		WORD channelsW = m_impl->mixFmt->nChannels;
		WORD formatTag = m_impl->mixFmt->wFormatTag;
		if (formatTag == WAVE_FORMAT_EXTENSIBLE && m_impl->mixFmt->cbSize >= 22)
		{
			const auto* ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(m_impl->mixFmt);
			formatTag = ex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
			bits = ex->Samples.wValidBitsPerSample > 0 ? ex->Samples.wValidBitsPerSample : m_impl->mixFmt->wBitsPerSample;
			channelsW = ex->Format.nChannels;
		}

		if (formatTag == WAVE_FORMAT_IEEE_FLOAT && bits == 32)
		{
			m_impl->sampleFmt = AV_SAMPLE_FMT_FLT;
			m_impl->bytesPerSample = 4;
		}
		else if (formatTag == WAVE_FORMAT_PCM && bits == 16)
		{
			m_impl->sampleFmt = AV_SAMPLE_FMT_S16;
			m_impl->bytesPerSample = 2;
		}
		else if (formatTag == WAVE_FORMAT_PCM && bits == 32)
		{
			m_impl->sampleFmt = AV_SAMPLE_FMT_S32;
			m_impl->bytesPerSample = 4;
		}
		else
		{
			err = QStringLiteral("Unsupported mix format: tag=%1 bits=%2").arg(formatTag).arg(bits);
			return false;
		}

		m_impl->channels = channelsW > 0 ? static_cast<int>(channelsW) : 2;
		m_impl->sampleRate = m_impl->mixFmt->nSamplesPerSec > 0 ? static_cast<int>(m_impl->mixFmt->nSamplesPerSec) : 48000;
		av_channel_layout_default(&m_impl->chLayout, m_impl->channels);

		const REFERENCE_TIME hnsBufferDuration = 10000000; // 1s
		hr = m_impl->client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, hnsBufferDuration, 0, m_impl->mixFmt, nullptr);
		if (FAILED(hr))
		{
			err = QStringLiteral("IAudioClient Initialize(loopback) failed: 0x%1").arg(static_cast<unsigned>(hr), 0, 16);
			return false;
		}
		hr = m_impl->client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&m_impl->capture));
		if (FAILED(hr) || !m_impl->capture)
		{
			err = QStringLiteral("IAudioCaptureClient GetService failed: 0x%1").arg(static_cast<unsigned>(hr), 0, 16);
			return false;
		}
		hr = m_impl->client->Start();
		if (FAILED(hr))
		{
			err = QStringLiteral("IAudioClient Start failed: 0x%1").arg(static_cast<unsigned>(hr), 0, 16);
			return false;
		}
		m_impl->started = true;
		return true;
#endif
	}

	bool WasapiLoopbackCapture::readInterleaved(std::vector<uint8_t>& out, int& outSamples)
	{
		out.clear();
		outSamples = 0;
#ifndef _WIN32
		return false;
#else
		if (!m_impl->capture || !m_impl->started || m_impl->bytesPerSample <= 0 || m_impl->channels <= 0)
		{
			return false;
		}
		UINT32 packetFrames = 0;
		HRESULT hr = m_impl->capture->GetNextPacketSize(&packetFrames);
		if (FAILED(hr) || packetFrames == 0)
		{
			return false;
		}
		while (packetFrames > 0)
		{
			BYTE* data = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;
			hr = m_impl->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
			if (FAILED(hr))
			{
				return false;
			}
			const size_t bytes = static_cast<size_t>(frames) * static_cast<size_t>(m_impl->channels) *
			                     static_cast<size_t>(m_impl->bytesPerSample);
			const size_t old = out.size();
			out.resize(old + bytes);
			if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
			{
				std::memset(out.data() + old, 0, bytes);
			}
			else if (data && bytes > 0)
			{
				std::memcpy(out.data() + old, data, bytes);
			}
			outSamples += static_cast<int>(frames);
			m_impl->capture->ReleaseBuffer(frames);
			hr = m_impl->capture->GetNextPacketSize(&packetFrames);
			if (FAILED(hr))
			{
				break;
			}
		}
		return outSamples > 0;
#endif
	}

	AVSampleFormat WasapiLoopbackCapture::sampleFmt() const
	{
		return m_impl->sampleFmt;
	}

	const AVChannelLayout& WasapiLoopbackCapture::channelLayout() const
	{
		return m_impl->chLayout;
	}

	int WasapiLoopbackCapture::sampleRate() const
	{
		return m_impl->sampleRate;
	}
}
