#ifndef FPLAYER_BACKEND_STREAM_FFMPEG_AUDIO_PIPELINE_H
#define FPLAYER_BACKEND_STREAM_FFMPEG_AUDIO_PIPELINE_H

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <cstdint>

namespace fplayer::streamffmpeg_audio_pipeline
{
	// 在编码帧数据上做原地平面混音；仅处理 FLTP/S16P，其他格式保持不变。
	void mixPlanarInPlace(AVSampleFormat sampleFmt, uint8_t** dstData, uint8_t** srcData, int channels, int samples);
	// 从第二路 FIFO 读取 samples 样本并混入 dstFrame；内部负责临时缓冲申请与释放。
	bool mixFromFifoIntoFrame(AVAudioFifo* mixFifo, AVFrame* dstFrame, const AVCodecContext* audioEncCtx, int samples);
	// 以固定 samples 填充编码帧：主 FIFO 读满 + 可选第二路混音。
	bool fillAudioEncFrameFromFifos(AVFrame* dstFrame, const AVCodecContext* audioEncCtx, AVAudioFifo* mainFifo, AVAudioFifo* mixFifo,
	                                int samples, bool enableMix);
	// 以 readSamples 读主 FIFO，并扩展到 frameSamples；尾部不足样本补静音（用于 flush 尾包）。
	bool fillAudioEncFrameFromFifosWithPadding(AVFrame* dstFrame, const AVCodecContext* audioEncCtx, AVAudioFifo* mainFifo,
	                                           AVAudioFifo* mixFifo, int readSamples, int frameSamples, bool enableMix);
}

#endif // FPLAYER_BACKEND_STREAM_FFMPEG_AUDIO_PIPELINE_H
