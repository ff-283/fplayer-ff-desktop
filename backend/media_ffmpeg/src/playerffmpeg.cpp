#include <fplayer/backend/media_ffmpeg/playerffmpeg.h>

#include <thread>
#include <mutex>
#include <deque>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <cstring>
#include <cstdint>

#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QIODevice>
#include <QThread>
#include <QDateTime>
#include <QString>
#include <QMetaObject>

#include <fplayer/common/fglwidget/fglwidget.h>
#include <logger/logger.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/time.h>
#include <libavutil/mathematics.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace fplayer
{
	struct PlayerFFmpeg::Impl
	{
		AVFormatContext* formatContext = nullptr;
		AVCodecContext* videoCodecContext = nullptr;
		AVCodecContext* audioCodecContext = nullptr;
		std::mutex videoCodecMutex;
		std::mutex audioCodecMutex;
		int videoStreamIndex = -1;
		int audioStreamIndex = -1;

		SwsContext* swsContext = nullptr;
		AVFrame* swsFrame = nullptr;
		int swsWidth = 0;
		int swsHeight = 0;
		AVPixelFormat swsSrcFormat = AV_PIX_FMT_NONE;

		SwrContext* swrContext = nullptr;
		/// 与 swr_alloc_set_opts2 的 in_sample_rate 一致；delay/输出长度换算须用同一基准，否则会听感像倍速。
		int swrInSampleRate = 0;
		AVChannelLayout outChannelLayout = AV_CHANNEL_LAYOUT_STEREO;
		AVSampleFormat outSampleFormat = AV_SAMPLE_FMT_S16;
		int outSampleRate = 48000;

		QAudioSink* audioSink = nullptr;
		QIODevice* audioIo = nullptr;

		std::thread demuxThread;
		std::thread decodeThread;
		std::thread audioThread;
		std::mutex videoQueueMutex;
		std::condition_variable videoQueueCv;
		std::deque<AVPacket*> videoPackets;
		std::mutex audioQueueMutex;
		std::condition_variable audioQueueCv;
		std::deque<AVPacket*> audioPackets;
		std::atomic<bool> stopRequested{false};
		std::atomic<bool> paused{true};
		std::atomic<bool> running{false};

		PreviewTarget previewTarget;
		FGLWidget* glWidget = nullptr;

		/// 视频时间轴锚点（毫秒，与 framePtsMs 同一换算）
		qint64 firstVideoPtsMs = -1;
		int64_t startClockUs = 0;
		std::atomic<qint64> durationMs{0};
		std::atomic<qint64> currentPosMs{0};
		std::atomic<qint64> seekRequestMs{-1};
		std::atomic<double> playbackRate{1.0};
		std::atomic<bool> audioResampleDirty{false};
		std::atomic<qint64> audioClockMs{-1};
		std::atomic<qint64> audioClockBasePtsMs{-1};
		std::atomic<qint64> audioClockBaseProcessedUs{0};
		std::atomic<qint64> avSyncBiasMs{0};
		std::atomic<qint64> statAudioBytes2s{0};
		std::atomic<qint64> statAudioFrames2s{0};
		std::atomic<qint64> statDroppedVideo2s{0};
		std::atomic<uint64_t> syncVersion{0};
		std::atomic<qint64> seekTargetMs{-1};
		std::atomic<bool> audioSinkResetPending{false};

		/// 预览 YUV：合并投递，避免每帧 QueuedConnection 堆积导致主线程多次 memcpy 卡顿。
		std::mutex previewFrameMutex;
		QByteArray previewPendingY;
		QByteArray previewPendingU;
		QByteArray previewPendingV;
		int previewW = 0;
		int previewH = 0;
		int previewYS = 0;
		int previewUS = 0;
		int previewVS = 0;
		bool previewHasPending = false;
		std::atomic<bool> previewDeliverQueued{false};

		void clearVideoConvert();
		void clearAudioConvert();
		void clearVideoPackets();
		void clearAudioPackets();
		void stopThread();
		void cleanup();
	};

	static bool openCodecContext(AVFormatContext* fmt, AVMediaType mediaType, int& streamIndex, AVCodecContext*& codecContext);
	static qint64 framePtsMs(const AVFrame* frame, const AVStream* stream);
static bool seekToMs(AVFormatContext* fmt, int streamIndex, qint64 positionMs)
{
	if (!fmt || streamIndex < 0 || streamIndex >= static_cast<int>(fmt->nb_streams))
	{
		return false;
	}
	AVStream* stream = fmt->streams[streamIndex];
	if (!stream)
	{
		return false;
	}

	const AVRational msQ{1, 1000};
	const int64_t targetTs = av_rescale_q(positionMs, msQ, stream->time_base);

	// 优先使用全局时间基 seek，交给 demuxer 在全局索引上定位。
	const int64_t globalTs = positionMs * 1000;
	int ret = avformat_seek_file(fmt, -1, INT64_MIN, globalTs, INT64_MAX, AVSEEK_FLAG_BACKWARD);
	if (ret < 0)
	{
		// 退化到按视频流 seek（对部分索引异常文件更稳）。
		ret = av_seek_frame(fmt, streamIndex, targetTs, AVSEEK_FLAG_BACKWARD);
	}
	if (ret < 0)
	{
		// 最后兜底：按全局时间基 seek。
		ret = av_seek_frame(fmt, -1, globalTs, AVSEEK_FLAG_BACKWARD);
	}
	return ret >= 0;
}
	static const char* avSampleFmtName(AVSampleFormat fmt)
	{
		const char* name = av_get_sample_fmt_name(fmt);
		return name ? name : "unknown";
	}
	static void logAudioSinkState(const QAudioSink* sink, const char* stage)
	{
		if (!sink)
		{
			LOG_WARN("[audio]", stage, " audio sink is null");
			return;
		}
		LOG_INFO("[audio]", stage, " state=", static_cast<int>(sink->state()), " error=", static_cast<int>(sink->error()));
	}
	static AVSampleFormat toAvSampleFormat(QAudioFormat::SampleFormat fmt)
	{
		switch (fmt)
		{
		case QAudioFormat::UInt8:
			return AV_SAMPLE_FMT_U8;
		case QAudioFormat::Int16:
			return AV_SAMPLE_FMT_S16;
		case QAudioFormat::Int32:
			return AV_SAMPLE_FMT_S32;
		case QAudioFormat::Float:
			return AV_SAMPLE_FMT_FLT;
		default:
			return AV_SAMPLE_FMT_S16;
		}
	}

	PlayerFFmpeg::PlayerFFmpeg() : m_impl(new Impl)
	{
		m_backend = MediaBackendType::FFmpeg;
		QAudioFormat format = QMediaDevices::defaultAudioOutput().preferredFormat();
		if (format.channelCount() <= 0)
		{
			format.setChannelCount(2);
		}
		if (format.sampleRate() <= 0)
		{
			format.setSampleRate(48000);
		}
		if (format.sampleFormat() == QAudioFormat::Unknown)
		{
			format.setSampleFormat(QAudioFormat::Int16);
		}
		m_impl->outSampleRate = format.sampleRate();
		m_impl->outSampleFormat = toAvSampleFormat(format.sampleFormat());
		av_channel_layout_uninit(&m_impl->outChannelLayout);
		av_channel_layout_default(&m_impl->outChannelLayout, format.channelCount());
		m_impl->audioSink = new QAudioSink(format, this);
		// 音频缓冲稍大一些，降低设备调度抖动带来的卡顿概率。
		m_impl->audioSink->setBufferSize(256 * 1024);
		m_impl->audioIo = m_impl->audioSink->start();
		LOG_INFO("[audio] output format: rate=", format.sampleRate(), " channels=", format.channelCount(),
		         " qtSampleFmt=", static_cast<int>(format.sampleFormat()), " avSampleFmt=", avSampleFmtName(m_impl->outSampleFormat));
		logAudioSinkState(m_impl->audioSink, "ctor");
	}

	PlayerFFmpeg::~PlayerFFmpeg()
	{
		stop();
		if (m_impl->audioSink)
		{
			m_impl->audioSink->stop();
		}
		delete m_impl;
	}

	bool PlayerFFmpeg::openFile(const QString& filePath)
	{
		if (filePath.isEmpty())
		{
			return false;
		}
		stop();
		m_impl->cleanup();

		if (avformat_open_input(&m_impl->formatContext, filePath.toUtf8().constData(), nullptr, nullptr) < 0)
		{
			LOG_ERROR("PlayerFFmpeg::openFile failed: open input");
			return false;
		}
		if (avformat_find_stream_info(m_impl->formatContext, nullptr) < 0)
		{
			LOG_ERROR("PlayerFFmpeg::openFile failed: stream info");
			m_impl->cleanup();
			return false;
		}
		if (m_impl->formatContext->duration > 0)
		{
			m_impl->durationMs.store(static_cast<qint64>(m_impl->formatContext->duration / (AV_TIME_BASE / 1000)));
		}
		else
		{
			m_impl->durationMs.store(0);
		}
		m_impl->currentPosMs.store(0);
		if (!openCodecContext(m_impl->formatContext, AVMEDIA_TYPE_VIDEO, m_impl->videoStreamIndex, m_impl->videoCodecContext))
		{
			LOG_ERROR("PlayerFFmpeg::openFile failed: video codec");
			m_impl->cleanup();
			return false;
		}
		openCodecContext(m_impl->formatContext, AVMEDIA_TYPE_AUDIO, m_impl->audioStreamIndex, m_impl->audioCodecContext);
		if (m_impl->audioCodecContext)
		{
			LOG_INFO("[audio] stream detected: index=", m_impl->audioStreamIndex,
			         " rate=", m_impl->audioCodecContext->sample_rate,
			         " channels=", m_impl->audioCodecContext->ch_layout.nb_channels,
			         " sampleFmt=", avSampleFmtName(m_impl->audioCodecContext->sample_fmt));
			// 延迟到拿到首帧再初始化 swr，避免容器元信息不完整导致静音。
			m_impl->clearAudioConvert();
		}
		else
		{
			LOG_WARN("[audio] no audio stream/decoder, video will be silent");
		}
		play();
		return true;
	}

	void PlayerFFmpeg::play()
	{
		if (!m_impl->formatContext || !m_impl->videoCodecContext)
		{
			return;
		}
		m_impl->paused.store(false);
		m_isPlaying.store(true);
		if (m_impl->audioSink)
		{
			m_impl->audioSink->resume();
			logAudioSinkState(m_impl->audioSink, "play");
		}
		if (m_impl->running.load())
		{
			return;
		}
		m_impl->running.store(true);
	m_impl->demuxThread = std::thread([this]() {
		AVPacket* packet = av_packet_alloc();
		while (!m_impl->stopRequested.load())
		{
			if (m_impl->paused.load())
			{
				QThread::msleep(5);
				continue;
			}
			const qint64 seekMs = m_impl->seekRequestMs.exchange(-1);
			if (seekMs >= 0)
			{
				seekToMs(m_impl->formatContext, m_impl->videoStreamIndex, seekMs);
				{
					std::lock_guard<std::mutex> vlock(m_impl->videoCodecMutex);
					avcodec_flush_buffers(m_impl->videoCodecContext);
				}
				if (m_impl->audioCodecContext)
				{
					std::lock_guard<std::mutex> alock(m_impl->audioCodecMutex);
					avcodec_flush_buffers(m_impl->audioCodecContext);
				}
				m_impl->clearVideoPackets();
				m_impl->clearAudioPackets();
				m_impl->firstVideoPtsMs = -1;
				m_impl->startClockUs = 0;
				m_impl->currentPosMs.store(seekMs);
				m_impl->seekTargetMs.store(seekMs);
				m_impl->audioResampleDirty.store(true);
				m_impl->audioClockMs.store(-1);
				m_impl->audioClockBasePtsMs.store(-1);
				m_impl->audioClockBaseProcessedUs.store(0);
				m_impl->avSyncBiasMs.store(0);
				m_impl->syncVersion.fetch_add(1);
				// 丢弃设备里旧的 PCM，避免 seek 后先播放过期音频造成卡顿/错位。
				m_impl->audioSinkResetPending.store(true);
			}

			const int ret = av_read_frame(m_impl->formatContext, packet);
			if (ret < 0)
			{
				if (ret == AVERROR_EOF)
				{
					// 默认行为：循环播放（EOF 自动回到开头）。不得 exit demux 线程，否则后续无法继续读包。
					seekToMs(m_impl->formatContext, m_impl->videoStreamIndex, 0);
					{
						std::lock_guard<std::mutex> vlock(m_impl->videoCodecMutex);
						avcodec_flush_buffers(m_impl->videoCodecContext);
					}
					if (m_impl->audioCodecContext)
					{
						std::lock_guard<std::mutex> alock(m_impl->audioCodecMutex);
						avcodec_flush_buffers(m_impl->audioCodecContext);
					}
					m_impl->clearVideoPackets();
					m_impl->clearAudioPackets();
					m_impl->firstVideoPtsMs = -1;
					m_impl->startClockUs = 0;
					m_impl->audioClockMs.store(-1);
					m_impl->audioClockBasePtsMs.store(-1);
					m_impl->audioClockBaseProcessedUs.store(0);
					m_impl->avSyncBiasMs.store(0);
					m_impl->seekTargetMs.store(-1);
					m_impl->syncVersion.fetch_add(1);
					m_impl->audioSinkResetPending.store(true);
					continue;
				}
				QThread::msleep(2);
				continue;
			}

			AVPacket* clonePacket = av_packet_alloc();
			if (!clonePacket)
			{
				av_packet_unref(packet);
				continue;
			}
			av_packet_move_ref(clonePacket, packet);
			if (!clonePacket->data && clonePacket->size <= 0)
			{
				av_packet_free(&clonePacket);
				continue;
			}
			if (clonePacket->stream_index == m_impl->videoStreamIndex)
			{
				std::lock_guard<std::mutex> lock(m_impl->videoQueueMutex);
				// 普通播放时：队列满丢新包，避免 demux 被阻塞进而拖垮音频供给。
				// seek 期间：改为丢旧包保新包，尽快推进到目标时间点。
				if (m_impl->videoPackets.size() >= 120)
				{
					if (m_impl->seekTargetMs.load() >= 0 && !m_impl->videoPackets.empty())
					{
						AVPacket* oldPacket = m_impl->videoPackets.front();
						m_impl->videoPackets.pop_front();
						av_packet_free(&oldPacket);
					}
					else
					{
						av_packet_free(&clonePacket);
						continue;
					}
				}
				m_impl->videoPackets.push_back(clonePacket);
				m_impl->videoQueueCv.notify_one();
			}
			else if (clonePacket->stream_index == m_impl->audioStreamIndex && m_impl->audioCodecContext)
			{
				std::unique_lock<std::mutex> lock(m_impl->audioQueueMutex);
				// 音频不丢旧包，短等待背压，优先连续性（减少可听卡顿）。
				if (m_impl->audioPackets.size() >= 240)
				{
					m_impl->audioQueueCv.wait_for(lock, std::chrono::milliseconds(20), [this]() {
						return m_impl->stopRequested.load() || m_impl->seekRequestMs.load() >= 0 || m_impl->audioPackets.size() < 240;
					});
				}
				if (m_impl->stopRequested.load())
				{
					av_packet_free(&clonePacket);
					break;
				}
				// seek 请求到来时，放弃当前包，尽快回到循环顶部处理 seek。
				if (m_impl->seekRequestMs.load() >= 0)
				{
					av_packet_free(&clonePacket);
					continue;
				}
				m_impl->audioPackets.push_back(clonePacket);
				m_impl->audioQueueCv.notify_one();
			}
			else
			{
				av_packet_free(&clonePacket);
			}
		}
		av_packet_free(&packet);
	});

	m_impl->decodeThread = std::thread([this]() {
		AVFrame* frame = av_frame_alloc();
		qint64 droppedVideoFramesSinceLastLog = 0;
		qint64 nominalFrameMs = 40;
		uint64_t localSyncVersion = m_impl->syncVersion.load();
		if (m_impl->videoStreamIndex >= 0 && m_impl->videoStreamIndex < static_cast<int>(m_impl->formatContext->nb_streams))
		{
			const AVStream* vs = m_impl->formatContext->streams[m_impl->videoStreamIndex];
			if (vs && vs->avg_frame_rate.num > 0 && vs->avg_frame_rate.den > 0)
			{
				const double fps = av_q2d(vs->avg_frame_rate);
				if (fps > 1.0)
				{
					nominalFrameMs = qMax<qint64>(10, static_cast<qint64>(1000.0 / fps));
				}
			}
		}
		qint64 lastLogMs = QDateTime::currentMSecsSinceEpoch();
		while (!m_impl->stopRequested.load())
		{
			const uint64_t globalSyncVersion = m_impl->syncVersion.load();
			if (globalSyncVersion != localSyncVersion)
			{
				localSyncVersion = globalSyncVersion;
				m_impl->firstVideoPtsMs = -1;
			}
			if (m_impl->paused.load())
			{
				QThread::msleep(5);
				continue;
			}
			AVPacket* packet = nullptr;
			{
				std::unique_lock<std::mutex> lock(m_impl->videoQueueMutex);
				m_impl->videoQueueCv.wait_for(lock, std::chrono::milliseconds(10), [this]() {
					return m_impl->stopRequested.load() || !m_impl->videoPackets.empty();
				});
				if (m_impl->stopRequested.load())
				{
					break;
				}
				if (m_impl->videoPackets.empty())
				{
					continue;
				}
				packet = m_impl->videoPackets.front();
				m_impl->videoPackets.pop_front();
				m_impl->videoQueueCv.notify_one();
			}
			if (!packet)
			{
				continue;
			}
			{
				std::lock_guard<std::mutex> vlock(m_impl->videoCodecMutex);
				if (avcodec_send_packet(m_impl->videoCodecContext, packet) < 0)
				{
					av_packet_free(&packet);
					continue;
				}
			}
			av_packet_free(&packet);
			while (!m_impl->stopRequested.load())
			{
				int ret = 0;
				{
					std::lock_guard<std::mutex> vlock(m_impl->videoCodecMutex);
					ret = avcodec_receive_frame(m_impl->videoCodecContext, frame);
				}
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				{
					break;
				}
				if (ret < 0)
				{
					break;
				}
				AVFrame* renderFrame = frame;
				const AVPixelFormat srcFmt = static_cast<AVPixelFormat>(frame->format);
				if (srcFmt != AV_PIX_FMT_YUV420P && srcFmt != AV_PIX_FMT_YUVJ420P)
				{
					if (!m_impl->swsContext || m_impl->swsWidth != frame->width || m_impl->swsHeight != frame->height || m_impl->swsSrcFormat != srcFmt)
					{
						m_impl->clearVideoConvert();
						m_impl->swsContext = sws_getContext(frame->width, frame->height, srcFmt, frame->width, frame->height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
						m_impl->swsFrame = av_frame_alloc();
						m_impl->swsFrame->format = AV_PIX_FMT_YUV420P;
						m_impl->swsFrame->width = frame->width;
						m_impl->swsFrame->height = frame->height;
						av_frame_get_buffer(m_impl->swsFrame, 32);
						m_impl->swsWidth = frame->width;
						m_impl->swsHeight = frame->height;
						m_impl->swsSrcFormat = srcFmt;
					}
					sws_scale(m_impl->swsContext, frame->data, frame->linesize, 0, frame->height, m_impl->swsFrame->data, m_impl->swsFrame->linesize);
					renderFrame = m_impl->swsFrame;
				}
				const AVStream* stream = m_impl->formatContext->streams[m_impl->videoStreamIndex];
				const qint64 videoPtsMsRaw = framePtsMs(frame, stream);
				const qint64 videoPtsMs = qMax<qint64>(0, videoPtsMsRaw);
				m_impl->currentPosMs.store(videoPtsMs);
				const qint64 seekTargetMs = m_impl->seekTargetMs.load();
				if (seekTargetMs >= 0)
				{
					// seek 后先丢弃旧帧，直到接近目标时间，避免“拖拽后位置错误/不动”。
					if (videoPtsMs + qMax<qint64>(nominalFrameMs, 30) < seekTargetMs)
					{
						av_frame_unref(frame);
						continue;
					}
					m_impl->seekTargetMs.store(-1);
					m_impl->firstVideoPtsMs = videoPtsMsRaw;
					m_impl->startClockUs = av_gettime_relative();
				}

				const qint64 audioClockMs = m_impl->audioClockMs.load();
				const double rate = qBound(1.0, m_impl->playbackRate.load(), 2.0);
				const int64_t nowUs = av_gettime_relative();
				// 有音频时以音频时钟为主（Matroska 等容器里音轨与时间基更容易与设备时钟一致）。
				if (audioClockMs >= 0)
				{
					const qint64 driftMs = videoPtsMsRaw - audioClockMs;
					if (driftMs > 40)
					{
						// 单次睡眠过长会让整条管线积压，主观感受为「卡顿」；略等即可。
						const int64_t sleepUs = static_cast<int64_t>(qMin<qint64>((driftMs - 25) * 1000LL, 33 * 1000LL));
						av_usleep(static_cast<unsigned int>(qMax<int64_t>(sleepUs, 0)));
					}
					else if (driftMs < -80)
					{
						++droppedVideoFramesSinceLastLog;
						av_frame_unref(frame);
						continue;
					}
				}
				else
				{
					if (m_impl->firstVideoPtsMs < 0)
					{
						m_impl->firstVideoPtsMs = videoPtsMsRaw;
						m_impl->startClockUs = av_gettime_relative();
					}
					const int64_t targetUs = m_impl->startClockUs + static_cast<int64_t>((videoPtsMsRaw - m_impl->firstVideoPtsMs) * 1000.0 / rate);
					if (targetUs > nowUs)
					{
						const int64_t sleepUs = targetUs - nowUs;
						av_usleep(static_cast<unsigned int>(qMin<int64_t>(sleepUs, 33 * 1000)));
					}
					else if (nowUs - targetUs > 200 * 1000)
					{
						++droppedVideoFramesSinceLastLog;
						av_frame_unref(frame);
						continue;
					}
				}
				const int yStride = renderFrame->linesize[0];
				const int uStride = renderFrame->linesize[1];
				const int vStride = renderFrame->linesize[2];
				const int width = renderFrame->width;
				const int height = renderFrame->height;
				const int uvHeight = height / 2;
				QByteArray yBuffer(reinterpret_cast<const char*>(renderFrame->data[0]), yStride * height);
				QByteArray uBuffer(reinterpret_cast<const char*>(renderFrame->data[1]), uStride * uvHeight);
				QByteArray vBuffer(reinterpret_cast<const char*>(renderFrame->data[2]), vStride * uvHeight);
				queuePreviewYuv(std::move(yBuffer), std::move(uBuffer), std::move(vBuffer), width, height, yStride, uStride, vStride);
				av_frame_unref(frame);
			}
			const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
			if (nowMs - lastLogMs >= 2000)
			{
				m_impl->statDroppedVideo2s.store(droppedVideoFramesSinceLastLog);
				LOG_INFO("[playback] droppedVideo/2s=", droppedVideoFramesSinceLastLog);
				lastLogMs = nowMs;
				droppedVideoFramesSinceLastLog = 0;
			}
		}
		av_frame_free(&frame);
	});

		m_impl->audioThread = std::thread([this]() {
			AVFrame* frame = av_frame_alloc();
			qint64 pcmBytesSinceLastLog = 0;
			qint64 audioFramesSinceLastLog = 0;
			qint64 lastLogMs = QDateTime::currentMSecsSinceEpoch();
			while (!m_impl->stopRequested.load())
			{
				if (m_impl->paused.load())
				{
					QThread::msleep(5);
					continue;
				}
				AVPacket* packet = nullptr;
				{
					std::unique_lock<std::mutex> lock(m_impl->audioQueueMutex);
					m_impl->audioQueueCv.wait_for(lock, std::chrono::milliseconds(10), [this]() {
						return m_impl->stopRequested.load() || !m_impl->audioPackets.empty();
					});
					if (m_impl->stopRequested.load())
					{
						break;
					}
					if (m_impl->audioPackets.empty())
					{
						continue;
					}
					packet = m_impl->audioPackets.front();
					m_impl->audioPackets.pop_front();
					m_impl->audioQueueCv.notify_one();
				}
				if (!packet || !m_impl->audioCodecContext)
				{
					if (packet)
					{
						av_packet_free(&packet);
					}
					continue;
				}
				if (m_impl->audioSinkResetPending.exchange(false) && m_impl->audioSink)
				{
					m_impl->audioSink->stop();
					m_impl->audioIo = m_impl->audioSink->start();
					m_impl->audioClockBasePtsMs.store(-1);
					m_impl->audioClockBaseProcessedUs.store(0);
					m_impl->audioClockMs.store(-1);
					logAudioSinkState(m_impl->audioSink, "seek-reset");
				}
				if (m_impl->audioResampleDirty.exchange(false))
				{
					m_impl->clearAudioConvert();
				}
				{
					std::lock_guard<std::mutex> alock(m_impl->audioCodecMutex);
					if (avcodec_send_packet(m_impl->audioCodecContext, packet) < 0)
					{
						av_packet_free(&packet);
						continue;
					}
				}
				av_packet_free(&packet);
				while (!m_impl->stopRequested.load())
				{
					int ret = 0;
					{
						std::lock_guard<std::mutex> alock(m_impl->audioCodecMutex);
						ret = avcodec_receive_frame(m_impl->audioCodecContext, frame);
					}
					if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
					{
						break;
					}
					if (ret < 0)
					{
						break;
					}
					if (!m_impl->swrContext)
					{
						AVChannelLayout srcLayout{};
						if (frame->ch_layout.nb_channels > 0)
						{
							av_channel_layout_copy(&srcLayout, &frame->ch_layout);
						}
						else if (m_impl->audioCodecContext->ch_layout.nb_channels > 0)
						{
							av_channel_layout_copy(&srcLayout, &m_impl->audioCodecContext->ch_layout);
						}
						else
						{
							av_channel_layout_default(&srcLayout, 2);
						}
						int srcSampleRate = frame->sample_rate;
						if (srcSampleRate <= 0 && m_impl->audioCodecContext->sample_rate > 0)
						{
							srcSampleRate = m_impl->audioCodecContext->sample_rate;
						}
						if (srcSampleRate <= 0)
						{
							srcSampleRate = m_impl->outSampleRate;
						}
						m_impl->swrInSampleRate = srcSampleRate;
						const double rate = qBound(1.0, m_impl->playbackRate.load(), 2.0);
						const int deviceRate = m_impl->outSampleRate;
						const int swrOutRate = qMax(8000, static_cast<int>(qRound(deviceRate / rate)));
						if (swr_alloc_set_opts2(&m_impl->swrContext, &m_impl->outChannelLayout, m_impl->outSampleFormat, swrOutRate,
						                        &srcLayout, static_cast<AVSampleFormat>(frame->format), srcSampleRate, 0, nullptr) < 0 ||
						    swr_init(m_impl->swrContext) < 0)
						{
							av_channel_layout_uninit(&srcLayout);
							m_impl->swrInSampleRate = 0;
							av_frame_unref(frame);
							continue;
						}
						av_channel_layout_uninit(&srcLayout);
					}
					const int outChannels = m_impl->outChannelLayout.nb_channels;
					const AVStream* audioStream = (m_impl->audioStreamIndex >= 0 && m_impl->audioStreamIndex < static_cast<int>(m_impl->formatContext->nb_streams))
						                              ? m_impl->formatContext->streams[m_impl->audioStreamIndex]
						                              : nullptr;
					qint64 audioPtsMsForClock = -1;
					if (audioStream)
					{
						audioPtsMsForClock = framePtsMs(frame, audioStream);
						if (m_impl->audioClockBasePtsMs.load() < 0)
						{
							m_impl->audioClockBasePtsMs.store(audioPtsMsForClock);
							const qint64 processedUs = m_impl->audioSink ? m_impl->audioSink->processedUSecs() : 0;
							m_impl->audioClockBaseProcessedUs.store(processedUs);
						}
					}
					const double rate = qBound(1.0, m_impl->playbackRate.load(), 2.0);
					const int deviceRate = m_impl->outSampleRate;
					const int swrOutRate = qMax(8000, static_cast<int>(qRound(deviceRate / rate)));
					const int inSr = m_impl->swrInSampleRate > 0 ? m_impl->swrInSampleRate : m_impl->audioCodecContext->sample_rate;
					const int inSrSafe = inSr > 0 ? inSr : deviceRate;
					const int dstSamples = av_rescale_rnd(swr_get_delay(m_impl->swrContext, static_cast<int64_t>(inSrSafe)) + frame->nb_samples,
					                                      static_cast<int64_t>(swrOutRate), static_cast<int64_t>(inSrSafe), AV_ROUND_UP);
					uint8_t* outData = nullptr;
					int outLineSize = 0;
					if (av_samples_alloc(&outData, &outLineSize, outChannels, dstSamples, m_impl->outSampleFormat, 0) >= 0)
					{
						const int converted = swr_convert(m_impl->swrContext, &outData, dstSamples, const_cast<const uint8_t**>(frame->data), frame->nb_samples);
						if (converted > 0 && m_impl->audioIo)
						{
							const int outBytes = av_samples_get_buffer_size(nullptr, outChannels, converted, m_impl->outSampleFormat, 1);
							const char* writePtr = reinterpret_cast<const char*>(outData);
							int remaining = outBytes;
							int spin = 0;
							// 音频线程可承受短等待：尽量写完整帧，减少“断续感”。
							while (remaining > 0 && !m_impl->stopRequested.load() && spin < 24)
							{
								const qint64 written = m_impl->audioIo->write(writePtr, remaining);
								if (written > 0)
								{
									writePtr += written;
									remaining -= static_cast<int>(written);
									pcmBytesSinceLastLog += written;
								}
								else
								{
									QThread::usleep(1000);
								}
								++spin;
							}
							if (remaining > 0)
							{
								LOG_TRACE("[audio] partial frame dropped bytes=", remaining);
							}
							++audioFramesSinceLastLog;
							if (audioStream && m_impl->audioSink)
							{
								const qint64 basePts = m_impl->audioClockBasePtsMs.load();
								const qint64 baseProcessedUs = m_impl->audioClockBaseProcessedUs.load();
								const qint64 nowProcessedUs = m_impl->audioSink->processedUSecs();
								const qint64 playedUs = qMax<qint64>(0, nowProcessedUs - baseProcessedUs);
								// processedUSecs 是设备侧“墙钟回放”的微秒数；倍速时 swr 已把时间压缩进 PCM，
								// 媒体时间轴推进应为 墙钟 * playbackRate，否则视频会按 1x 跟音频对拍。
								m_impl->audioClockMs.store(basePts + static_cast<qint64>(playedUs * rate / 1000.0));
							}
						}
						else if (audioStream && audioPtsMsForClock >= 0 && !m_impl->audioSink)
						{
							m_impl->audioClockMs.store(audioPtsMsForClock);
						}
						av_freep(&outData);
					}
					av_frame_unref(frame);
				}
				const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
				if (nowMs - lastLogMs >= 2000)
				{
					m_impl->statAudioFrames2s.store(audioFramesSinceLastLog);
					m_impl->statAudioBytes2s.store(pcmBytesSinceLastLog);
					LOG_INFO("[playback] audioFrames/2s=", audioFramesSinceLastLog, " audioBytes/2s=", pcmBytesSinceLastLog);
					logAudioSinkState(m_impl->audioSink, "runtime");
					lastLogMs = nowMs;
					pcmBytesSinceLastLog = 0;
					audioFramesSinceLastLog = 0;
				}
			}
			av_frame_free(&frame);
		});
	}

	void PlayerFFmpeg::pause()
	{
		m_impl->paused.store(true);
		m_isPlaying.store(false);
		if (m_impl->audioSink)
		{
			m_impl->audioSink->suspend();
			logAudioSinkState(m_impl->audioSink, "pause");
		}
	}

	void PlayerFFmpeg::stop()
	{
		m_isPlaying.store(false);
		m_impl->paused.store(true);
		m_impl->stopThread();
		if (m_impl->audioSink)
		{
			m_impl->audioSink->stop();
			m_impl->audioIo = m_impl->audioSink->start();
			logAudioSinkState(m_impl->audioSink, "stop");
		}
	}

	bool PlayerFFmpeg::isPlaying() const
	{
		return m_isPlaying;
	}

	qint64 PlayerFFmpeg::durationMs() const
	{
		return m_impl->durationMs.load();
	}

	qint64 PlayerFFmpeg::positionMs() const
	{
		return m_impl->currentPosMs.load();
	}

	bool PlayerFFmpeg::seekMs(qint64 positionMs)
	{
		const qint64 total = m_impl->durationMs.load();
		if (!m_impl->formatContext || total <= 0)
		{
			return false;
		}
		if (positionMs < 0)
		{
			positionMs = 0;
		}
		if (positionMs > total)
		{
			positionMs = total;
		}
		m_impl->seekRequestMs.store(positionMs);
	m_impl->videoQueueCv.notify_all();
	m_impl->audioQueueCv.notify_all();
		return true;
	}

void PlayerFFmpeg::setPlaybackRate(double rate)
{
	const double normalized = qBound(1.0, rate, 2.0);
	m_impl->playbackRate.store(normalized);
	m_impl->audioResampleDirty.store(true);
}

double PlayerFFmpeg::playbackRate() const
{
	return m_impl->playbackRate.load();
}

QString PlayerFFmpeg::debugStats() const
{
	size_t videoQ = 0;
	size_t audioQ = 0;
	{
		std::lock_guard<std::mutex> lock(m_impl->videoQueueMutex);
		videoQ = m_impl->videoPackets.size();
	}
	{
		std::lock_guard<std::mutex> lock(m_impl->audioQueueMutex);
		audioQ = m_impl->audioPackets.size();
	}
	return QString("VQ:%1 AQ:%2 DropV/2s:%3 AudFrm/2s:%4 AudKB/2s:%5")
		.arg(static_cast<qulonglong>(videoQ))
		.arg(static_cast<qulonglong>(audioQ))
		.arg(m_impl->statDroppedVideo2s.load())
		.arg(m_impl->statAudioFrames2s.load())
		.arg(m_impl->statAudioBytes2s.load() / 1024);
}

	void PlayerFFmpeg::setPreviewTarget(const PreviewTarget& target)
	{
		{
			std::lock_guard<std::mutex> lock(m_impl->previewFrameMutex);
			m_impl->previewHasPending = false;
			m_impl->previewPendingY.clear();
			m_impl->previewPendingU.clear();
			m_impl->previewPendingV.clear();
			m_impl->previewDeliverQueued.store(false);
		}
		m_impl->previewTarget = target;
		m_impl->glWidget = nullptr;
		if (!target.backend_hint)
		{
			return;
		}
		m_impl->glWidget = static_cast<FGLWidget*>(target.backend_hint);
	}

	void PlayerFFmpeg::queuePreviewYuv(QByteArray y, QByteArray u, QByteArray v, const int width, const int height, const int yStride, const int uStride, const int vStride)
	{
		if (!m_impl->glWidget)
		{
			return;
		}
		{
			std::lock_guard<std::mutex> lock(m_impl->previewFrameMutex);
			m_impl->previewPendingY = std::move(y);
			m_impl->previewPendingU = std::move(u);
			m_impl->previewPendingV = std::move(v);
			m_impl->previewW = width;
			m_impl->previewH = height;
			m_impl->previewYS = yStride;
			m_impl->previewUS = uStride;
			m_impl->previewVS = vStride;
			m_impl->previewHasPending = true;
		}
		bool expected = false;
		if (m_impl->previewDeliverQueued.compare_exchange_strong(expected, true))
		{
			QMetaObject::invokeMethod(this, "deliverPreviewFrame", Qt::QueuedConnection);
		}
	}

	void PlayerFFmpeg::deliverPreviewFrame()
	{
		QByteArray y;
		QByteArray u;
		QByteArray v;
		int w = 0;
		int h = 0;
		int ys = 0;
		int us = 0;
		int vs = 0;
		bool have = false;
		{
			std::lock_guard<std::mutex> lock(m_impl->previewFrameMutex);
			if (m_impl->previewHasPending)
			{
				have = true;
				y = std::move(m_impl->previewPendingY);
				u = std::move(m_impl->previewPendingU);
				v = std::move(m_impl->previewPendingV);
				w = m_impl->previewW;
				h = m_impl->previewH;
				ys = m_impl->previewYS;
				us = m_impl->previewUS;
				vs = m_impl->previewVS;
				m_impl->previewHasPending = false;
			}
		}
		if (have && m_impl->glWidget)
		{
			m_impl->glWidget->updateYUVFrame(y, u, v, w, h, ys, us, vs);
		}
		std::lock_guard<std::mutex> lock(m_impl->previewFrameMutex);
		if (m_impl->previewHasPending)
		{
			QMetaObject::invokeMethod(this, "deliverPreviewFrame", Qt::QueuedConnection);
		}
		else
		{
			m_impl->previewDeliverQueued.store(false);
		}
	}

	void PlayerFFmpeg::Impl::clearVideoConvert()
	{
		if (swsFrame)
		{
			av_frame_free(&swsFrame);
		}
		if (swsContext)
		{
			sws_freeContext(swsContext);
			swsContext = nullptr;
		}
		swsWidth = 0;
		swsHeight = 0;
		swsSrcFormat = AV_PIX_FMT_NONE;
	}

	void PlayerFFmpeg::Impl::clearAudioConvert()
	{
		if (swrContext)
		{
			swr_free(&swrContext);
			swrContext = nullptr;
		}
		swrInSampleRate = 0;
	}

void PlayerFFmpeg::Impl::clearAudioPackets()
{
	std::lock_guard<std::mutex> lock(audioQueueMutex);
	while (!audioPackets.empty())
	{
		AVPacket* packet = audioPackets.front();
		audioPackets.pop_front();
		av_packet_free(&packet);
	}
}

void PlayerFFmpeg::Impl::clearVideoPackets()
{
	std::lock_guard<std::mutex> lock(videoQueueMutex);
	while (!videoPackets.empty())
	{
		AVPacket* packet = videoPackets.front();
		videoPackets.pop_front();
		av_packet_free(&packet);
	}
}

	void PlayerFFmpeg::Impl::stopThread()
	{
		stopRequested.store(true);
		paused.store(false);
	videoQueueCv.notify_all();
	audioQueueCv.notify_all();
	if (demuxThread.joinable())
	{
		demuxThread.join();
	}
		if (decodeThread.joinable())
		{
			decodeThread.join();
		}
	if (audioThread.joinable())
	{
		audioThread.join();
	}
	clearVideoPackets();
	clearAudioPackets();
		{
			std::lock_guard<std::mutex> lock(previewFrameMutex);
			previewHasPending = false;
			previewPendingY.clear();
			previewPendingU.clear();
			previewPendingV.clear();
			previewDeliverQueued.store(false);
		}
		running.store(false);
		stopRequested.store(false);
	}

	void PlayerFFmpeg::Impl::cleanup()
	{
		clearVideoConvert();
		clearAudioConvert();
		if (videoCodecContext)
		{
			avcodec_free_context(&videoCodecContext);
		}
		if (audioCodecContext)
		{
			avcodec_free_context(&audioCodecContext);
		}
		if (formatContext)
		{
			avformat_close_input(&formatContext);
		}
		videoStreamIndex = -1;
		audioStreamIndex = -1;
		firstVideoPtsMs = -1;
		startClockUs = 0;
		durationMs.store(0);
		currentPosMs.store(0);
		seekRequestMs.store(-1);
		playbackRate.store(1.0);
		audioResampleDirty.store(false);
		audioClockMs.store(-1);
		audioClockBasePtsMs.store(-1);
		audioClockBaseProcessedUs.store(0);
		avSyncBiasMs.store(0);
		statAudioBytes2s.store(0);
		statAudioFrames2s.store(0);
		statDroppedVideo2s.store(0);
		syncVersion.store(0);
		audioSinkResetPending.store(false);
		seekTargetMs.store(-1);
		clearVideoPackets();
		clearAudioPackets();
	}

	static bool openCodecContext(AVFormatContext* fmt, AVMediaType mediaType, int& streamIndex, AVCodecContext*& codecContext)
	{
		const int idx = av_find_best_stream(fmt, mediaType, -1, -1, nullptr, 0);
		if (idx < 0)
		{
			return false;
		}
		streamIndex = idx;
		AVStream* stream = fmt->streams[idx];
		const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
		if (!codec)
		{
			return false;
		}
		codecContext = avcodec_alloc_context3(codec);
		if (!codecContext)
		{
			return false;
		}
		if (avcodec_parameters_to_context(codecContext, stream->codecpar) < 0)
		{
			return false;
		}
		if (avcodec_open2(codecContext, codec, nullptr) < 0)
		{
			return false;
		}
		return true;
	}

	static qint64 framePtsMs(const AVFrame* frame, const AVStream* stream)
	{
		if (!frame || !stream)
		{
			return 0;
		}
		const int64_t pts = (frame->best_effort_timestamp == AV_NOPTS_VALUE) ? frame->pts : frame->best_effort_timestamp;
		if (pts == AV_NOPTS_VALUE)
		{
			return 0;
		}
		return av_rescale_q(pts, stream->time_base, AVRational{1, 1000});
	}
}
