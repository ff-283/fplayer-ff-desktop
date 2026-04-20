#include "audio_pipeline.h"

namespace fplayer::streamffmpeg_audio_pipeline
{
	void mixPlanarInPlace(const AVSampleFormat sampleFmt, uint8_t** dstData, uint8_t** srcData, const int channels, const int samples)
	{
		if (!dstData || !srcData || channels <= 0 || samples <= 0)
		{
			return;
		}

		if (sampleFmt == AV_SAMPLE_FMT_FLTP)
		{
			for (int ch = 0; ch < channels; ++ch)
			{
				float* dst = reinterpret_cast<float*>(dstData[ch]);
				const float* src = reinterpret_cast<const float*>(srcData[ch]);
				if (!dst || !src)
				{
					continue;
				}
				for (int i = 0; i < samples; ++i)
				{
					dst[i] = (dst[i] + src[i]) * 0.5f;
				}
			}
			return;
		}

		if (sampleFmt == AV_SAMPLE_FMT_S16P)
		{
			for (int ch = 0; ch < channels; ++ch)
			{
				int16_t* dst = reinterpret_cast<int16_t*>(dstData[ch]);
				const int16_t* src = reinterpret_cast<const int16_t*>(srcData[ch]);
				if (!dst || !src)
				{
					continue;
				}
				for (int i = 0; i < samples; ++i)
				{
					const int mixed = (static_cast<int>(dst[i]) + static_cast<int>(src[i])) / 2;
					dst[i] = static_cast<int16_t>(mixed < -32768 ? -32768 : (mixed > 32767 ? 32767 : mixed));
				}
			}
		}
	}

	bool mixFromFifoIntoFrame(AVAudioFifo* mixFifo, AVFrame* dstFrame, const AVCodecContext* audioEncCtx, const int samples)
	{
		if (!mixFifo || !dstFrame || !audioEncCtx || samples <= 0)
		{
			return false;
		}

		uint8_t** mixData = nullptr;
		int mixLinesize = 0;
		if (av_samples_alloc_array_and_samples(&mixData, &mixLinesize, audioEncCtx->ch_layout.nb_channels, samples,
		                                       audioEncCtx->sample_fmt, 0) < 0)
		{
			return false;
		}

		const bool readOk = av_audio_fifo_read(mixFifo, reinterpret_cast<void**>(mixData), samples) == samples;
		if (readOk)
		{
			mixPlanarInPlace(audioEncCtx->sample_fmt, dstFrame->data, mixData, audioEncCtx->ch_layout.nb_channels, samples);
		}

		av_freep(&mixData[0]);
		av_freep(&mixData);
		return readOk;
	}

	bool fillAudioEncFrameFromFifos(AVFrame* dstFrame, const AVCodecContext* audioEncCtx, AVAudioFifo* mainFifo, AVAudioFifo* mixFifo,
	                                const int samples, const bool enableMix)
	{
		if (!dstFrame || !audioEncCtx || !mainFifo || samples <= 0)
		{
			return false;
		}

		av_frame_unref(dstFrame);
		dstFrame->nb_samples = samples;
		dstFrame->format = audioEncCtx->sample_fmt;
		dstFrame->sample_rate = audioEncCtx->sample_rate;
		av_channel_layout_copy(&dstFrame->ch_layout, &audioEncCtx->ch_layout);

		if (av_frame_get_buffer(dstFrame, 0) < 0 ||
		    av_audio_fifo_read(mainFifo, reinterpret_cast<void**>(dstFrame->data), samples) < samples)
		{
			return false;
		}

		// 混音失败不阻断主流程，保持“主路可用则继续推流”的降级语义。
		if (enableMix && mixFifo)
		{
			(void)mixFromFifoIntoFrame(mixFifo, dstFrame, audioEncCtx, samples);
		}
		return true;
	}

	bool fillAudioEncFrameFromFifosWithPadding(AVFrame* dstFrame, const AVCodecContext* audioEncCtx, AVAudioFifo* mainFifo,
	                                           AVAudioFifo* mixFifo, const int readSamples, const int frameSamples,
	                                           const bool enableMix)
	{
		if (!dstFrame || !audioEncCtx || !mainFifo || readSamples <= 0 || frameSamples <= 0 || readSamples > frameSamples)
		{
			return false;
		}
		av_frame_unref(dstFrame);
		dstFrame->nb_samples = frameSamples;
		dstFrame->format = audioEncCtx->sample_fmt;
		dstFrame->sample_rate = audioEncCtx->sample_rate;
		av_channel_layout_copy(&dstFrame->ch_layout, &audioEncCtx->ch_layout);
		if (av_frame_get_buffer(dstFrame, 0) < 0 ||
		    av_audio_fifo_read(mainFifo, reinterpret_cast<void**>(dstFrame->data), readSamples) < readSamples)
		{
			return false;
		}
		// flush 场景按已读样本混音，避免对补静音区间做无意义叠加。
		if (enableMix && mixFifo)
		{
			(void)mixFromFifoIntoFrame(mixFifo, dstFrame, audioEncCtx, readSamples);
		}
		if (readSamples < frameSamples)
		{
			av_samples_set_silence(dstFrame->data, readSamples, frameSamples - readSamples, audioEncCtx->ch_layout.nb_channels,
			                       audioEncCtx->sample_fmt);
		}
		return true;
	}
}
