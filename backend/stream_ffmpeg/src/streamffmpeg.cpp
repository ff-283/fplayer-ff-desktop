#include <fplayer/backend/stream_ffmpeg/streamffmpeg.h>

extern "C"
{
#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <QList>
#include <QMutexLocker>
#include <QThread>
#include <fplayer/common/cameraframebus/cameraframebus.h>
#include <fplayer/common/screenframebus/screenframebus.h>
#include "audio_pipeline.h"
#include "streamffmpeg_helpers.h"
#include "platform/windows/audioinputprobe.h"
#include "platform/windows/wasapiloopbackcapture.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
	using fplayer::streamffmpeg_helpers::CaptureParams;
	using fplayer::streamffmpeg_helpers::PushInputKind;
	using fplayer::streamffmpeg_helpers::PushInputRoute;
	using fplayer::streamffmpeg_helpers::VideoEncoderChoice;
	using fplayer::streamffmpeg_helpers::appendLimited;
	using fplayer::streamffmpeg_helpers::canCreateD3D11HwDevice;
	using fplayer::streamffmpeg_helpers::encoderHwFramesHint;
	using fplayer::streamffmpeg_helpers::estimateBitrateKbps;
	using fplayer::streamffmpeg_helpers::parseCaptureParams;
	using fplayer::streamffmpeg_helpers::parsePushInputRoute;
	using fplayer::streamffmpeg_helpers::pickEncoderPixelFormat;
	using fplayer::streamffmpeg_helpers::pickVideoEncoderCandidates;
	using fplayer::streamffmpeg_helpers::pushStartedLog;
	using fplayer::streamffmpeg_audio_pipeline::fillAudioEncFrameFromFifos;
	using fplayer::streamffmpeg_audio_pipeline::fillAudioEncFrameFromFifosWithPadding;
}

int fplayer::StreamFFmpeg::interruptCallback(void* opaque)
{
	const auto* self = static_cast<const std::atomic<bool>*>(opaque);
	return self && self->load(std::memory_order_relaxed) ? 1 : 0;
}

fplayer::StreamFFmpeg::StreamFFmpeg(QObject* parent) : QObject(parent)
{
	static std::once_flag netInit;
	std::call_once(netInit, []() { avformat_network_init(); });
}

fplayer::StreamFFmpeg::~StreamFFmpeg()
{
	stop();
}

bool fplayer::StreamFFmpeg::startPush(const QString& inputUrl, const QString& outputUrl)
{
	if (inputUrl.trimmed().isEmpty() || outputUrl.trimmed().isEmpty())
	{
		setLastError(QStringLiteral("推流输入或输出地址为空"));
		return false;
	}
	stop();
	{
		QMutexLocker locker(&m_mutex);
		m_lastError.clear();
		m_recentLog.clear();
		m_lastExitCode = 0;
	}
	m_stopRequest.store(false, std::memory_order_relaxed);
	m_running.store(true, std::memory_order_relaxed);
	m_completedSession.store(false, std::memory_order_relaxed);
	{
		const char* cfg = avcodec_configuration();
		const QString cfgText = QString::fromUtf8(cfg ? cfg : "");
		const bool d3d11vaReady = canCreateD3D11HwDevice();
		appendLogLine(QStringLiteral("[诊断] FFmpeg编码器可见性: nvenc=%1 amf=%2")
		              .arg(avcodec_find_encoder_by_name("h264_nvenc") ? QStringLiteral("yes") : QStringLiteral("no"))
		              .arg(avcodec_find_encoder_by_name("h264_amf") ? QStringLiteral("yes") : QStringLiteral("no")));
		appendLogLine(QStringLiteral("[诊断] FFmpeg配置包含: --enable-nvenc=%1 --enable-amf=%2")
		              .arg(cfgText.contains(QStringLiteral("--enable-nvenc")) ? QStringLiteral("yes") : QStringLiteral("no"))
		              .arg(cfgText.contains(QStringLiteral("--enable-amf")) ? QStringLiteral("yes") : QStringLiteral("no")));
		appendLogLine(QStringLiteral("[诊断] D3D11VA硬件设备可用=%1")
		              .arg(d3d11vaReady ? QStringLiteral("yes") : QStringLiteral("no")));
	}
	const PushInputRoute route = parsePushInputRoute(inputUrl);
	if (route.kind == PushInputKind::CameraCapture)
	{
		const CaptureParams cameraParams = parseCaptureParams(route.spec);
		if (cameraParams.device.isEmpty())
		{
			m_running.store(false, std::memory_order_relaxed);
			setLastError(QStringLiteral("摄像头推流设备参数为空"));
			return false;
		}
	}
	if (!startPushWorkerByRoute(route, inputUrl, outputUrl))
	{
		return false;
	}
	appendLogLine(pushStartedLog(route.kind));
	return true;
}

bool fplayer::StreamFFmpeg::startPushWorkerByRoute(const PushInputRoute& route, const QString& inputUrl, const QString& outputUrl)
{
	try
	{
		if (route.kind == PushInputKind::ScreenCapture)
		{
			m_worker = std::make_unique<std::thread>([this, outputUrl, route]() {
				pushScreenLoop(outputUrl, route.spec);
			});
		}
		else if (route.kind == PushInputKind::ScreenPreview)
		{
			m_worker = std::make_unique<std::thread>([this, outputUrl, route]() {
				pushScreenPreviewLoop(outputUrl, route.spec);
			});
		}
		else if (route.kind == PushInputKind::CameraCapture)
		{
			m_worker = std::make_unique<std::thread>([this, outputUrl, route]() {
				pushCameraLoop(outputUrl, route.spec);
			});
		}
		else if (route.kind == PushInputKind::CameraPreview)
		{
			m_worker = std::make_unique<std::thread>([this, outputUrl, route]() {
				pushCameraPreviewLoop(outputUrl, route.spec);
			});
		}
		else if (route.kind == PushInputKind::FileTranscode)
		{
			m_worker = std::make_unique<std::thread>([this, outputUrl, route]() {
				transcodeFileLoop(outputUrl, route.spec);
			});
		}
		else
		{
			m_worker = std::make_unique<std::thread>([this, inputUrl, outputUrl]() {
				remuxLoop(inputUrl, outputUrl, "flv");
			});
		}
		return true;
	}
	catch (...)
	{
		m_running.store(false, std::memory_order_relaxed);
		setLastError(QStringLiteral("无法启动推流线程"));
		return false;
	}
}

bool fplayer::StreamFFmpeg::startPull(const QString& inputUrl, const QString& outputUrl)
{
	if (inputUrl.trimmed().isEmpty() || outputUrl.trimmed().isEmpty())
	{
		setLastError(QStringLiteral("拉流输入或输出地址为空"));
		return false;
	}
	stop();
	{
		QMutexLocker locker(&m_mutex);
		m_lastError.clear();
		m_recentLog.clear();
		m_lastExitCode = 0;
	}
	m_stopRequest.store(false, std::memory_order_relaxed);
	m_running.store(true, std::memory_order_relaxed);
	m_completedSession.store(false, std::memory_order_relaxed);
	if (!startPullWorker(inputUrl, outputUrl))
	{
		return false;
	}
	appendLogLine(QStringLiteral("[拉流] 已启动（libavformat 转封装 copy）"));
	return true;
}

bool fplayer::StreamFFmpeg::startPullWorker(const QString& inputUrl, const QString& outputUrl)
{
	try
	{
		m_worker = std::make_unique<std::thread>([this, inputUrl, outputUrl]() {
			remuxLoop(inputUrl, outputUrl, nullptr);
		});
		return true;
	}
	catch (...)
	{
		m_running.store(false, std::memory_order_relaxed);
		setLastError(QStringLiteral("无法启动拉流线程"));
		return false;
	}
}

void fplayer::StreamFFmpeg::stop()
{
	m_stopRequest.store(true, std::memory_order_relaxed);
	if (m_worker && m_worker->joinable())
	{
		m_worker->join();
	}
	m_worker.reset();
	m_stopRequest.store(false, std::memory_order_relaxed);
	m_running.store(false, std::memory_order_relaxed);
}

bool fplayer::StreamFFmpeg::isRunning() const
{
	return m_running.load(std::memory_order_relaxed);
}

QString fplayer::StreamFFmpeg::lastError() const
{
	QMutexLocker locker(&m_mutex);
	return m_lastError;
}

QString fplayer::StreamFFmpeg::recentLog() const
{
	QMutexLocker locker(&m_mutex);
	return m_recentLog;
}

int fplayer::StreamFFmpeg::lastExitCode() const
{
	QMutexLocker locker(&m_mutex);
	return m_lastExitCode;
}

QStringList fplayer::StreamFFmpeg::availableVideoEncoders() const
{
	QStringList list;
	list << QStringLiteral("cpu");
	if (avcodec_find_encoder_by_name("h264_nvenc"))
	{
		list << QStringLiteral("nvenc");
	}
	if (avcodec_find_encoder_by_name("h264_amf"))
	{
		list << QStringLiteral("amf");
	}
	return list;
}

bool fplayer::StreamFFmpeg::hasCompletedStreamSession() const
{
	return m_completedSession.load(std::memory_order_relaxed);
}

void fplayer::StreamFFmpeg::appendLogLine(const QString& line)
{
	QMutexLocker locker(&m_mutex);
	appendLimited(m_recentLog, line + QLatin1Char('\n'));
}

void fplayer::StreamFFmpeg::setLastError(const QString& error)
{
	QMutexLocker locker(&m_mutex);
	m_lastError = error;
}

void fplayer::StreamFFmpeg::remuxLoop(const QString& inputUrl, const QString& outputUrl, const char* outputShortName)
{
	AVFormatContext* ifmt = nullptr;
	AVFormatContext* ofmt = nullptr;
	AVPacket* pkt = nullptr;
	int ret = 0;
	int exitCode = 0;
	bool wroteHeader = false;

	const QByteArray inUtf8 = inputUrl.toUtf8();
	const QByteArray outUtf8 = outputUrl.toUtf8();
	const char* inPath = inUtf8.constData();
	const char* outPath = outUtf8.constData();

	pkt = av_packet_alloc();
	if (!pkt)
	{
		exitCode = AVERROR(ENOMEM);
		setLastError(QStringLiteral("分配 AVPacket 失败"));
		goto cleanup;
	}

	ifmt = avformat_alloc_context();
	if (!ifmt)
	{
		exitCode = AVERROR(ENOMEM);
		setLastError(QStringLiteral("分配输入 AVFormatContext 失败"));
		goto cleanup;
	}
	ifmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
	ifmt->interrupt_callback.opaque = &m_stopRequest;

	ret = avformat_open_input(&ifmt, inPath, nullptr, nullptr);
	if (ret < 0)
	{
		exitCode = ret;
		char errbuf[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret, errbuf, sizeof(errbuf));
		setLastError(QStringLiteral("打开输入失败: %1").arg(QString::fromUtf8(errbuf)));
		// 打开失败时不能用 avformat_close_input；若仍持有预分配的 context 需单独释放
		if (ifmt)
		{
			avformat_free_context(ifmt);
			ifmt = nullptr;
		}
		goto cleanup;
	}

	ret = avformat_find_stream_info(ifmt, nullptr);
	if (ret < 0)
	{
		exitCode = ret;
		char errbuf[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret, errbuf, sizeof(errbuf));
		setLastError(QStringLiteral("解析流信息失败: %1").arg(QString::fromUtf8(errbuf)));
		goto cleanup;
	}

	ret = avformat_alloc_output_context2(&ofmt, nullptr, outputShortName, outPath);
	if (ret < 0 || !ofmt)
	{
		exitCode = ret ? ret : AVERROR_UNKNOWN;
		setLastError(QStringLiteral("创建输出封装器失败"));
		goto cleanup;
	}
	ofmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
	ofmt->interrupt_callback.opaque = &m_stopRequest;

	for (unsigned i = 0; i < ifmt->nb_streams; i++)
	{
		AVStream* inStr = ifmt->streams[i];
		AVStream* outStr = avformat_new_stream(ofmt, nullptr);
		if (!outStr)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("创建输出流失败"));
			goto cleanup;
		}
		ret = avcodec_parameters_copy(outStr->codecpar, inStr->codecpar);
		if (ret < 0)
		{
			exitCode = ret;
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			setLastError(QStringLiteral("复制编解码参数失败: %1").arg(QString::fromUtf8(errbuf)));
			goto cleanup;
		}
		outStr->codecpar->codec_tag = 0;
	}

	if (!(ofmt->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open2(&ofmt->pb, outPath, AVIO_FLAG_WRITE, nullptr, nullptr);
		if (ret < 0)
		{
			exitCode = ret;
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			setLastError(QStringLiteral("打开输出失败: %1").arg(QString::fromUtf8(errbuf)));
			goto cleanup;
		}
	}

	ret = avformat_write_header(ofmt, nullptr);
	if (ret < 0)
	{
		exitCode = ret;
		char errbuf[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret, errbuf, sizeof(errbuf));
		setLastError(QStringLiteral("写入文件头失败: %1").arg(QString::fromUtf8(errbuf)));
		goto cleanup;
	}
	wroteHeader = true;

	while (!m_stopRequest.load(std::memory_order_relaxed))
	{
		ret = av_read_frame(ifmt, pkt);
		if (ret < 0)
		{
			if (ret == AVERROR_EOF)
			{
				break;
			}
			exitCode = ret;
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			setLastError(QStringLiteral("读取数据包失败: %1").arg(QString::fromUtf8(errbuf)));
			break;
		}
		if (pkt->stream_index >= static_cast<int>(ifmt->nb_streams))
		{
			av_packet_unref(pkt);
			continue;
		}
		AVStream* inStr = ifmt->streams[pkt->stream_index];
		AVStream* outStr = ofmt->streams[pkt->stream_index];
		av_packet_rescale_ts(pkt, inStr->time_base, outStr->time_base);
		pkt->pos = -1;
		ret = av_interleaved_write_frame(ofmt, pkt);
		av_packet_unref(pkt);
		if (ret < 0)
		{
			exitCode = ret;
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			setLastError(QStringLiteral("写入数据包失败: %1").arg(QString::fromUtf8(errbuf)));
			break;
		}
	}

	if (m_stopRequest.load(std::memory_order_relaxed))
	{
		appendLogLine(QStringLiteral("用户已停止"));
	}
	if (wroteHeader && ofmt)
	{
		ret = av_write_trailer(ofmt);
		if (ret < 0 && exitCode == 0)
		{
			exitCode = ret;
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			setLastError(QStringLiteral("写入文件尾失败: %1").arg(QString::fromUtf8(errbuf)));
		}
	}

cleanup:
	if (ofmt && !(ofmt->oformat->flags & AVFMT_NOFILE) && ofmt->pb)
	{
		avio_closep(&ofmt->pb);
	}
	if (ofmt)
	{
		avformat_free_context(ofmt);
		ofmt = nullptr;
	}
	if (ifmt)
	{
		avformat_close_input(&ifmt);
		ifmt = nullptr;
	}
	if (pkt)
	{
		av_packet_free(&pkt);
	}

	{
		QMutexLocker locker(&m_mutex);
		m_lastExitCode = exitCode;
	}
	m_completedSession.store(true, std::memory_order_relaxed);
	m_running.store(false, std::memory_order_relaxed);
}

void fplayer::StreamFFmpeg::transcodeFileLoop(const QString& outputUrl, const QString& transcodeSpec)
{
	AVFormatContext* ifmt = nullptr;
	AVFormatContext* ofmt = nullptr;
	AVCodecContext* decCtx = nullptr;
	AVCodecContext* encCtx = nullptr;
	AVPacket* inPkt = nullptr;
	AVPacket* outPkt = nullptr;
	AVFrame* decFrame = nullptr;
	SwsContext* sws = nullptr;
	AVFrame* encFrame = nullptr;
	int exitCode = 0;
	int ret = 0;
	bool wroteHeader = false;
	int videoIndex = -1;
	int audioIndex = -1;
	AVStream* outAudioStream = nullptr;
	int64_t lastPts = -1;
	const CaptureParams params = parseCaptureParams(transcodeSpec);
	QByteArray inUtf8;
	QByteArray outUtf8;
	const char* inPath = nullptr;
	const char* outPath = nullptr;
	if (params.source.trimmed().isEmpty())
	{
		setLastError(QStringLiteral("文件转码推流缺少输入源"));
		goto cleanup;
	}
	inUtf8 = params.source.toUtf8();
	outUtf8 = outputUrl.toUtf8();
	inPath = inUtf8.constData();
	outPath = outUtf8.constData();

	inPkt = av_packet_alloc();
	outPkt = av_packet_alloc();
	decFrame = av_frame_alloc();
	encFrame = av_frame_alloc();
	if (!inPkt || !outPkt || !decFrame || !encFrame)
	{
		exitCode = AVERROR(ENOMEM);
		setLastError(QStringLiteral("分配转码资源失败"));
		goto cleanup;
	}
	ifmt = avformat_alloc_context();
	if (!ifmt)
	{
		exitCode = AVERROR(ENOMEM);
		setLastError(QStringLiteral("创建输入上下文失败"));
		goto cleanup;
	}
	ifmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
	ifmt->interrupt_callback.opaque = &m_stopRequest;
	ret = avformat_open_input(&ifmt, inPath, nullptr, nullptr);
	if (ret < 0)
	{
		exitCode = ret;
		setLastError(QStringLiteral("打开输入失败"));
		goto cleanup;
	}
	ret = avformat_find_stream_info(ifmt, nullptr);
	if (ret < 0)
	{
		exitCode = ret;
		setLastError(QStringLiteral("读取输入流信息失败"));
		goto cleanup;
	}
	for (unsigned i = 0; i < ifmt->nb_streams; ++i)
	{
		if (ifmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			videoIndex = static_cast<int>(i);
			break;
		}
	}
	for (unsigned i = 0; i < ifmt->nb_streams; ++i)
	{
		if (ifmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			audioIndex = static_cast<int>(i);
			break;
		}
	}
	if (videoIndex < 0)
	{
		exitCode = AVERROR_STREAM_NOT_FOUND;
		setLastError(QStringLiteral("未找到输入视频流"));
		goto cleanup;
	}
	{
		const AVCodec* dec = avcodec_find_decoder(ifmt->streams[videoIndex]->codecpar->codec_id);
		decCtx = avcodec_alloc_context3(dec);
		if (!decCtx || !dec)
		{
			exitCode = AVERROR_DECODER_NOT_FOUND;
			setLastError(QStringLiteral("创建解码器失败"));
			goto cleanup;
		}
		avcodec_parameters_to_context(decCtx, ifmt->streams[videoIndex]->codecpar);
		ret = avcodec_open2(decCtx, dec, nullptr);
		if (ret < 0)
		{
			exitCode = ret;
			setLastError(QStringLiteral("打开解码器失败"));
			goto cleanup;
		}
	}
	ret = avformat_alloc_output_context2(&ofmt, nullptr, "flv", outPath);
	if (ret < 0 || !ofmt)
	{
		exitCode = ret ? ret : AVERROR_UNKNOWN;
		setLastError(QStringLiteral("创建输出上下文失败"));
		goto cleanup;
	}
	{
		const int outFps = params.fps > 0 ? params.fps : 30;
		const int outW = params.width > 0 ? params.width : decCtx->width;
		const int outH = params.height > 0 ? params.height : decCtx->height;
		const int outKbps = params.bitrateKbps > 0 ? params.bitrateKbps : estimateBitrateKbps(outW, outH, outFps);
		const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_H264);
		if (!enc)
		{
			enc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
		}
		encCtx = avcodec_alloc_context3(enc);
		if (!encCtx || !enc)
		{
			exitCode = AVERROR_ENCODER_NOT_FOUND;
			setLastError(QStringLiteral("创建编码器失败"));
			goto cleanup;
		}
		encCtx->width = outW;
		encCtx->height = outH;
		encCtx->pix_fmt = pickEncoderPixelFormat(enc, false);
		encCtx->time_base = AVRational{1, outFps};
		encCtx->framerate = AVRational{outFps, 1};
		encCtx->bit_rate = static_cast<int64_t>(outKbps) * 1000;
		encCtx->rc_max_rate = encCtx->bit_rate;
		encCtx->rc_buffer_size = encCtx->bit_rate * 2;
		encCtx->gop_size = outFps * 2;
		encCtx->max_b_frames = 0;
		if (enc->id == AV_CODEC_ID_H264 && encCtx->priv_data)
		{
			av_opt_set(encCtx->priv_data, "preset", "superfast", 0);
			av_opt_set(encCtx->priv_data, "tune", "zerolatency", 0);
		}
		appendLogLine(QStringLiteral("[文件转码] 输入=%1 输出=%2x%3 FPS=%4 码率=%5kbps")
		              .arg(params.source)
		              .arg(encCtx->width)
		              .arg(encCtx->height)
		              .arg(outFps)
		              .arg(outKbps));
		ret = avcodec_open2(encCtx, enc, nullptr);
		if (ret < 0)
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			exitCode = ret;
			setLastError(QStringLiteral("打开编码器失败: %1 (encoder=%2, pix_fmt=%3)")
				             .arg(QString::fromUtf8(errbuf))
				             .arg(QString::fromLatin1(enc->name ? enc->name : "unknown"))
				             .arg(static_cast<int>(encCtx->pix_fmt)));
			goto cleanup;
		}
		AVStream* outStream = avformat_new_stream(ofmt, nullptr);
		if (!outStream)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("创建输出流失败"));
			goto cleanup;
		}
		outStream->time_base = encCtx->time_base;
		avcodec_parameters_from_context(outStream->codecpar, encCtx);
		if (audioIndex >= 0)
		{
			outAudioStream = avformat_new_stream(ofmt, nullptr);
			if (!outAudioStream)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建输出音频流失败"));
				goto cleanup;
			}
			ret = avcodec_parameters_copy(outAudioStream->codecpar, ifmt->streams[audioIndex]->codecpar);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("复制音频参数失败"));
				goto cleanup;
			}
			outAudioStream->codecpar->codec_tag = 0;
			outAudioStream->time_base = ifmt->streams[audioIndex]->time_base;
		}
	}
	if (!(ofmt->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open2(&ofmt->pb, outPath, AVIO_FLAG_WRITE, nullptr, nullptr);
		if (ret < 0)
		{
			exitCode = ret;
			setLastError(QStringLiteral("打开输出地址失败"));
			goto cleanup;
		}
	}
	ret = avformat_write_header(ofmt, nullptr);
	if (ret < 0)
	{
		exitCode = ret;
		setLastError(QStringLiteral("写入输出头失败"));
		goto cleanup;
	}
	wroteHeader = true;
	encFrame->format = encCtx->pix_fmt;
	encFrame->width = encCtx->width;
	encFrame->height = encCtx->height;
	ret = av_frame_get_buffer(encFrame, 32);
	if (ret < 0)
	{
		exitCode = ret;
		setLastError(QStringLiteral("分配编码帧失败"));
		goto cleanup;
	}
	sws = sws_getContext(decCtx->width, decCtx->height, decCtx->pix_fmt, encCtx->width, encCtx->height, encCtx->pix_fmt,
	                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
	if (!sws)
	{
		exitCode = AVERROR(EINVAL);
		setLastError(QStringLiteral("创建像素转换失败"));
		goto cleanup;
	}

	while (!m_stopRequest.load(std::memory_order_relaxed))
	{
		ret = av_read_frame(ifmt, inPkt);
		if (ret < 0)
		{
			break;
		}
		if (inPkt->stream_index != videoIndex)
		{
			if (inPkt->stream_index == audioIndex && outAudioStream)
			{
				av_packet_rescale_ts(inPkt, ifmt->streams[audioIndex]->time_base, outAudioStream->time_base);
				inPkt->stream_index = outAudioStream->index;
				av_interleaved_write_frame(ofmt, inPkt);
			}
			av_packet_unref(inPkt);
			continue;
		}
		ret = avcodec_send_packet(decCtx, inPkt);
		av_packet_unref(inPkt);
		if (ret < 0)
		{
			continue;
		}
		while (avcodec_receive_frame(decCtx, decFrame) == 0)
		{
			av_frame_make_writable(encFrame);
			sws_scale(sws, decFrame->data, decFrame->linesize, 0, decCtx->height, encFrame->data, encFrame->linesize);
			int64_t pts = decFrame->best_effort_timestamp;
			if (pts != AV_NOPTS_VALUE)
			{
				pts = av_rescale_q(pts, ifmt->streams[videoIndex]->time_base, encCtx->time_base);
				if (pts <= lastPts)
				{
					pts = lastPts + 1;
				}
			}
			else
			{
				pts = lastPts + 1;
			}
			lastPts = pts;
			encFrame->pts = pts;
			ret = avcodec_send_frame(encCtx, encFrame);
			if (ret < 0)
			{
				continue;
			}
			while (avcodec_receive_packet(encCtx, outPkt) == 0)
			{
				av_packet_rescale_ts(outPkt, encCtx->time_base, ofmt->streams[0]->time_base);
				outPkt->stream_index = 0;
				av_interleaved_write_frame(ofmt, outPkt);
				av_packet_unref(outPkt);
			}
			av_frame_unref(decFrame);
		}
	}
	avcodec_send_frame(encCtx, nullptr);
	while (avcodec_receive_packet(encCtx, outPkt) == 0)
	{
		av_packet_rescale_ts(outPkt, encCtx->time_base, ofmt->streams[0]->time_base);
		outPkt->stream_index = 0;
		av_interleaved_write_frame(ofmt, outPkt);
		av_packet_unref(outPkt);
	}
	if (wroteHeader)
	{
		av_write_trailer(ofmt);
	}

cleanup:
	if (sws)
	{
		sws_freeContext(sws);
	}
	if (encFrame)
	{
		av_frame_free(&encFrame);
	}
	if (decFrame)
	{
		av_frame_free(&decFrame);
	}
	if (inPkt)
	{
		av_packet_free(&inPkt);
	}
	if (outPkt)
	{
		av_packet_free(&outPkt);
	}
	if (encCtx)
	{
		avcodec_free_context(&encCtx);
	}
	if (decCtx)
	{
		avcodec_free_context(&decCtx);
	}
	if (ofmt)
	{
		if (!(ofmt->oformat->flags & AVFMT_NOFILE) && ofmt->pb)
		{
			avio_closep(&ofmt->pb);
		}
		avformat_free_context(ofmt);
	}
	if (ifmt)
	{
		avformat_close_input(&ifmt);
	}
	{
		QMutexLocker locker(&m_mutex);
		m_lastExitCode = exitCode;
	}
	m_completedSession.store(true, std::memory_order_relaxed);
	m_running.store(false, std::memory_order_relaxed);
}

void fplayer::StreamFFmpeg::pushScreenLoop(const QString& outputUrl, const QString& captureSpec)
{
	AVFormatContext* ifmt = nullptr;
	AVFormatContext* ofmt = nullptr;
	AVCodecContext* decCtx = nullptr;
	AVCodecContext* encCtx = nullptr;
	AVPacket* inPkt = nullptr;
	AVPacket* outPkt = nullptr;
	AVFrame* decFrame = nullptr;
	AVFrame* encFrame = nullptr;
	SwsContext* sws = nullptr;
	int exitCode = 0;
	int ret = 0;
	bool wroteHeader = false;
	int64_t framePts = 0;
	int videoIndex = -1;
	int64_t lastPts = -1;

	const QByteArray outUtf8 = outputUrl.toUtf8();
	const char* outPath = outUtf8.constData();
	const CaptureParams params = parseCaptureParams(captureSpec);
	const int targetFps = qMax(1, params.fps);
	avdevice_register_all();
	AVFormatContext* audioIfmt = nullptr;
	AVCodecContext* audioDecCtx = nullptr;
	AVFormatContext* audioIfmt2 = nullptr;
	AVCodecContext* audioDecCtx2 = nullptr;
	AVCodecContext* audioEncCtx = nullptr;
	SwrContext* audioSwr = nullptr;
	SwrContext* audioSwr2 = nullptr;
	AVAudioFifo* audioFifo = nullptr;
	AVAudioFifo* audioFifo2 = nullptr;
	AVPacket* audioPkt = nullptr;
	AVPacket* audioPkt2 = nullptr;
	AVPacket* audioOutPkt = nullptr;
	AVFrame* audioDecFrame = nullptr;
	AVFrame* audioDecFrame2 = nullptr;
	AVFrame* audioEncFrame = nullptr;
	int audioIndex = -1;
	int audioIndex2 = -1;
	AVStream* outAudioStream = nullptr;
	int64_t audioPts = 0;
	fplayer::windows_api::WasapiLoopbackCapture wasapiAudio;
	fplayer::windows_api::WasapiLoopbackCapture wasapiAudio2;
	bool useWasapiAudio = false;
	bool useWasapiAudio2 = false;
	std::mutex muxWriteMutex;
	std::atomic<bool> audioThreadStop{false};
	std::atomic<int> audioThreadErr{0};
	std::thread audioWorker;
	bool audioThreadStarted = false;

	const QString requestedAudioOut = params.audioOutputSource.trimmed();
	const QString requestedAudioIn = params.audioInputSource.trimmed();
	const bool hasIn = !requestedAudioIn.isEmpty() && requestedAudioIn != QStringLiteral("off");
	const bool hasOut = !requestedAudioOut.isEmpty() && requestedAudioOut != QStringLiteral("off");
	const bool dualAudioRequested = hasIn && hasOut && requestedAudioIn != requestedAudioOut;
	const QString audioSourcePrimary = dualAudioRequested ? requestedAudioIn : QString();
	const QString audioSourceSecondary = dualAudioRequested ? requestedAudioOut : QString();
	const QString resolvedAudioSource = hasOut ? requestedAudioOut : (hasIn ? requestedAudioIn : QString());
	const bool enableAudio = !resolvedAudioSource.isEmpty() && resolvedAudioSource != QStringLiteral("off");
	bool audioActive = enableAudio;
	if (audioActive)
	{
		QString openedAudioDevice;
		QString openDetail;
		const QString selectedPrimary = dualAudioRequested ? audioSourcePrimary : resolvedAudioSource;
		const bool primaryFromOutputSelection = !dualAudioRequested && hasOut;
		if (!fplayer::windows_api::openDshowAudioInputWithFallback(selectedPrimary, m_stopRequest, audioIfmt, openedAudioDevice, openDetail))
		{
			QString wasapiErr;
			if ((primaryFromOutputSelection || selectedPrimary == QStringLiteral("system")) && wasapiAudio.init(wasapiErr))
			{
				useWasapiAudio = true;
				openedAudioDevice = QStringLiteral("native-wasapi-loopback(default)");
			}
			else
			{
				appendLogLine(QStringLiteral("[屏幕推流] 音频采集初始化失败，已自动降级为无声推流: %1; wasapi-native=%2")
					              .arg(openDetail)
					              .arg(wasapiErr));
				audioActive = false;
			}
		}
		if (audioActive)
		{
			if (audioIfmt)
			{
				audioIfmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
				audioIfmt->interrupt_callback.opaque = &m_stopRequest;
			}
			appendLogLine(QStringLiteral("[屏幕推流] 音频设备=%1").arg(openedAudioDevice));
		}
		if (!audioActive || (!audioIfmt && !useWasapiAudio))
		{
			goto audio_init_done;
		}
		if (audioIfmt)
		{
			ret = avformat_find_stream_info(audioIfmt, nullptr);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("读取音频流信息失败"));
				goto cleanup;
			}
			for (unsigned i = 0; i < audioIfmt->nb_streams; ++i)
			{
				if (audioIfmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
				{
					audioIndex = static_cast<int>(i);
					break;
				}
			}
			if (audioIndex < 0)
			{
				exitCode = AVERROR_STREAM_NOT_FOUND;
				setLastError(QStringLiteral("未找到可用系统音频流"));
				goto cleanup;
			}
			const AVCodecParameters* inAudioPar = audioIfmt->streams[audioIndex]->codecpar;
			const AVCodec* audioDec = avcodec_find_decoder(inAudioPar->codec_id);
			if (!audioDec)
			{
				exitCode = AVERROR_DECODER_NOT_FOUND;
				setLastError(QStringLiteral("未找到音频解码器"));
				goto cleanup;
			}
			audioDecCtx = avcodec_alloc_context3(audioDec);
			if (!audioDecCtx)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建音频解码器上下文失败"));
				goto cleanup;
			}
			ret = avcodec_parameters_to_context(audioDecCtx, inAudioPar);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("加载音频输入参数失败"));
				goto cleanup;
			}
			ret = avcodec_open2(audioDecCtx, audioDec, nullptr);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("打开音频解码器失败"));
				goto cleanup;
			}
			audioPkt = av_packet_alloc();
			audioDecFrame = av_frame_alloc();
			if (!audioPkt || !audioDecFrame)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("分配音频解码资源失败"));
				goto cleanup;
			}
		}
		audioOutPkt = av_packet_alloc();
		audioEncFrame = av_frame_alloc();
		if (!audioOutPkt || !audioEncFrame)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("分配音频编码资源失败"));
			goto cleanup;
		}
		if (audioActive && dualAudioRequested)
		{
			QString openedAudioDevice2;
			QString openDetail2;
			if (!fplayer::windows_api::openDshowAudioInputWithFallback(audioSourceSecondary, m_stopRequest, audioIfmt2, openedAudioDevice2,
			                                                           openDetail2))
			{
				QString wasapiErr2;
				if (wasapiAudio2.init(wasapiErr2))
				{
					useWasapiAudio2 = true;
					openedAudioDevice2 = QStringLiteral("native-wasapi-loopback(default)");
					appendLogLine(QStringLiteral("[屏幕推流] 音频副设备=%1").arg(openedAudioDevice2));
				}
				else
				{
					appendLogLine(QStringLiteral("[屏幕推流] 副音频源不可用，退化为单音频: %1; wasapi-native=%2")
						              .arg(openDetail2)
						              .arg(wasapiErr2));
				}
			}
			if (audioIfmt2)
			{
				audioIfmt2->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
				audioIfmt2->interrupt_callback.opaque = &m_stopRequest;
				const int ret2 = avformat_find_stream_info(audioIfmt2, nullptr);
				if (ret2 >= 0)
				{
					for (unsigned i = 0; i < audioIfmt2->nb_streams; ++i)
					{
						if (audioIfmt2->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
						{
							audioIndex2 = static_cast<int>(i);
							break;
						}
					}
					if (audioIndex2 >= 0)
					{
						const AVCodecParameters* inAudioPar2 = audioIfmt2->streams[audioIndex2]->codecpar;
						const AVCodec* audioDec2 = avcodec_find_decoder(inAudioPar2->codec_id);
						if (audioDec2)
						{
							audioDecCtx2 = avcodec_alloc_context3(audioDec2);
							if (audioDecCtx2 && avcodec_parameters_to_context(audioDecCtx2, inAudioPar2) >= 0 &&
							    avcodec_open2(audioDecCtx2, audioDec2, nullptr) >= 0)
							{
								audioPkt2 = av_packet_alloc();
								audioDecFrame2 = av_frame_alloc();
								appendLogLine(QStringLiteral("[屏幕推流] 音频副设备=%1").arg(openedAudioDevice2));
							}
						}
					}
				}
			}
		}
	}
audio_init_done:

	inPkt = av_packet_alloc();
	outPkt = av_packet_alloc();
	decFrame = av_frame_alloc();
	encFrame = av_frame_alloc();
	if (!inPkt || !outPkt || !decFrame || !encFrame)
	{
		exitCode = AVERROR(ENOMEM);
		setLastError(QStringLiteral("分配编码资源失败"));
		goto cleanup;
	}

	{
		AVDictionary* inOpts = nullptr;
		av_dict_set(&inOpts, "framerate", QByteArray::number(targetFps).constData(), 0);
		av_dict_set(&inOpts, "draw_mouse", "1", 0);
		av_dict_set(&inOpts, "rtbufsize", "256M", 0);
		if (params.width > 0 && params.height > 0)
		{
			const QByteArray sizeText = QByteArray::number(params.width) + "x" + QByteArray::number(params.height);
			av_dict_set(&inOpts, "video_size", sizeText.constData(), 0);
		}
		av_dict_set(&inOpts, "offset_x", QByteArray::number(params.x).constData(), 0);
		av_dict_set(&inOpts, "offset_y", QByteArray::number(params.y).constData(), 0);
		const AVInputFormat* inFmt = av_find_input_format("gdigrab");
		ifmt = avformat_alloc_context();
		if (!ifmt)
		{
			av_dict_free(&inOpts);
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("创建采集上下文失败"));
			goto cleanup;
		}
		ifmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
		ifmt->interrupt_callback.opaque = &m_stopRequest;
		ret = avformat_open_input(&ifmt, "desktop", inFmt, &inOpts);
		av_dict_free(&inOpts);
		if (ret < 0)
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			exitCode = ret;
			setLastError(QStringLiteral("打开桌面采集失败: %1").arg(QString::fromUtf8(errbuf)));
			goto cleanup;
		}
	}
	ret = avformat_find_stream_info(ifmt, nullptr);
	if (ret < 0)
	{
		exitCode = ret;
		setLastError(QStringLiteral("读取桌面流信息失败"));
		goto cleanup;
	}
	for (unsigned i = 0; i < ifmt->nb_streams; ++i)
	{
		if (ifmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			videoIndex = static_cast<int>(i);
			break;
		}
	}
	if (videoIndex < 0)
	{
		exitCode = AVERROR_STREAM_NOT_FOUND;
		setLastError(QStringLiteral("未找到桌面视频流"));
		goto cleanup;
	}
	{
		const AVCodec* dec = avcodec_find_decoder(ifmt->streams[videoIndex]->codecpar->codec_id);
		if (!dec)
		{
			exitCode = AVERROR_DECODER_NOT_FOUND;
			setLastError(QStringLiteral("未找到桌面解码器"));
			goto cleanup;
		}
		decCtx = avcodec_alloc_context3(dec);
		if (!decCtx)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("创建解码器上下文失败"));
			goto cleanup;
		}
		avcodec_parameters_to_context(decCtx, ifmt->streams[videoIndex]->codecpar);
		ret = avcodec_open2(decCtx, dec, nullptr);
		if (ret < 0)
		{
			exitCode = ret;
			setLastError(QStringLiteral("打开桌面解码器失败"));
			goto cleanup;
		}
	}

	ret = avformat_alloc_output_context2(&ofmt, nullptr, "flv", outPath);
	if (ret < 0 || !ofmt)
	{
		exitCode = ret ? ret : AVERROR_UNKNOWN;
		setLastError(QStringLiteral("创建推流输出上下文失败"));
		goto cleanup;
	}
	// Live push: reduce mux/IO buffering so receiver gets playable data quickly.
	ofmt->flags |= AVFMT_FLAG_FLUSH_PACKETS;
	ofmt->max_interleave_delta = 0;
	ofmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
	ofmt->interrupt_callback.opaque = &m_stopRequest;

	{
		const QList<VideoEncoderChoice> candidates = pickVideoEncoderCandidates(params.videoEncoder);
		if (candidates.isEmpty())
		{
			exitCode = AVERROR_ENCODER_NOT_FOUND;
			setLastError(QStringLiteral("未找到可用视频编码器（当前请求=%1）。CPU 模式需要 libx264（或 libopenh264）。").arg(params.videoEncoder));
			goto cleanup;
		}
		VideoEncoderChoice encChoice;
		QString lastOpenErrDetail;
		int lastOpenRet = AVERROR_UNKNOWN;
		bool opened = false;
		for (const VideoEncoderChoice& cand : candidates)
		{
			if (!cand.codec)
			{
				continue;
			}
			if (encCtx)
			{
				avcodec_free_context(&encCtx);
				encCtx = nullptr;
			}
			encCtx = avcodec_alloc_context3(cand.codec);
			if (!encCtx)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建编码器上下文失败"));
				goto cleanup;
			}
			const AVCodec* enc = cand.codec;
			encCtx->width = params.outWidth > 0 ? params.outWidth : decCtx->width;
			encCtx->height = params.outHeight > 0 ? params.outHeight : decCtx->height;
			encCtx->pix_fmt = pickEncoderPixelFormat(enc, cand.isHardware);
			encCtx->time_base = AVRational{1, targetFps};
			encCtx->framerate = AVRational{targetFps, 1};
			encCtx->gop_size = targetFps * 2;
			encCtx->max_b_frames = 0;
			const int bitrateKbps = params.bitrateKbps > 0
				                        ? params.bitrateKbps
				                        : estimateBitrateKbps(encCtx->width, encCtx->height, targetFps);
			encCtx->bit_rate = static_cast<int64_t>(bitrateKbps) * 1000;
			encCtx->rc_max_rate = encCtx->bit_rate;
			encCtx->rc_buffer_size = encCtx->bit_rate * 2;
			if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
			{
				encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			}
			if (cand.isHardware && encCtx->priv_data)
			{
				av_opt_set(encCtx->priv_data, "preset", "p1", 0);
				av_opt_set(encCtx->priv_data, "tune", "ll", 0);
				av_opt_set(encCtx->priv_data, "rc", "cbr", 0);
				av_opt_set(encCtx->priv_data, "zerolatency", "1", 0);
			}
			else if (enc->id == AV_CODEC_ID_H264 && encCtx->priv_data)
			{
				av_opt_set(encCtx->priv_data, "preset", "ultrafast", 0);
				av_opt_set(encCtx->priv_data, "tune", "zerolatency", 0);
			}
			ret = avcodec_open2(encCtx, enc, nullptr);
			if (ret < 0)
			{
				char errbuf[AV_ERROR_MAX_STRING_SIZE];
				av_strerror(ret, errbuf, sizeof(errbuf));
				lastOpenRet = ret;
				lastOpenErrDetail = QStringLiteral("%1: %2 (pix_fmt=%3)")
					                    .arg(cand.name.isEmpty() ? QString::fromLatin1(enc->name ? enc->name : "?") : cand.name)
					                    .arg(QString::fromUtf8(errbuf))
					                    .arg(static_cast<int>(encCtx->pix_fmt));
				appendLogLine(QStringLiteral("[屏幕推流] 编码器打开失败，%1").arg(lastOpenErrDetail));
				continue;
			}
			encChoice = cand;
			opened = true;
			break;
		}
		if (!opened)
		{
			exitCode = lastOpenRet;
			setLastError(QStringLiteral("所有备选编码器均无法打开（最后: %1）").arg(lastOpenErrDetail));
			goto cleanup;
		}
		const AVCodec* enc = encChoice.codec;
		const int bitrateKbpsLog = params.bitrateKbps > 0
			                           ? params.bitrateKbps
			                           : estimateBitrateKbps(encCtx->width, encCtx->height, targetFps);
		appendLogLine(QStringLiteral("[屏幕推流] 采集区域=%1,%2 %3x%4 输出=%5x%6 FPS=%7 码率=%8kbps")
		              .arg(params.x)
		              .arg(params.y)
		              .arg(decCtx->width)
		              .arg(decCtx->height)
		              .arg(encCtx->width)
		              .arg(encCtx->height)
		              .arg(targetFps)
		              .arg(bitrateKbpsLog));
		appendLogLine(QStringLiteral("[屏幕推流] 编码器=%1（%2）")
		              .arg(encChoice.name.isEmpty() ? QString::fromLatin1(enc->name ? enc->name : "unknown") : encChoice.name)
		              .arg(encChoice.isHardware ? QStringLiteral("硬件") : QStringLiteral("软件")));
		appendLogLine(QStringLiteral("[屏幕推流] 编码器硬件帧能力=%1").arg(encoderHwFramesHint(enc)));
		if (audioActive)
		{
			appendLogLine(QStringLiteral("[屏幕推流] 音频来源 in=%1 out=%2 picked=%3")
				              .arg(params.audioInputSource)
				              .arg(params.audioOutputSource)
				              .arg(resolvedAudioSource));
		}
		AVStream* outStream = avformat_new_stream(ofmt, nullptr);
		if (!outStream)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("创建输出视频流失败"));
			goto cleanup;
		}
		outStream->time_base = encCtx->time_base;
		avcodec_parameters_from_context(outStream->codecpar, encCtx);
		if (audioActive && ((audioIfmt && audioIndex >= 0) || useWasapiAudio))
		{
			const AVCodec* audioEnc = avcodec_find_encoder(AV_CODEC_ID_AAC);
			if (!audioEnc)
			{
				exitCode = AVERROR_ENCODER_NOT_FOUND;
				setLastError(QStringLiteral("未找到 AAC 音频编码器，无法推送声音"));
				goto cleanup;
			}
			audioEncCtx = avcodec_alloc_context3(audioEnc);
			if (!audioEncCtx)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建音频编码器上下文失败"));
				goto cleanup;
			}
			const int inSampleRate = useWasapiAudio
				                         ? (wasapiAudio.sampleRate() > 0 ? wasapiAudio.sampleRate() : 48000)
				                         : (audioDecCtx && audioDecCtx->sample_rate > 0 ? audioDecCtx->sample_rate : 48000);
			audioEncCtx->sample_rate = inSampleRate;
			if (audioEnc->supported_samplerates)
			{
				int bestRate = audioEnc->supported_samplerates[0];
				int bestDiff = qAbs(bestRate - audioEncCtx->sample_rate);
				for (const int* p = audioEnc->supported_samplerates; *p != 0; ++p)
				{
					const int diff = qAbs(*p - audioEncCtx->sample_rate);
					if (diff < bestDiff)
					{
						bestRate = *p;
						bestDiff = diff;
					}
				}
				audioEncCtx->sample_rate = bestRate;
			}
			const int srcChannels = useWasapiAudio
				                        ? wasapiAudio.channelLayout().nb_channels
				                        : (audioDecCtx ? audioDecCtx->ch_layout.nb_channels : 0);
			const int targetChannels = (srcChannels == 1) ? 1 : 2;
			av_channel_layout_uninit(&audioEncCtx->ch_layout);
			av_channel_layout_default(&audioEncCtx->ch_layout, targetChannels);
			if (audioEnc->ch_layouts && audioEnc->ch_layouts[0].nb_channels > 0)
			{
				const AVChannelLayout* best = &audioEnc->ch_layouts[0];
				int bestDiff = qAbs(best->nb_channels - targetChannels);
				for (const AVChannelLayout* p = audioEnc->ch_layouts; p && p->nb_channels > 0; ++p)
				{
					const int diff = qAbs(p->nb_channels - targetChannels);
					if (diff < bestDiff)
					{
						best = p;
						bestDiff = diff;
					}
				}
				av_channel_layout_uninit(&audioEncCtx->ch_layout);
				if (av_channel_layout_copy(&audioEncCtx->ch_layout, best) < 0)
				{
					av_channel_layout_default(&audioEncCtx->ch_layout, targetChannels);
				}
			}
			if (audioEncCtx->ch_layout.nb_channels <= 0)
			{
				av_channel_layout_uninit(&audioEncCtx->ch_layout);
				av_channel_layout_default(&audioEncCtx->ch_layout, 2);
			}
			audioEncCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
			if (audioEnc->sample_fmts)
			{
				bool fltpSupported = false;
				for (const AVSampleFormat* p = audioEnc->sample_fmts; *p != AV_SAMPLE_FMT_NONE; ++p)
				{
					if (*p == AV_SAMPLE_FMT_FLTP)
					{
						fltpSupported = true;
						break;
					}
				}
				if (!fltpSupported)
				{
					audioEncCtx->sample_fmt = audioEnc->sample_fmts[0];
				}
			}
			audioEncCtx->bit_rate = 128000;
			audioEncCtx->time_base = AVRational{1, audioEncCtx->sample_rate};
			audioEncCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
			if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
			{
				audioEncCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			}
			ret = avcodec_open2(audioEncCtx, audioEnc, nullptr);
			if (ret < 0)
			{
				char errbuf[AV_ERROR_MAX_STRING_SIZE];
				av_strerror(ret, errbuf, sizeof(errbuf));
				appendLogLine(QStringLiteral("[屏幕推流] 打开音频编码器失败: %1; sr=%2 ch=%3 fmt=%4")
					              .arg(QString::fromUtf8(errbuf))
					              .arg(audioEncCtx->sample_rate)
					              .arg(audioEncCtx->ch_layout.nb_channels)
					              .arg(static_cast<int>(audioEncCtx->sample_fmt)));
				exitCode = ret;
				setLastError(QStringLiteral("打开音频编码器失败"));
				goto cleanup;
			}
			outAudioStream = avformat_new_stream(ofmt, nullptr);
			if (!outAudioStream)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建输出音频流失败"));
				goto cleanup;
			}
			ret = avcodec_parameters_from_context(outAudioStream->codecpar, audioEncCtx);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("写入输出音频参数失败"));
				goto cleanup;
			}
			outAudioStream->codecpar->codec_tag = 0;
			outAudioStream->time_base = audioEncCtx->time_base;
			const AVChannelLayout* inLayout = nullptr;
			if (useWasapiAudio)
			{
				inLayout = wasapiAudio.channelLayout().nb_channels > 0 ? &wasapiAudio.channelLayout() : nullptr;
			}
			else
			{
				inLayout = audioDecCtx->ch_layout.nb_channels > 0 ? &audioDecCtx->ch_layout : nullptr;
			}
			AVChannelLayout defaultInLayout;
			if (!inLayout)
			{
				av_channel_layout_default(&defaultInLayout, 2);
				inLayout = &defaultInLayout;
			}
			const AVSampleFormat inSampleFmt = useWasapiAudio ? wasapiAudio.sampleFmt() : audioDecCtx->sample_fmt;
			const int inSampleRate2 = useWasapiAudio ? inSampleRate : audioDecCtx->sample_rate;
			ret = swr_alloc_set_opts2(&audioSwr, &audioEncCtx->ch_layout, audioEncCtx->sample_fmt, audioEncCtx->sample_rate,
			                          inLayout, inSampleFmt, inSampleRate2, 0, nullptr);
			if (!inLayout || inLayout == &defaultInLayout)
			{
				av_channel_layout_uninit(&defaultInLayout);
			}
			if (ret < 0 || !audioSwr)
			{
				exitCode = ret < 0 ? ret : AVERROR(EINVAL);
				setLastError(QStringLiteral("创建音频重采样器失败"));
				goto cleanup;
			}
			ret = swr_init(audioSwr);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("初始化音频重采样器失败"));
				goto cleanup;
			}
			const int fifoInitSamples = qMax(audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024, 1024) * 8;
			audioFifo = av_audio_fifo_alloc(audioEncCtx->sample_fmt, audioEncCtx->ch_layout.nb_channels, fifoInitSamples);
			if (!audioFifo)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建音频FIFO失败"));
				goto cleanup;
			}
			if ((audioDecCtx2 && audioPkt2 && audioDecFrame2) || useWasapiAudio2)
			{
				const AVChannelLayout* inLayout2 = nullptr;
				if (useWasapiAudio2)
				{
					inLayout2 = wasapiAudio2.channelLayout().nb_channels > 0 ? &wasapiAudio2.channelLayout() : nullptr;
				}
				else
				{
					inLayout2 = (audioDecCtx2 && audioDecCtx2->ch_layout.nb_channels > 0) ? &audioDecCtx2->ch_layout : nullptr;
				}
				AVChannelLayout defaultInLayout2;
				if (!inLayout2)
				{
					av_channel_layout_default(&defaultInLayout2, 2);
					inLayout2 = &defaultInLayout2;
				}
				const AVSampleFormat inSampleFmt2 = useWasapiAudio2 ? wasapiAudio2.sampleFmt() : audioDecCtx2->sample_fmt;
				const int inSampleRate3 = useWasapiAudio2
					                          ? (wasapiAudio2.sampleRate() > 0 ? wasapiAudio2.sampleRate() : audioEncCtx->sample_rate)
					                          : audioDecCtx2->sample_rate;
				if (swr_alloc_set_opts2(&audioSwr2, &audioEncCtx->ch_layout, audioEncCtx->sample_fmt, audioEncCtx->sample_rate,
				                        inLayout2, inSampleFmt2, inSampleRate3, 0, nullptr) >= 0 &&
				    audioSwr2 && swr_init(audioSwr2) >= 0)
				{
					audioFifo2 = av_audio_fifo_alloc(audioEncCtx->sample_fmt, audioEncCtx->ch_layout.nb_channels, fifoInitSamples);
				}
				if (!inLayout2 || inLayout2 == &defaultInLayout2)
				{
					av_channel_layout_uninit(&defaultInLayout2);
				}
				if (!audioFifo2)
				{
					appendLogLine(QStringLiteral("[摄像头预览推流] 第二音频源重采样/缓存初始化失败，降级单音频"));
					if (audioSwr2)
					{
						swr_free(&audioSwr2);
					}
				}
			}
		}
	}

	if (!(ofmt->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open2(&ofmt->pb, outPath, AVIO_FLAG_WRITE, nullptr, nullptr);
		if (ret < 0)
		{
			exitCode = ret;
			setLastError(QStringLiteral("打开推流输出地址失败"));
			goto cleanup;
		}
	}
	ret = avformat_write_header(ofmt, nullptr);
	if (ret < 0)
	{
		exitCode = ret;
		setLastError(QStringLiteral("写入推流头失败"));
		goto cleanup;
	}
	wroteHeader = true;

	encFrame->format = encCtx->pix_fmt;
	encFrame->width = encCtx->width;
	encFrame->height = encCtx->height;
	ret = av_frame_get_buffer(encFrame, 32);
	if (ret < 0)
	{
		exitCode = ret;
		setLastError(QStringLiteral("分配编码帧缓冲失败"));
		goto cleanup;
	}
	sws = sws_getContext(decCtx->width, decCtx->height, decCtx->pix_fmt, encCtx->width, encCtx->height, encCtx->pix_fmt,
	                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
	if (!sws)
	{
		exitCode = AVERROR(EINVAL);
		setLastError(QStringLiteral("创建像素格式转换器失败"));
		goto cleanup;
	}
	if (audioActive && outAudioStream && audioSwr && audioEncCtx && audioOutPkt && audioEncFrame && audioFifo)
	{
		audioThreadStarted = true;
		audioWorker = std::thread([&]() {
			const int encSamples = audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024;
			const bool dualMixEnabled = audioFifo2 && audioSwr2 &&
				(useWasapiAudio2 || (audioIfmt2 && audioPkt2 && audioDecCtx2 && audioDecFrame2 && audioIndex2 >= 0));
			const auto writeEncodedPackets = [&]() -> bool {
				while (avcodec_receive_packet(audioEncCtx, audioOutPkt) == 0)
				{
					av_packet_rescale_ts(audioOutPkt, audioEncCtx->time_base, outAudioStream->time_base);
					audioOutPkt->stream_index = outAudioStream->index;
					int aw = 0;
					{
						std::lock_guard<std::mutex> lk(muxWriteMutex);
						aw = av_interleaved_write_frame(ofmt, audioOutPkt);
					}
					if (aw < 0)
					{
						audioThreadErr.store(aw, std::memory_order_relaxed);
						audioThreadStop.store(true, std::memory_order_relaxed);
						av_packet_unref(audioOutPkt);
						return false;
					}
					av_packet_unref(audioOutPkt);
				}
				return true;
			};
			const auto enqueueFromDshow =
				[&](AVFormatContext* ifmt, AVPacket* pkt, AVCodecContext* decCtx, AVFrame* decFrame, int index, SwrContext* swr,
				    AVAudioFifo* fifo) -> bool {
				const int audioRet = av_read_frame(ifmt, pkt);
				if (audioRet < 0)
				{
					return false;
				}
				bool wrote = false;
				if (pkt->stream_index == index)
				{
					const int decSendRet = avcodec_send_packet(decCtx, pkt);
					if (decSendRet >= 0)
					{
						while (avcodec_receive_frame(decCtx, decFrame) == 0)
						{
							const int dstSamples = av_rescale_rnd(
								swr_get_delay(swr, decCtx->sample_rate) + decFrame->nb_samples,
								audioEncCtx->sample_rate,
								decCtx->sample_rate,
								AV_ROUND_UP);
							if (dstSamples <= 0)
							{
								av_frame_unref(decFrame);
								continue;
							}
							uint8_t** dstData = nullptr;
							int dstLinesize = 0;
							if (av_samples_alloc_array_and_samples(&dstData, &dstLinesize, audioEncCtx->ch_layout.nb_channels, dstSamples,
							                                       audioEncCtx->sample_fmt, 0) >= 0)
							{
								const int converted = swr_convert(swr, dstData, dstSamples,
								                                  (const uint8_t* const*)decFrame->data, decFrame->nb_samples);
								if (converted > 0)
								{
									const int need = av_audio_fifo_size(fifo) + converted;
									if (av_audio_fifo_realloc(fifo, need) >= 0 &&
									    av_audio_fifo_write(fifo, reinterpret_cast<void**>(dstData), converted) >= converted)
									{
										wrote = true;
									}
								}
								av_freep(&dstData[0]);
								av_freep(&dstData);
							}
							av_frame_unref(decFrame);
						}
					}
				}
				av_packet_unref(pkt);
				return wrote;
			};
			const auto enqueueFromWasapi = [&](fplayer::windows_api::WasapiLoopbackCapture& capture, SwrContext* swr, AVAudioFifo* fifo) -> bool {
				std::vector<uint8_t> captured;
				int capturedSamples = 0;
				if (!capture.readInterleaved(captured, capturedSamples) || capturedSamples <= 0)
				{
					return false;
				}
				const uint8_t* inData[1] = {captured.data()};
				const int dstSamples = av_rescale_rnd(
					swr_get_delay(swr, capture.sampleRate()) + capturedSamples,
					audioEncCtx->sample_rate,
					capture.sampleRate(),
					AV_ROUND_UP);
				if (dstSamples <= 0)
				{
					return false;
				}
				uint8_t** dstData = nullptr;
				int dstLinesize = 0;
				if (av_samples_alloc_array_and_samples(&dstData, &dstLinesize, audioEncCtx->ch_layout.nb_channels, dstSamples,
				                                       audioEncCtx->sample_fmt, 0) < 0)
				{
					return false;
				}
				bool wrote = false;
				const int converted = swr_convert(swr, dstData, dstSamples, inData, capturedSamples);
				if (converted > 0)
				{
					const int need = av_audio_fifo_size(fifo) + converted;
					if (av_audio_fifo_realloc(fifo, need) >= 0 &&
					    av_audio_fifo_write(fifo, reinterpret_cast<void**>(dstData), converted) >= converted)
					{
						wrote = true;
					}
				}
				av_freep(&dstData[0]);
				av_freep(&dstData);
				return wrote;
			};
			while (!m_stopRequest.load(std::memory_order_relaxed) && !audioThreadStop.load(std::memory_order_relaxed))
			{
				bool gotMain = false;
				bool gotSecond = false;
				if (useWasapiAudio)
				{
					gotMain = enqueueFromWasapi(wasapiAudio, audioSwr, audioFifo);
				}
				else if (audioIfmt && audioPkt && audioDecCtx)
				{
					gotMain = enqueueFromDshow(audioIfmt, audioPkt, audioDecCtx, audioDecFrame, audioIndex, audioSwr, audioFifo);
				}
				if (dualMixEnabled)
				{
					if (useWasapiAudio2)
					{
						gotSecond = enqueueFromWasapi(wasapiAudio2, audioSwr2, audioFifo2);
					}
					else if (audioIfmt2 && audioPkt2 && audioDecCtx2 && audioDecFrame2 && audioIndex2 >= 0)
					{
						gotSecond = enqueueFromDshow(audioIfmt2, audioPkt2, audioDecCtx2, audioDecFrame2, audioIndex2, audioSwr2, audioFifo2);
					}
				}
				while (av_audio_fifo_size(audioFifo) >= encSamples)
				{
					if (dualMixEnabled && av_audio_fifo_size(audioFifo2) < encSamples)
					{
						break;
					}
					if (!fillAudioEncFrameFromFifos(audioEncFrame, audioEncCtx, audioFifo, audioFifo2, encSamples, dualMixEnabled))
					{
						break;
					}
					audioEncFrame->pts = audioPts;
					audioPts += encSamples;
					if (avcodec_send_frame(audioEncCtx, audioEncFrame) < 0)
					{
						break;
					}
					if (!writeEncodedPackets())
					{
						return;
					}
				}
				if (!gotMain && !gotSecond)
				{
					QThread::msleep(2);
				}
			}
			avcodec_send_frame(audioEncCtx, nullptr);
			(void)writeEncodedPackets();
		});
	}
	while (!m_stopRequest.load(std::memory_order_relaxed))
	{
		const int threadAudioErr = audioThreadErr.load(std::memory_order_relaxed);
		if (threadAudioErr < 0)
		{
			exitCode = threadAudioErr;
			setLastError(QStringLiteral("写入音频包失败"));
			break;
		}
		ret = av_read_frame(ifmt, inPkt);
		if (ret < 0)
		{
			if (ret == AVERROR_EOF)
			{
				break;
			}
			if (ret == AVERROR(EAGAIN))
			{
				continue;
			}
			exitCode = ret;
			setLastError(QStringLiteral("读取桌面帧失败"));
			break;
		}
		if (inPkt->stream_index != videoIndex)
		{
			av_packet_unref(inPkt);
			continue;
		}
		ret = avcodec_send_packet(decCtx, inPkt);
		av_packet_unref(inPkt);
		if (ret < 0)
		{
			continue;
		}
		while (avcodec_receive_frame(decCtx, decFrame) == 0)
		{
			av_frame_make_writable(encFrame);
			sws_scale(sws, decFrame->data, decFrame->linesize, 0, decCtx->height, encFrame->data, encFrame->linesize);
			int64_t pts = decFrame->best_effort_timestamp;
			if (pts != AV_NOPTS_VALUE)
			{
				pts = av_rescale_q(pts, ifmt->streams[videoIndex]->time_base, encCtx->time_base);
				if (pts <= lastPts)
				{
					pts = lastPts + 1;
				}
			}
			else
			{
				pts = framePts++;
			}
			lastPts = pts;
			encFrame->pts = pts;
			ret = avcodec_send_frame(encCtx, encFrame);
			if (ret < 0)
			{
				continue;
			}
			while (avcodec_receive_packet(encCtx, outPkt) == 0)
			{
				av_packet_rescale_ts(outPkt, encCtx->time_base, ofmt->streams[0]->time_base);
				outPkt->stream_index = 0;
				{
					std::lock_guard<std::mutex> lk(muxWriteMutex);
					av_interleaved_write_frame(ofmt, outPkt);
				}
				av_packet_unref(outPkt);
			}
			av_frame_unref(decFrame);
		}
	}
	audioThreadStop.store(true, std::memory_order_relaxed);
	if (audioThreadStarted && audioWorker.joinable())
	{
		audioWorker.join();
	}

	avcodec_send_frame(encCtx, nullptr);
	while (avcodec_receive_packet(encCtx, outPkt) == 0)
	{
		av_packet_rescale_ts(outPkt, encCtx->time_base, ofmt->streams[0]->time_base);
		outPkt->stream_index = 0;
		{
			std::lock_guard<std::mutex> lk(muxWriteMutex);
			av_interleaved_write_frame(ofmt, outPkt);
		}
		av_packet_unref(outPkt);
	}
	if (wroteHeader)
	{
		av_write_trailer(ofmt);
	}

cleanup:
	if (sws)
	{
		sws_freeContext(sws);
	}
	if (decFrame)
	{
		av_frame_free(&decFrame);
	}
	if (inPkt)
	{
		av_packet_free(&inPkt);
	}
	if (encFrame)
	{
		av_frame_free(&encFrame);
	}
	if (outPkt)
	{
		av_packet_free(&outPkt);
	}
	if (encCtx)
	{
		avcodec_free_context(&encCtx);
	}
	if (decCtx)
	{
		avcodec_free_context(&decCtx);
	}
	if (ofmt)
	{
		if (!(ofmt->oformat->flags & AVFMT_NOFILE) && ofmt->pb)
		{
			avio_closep(&ofmt->pb);
		}
		avformat_free_context(ofmt);
	}
	if (ifmt)
	{
		avformat_close_input(&ifmt);
	}
	if (audioPkt)
	{
		av_packet_free(&audioPkt);
	}
	if (audioOutPkt)
	{
		av_packet_free(&audioOutPkt);
	}
	if (audioPkt2)
	{
		av_packet_free(&audioPkt2);
	}
	if (audioDecFrame)
	{
		av_frame_free(&audioDecFrame);
	}
	if (audioDecFrame2)
	{
		av_frame_free(&audioDecFrame2);
	}
	if (audioEncFrame)
	{
		av_frame_free(&audioEncFrame);
	}
	if (audioSwr)
	{
		swr_free(&audioSwr);
	}
	if (audioSwr2)
	{
		swr_free(&audioSwr2);
	}
	if (audioFifo)
	{
		av_audio_fifo_free(audioFifo);
		audioFifo = nullptr;
	}
	if (audioFifo2)
	{
		av_audio_fifo_free(audioFifo2);
		audioFifo2 = nullptr;
	}
	if (audioEncCtx)
	{
		avcodec_free_context(&audioEncCtx);
	}
	if (audioDecCtx)
	{
		avcodec_free_context(&audioDecCtx);
	}
	if (audioDecCtx2)
	{
		avcodec_free_context(&audioDecCtx2);
	}
	if (audioIfmt)
	{
		avformat_close_input(&audioIfmt);
	}
	if (audioIfmt2)
	{
		avformat_close_input(&audioIfmt2);
	}
	{
		QMutexLocker locker(&m_mutex);
		m_lastExitCode = exitCode;
	}
	m_completedSession.store(true, std::memory_order_relaxed);
	m_running.store(false, std::memory_order_relaxed);
}

void fplayer::StreamFFmpeg::pushScreenPreviewLoop(const QString& outputUrl, const QString& captureSpec)
{
	AVFormatContext* ofmt = nullptr;
	AVCodecContext* encCtx = nullptr;
	AVPacket* outPkt = nullptr;
	AVFrame* encFrame = nullptr;
	SwsContext* sws = nullptr;
	int exitCode = 0;
	int ret = 0;
	bool wroteHeader = false;
	uint64_t lastSerial = 0;
	int srcWidth = 0;
	int srcHeight = 0;
	int64_t lastPts = -1;
	bool writeFailed = false;
	auto nextEncodeAt = std::chrono::steady_clock::time_point{};
	auto startClock = std::chrono::steady_clock::time_point{};
	auto avStatWindowStart = std::chrono::steady_clock::time_point{};
	int avStatVideoPkts = 0;
	fplayer::ScreenFrame frame;

	const QByteArray outUtf8 = outputUrl.toUtf8();
	const char* outPath = outUtf8.constData();
	const CaptureParams params = parseCaptureParams(captureSpec);
	const int targetFps = qMax(1, params.fps);
	const int frameIntervalMs = qMax(1, 1000 / targetFps);

	avdevice_register_all();
	AVFormatContext* audioIfmt = nullptr;
	AVCodecContext* audioDecCtx = nullptr;
	AVFormatContext* audioIfmt2 = nullptr;
	AVCodecContext* audioDecCtx2 = nullptr;
	AVCodecContext* audioEncCtx = nullptr;
	SwrContext* audioSwr = nullptr;
	SwrContext* audioSwr2 = nullptr;
	AVAudioFifo* audioFifo = nullptr;
	AVAudioFifo* audioFifo2 = nullptr;
	AVPacket* audioPkt = nullptr;
	AVPacket* audioPkt2 = nullptr;
	AVPacket* audioOutPkt = nullptr;
	AVFrame* audioDecFrame = nullptr;
	AVFrame* audioDecFrame2 = nullptr;
	AVFrame* audioEncFrame = nullptr;
	int audioIndex = -1;
	int audioIndex2 = -1;
	AVStream* outAudioStream = nullptr;
	int64_t audioPts = 0;
	fplayer::windows_api::WasapiLoopbackCapture wasapiAudio;
	fplayer::windows_api::WasapiLoopbackCapture wasapiAudio2;
	bool useWasapiAudio = false;
	bool useWasapiAudio2 = false;
	std::atomic<int> avStatAudioPkts{0};
	std::atomic<int> avStatAudioMainSamples{0};
	std::atomic<int> avStatAudioSecondSamples{0};
	std::atomic<int> avStatAudioMainReads{0};
	std::atomic<int> avStatAudioSecondReads{0};
	std::mutex muxWriteMutex;
	std::atomic<bool> audioThreadStop{false};
	std::atomic<int> audioThreadErr{0};
	std::thread audioWorker;
	bool audioThreadStarted = false;

	const QString requestedAudioOut = params.audioOutputSource.trimmed();
	const QString requestedAudioIn = params.audioInputSource.trimmed();
	const bool hasIn = !requestedAudioIn.isEmpty() && requestedAudioIn != QStringLiteral("off");
	const bool hasOut = !requestedAudioOut.isEmpty() && requestedAudioOut != QStringLiteral("off");
	const bool dualAudioRequested = hasIn && hasOut && requestedAudioIn != requestedAudioOut;
	const QString audioSourcePrimary = dualAudioRequested ? requestedAudioIn : QString();
	const QString audioSourceSecondary = dualAudioRequested ? requestedAudioOut : QString();
	const QString resolvedAudioSource = hasOut ? requestedAudioOut : (hasIn ? requestedAudioIn : QString());
	const bool enableAudio = !resolvedAudioSource.isEmpty() && resolvedAudioSource != QStringLiteral("off");
	bool audioActive = enableAudio;
	if (audioActive)
	{
		QString openedAudioDevice;
		QString openDetail;
		const QString selectedPrimarySource = dualAudioRequested ? audioSourcePrimary : resolvedAudioSource;
		const bool primaryFromOutputSelection = !dualAudioRequested && hasOut;
		if (!fplayer::windows_api::openDshowAudioInputWithFallback(selectedPrimarySource, m_stopRequest, audioIfmt, openedAudioDevice, openDetail))
		{
			QString wasapiErr;
			if ((primaryFromOutputSelection || selectedPrimarySource == QStringLiteral("system")) && wasapiAudio.init(wasapiErr))
			{
				useWasapiAudio = true;
				openedAudioDevice = QStringLiteral("native-wasapi-loopback(default)");
				appendLogLine(QStringLiteral("[屏幕推流] dshow音频不可用，已切换系统API回采: %1").arg(openDetail));
			}
			else
			{
				appendLogLine(QStringLiteral("[屏幕推流] 音频采集初始化失败，已自动降级为无声推流: %1; wasapi-native=%2")
					              .arg(openDetail)
					              .arg(wasapiErr));
				audioActive = false;
			}
		}
		if (audioActive)
		{
			if (audioIfmt)
			{
				audioIfmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
				audioIfmt->interrupt_callback.opaque = &m_stopRequest;
			}
			appendLogLine(QStringLiteral("[屏幕推流] 音频设备=%1").arg(openedAudioDevice));
		}
		if (!audioActive || (!audioIfmt && !useWasapiAudio))
		{
			goto audio_preview_init_done;
		}
		if (audioIfmt)
		{
			ret = avformat_find_stream_info(audioIfmt, nullptr);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("读取音频流信息失败"));
				goto cleanup;
			}
			for (unsigned i = 0; i < audioIfmt->nb_streams; ++i)
			{
				if (audioIfmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
				{
					audioIndex = static_cast<int>(i);
					break;
				}
			}
			if (audioIndex < 0)
			{
				exitCode = AVERROR_STREAM_NOT_FOUND;
				setLastError(QStringLiteral("未找到可用音频流"));
				goto cleanup;
			}
			const AVCodecParameters* inAudioPar = audioIfmt->streams[audioIndex]->codecpar;
			const AVCodec* audioDec = avcodec_find_decoder(inAudioPar->codec_id);
			if (!audioDec)
			{
				exitCode = AVERROR_DECODER_NOT_FOUND;
				setLastError(QStringLiteral("未找到音频解码器"));
				goto cleanup;
			}
			audioDecCtx = avcodec_alloc_context3(audioDec);
			if (!audioDecCtx)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建音频解码器上下文失败"));
				goto cleanup;
			}
			ret = avcodec_parameters_to_context(audioDecCtx, inAudioPar);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("加载音频输入参数失败"));
				goto cleanup;
			}
			ret = avcodec_open2(audioDecCtx, audioDec, nullptr);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("打开音频解码器失败"));
				goto cleanup;
			}
		}
		audioPkt = av_packet_alloc();
		audioPkt2 = av_packet_alloc();
		audioOutPkt = av_packet_alloc();
		audioDecFrame = av_frame_alloc();
		audioDecFrame2 = av_frame_alloc();
		audioEncFrame = av_frame_alloc();
		if (!audioPkt || !audioOutPkt || !audioDecFrame || !audioEncFrame || !audioPkt2 || !audioDecFrame2)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("分配音频编码资源失败"));
			goto cleanup;
		}
		if (audioActive && dualAudioRequested)
		{
			QString openedAudioDevice2;
			QString openDetail2;
			if (!fplayer::windows_api::openDshowAudioInputWithFallback(audioSourceSecondary, m_stopRequest, audioIfmt2, openedAudioDevice2, openDetail2))
			{
				QString wasapiErr2;
				if (wasapiAudio2.init(wasapiErr2))
				{
					useWasapiAudio2 = true;
					openedAudioDevice2 = QStringLiteral("native-wasapi-loopback(default)");
				}
				else
				{
					appendLogLine(QStringLiteral("[屏幕推流] 第二音频源不可用，降级单音频: %1; wasapi-native=%2")
						              .arg(openDetail2)
						              .arg(wasapiErr2));
				}
			}
			if (audioIfmt2)
			{
				audioIfmt2->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
				audioIfmt2->interrupt_callback.opaque = &m_stopRequest;
				const int ret2 = avformat_find_stream_info(audioIfmt2, nullptr);
				if (ret2 >= 0)
				{
					for (unsigned i = 0; i < audioIfmt2->nb_streams; ++i)
					{
						if (audioIfmt2->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
						{
							audioIndex2 = static_cast<int>(i);
							break;
						}
					}
					if (audioIndex2 >= 0)
					{
						const AVCodecParameters* inAudioPar2 = audioIfmt2->streams[audioIndex2]->codecpar;
						const AVCodec* audioDec2 = avcodec_find_decoder(inAudioPar2->codec_id);
						if (audioDec2)
						{
							audioDecCtx2 = avcodec_alloc_context3(audioDec2);
							if (!(audioDecCtx2 && avcodec_parameters_to_context(audioDecCtx2, inAudioPar2) >= 0 &&
							      avcodec_open2(audioDecCtx2, audioDec2, nullptr) >= 0))
							{
								if (audioDecCtx2)
								{
									avcodec_free_context(&audioDecCtx2);
								}
								audioIndex2 = -1;
							}
						}
					}
				}
			}
		}
	}
audio_preview_init_done:

	fplayer::ScreenFrameBus::instance().setPublishTargetSize(0, 0);
	outPkt = av_packet_alloc();
	encFrame = av_frame_alloc();
	if (!outPkt || !encFrame)
	{
		exitCode = AVERROR(ENOMEM);
		setLastError(QStringLiteral("分配编码资源失败"));
		goto cleanup;
	}

	ret = avformat_alloc_output_context2(&ofmt, nullptr, "flv", outPath);
	if (ret < 0 || !ofmt)
	{
		exitCode = ret ? ret : AVERROR_UNKNOWN;
		setLastError(QStringLiteral("创建推流输出上下文失败"));
		goto cleanup;
	}
	// Live push: reduce mux/IO buffering so receiver gets playable data quickly.
	ofmt->flags |= AVFMT_FLAG_FLUSH_PACKETS;
	ofmt->max_interleave_delta = 0;
	ofmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
	ofmt->interrupt_callback.opaque = &m_stopRequest;

	// 等待当前屏幕预览帧可用，避免推流线程空跑。
	for (int i = 0; i < 300 && !m_stopRequest.load(std::memory_order_relaxed); ++i)
	{
		if (fplayer::ScreenFrameBus::instance().snapshotIfNew(lastSerial, frame) && frame.width > 0 && frame.height > 0)
		{
			break;
		}
		QThread::msleep(10);
	}
	if (!frame.valid || frame.width <= 0 || frame.height <= 0)
	{
		exitCode = AVERROR(EINVAL);
		setLastError(QStringLiteral("当前屏幕预览无可用帧，无法开始推流"));
		goto cleanup;
	}
	lastSerial = frame.serial;

	{
		const QList<VideoEncoderChoice> candidates = pickVideoEncoderCandidates(params.videoEncoder);
		if (candidates.isEmpty())
		{
			exitCode = AVERROR_ENCODER_NOT_FOUND;
			setLastError(QStringLiteral("未找到可用视频编码器（当前请求=%1）。CPU 模式需要 libx264（或 libopenh264）。").arg(params.videoEncoder));
			goto cleanup;
		}
		VideoEncoderChoice encChoice;
		QString lastOpenErrDetail;
		int lastOpenRet = AVERROR_UNKNOWN;
		bool opened = false;
		for (const VideoEncoderChoice& cand : candidates)
		{
			if (!cand.codec)
			{
				continue;
			}
			if (encCtx)
			{
				avcodec_free_context(&encCtx);
				encCtx = nullptr;
			}
			encCtx = avcodec_alloc_context3(cand.codec);
			if (!encCtx)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建编码器上下文失败"));
				goto cleanup;
			}
			const AVCodec* enc = cand.codec;
			const int outW = params.outWidth > 0 ? params.outWidth : frame.width;
			const int outH = params.outHeight > 0 ? params.outHeight : frame.height;
			encCtx->width = qMax(2, outW) & ~1;
			encCtx->height = qMax(2, outH) & ~1;
			encCtx->pix_fmt = pickEncoderPixelFormat(enc, cand.isHardware);
			encCtx->time_base = AVRational{1, targetFps};
			encCtx->framerate = AVRational{targetFps, 1};
			encCtx->gop_size = targetFps * 2;
			encCtx->max_b_frames = 0;
			const int bitrateKbps = params.bitrateKbps > 0
				                        ? params.bitrateKbps
				                        : estimateBitrateKbps(encCtx->width, encCtx->height, targetFps);
			encCtx->bit_rate = static_cast<int64_t>(bitrateKbps) * 1000;
			encCtx->rc_max_rate = encCtx->bit_rate;
			encCtx->rc_buffer_size = encCtx->bit_rate * 2;
			if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
			{
				encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			}
			if (cand.isHardware && encCtx->priv_data)
			{
				av_opt_set(encCtx->priv_data, "preset", "p1", 0);
				av_opt_set(encCtx->priv_data, "tune", "ll", 0);
				av_opt_set(encCtx->priv_data, "rc", "cbr", 0);
				av_opt_set(encCtx->priv_data, "zerolatency", "1", 0);
			}
			else if (enc->id == AV_CODEC_ID_H264 && encCtx->priv_data)
			{
				av_opt_set(encCtx->priv_data, "preset", "ultrafast", 0);
				av_opt_set(encCtx->priv_data, "tune", "zerolatency", 0);
			}
			ret = avcodec_open2(encCtx, enc, nullptr);
			if (ret < 0)
			{
				char errbuf[AV_ERROR_MAX_STRING_SIZE];
				av_strerror(ret, errbuf, sizeof(errbuf));
				lastOpenRet = ret;
				lastOpenErrDetail = QStringLiteral("%1: %2 (pix_fmt=%3)")
					                    .arg(cand.name.isEmpty() ? QString::fromLatin1(enc->name ? enc->name : "?") : cand.name)
					                    .arg(QString::fromUtf8(errbuf))
					                    .arg(static_cast<int>(encCtx->pix_fmt));
				appendLogLine(QStringLiteral("[屏幕推流] 编码器打开失败，%1").arg(lastOpenErrDetail));
				continue;
			}
			encChoice = cand;
			opened = true;
			break;
		}
		if (!opened)
		{
			exitCode = lastOpenRet;
			setLastError(QStringLiteral("所有备选编码器均无法打开（最后: %1）").arg(lastOpenErrDetail));
			goto cleanup;
		}
		const AVCodec* enc = encChoice.codec;
		const int bitrateKbpsLog = params.bitrateKbps > 0
			                           ? params.bitrateKbps
			                           : estimateBitrateKbps(encCtx->width, encCtx->height, targetFps);
		AVStream* outStream = avformat_new_stream(ofmt, nullptr);
		if (!outStream)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("创建输出视频流失败"));
			goto cleanup;
		}
		outStream->time_base = encCtx->time_base;
		avcodec_parameters_from_context(outStream->codecpar, encCtx);
		if (audioActive && ((audioIfmt && audioIndex >= 0) || useWasapiAudio))
		{
			const AVCodec* audioEnc = avcodec_find_encoder(AV_CODEC_ID_AAC);
			if (!audioEnc)
			{
				exitCode = AVERROR_ENCODER_NOT_FOUND;
				setLastError(QStringLiteral("未找到 AAC 音频编码器，无法推送声音"));
				goto cleanup;
			}
			audioEncCtx = avcodec_alloc_context3(audioEnc);
			if (!audioEncCtx)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建音频编码器上下文失败"));
				goto cleanup;
			}
			const int inSampleRate = useWasapiAudio
				                         ? (wasapiAudio.sampleRate() > 0 ? wasapiAudio.sampleRate() : 48000)
				                         : (audioDecCtx && audioDecCtx->sample_rate > 0 ? audioDecCtx->sample_rate : 48000);
			audioEncCtx->sample_rate = inSampleRate;
			if (audioEnc->supported_samplerates)
			{
				int bestRate = audioEnc->supported_samplerates[0];
				int bestDiff = qAbs(bestRate - audioEncCtx->sample_rate);
				for (const int* p = audioEnc->supported_samplerates; *p != 0; ++p)
				{
					const int diff = qAbs(*p - audioEncCtx->sample_rate);
					if (diff < bestDiff)
					{
						bestRate = *p;
						bestDiff = diff;
					}
				}
				audioEncCtx->sample_rate = bestRate;
			}
			const int srcChannels = useWasapiAudio
				                        ? wasapiAudio.channelLayout().nb_channels
				                        : (audioDecCtx ? audioDecCtx->ch_layout.nb_channels : 0);
			const int targetChannels = (srcChannels == 1) ? 1 : 2;
			av_channel_layout_uninit(&audioEncCtx->ch_layout);
			av_channel_layout_default(&audioEncCtx->ch_layout, targetChannels);
			if (audioEnc->ch_layouts && audioEnc->ch_layouts[0].nb_channels > 0)
			{
				const AVChannelLayout* best = &audioEnc->ch_layouts[0];
				int bestDiff = qAbs(best->nb_channels - targetChannels);
				for (const AVChannelLayout* p = audioEnc->ch_layouts; p && p->nb_channels > 0; ++p)
				{
					const int diff = qAbs(p->nb_channels - targetChannels);
					if (diff < bestDiff)
					{
						best = p;
						bestDiff = diff;
					}
				}
				av_channel_layout_uninit(&audioEncCtx->ch_layout);
				if (av_channel_layout_copy(&audioEncCtx->ch_layout, best) < 0)
				{
					av_channel_layout_default(&audioEncCtx->ch_layout, targetChannels);
				}
			}
			if (audioEncCtx->ch_layout.nb_channels <= 0)
			{
				av_channel_layout_uninit(&audioEncCtx->ch_layout);
				av_channel_layout_default(&audioEncCtx->ch_layout, 2);
			}
			audioEncCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
			if (audioEnc->sample_fmts)
			{
				bool fltpSupported = false;
				for (const AVSampleFormat* p = audioEnc->sample_fmts; *p != AV_SAMPLE_FMT_NONE; ++p)
				{
					if (*p == AV_SAMPLE_FMT_FLTP)
					{
						fltpSupported = true;
						break;
					}
				}
				if (!fltpSupported)
				{
					audioEncCtx->sample_fmt = audioEnc->sample_fmts[0];
				}
			}
			audioEncCtx->bit_rate = 128000;
			audioEncCtx->time_base = AVRational{1, audioEncCtx->sample_rate};
			audioEncCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
			if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
			{
				audioEncCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			}
			ret = avcodec_open2(audioEncCtx, audioEnc, nullptr);
			if (ret < 0)
			{
				char errbuf[AV_ERROR_MAX_STRING_SIZE];
				av_strerror(ret, errbuf, sizeof(errbuf));
				appendLogLine(QStringLiteral("[屏幕推流] 打开音频编码器失败: %1; sr=%2 ch=%3 fmt=%4")
					              .arg(QString::fromUtf8(errbuf))
					              .arg(audioEncCtx->sample_rate)
					              .arg(audioEncCtx->ch_layout.nb_channels)
					              .arg(static_cast<int>(audioEncCtx->sample_fmt)));
				exitCode = ret;
				setLastError(QStringLiteral("打开音频编码器失败"));
				goto cleanup;
			}
			outAudioStream = avformat_new_stream(ofmt, nullptr);
			if (!outAudioStream)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建输出音频流失败"));
				goto cleanup;
			}
			ret = avcodec_parameters_from_context(outAudioStream->codecpar, audioEncCtx);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("写入输出音频参数失败"));
				goto cleanup;
			}
			outAudioStream->codecpar->codec_tag = 0;
			outAudioStream->time_base = audioEncCtx->time_base;
			const AVChannelLayout* inLayout = nullptr;
			if (useWasapiAudio)
			{
				inLayout = wasapiAudio.channelLayout().nb_channels > 0 ? &wasapiAudio.channelLayout() : nullptr;
			}
			else
			{
				inLayout = audioDecCtx->ch_layout.nb_channels > 0 ? &audioDecCtx->ch_layout : nullptr;
			}
			AVChannelLayout defaultInLayout;
			if (!inLayout)
			{
				av_channel_layout_default(&defaultInLayout, 2);
				inLayout = &defaultInLayout;
			}
			const AVSampleFormat inSampleFmt = useWasapiAudio ? wasapiAudio.sampleFmt() : audioDecCtx->sample_fmt;
			const int inSampleRate2 = useWasapiAudio ? inSampleRate : audioDecCtx->sample_rate;
			ret = swr_alloc_set_opts2(&audioSwr, &audioEncCtx->ch_layout, audioEncCtx->sample_fmt, audioEncCtx->sample_rate,
			                          inLayout, inSampleFmt, inSampleRate2, 0, nullptr);
			if (!inLayout || inLayout == &defaultInLayout)
			{
				av_channel_layout_uninit(&defaultInLayout);
			}
			if (ret < 0 || !audioSwr)
			{
				exitCode = ret < 0 ? ret : AVERROR(EINVAL);
				setLastError(QStringLiteral("创建音频重采样器失败"));
				goto cleanup;
			}
			ret = swr_init(audioSwr);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("初始化音频重采样器失败"));
				goto cleanup;
			}
			const int fifoInitSamples = qMax(audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024, 1024) * 8;
			audioFifo = av_audio_fifo_alloc(audioEncCtx->sample_fmt, audioEncCtx->ch_layout.nb_channels, fifoInitSamples);
			if (!audioFifo)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建音频FIFO失败"));
				goto cleanup;
			}
			if ((audioDecCtx2 && audioIndex2 >= 0) || useWasapiAudio2)
			{
				const AVChannelLayout* inLayout2 = nullptr;
				if (useWasapiAudio2)
				{
					inLayout2 = wasapiAudio2.channelLayout().nb_channels > 0 ? &wasapiAudio2.channelLayout() : nullptr;
				}
				else
				{
					inLayout2 = audioDecCtx2->ch_layout.nb_channels > 0 ? &audioDecCtx2->ch_layout : nullptr;
				}
				AVChannelLayout defaultInLayout2;
				if (!inLayout2)
				{
					av_channel_layout_default(&defaultInLayout2, 2);
					inLayout2 = &defaultInLayout2;
				}
				const AVSampleFormat inSampleFmt2 = useWasapiAudio2 ? wasapiAudio2.sampleFmt() : audioDecCtx2->sample_fmt;
				const int inSampleRate3 = useWasapiAudio2 ? qMax(1, wasapiAudio2.sampleRate()) : audioDecCtx2->sample_rate;
				const int ret2 = swr_alloc_set_opts2(&audioSwr2, &audioEncCtx->ch_layout, audioEncCtx->sample_fmt, audioEncCtx->sample_rate,
				                                     inLayout2, inSampleFmt2, inSampleRate3, 0, nullptr);
				if (!inLayout2 || inLayout2 == &defaultInLayout2)
				{
					av_channel_layout_uninit(&defaultInLayout2);
				}
				if (ret2 >= 0 && audioSwr2 && swr_init(audioSwr2) >= 0)
				{
					audioFifo2 = av_audio_fifo_alloc(audioEncCtx->sample_fmt, audioEncCtx->ch_layout.nb_channels, fifoInitSamples);
				}
				if (!audioFifo2)
				{
					if (audioSwr2)
					{
						swr_free(&audioSwr2);
					}
					appendLogLine(QStringLiteral("[屏幕推流] 第二音频源重采样初始化失败，降级单音频"));
				}
			}
		}
	appendLogLine(QStringLiteral("[屏幕推流] 来源=DXGI预览帧 输入=%1x%2 输出=%3x%4 FPS=%5 码率=%6kbps")
		              .arg(frame.width)
		              .arg(frame.height)
		              .arg(encCtx->width)
		              .arg(encCtx->height)
		              .arg(targetFps)
		              .arg(bitrateKbpsLog));
		appendLogLine(QStringLiteral("[屏幕推流] 编码器=%1（%2）")
		              .arg(encChoice.name.isEmpty() ? QString::fromLatin1(enc->name ? enc->name : "unknown") : encChoice.name)
		              .arg(encChoice.isHardware ? QStringLiteral("硬件") : QStringLiteral("软件")));
	appendLogLine(QStringLiteral("[屏幕推流] 编码器硬件帧能力=%1").arg(encoderHwFramesHint(enc)));
	appendLogLine(QStringLiteral("[屏幕推流] 编码像素格式=%1").arg(static_cast<int>(encCtx->pix_fmt)));
		appendLogLine(QStringLiteral("[屏幕推流] 编码节流=严格FPS 同分辨率=%1")
		              .arg((frame.width == encCtx->width && frame.height == encCtx->height) ? QStringLiteral("直拷贝") : QStringLiteral("缩放")));
		fplayer::ScreenFrameBus::instance().setPublishTargetSize(encCtx->width, encCtx->height);
		appendLogLine(QStringLiteral("[屏幕推流] DXGI发布尺寸=%1x%2（采集阶段直接缩放）").arg(encCtx->width).arg(encCtx->height));
		if (audioActive)
		{
			appendLogLine(QStringLiteral("[屏幕推流] 音频来源 in=%1 out=%2 picked=%3 dual=%4")
				              .arg(params.audioInputSource)
				              .arg(params.audioOutputSource)
				              .arg(resolvedAudioSource)
				              .arg(dualAudioRequested ? QStringLiteral("on") : QStringLiteral("off")));
		}
		else if (enableAudio)
		{
			appendLogLine(QStringLiteral("[屏幕推流] 音频已禁用，继续仅视频推流"));
		}
	}

	if (!(ofmt->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open2(&ofmt->pb, outPath, AVIO_FLAG_WRITE, nullptr, nullptr);
		if (ret < 0)
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			exitCode = ret;
			setLastError(QStringLiteral("打开推流输出地址失败: %1").arg(QString::fromUtf8(errbuf)));
			goto cleanup;
		}
	}
	ret = avformat_write_header(ofmt, nullptr);
	if (ret < 0)
	{
		char errbuf[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret, errbuf, sizeof(errbuf));
		exitCode = ret;
		setLastError(QStringLiteral("写入推流头失败: %1").arg(QString::fromUtf8(errbuf)));
		goto cleanup;
	}
	wroteHeader = true;

	encFrame->format = encCtx->pix_fmt;
	encFrame->width = encCtx->width;
	encFrame->height = encCtx->height;
	ret = av_frame_get_buffer(encFrame, 32);
	if (ret < 0)
	{
		exitCode = ret;
		setLastError(QStringLiteral("分配编码帧缓冲失败"));
		goto cleanup;
	}

	srcWidth = frame.width;
	srcHeight = frame.height;
	if (srcWidth != encCtx->width || srcHeight != encCtx->height || encCtx->pix_fmt != AV_PIX_FMT_YUV420P)
	{
		sws = sws_getContext(srcWidth, srcHeight, AV_PIX_FMT_YUV420P, encCtx->width, encCtx->height, encCtx->pix_fmt, SWS_BILINEAR,
		                     nullptr, nullptr, nullptr);
		if (!sws)
		{
			exitCode = AVERROR(EINVAL);
			setLastError(QStringLiteral("创建缩放上下文失败"));
			goto cleanup;
		}
	}
	startClock = std::chrono::steady_clock::now();
	nextEncodeAt = startClock;
	avStatWindowStart = startClock;
	avStatVideoPkts = 0;
	if (audioActive && outAudioStream && audioSwr && audioEncCtx && audioOutPkt && audioEncFrame)
	{
		audioThreadStarted = true;
		audioWorker = std::thread([&]() {
			int64_t lastAudioWriteTs = AV_NOPTS_VALUE;
			const bool dualMixEnabled = audioFifo2 && audioSwr2 &&
				(useWasapiAudio2 || (audioIfmt2 && audioPkt2 && audioDecCtx2 && audioDecFrame2 && audioIndex2 >= 0));
			while (!m_stopRequest.load(std::memory_order_relaxed) && !audioThreadStop.load(std::memory_order_relaxed))
			{
				if (dualMixEnabled)
				{
					if (useWasapiAudio2)
					{
						std::vector<uint8_t> captured2;
						int capturedSamples2 = 0;
						if (wasapiAudio2.readInterleaved(captured2, capturedSamples2) && capturedSamples2 > 0)
						{
							const uint8_t* inData2[1] = {captured2.data()};
							const int dstSamples2 = av_rescale_rnd(
								swr_get_delay(audioSwr2, wasapiAudio2.sampleRate()) + capturedSamples2,
								audioEncCtx->sample_rate,
								wasapiAudio2.sampleRate(),
								AV_ROUND_UP);
							if (dstSamples2 > 0)
							{
								uint8_t** dstData2 = nullptr;
								int dstLinesize2 = 0;
								if (av_samples_alloc_array_and_samples(&dstData2, &dstLinesize2, audioEncCtx->ch_layout.nb_channels, dstSamples2,
								                                       audioEncCtx->sample_fmt, 0) >= 0)
								{
									const int converted2 = swr_convert(audioSwr2, dstData2, dstSamples2, inData2, capturedSamples2);
									if (converted2 > 0)
									{
						avStatAudioSecondReads.fetch_add(1, std::memory_order_relaxed);
						avStatAudioSecondSamples.fetch_add(converted2, std::memory_order_relaxed);
										const int fifoNeed2 = av_audio_fifo_size(audioFifo2) + converted2;
										if (av_audio_fifo_realloc(audioFifo2, fifoNeed2) >= 0)
										{
											av_audio_fifo_write(audioFifo2, reinterpret_cast<void**>(dstData2), converted2);
										}
									}
									av_freep(&dstData2[0]);
									av_freep(&dstData2);
								}
							}
						}
					}
					else if (audioIfmt2 && audioPkt2 && audioDecCtx2 && audioDecFrame2 && audioIndex2 >= 0)
					{
						const int audioRet2 = av_read_frame(audioIfmt2, audioPkt2);
						if (audioRet2 >= 0)
						{
							if (audioPkt2->stream_index == audioIndex2)
							{
								const int decSendRet2 = avcodec_send_packet(audioDecCtx2, audioPkt2);
								if (decSendRet2 >= 0)
								{
									while (avcodec_receive_frame(audioDecCtx2, audioDecFrame2) == 0)
									{
										const int dstSamples2 = av_rescale_rnd(
											swr_get_delay(audioSwr2, audioDecCtx2->sample_rate) + audioDecFrame2->nb_samples,
											audioEncCtx->sample_rate,
											audioDecCtx2->sample_rate,
											AV_ROUND_UP);
										if (dstSamples2 > 0)
										{
											uint8_t** dstData2 = nullptr;
											int dstLinesize2 = 0;
											if (av_samples_alloc_array_and_samples(&dstData2, &dstLinesize2, audioEncCtx->ch_layout.nb_channels,
											                                       dstSamples2, audioEncCtx->sample_fmt, 0) >= 0)
											{
												const int converted2 = swr_convert(audioSwr2, dstData2, dstSamples2,
												                                    (const uint8_t* const*)audioDecFrame2->data, audioDecFrame2->nb_samples);
												if (converted2 > 0)
												{
													avStatAudioSecondReads.fetch_add(1, std::memory_order_relaxed);
													avStatAudioSecondSamples.fetch_add(converted2, std::memory_order_relaxed);
													const int fifoNeed2 = av_audio_fifo_size(audioFifo2) + converted2;
													if (av_audio_fifo_realloc(audioFifo2, fifoNeed2) >= 0)
													{
														av_audio_fifo_write(audioFifo2, reinterpret_cast<void**>(dstData2), converted2);
													}
												}
												av_freep(&dstData2[0]);
												av_freep(&dstData2);
											}
										}
										av_frame_unref(audioDecFrame2);
									}
								}
							}
							av_packet_unref(audioPkt2);
						}
					}
				}
				if (useWasapiAudio)
				{
					std::vector<uint8_t> captured;
					int capturedSamples = 0;
					if (!wasapiAudio.readInterleaved(captured, capturedSamples) || capturedSamples <= 0)
					{
						QThread::msleep(2);
						continue;
					}
					if (!audioFifo)
					{
						audioThreadErr.store(AVERROR(EINVAL), std::memory_order_relaxed);
						audioThreadStop.store(true, std::memory_order_relaxed);
						return;
					}
					const uint8_t* inData[1] = {captured.data()};
					const int dstSamples = av_rescale_rnd(
						swr_get_delay(audioSwr, wasapiAudio.sampleRate()) + capturedSamples,
						audioEncCtx->sample_rate,
						wasapiAudio.sampleRate(),
						AV_ROUND_UP);
					if (dstSamples <= 0)
					{
						continue;
					}
					uint8_t** dstData = nullptr;
					int dstLinesize = 0;
					if (av_samples_alloc_array_and_samples(&dstData, &dstLinesize, audioEncCtx->ch_layout.nb_channels, dstSamples,
					                                       audioEncCtx->sample_fmt, 0) < 0)
					{
						continue;
					}
					const int converted = swr_convert(audioSwr, dstData, dstSamples, inData, capturedSamples);
					if (converted > 0)
					{
						avStatAudioMainReads.fetch_add(1, std::memory_order_relaxed);
						avStatAudioMainSamples.fetch_add(converted, std::memory_order_relaxed);
						const int fifoNeed = av_audio_fifo_size(audioFifo) + converted;
						if (av_audio_fifo_realloc(audioFifo, fifoNeed) < 0 ||
						    av_audio_fifo_write(audioFifo, reinterpret_cast<void**>(dstData), converted) < converted)
						{
							audioThreadErr.store(AVERROR(ENOMEM), std::memory_order_relaxed);
							audioThreadStop.store(true, std::memory_order_relaxed);
							av_freep(&dstData[0]);
							av_freep(&dstData);
							return;
						}
					}
					av_freep(&dstData[0]);
					av_freep(&dstData);
					const int encSamples = audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024;
					while (av_audio_fifo_size(audioFifo) >= encSamples)
					{
						const bool enableMix = dualMixEnabled && av_audio_fifo_size(audioFifo2) >= encSamples;
						if (!fillAudioEncFrameFromFifos(audioEncFrame, audioEncCtx, audioFifo, audioFifo2, encSamples, enableMix))
						{
							break;
						}
						audioEncFrame->pts = audioPts;
						audioPts += encSamples;
						const int sendRet = avcodec_send_frame(audioEncCtx, audioEncFrame);
						if (sendRet < 0)
						{
							char errbuf[AV_ERROR_MAX_STRING_SIZE];
							av_strerror(sendRet, errbuf, sizeof(errbuf));
							appendLogLine(QStringLiteral("[屏幕推流] 音频送编码失败: %1 nb=%2 fs=%3")
								              .arg(QString::fromUtf8(errbuf))
								              .arg(encSamples)
								              .arg(audioEncCtx->frame_size));
							break;
						}
						while (avcodec_receive_packet(audioEncCtx, audioOutPkt) == 0)
						{
							av_packet_rescale_ts(audioOutPkt, audioEncCtx->time_base, outAudioStream->time_base);
							audioOutPkt->stream_index = outAudioStream->index;
							if (audioOutPkt->pts == AV_NOPTS_VALUE)
							{
								audioOutPkt->pts = audioOutPkt->dts;
							}
							if (audioOutPkt->dts == AV_NOPTS_VALUE)
							{
								audioOutPkt->dts = audioOutPkt->pts;
							}
							int64_t curTs = audioOutPkt->dts != AV_NOPTS_VALUE ? audioOutPkt->dts : audioOutPkt->pts;
							if (lastAudioWriteTs != AV_NOPTS_VALUE && curTs != AV_NOPTS_VALUE && curTs <= lastAudioWriteTs)
							{
								curTs = lastAudioWriteTs + 1;
								audioOutPkt->pts = curTs;
								audioOutPkt->dts = curTs;
							}
							if (curTs != AV_NOPTS_VALUE)
							{
								lastAudioWriteTs = curTs;
							}
							if (audioOutPkt->duration <= 0)
							{
								audioOutPkt->duration = av_rescale_q(
									audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024,
									audioEncCtx->time_base,
									outAudioStream->time_base);
							}
							int aw = 0;
							{
								std::lock_guard<std::mutex> lk(muxWriteMutex);
								aw = av_interleaved_write_frame(ofmt, audioOutPkt);
							}
							if (aw < 0)
							{
								audioThreadErr.store(aw, std::memory_order_relaxed);
								audioThreadStop.store(true, std::memory_order_relaxed);
								av_packet_unref(audioOutPkt);
								return;
							}
							avStatAudioPkts.fetch_add(1, std::memory_order_relaxed);
							av_packet_unref(audioOutPkt);
						}
					}
				}
				else if (audioIfmt && audioPkt && audioDecCtx)
				{
					const int maxAudioPacketsPerTick = 4;
					for (int audioDrain = 0; audioDrain < maxAudioPacketsPerTick; ++audioDrain)
					{
						const int audioRet = av_read_frame(audioIfmt, audioPkt);
						if (audioRet < 0)
						{
							break;
						}
						if (audioPkt->stream_index == audioIndex)
						{
							const int decSendRet = avcodec_send_packet(audioDecCtx, audioPkt);
							if (decSendRet >= 0)
							{
								while (avcodec_receive_frame(audioDecCtx, audioDecFrame) == 0)
								{
									const int dstSamples = av_rescale_rnd(
										swr_get_delay(audioSwr, audioDecCtx->sample_rate) + audioDecFrame->nb_samples,
										audioEncCtx->sample_rate,
										audioDecCtx->sample_rate,
										AV_ROUND_UP);
									if (!audioFifo)
									{
										audioThreadErr.store(AVERROR(EINVAL), std::memory_order_relaxed);
										audioThreadStop.store(true, std::memory_order_relaxed);
										return;
									}
									uint8_t** dstData = nullptr;
									int dstLinesize = 0;
									if (av_samples_alloc_array_and_samples(&dstData, &dstLinesize, audioEncCtx->ch_layout.nb_channels, dstSamples,
									                                       audioEncCtx->sample_fmt, 0) >= 0)
									{
										const int converted = swr_convert(audioSwr, dstData, dstSamples,
										                                  (const uint8_t* const*)audioDecFrame->data, audioDecFrame->nb_samples);
										if (converted > 0)
										{
											avStatAudioMainReads.fetch_add(1, std::memory_order_relaxed);
											avStatAudioMainSamples.fetch_add(converted, std::memory_order_relaxed);
											const int fifoNeed = av_audio_fifo_size(audioFifo) + converted;
											if (av_audio_fifo_realloc(audioFifo, fifoNeed) < 0 ||
											    av_audio_fifo_write(audioFifo, reinterpret_cast<void**>(dstData), converted) < converted)
											{
												audioThreadErr.store(AVERROR(ENOMEM), std::memory_order_relaxed);
												audioThreadStop.store(true, std::memory_order_relaxed);
												av_freep(&dstData[0]);
												av_freep(&dstData);
												return;
											}
										}
										av_freep(&dstData[0]);
										av_freep(&dstData);
									}
									const int encSamples = audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024;
									while (av_audio_fifo_size(audioFifo) >= encSamples)
									{
										const bool enableMix = dualMixEnabled && av_audio_fifo_size(audioFifo2) >= encSamples;
										if (!fillAudioEncFrameFromFifos(audioEncFrame, audioEncCtx, audioFifo, audioFifo2, encSamples, enableMix))
										{
											break;
										}
										audioEncFrame->pts = audioPts;
										audioPts += encSamples;
										if (avcodec_send_frame(audioEncCtx, audioEncFrame) < 0)
										{
											break;
										}
										while (avcodec_receive_packet(audioEncCtx, audioOutPkt) == 0)
										{
											av_packet_rescale_ts(audioOutPkt, audioEncCtx->time_base, outAudioStream->time_base);
											audioOutPkt->stream_index = outAudioStream->index;
											if (audioOutPkt->pts == AV_NOPTS_VALUE)
											{
												audioOutPkt->pts = audioOutPkt->dts;
											}
											if (audioOutPkt->dts == AV_NOPTS_VALUE)
											{
												audioOutPkt->dts = audioOutPkt->pts;
											}
											int64_t curTs = audioOutPkt->dts != AV_NOPTS_VALUE ? audioOutPkt->dts : audioOutPkt->pts;
											if (lastAudioWriteTs != AV_NOPTS_VALUE && curTs != AV_NOPTS_VALUE && curTs <= lastAudioWriteTs)
											{
												curTs = lastAudioWriteTs + 1;
												audioOutPkt->pts = curTs;
												audioOutPkt->dts = curTs;
											}
											if (curTs != AV_NOPTS_VALUE)
											{
												lastAudioWriteTs = curTs;
											}
											if (audioOutPkt->duration <= 0)
											{
												audioOutPkt->duration = av_rescale_q(
													audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024,
													audioEncCtx->time_base,
													outAudioStream->time_base);
											}
											int aw = 0;
											{
												std::lock_guard<std::mutex> lk(muxWriteMutex);
												aw = av_interleaved_write_frame(ofmt, audioOutPkt);
											}
											if (aw < 0)
											{
												audioThreadErr.store(aw, std::memory_order_relaxed);
												audioThreadStop.store(true, std::memory_order_relaxed);
												av_packet_unref(audioOutPkt);
												return;
											}
											avStatAudioPkts.fetch_add(1, std::memory_order_relaxed);
											av_packet_unref(audioOutPkt);
										}
									}
									av_frame_unref(audioDecFrame);
								}
							}
						}
						av_packet_unref(audioPkt);
					}
				}
				else
				{
					QThread::msleep(2);
				}
			}
			if (audioEncCtx && outAudioStream && audioOutPkt)
			{
				if (audioFifo)
				{
					const int encSamples = audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024;
					while (av_audio_fifo_size(audioFifo) > 0)
					{
						const int remain = av_audio_fifo_size(audioFifo);
						const int readSamples = qMin(encSamples, remain);
						const bool enableMix = dualMixEnabled && av_audio_fifo_size(audioFifo2) >= readSamples;
						if (!fillAudioEncFrameFromFifosWithPadding(
							    audioEncFrame, audioEncCtx, audioFifo, audioFifo2, readSamples, encSamples, enableMix))
						{
							break;
						}
						audioEncFrame->pts = audioPts;
						audioPts += encSamples;
						if (avcodec_send_frame(audioEncCtx, audioEncFrame) < 0)
						{
							break;
						}
						while (avcodec_receive_packet(audioEncCtx, audioOutPkt) == 0)
						{
							av_packet_rescale_ts(audioOutPkt, audioEncCtx->time_base, outAudioStream->time_base);
							audioOutPkt->stream_index = outAudioStream->index;
							if (audioOutPkt->pts == AV_NOPTS_VALUE)
							{
								audioOutPkt->pts = audioOutPkt->dts;
							}
							if (audioOutPkt->dts == AV_NOPTS_VALUE)
							{
								audioOutPkt->dts = audioOutPkt->pts;
							}
							int64_t curTs = audioOutPkt->dts != AV_NOPTS_VALUE ? audioOutPkt->dts : audioOutPkt->pts;
							if (lastAudioWriteTs != AV_NOPTS_VALUE && curTs != AV_NOPTS_VALUE && curTs <= lastAudioWriteTs)
							{
								curTs = lastAudioWriteTs + 1;
								audioOutPkt->pts = curTs;
								audioOutPkt->dts = curTs;
							}
							if (curTs != AV_NOPTS_VALUE)
							{
								lastAudioWriteTs = curTs;
							}
							if (audioOutPkt->duration <= 0)
							{
								audioOutPkt->duration = av_rescale_q(
									audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024,
									audioEncCtx->time_base,
									outAudioStream->time_base);
							}
							int aw = 0;
							{
								std::lock_guard<std::mutex> lk(muxWriteMutex);
								aw = av_interleaved_write_frame(ofmt, audioOutPkt);
							}
							if (aw < 0)
							{
								audioThreadErr.store(aw, std::memory_order_relaxed);
								av_packet_unref(audioOutPkt);
								break;
							}
							avStatAudioPkts.fetch_add(1, std::memory_order_relaxed);
							av_packet_unref(audioOutPkt);
						}
						if (audioThreadErr.load(std::memory_order_relaxed) < 0)
						{
							break;
						}
					}
				}
				avcodec_send_frame(audioEncCtx, nullptr);
				while (avcodec_receive_packet(audioEncCtx, audioOutPkt) == 0)
				{
					av_packet_rescale_ts(audioOutPkt, audioEncCtx->time_base, outAudioStream->time_base);
					audioOutPkt->stream_index = outAudioStream->index;
					int aw = 0;
					{
						std::lock_guard<std::mutex> lk(muxWriteMutex);
						aw = av_interleaved_write_frame(ofmt, audioOutPkt);
					}
					if (aw < 0)
					{
						audioThreadErr.store(aw, std::memory_order_relaxed);
						av_packet_unref(audioOutPkt);
						break;
					}
					avStatAudioPkts.fetch_add(1, std::memory_order_relaxed);
					av_packet_unref(audioOutPkt);
				}
			}
		});
	}

	while (!m_stopRequest.load(std::memory_order_relaxed))
	{
		const int threadAudioErr = audioThreadErr.load(std::memory_order_relaxed);
		if (threadAudioErr < 0)
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(threadAudioErr, errbuf, sizeof(errbuf));
			exitCode = threadAudioErr;
			setLastError(QStringLiteral("写入音频包失败: %1").arg(QString::fromUtf8(errbuf)));
			writeFailed = true;
			break;
		}
		if (!fplayer::ScreenFrameBus::instance().snapshotIfNew(lastSerial, frame) || frame.width <= 0 || frame.height <= 0)
		{
			QThread::msleep(2);
			continue;
		}
		lastSerial = frame.serial;
		const auto now = std::chrono::steady_clock::now();
		if (now < nextEncodeAt)
		{
			continue;
		}
		nextEncodeAt = now + std::chrono::milliseconds(frameIntervalMs);

		// 源分辨率变化时重建缩放器，保持推流线程稳定。
		const bool sizeChanged = (frame.width != srcWidth) || (frame.height != srcHeight);
		if (sizeChanged)
		{
			if (sws)
			{
				sws_freeContext(sws);
				sws = nullptr;
			}
			srcWidth = frame.width;
			srcHeight = frame.height;
			if (srcWidth != encCtx->width || srcHeight != encCtx->height || encCtx->pix_fmt != AV_PIX_FMT_YUV420P)
			{
				sws = sws_getContext(srcWidth, srcHeight, AV_PIX_FMT_YUV420P, encCtx->width, encCtx->height, encCtx->pix_fmt,
				                     SWS_BILINEAR, nullptr, nullptr, nullptr);
				if (!sws)
				{
					exitCode = AVERROR(EINVAL);
					setLastError(QStringLiteral("重建缩放上下文失败"));
					break;
				}
			}
		}

		const uint8_t* srcData[4] = {
			reinterpret_cast<const uint8_t*>(frame.y.constData()),
			reinterpret_cast<const uint8_t*>(frame.u.constData()),
			reinterpret_cast<const uint8_t*>(frame.v.constData()),
			nullptr
		};
		int srcStride[4] = {frame.yStride, frame.uStride, frame.vStride, 0};
		ret = av_frame_make_writable(encFrame);
		if (ret < 0)
		{
			continue;
		}
		if (sws)
		{
			sws_scale(sws, srcData, srcStride, 0, frame.height, encFrame->data, encFrame->linesize);
		}
		else
		{
			for (int y = 0; y < encCtx->height; ++y)
			{
				memcpy(encFrame->data[0] + y * encFrame->linesize[0], srcData[0] + y * srcStride[0], encCtx->width);
			}
			for (int y = 0; y < encCtx->height / 2; ++y)
			{
				memcpy(encFrame->data[1] + y * encFrame->linesize[1], srcData[1] + y * srcStride[1], encCtx->width / 2);
				memcpy(encFrame->data[2] + y * encFrame->linesize[2], srcData[2] + y * srcStride[2], encCtx->width / 2);
			}
		}
		const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(now - startClock).count();
		int64_t pts = av_rescale_q(elapsedUs, AVRational{1, 1000000}, encCtx->time_base);
		if (pts <= lastPts)
		{
			pts = lastPts + 1;
		}
		lastPts = pts;
		encFrame->pts = pts;
		ret = avcodec_send_frame(encCtx, encFrame);
		if (ret < 0)
		{
			continue;
		}
		while (avcodec_receive_packet(encCtx, outPkt) == 0)
		{
			av_packet_rescale_ts(outPkt, encCtx->time_base, ofmt->streams[0]->time_base);
			outPkt->stream_index = 0;
			{
				std::lock_guard<std::mutex> lk(muxWriteMutex);
				ret = av_interleaved_write_frame(ofmt, outPkt);
			}
			av_packet_unref(outPkt);
			if (ret < 0)
			{
				char errbuf[AV_ERROR_MAX_STRING_SIZE];
				av_strerror(ret, errbuf, sizeof(errbuf));
				exitCode = ret;
				setLastError(QStringLiteral("写入视频包失败: %1").arg(QString::fromUtf8(errbuf)));
				writeFailed = true;
				break;
			}
			++avStatVideoPkts;
		}
		if (writeFailed)
		{
			break;
		}
		const auto statNow = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(statNow - avStatWindowStart).count() >= 1000)
		{
			const int audioPkts = avStatAudioPkts.exchange(0, std::memory_order_relaxed);
			const int mainReads = avStatAudioMainReads.exchange(0, std::memory_order_relaxed);
			const int secondReads = avStatAudioSecondReads.exchange(0, std::memory_order_relaxed);
			const int mainSamples = avStatAudioMainSamples.exchange(0, std::memory_order_relaxed);
			const int secondSamples = avStatAudioSecondSamples.exchange(0, std::memory_order_relaxed);
			appendLogLine(QStringLiteral("[屏幕推流] 发包统计 1s: video=%1 audio=%2")
				              .arg(avStatVideoPkts)
				              .arg(audioPkts));
			appendLogLine(QStringLiteral("[屏幕推流] 音频诊断 1s: main_reads=%1 main_samples=%2 second_reads=%3 second_samples=%4 fifo_main=%5 fifo_second=%6 mode_main=%7 mode_second=%8 dualMix=%9")
				              .arg(mainReads)
				              .arg(mainSamples)
				              .arg(secondReads)
				              .arg(secondSamples)
				              .arg(audioFifo ? av_audio_fifo_size(audioFifo) : -1)
				              .arg(audioFifo2 ? av_audio_fifo_size(audioFifo2) : -1)
				              .arg(useWasapiAudio ? QStringLiteral("wasapi") : QStringLiteral("dshow"))
				              .arg(useWasapiAudio2 ? QStringLiteral("wasapi") : QStringLiteral("dshow"))
				              .arg(dualAudioRequested ? QStringLiteral("on") : QStringLiteral("off")));
			avStatVideoPkts = 0;
			avStatWindowStart = statNow;
		}
	}
	audioThreadStop.store(true, std::memory_order_relaxed);
	if (audioThreadStarted && audioWorker.joinable())
	{
		audioWorker.join();
	}
	if (!writeFailed)
	{
		const int threadAudioErr = audioThreadErr.load(std::memory_order_relaxed);
		if (threadAudioErr < 0 && exitCode == 0)
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(threadAudioErr, errbuf, sizeof(errbuf));
			exitCode = threadAudioErr;
			setLastError(QStringLiteral("写入音频包失败: %1").arg(QString::fromUtf8(errbuf)));
			writeFailed = true;
		}
	}

	if (!writeFailed)
	{
		avcodec_send_frame(encCtx, nullptr);
		while (avcodec_receive_packet(encCtx, outPkt) == 0)
		{
			av_packet_rescale_ts(outPkt, encCtx->time_base, ofmt->streams[0]->time_base);
			outPkt->stream_index = 0;
			{
				std::lock_guard<std::mutex> lk(muxWriteMutex);
				ret = av_interleaved_write_frame(ofmt, outPkt);
			}
			av_packet_unref(outPkt);
			if (ret < 0)
			{
				char errbuf[AV_ERROR_MAX_STRING_SIZE];
				av_strerror(ret, errbuf, sizeof(errbuf));
				exitCode = ret;
				setLastError(QStringLiteral("写入尾包失败: %1").arg(QString::fromUtf8(errbuf)));
				writeFailed = true;
				break;
			}
		}
	}
	if (wroteHeader)
	{
		ret = av_write_trailer(ofmt);
		if (ret < 0 && exitCode == 0)
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			exitCode = ret;
			setLastError(QStringLiteral("写入推流尾失败: %1").arg(QString::fromUtf8(errbuf)));
		}
	}

cleanup:
	fplayer::ScreenFrameBus::instance().setPublishTargetSize(0, 0);
	if (exitCode < 0)
	{
		const QString err = lastError();
		if (!err.trimmed().isEmpty())
		{
			appendLogLine(QStringLiteral("[屏幕推流] 失败: %1 (code=%2)").arg(err).arg(exitCode));
		}
	}
	if (sws)
	{
		sws_freeContext(sws);
	}
	if (encFrame)
	{
		av_frame_free(&encFrame);
	}
	if (outPkt)
	{
		av_packet_free(&outPkt);
	}
	if (encCtx)
	{
		avcodec_free_context(&encCtx);
	}
	if (audioPkt)
	{
		av_packet_free(&audioPkt);
	}
	if (audioOutPkt)
	{
		av_packet_free(&audioOutPkt);
	}
	if (audioDecFrame)
	{
		av_frame_free(&audioDecFrame);
	}
	if (audioEncFrame)
	{
		av_frame_free(&audioEncFrame);
	}
	if (audioSwr)
	{
		swr_free(&audioSwr);
	}
	if (audioFifo)
	{
		av_audio_fifo_free(audioFifo);
		audioFifo = nullptr;
	}
	if (audioEncCtx)
	{
		avcodec_free_context(&audioEncCtx);
	}
	if (audioDecCtx)
	{
		avcodec_free_context(&audioDecCtx);
	}
	if (audioIfmt)
	{
		avformat_close_input(&audioIfmt);
	}
	if (ofmt)
	{
		if (!(ofmt->oformat->flags & AVFMT_NOFILE) && ofmt->pb)
		{
			avio_closep(&ofmt->pb);
		}
		avformat_free_context(ofmt);
	}
	{
		QMutexLocker locker(&m_mutex);
		m_lastExitCode = exitCode;
	}
	m_completedSession.store(true, std::memory_order_relaxed);
	m_running.store(false, std::memory_order_relaxed);
}

void fplayer::StreamFFmpeg::pushCameraPreviewLoop(const QString& outputUrl, const QString& captureSpec)
{
	AVFormatContext* ofmt = nullptr;
	AVCodecContext* encCtx = nullptr;
	AVPacket* outPkt = nullptr;
	AVFrame* encFrame = nullptr;
	int exitCode = 0;
	int ret = 0;
	bool wroteHeader = false;
	int64_t framePts = 0;
	uint64_t lastSerial = 0;
	auto avStatWindowStart = std::chrono::steady_clock::time_point{};
	int avStatVideoPkts = 0;
	int avStatAudioPkts = 0;
	fplayer::CameraFrame frame;
	AVFormatContext* audioIfmt = nullptr;
	AVCodecContext* audioDecCtx = nullptr;
	AVFormatContext* audioIfmt2 = nullptr;
	AVCodecContext* audioDecCtx2 = nullptr;
	AVCodecContext* audioEncCtx = nullptr;
	SwrContext* audioSwr = nullptr;
	SwrContext* audioSwr2 = nullptr;
	AVAudioFifo* audioFifo = nullptr;
	AVAudioFifo* audioFifo2 = nullptr;
	AVPacket* audioPkt = nullptr;
	AVPacket* audioPkt2 = nullptr;
	AVPacket* audioOutPkt = nullptr;
	AVFrame* audioDecFrame = nullptr;
	AVFrame* audioDecFrame2 = nullptr;
	AVFrame* audioEncFrame = nullptr;
	int audioIndex = -1;
	int audioIndex2 = -1;
	AVStream* outAudioStream = nullptr;
	int64_t audioPts = 0;
	fplayer::windows_api::WasapiLoopbackCapture wasapiAudio;
	fplayer::windows_api::WasapiLoopbackCapture wasapiAudio2;
	bool useWasapiAudio = false;
	bool useWasapiAudio2 = false;
	std::mutex muxWriteMutex;
	std::atomic<bool> audioThreadStop{false};
	std::atomic<int> audioThreadErr{0};
	std::thread audioWorker;
	bool audioThreadStarted = false;

	const QByteArray outUtf8 = outputUrl.toUtf8();
	const char* outPath = outUtf8.constData();
	const CaptureParams params = parseCaptureParams(captureSpec);
	const int targetFps = qMax(1, params.fps);
	const QString requestedAudioOut = params.audioOutputSource.trimmed();
	const QString requestedAudioIn = params.audioInputSource.trimmed();
	const bool hasIn = !requestedAudioIn.isEmpty() && requestedAudioIn != QStringLiteral("off");
	const bool hasOut = !requestedAudioOut.isEmpty() && requestedAudioOut != QStringLiteral("off");
	const bool dualAudioRequested = hasIn && hasOut && requestedAudioIn != requestedAudioOut;
	const QString audioPrimarySource = dualAudioRequested ? requestedAudioIn : QString();
	const QString audioSecondarySource = dualAudioRequested ? requestedAudioOut : QString();
	const QString resolvedAudioSource = hasOut ? requestedAudioOut : (hasIn ? requestedAudioIn : QString());
	const bool enableAudio = !resolvedAudioSource.isEmpty() && resolvedAudioSource != QStringLiteral("off");
	bool audioActive = enableAudio;
	if (audioActive)
	{
		QString openedAudioDevice;
		QString openDetail;
		const QString selectedPrimary = dualAudioRequested ? audioPrimarySource : resolvedAudioSource;
		if (!fplayer::windows_api::openDshowAudioInputWithFallback(selectedPrimary, m_stopRequest, audioIfmt, openedAudioDevice, openDetail))
		{
			QString wasapiErr;
			if (selectedPrimary == QStringLiteral("system") && wasapiAudio.init(wasapiErr))
			{
				useWasapiAudio = true;
				openedAudioDevice = QStringLiteral("native-wasapi-loopback(default)");
				appendLogLine(QStringLiteral("[摄像头预览推流] dshow音频不可用，已切换系统API回采: %1").arg(openDetail));
			}
			else
			{
				appendLogLine(QStringLiteral("[摄像头预览推流] 音频采集初始化失败，已自动降级为无声推流: %1; wasapi-native=%2")
					              .arg(openDetail)
					              .arg(wasapiErr));
				audioActive = false;
			}
		}
		if (audioActive)
		{
			if (audioIfmt)
			{
				audioIfmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
				audioIfmt->interrupt_callback.opaque = &m_stopRequest;
			}
			appendLogLine(QStringLiteral("[摄像头预览推流] 音频主设备=%1").arg(openedAudioDevice));
		}
		if (audioActive && audioIfmt)
		{
			ret = avformat_find_stream_info(audioIfmt, nullptr);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("读取音频流信息失败"));
				goto cleanup;
			}
			for (unsigned i = 0; i < audioIfmt->nb_streams; ++i)
			{
				if (audioIfmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
				{
					audioIndex = static_cast<int>(i);
					break;
				}
			}
			if (audioIndex < 0)
			{
				exitCode = AVERROR_STREAM_NOT_FOUND;
				setLastError(QStringLiteral("未找到可用音频流"));
				goto cleanup;
			}
			const AVCodecParameters* inAudioPar = audioIfmt->streams[audioIndex]->codecpar;
			const AVCodec* audioDec = avcodec_find_decoder(inAudioPar->codec_id);
			if (!audioDec)
			{
				exitCode = AVERROR_DECODER_NOT_FOUND;
				setLastError(QStringLiteral("未找到音频解码器"));
				goto cleanup;
			}
			audioDecCtx = avcodec_alloc_context3(audioDec);
			if (!audioDecCtx)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建音频解码器上下文失败"));
				goto cleanup;
			}
			ret = avcodec_parameters_to_context(audioDecCtx, inAudioPar);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("加载音频输入参数失败"));
				goto cleanup;
			}
			ret = avcodec_open2(audioDecCtx, audioDec, nullptr);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("打开音频解码器失败"));
				goto cleanup;
			}
		}
		audioPkt = av_packet_alloc();
		audioOutPkt = av_packet_alloc();
		audioDecFrame = av_frame_alloc();
		audioEncFrame = av_frame_alloc();
		if (!audioPkt || !audioOutPkt || !audioDecFrame || !audioEncFrame)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("分配音频编码资源失败"));
			goto cleanup;
		}
		if (audioActive && dualAudioRequested)
		{
			QString openedAudioDevice2;
			QString openDetail2;
			if (!fplayer::windows_api::openDshowAudioInputWithFallback(audioSecondarySource, m_stopRequest, audioIfmt2, openedAudioDevice2, openDetail2))
			{
				QString wasapiErr2;
				if (audioSecondarySource == QStringLiteral("system") && wasapiAudio2.init(wasapiErr2))
				{
					useWasapiAudio2 = true;
					openedAudioDevice2 = QStringLiteral("native-wasapi-loopback(default)");
				}
				else
				{
					appendLogLine(QStringLiteral("[摄像头预览推流] 第二音频源不可用，降级单音频: %1; wasapi-native=%2")
						              .arg(openDetail2)
						              .arg(wasapiErr2));
				}
			}
			if (audioIfmt2)
			{
				audioIfmt2->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
				audioIfmt2->interrupt_callback.opaque = &m_stopRequest;
				const int ret2 = avformat_find_stream_info(audioIfmt2, nullptr);
				if (ret2 >= 0)
				{
					for (unsigned i = 0; i < audioIfmt2->nb_streams; ++i)
					{
						if (audioIfmt2->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
						{
							audioIndex2 = static_cast<int>(i);
							break;
						}
					}
					if (audioIndex2 >= 0)
					{
						const AVCodecParameters* inAudioPar2 = audioIfmt2->streams[audioIndex2]->codecpar;
						const AVCodec* audioDec2 = avcodec_find_decoder(inAudioPar2->codec_id);
						if (audioDec2)
						{
							audioDecCtx2 = avcodec_alloc_context3(audioDec2);
							if (audioDecCtx2 && avcodec_parameters_to_context(audioDecCtx2, inAudioPar2) >= 0 &&
							    avcodec_open2(audioDecCtx2, audioDec2, nullptr) >= 0)
							{
								audioPkt2 = av_packet_alloc();
								audioDecFrame2 = av_frame_alloc();
								appendLogLine(QStringLiteral("[摄像头预览推流] 音频副设备=%1").arg(openedAudioDevice2));
							}
						}
					}
				}
			}
		}
	}
	outPkt = av_packet_alloc();
	encFrame = av_frame_alloc();
	if (!outPkt || !encFrame)
	{
		exitCode = AVERROR(ENOMEM);
		setLastError(QStringLiteral("分配编码资源失败"));
		goto cleanup;
	}

	ret = avformat_alloc_output_context2(&ofmt, nullptr, "flv", outPath);
	if (ret < 0 || !ofmt)
	{
		exitCode = ret ? ret : AVERROR_UNKNOWN;
		setLastError(QStringLiteral("创建推流输出上下文失败"));
		goto cleanup;
	}
	ofmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
	ofmt->interrupt_callback.opaque = &m_stopRequest;

	// 等待当前摄像头预览帧可用，避免推流线程空跑。
	frame = fplayer::CameraFrameBus::instance().snapshot();
	for (int i = 0; i < 200 && (!frame.valid || frame.width <= 0 || frame.height <= 0) && !m_stopRequest.load(std::memory_order_relaxed); ++i)
	{
		QThread::msleep(10);
		frame = fplayer::CameraFrameBus::instance().snapshot();
	}
	if (!frame.valid || frame.width <= 0 || frame.height <= 0)
	{
		exitCode = AVERROR(EINVAL);
		setLastError(QStringLiteral("当前摄像头预览无可用帧，无法开始推流"));
		goto cleanup;
	}
	lastSerial = frame.serial;

	{
		const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_H264);
		if (!enc)
		{
			enc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
		}
		if (!enc)
		{
			exitCode = AVERROR_ENCODER_NOT_FOUND;
			setLastError(QStringLiteral("未找到可用视频编码器(H264/MPEG4)"));
			goto cleanup;
		}
		encCtx = avcodec_alloc_context3(enc);
		if (!encCtx)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("创建编码器上下文失败"));
			goto cleanup;
		}
		encCtx->width = frame.width;
		encCtx->height = frame.height;
		encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
		encCtx->time_base = AVRational{1, targetFps};
		encCtx->framerate = AVRational{targetFps, 1};
		encCtx->gop_size = targetFps * 2;
		encCtx->max_b_frames = 0;
		const int bitrateKbps = params.bitrateKbps > 0
			                        ? params.bitrateKbps
			                        : estimateBitrateKbps(encCtx->width, encCtx->height, targetFps);
		encCtx->bit_rate = static_cast<int64_t>(bitrateKbps) * 1000;
		encCtx->rc_max_rate = encCtx->bit_rate;
		encCtx->rc_buffer_size = encCtx->bit_rate * 2;
		if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
		{
			encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		}
		if (enc->id == AV_CODEC_ID_H264 && encCtx->priv_data)
		{
			av_opt_set(encCtx->priv_data, "preset", "superfast", 0);
			av_opt_set(encCtx->priv_data, "tune", "zerolatency", 0);
		}
		appendLogLine(QStringLiteral("[摄像头预览推流] 输入=%1x%2 输出=%3x%4 FPS=%5 码率=%6kbps")
		              .arg(frame.width)
		              .arg(frame.height)
		              .arg(encCtx->width)
		              .arg(encCtx->height)
		              .arg(targetFps)
		              .arg(bitrateKbps));
		ret = avcodec_open2(encCtx, enc, nullptr);
		if (ret < 0)
		{
			exitCode = ret;
			setLastError(QStringLiteral("打开编码器失败"));
			goto cleanup;
		}
		AVStream* outStream = avformat_new_stream(ofmt, nullptr);
		if (!outStream)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("创建输出视频流失败"));
			goto cleanup;
		}
		outStream->time_base = encCtx->time_base;
		avcodec_parameters_from_context(outStream->codecpar, encCtx);
		if (audioActive && ((audioIfmt && audioIndex >= 0) || useWasapiAudio))
		{
			const AVCodec* audioEnc = avcodec_find_encoder(AV_CODEC_ID_AAC);
			if (!audioEnc)
			{
				exitCode = AVERROR_ENCODER_NOT_FOUND;
				setLastError(QStringLiteral("未找到 AAC 音频编码器，无法推送声音"));
				goto cleanup;
			}
			audioEncCtx = avcodec_alloc_context3(audioEnc);
			if (!audioEncCtx)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建音频编码器上下文失败"));
				goto cleanup;
			}
			const int inSampleRate = useWasapiAudio
				                         ? (wasapiAudio.sampleRate() > 0 ? wasapiAudio.sampleRate() : 48000)
				                         : (audioDecCtx && audioDecCtx->sample_rate > 0 ? audioDecCtx->sample_rate : 48000);
			audioEncCtx->sample_rate = inSampleRate;
			if (audioEnc->supported_samplerates)
			{
				int bestRate = audioEnc->supported_samplerates[0];
				int bestDiff = qAbs(bestRate - audioEncCtx->sample_rate);
				for (const int* p = audioEnc->supported_samplerates; *p != 0; ++p)
				{
					const int diff = qAbs(*p - audioEncCtx->sample_rate);
					if (diff < bestDiff)
					{
						bestRate = *p;
						bestDiff = diff;
					}
				}
				audioEncCtx->sample_rate = bestRate;
			}
			const int srcChannels = useWasapiAudio
				                        ? wasapiAudio.channelLayout().nb_channels
				                        : (audioDecCtx ? audioDecCtx->ch_layout.nb_channels : 0);
			const int targetChannels = (srcChannels == 1) ? 1 : 2;
			av_channel_layout_uninit(&audioEncCtx->ch_layout);
			av_channel_layout_default(&audioEncCtx->ch_layout, targetChannels);
			if (audioEnc->ch_layouts && audioEnc->ch_layouts[0].nb_channels > 0)
			{
				const AVChannelLayout* best = &audioEnc->ch_layouts[0];
				int bestDiff = qAbs(best->nb_channels - targetChannels);
				for (const AVChannelLayout* p = audioEnc->ch_layouts; p && p->nb_channels > 0; ++p)
				{
					const int diff = qAbs(p->nb_channels - targetChannels);
					if (diff < bestDiff)
					{
						best = p;
						bestDiff = diff;
					}
				}
				av_channel_layout_uninit(&audioEncCtx->ch_layout);
				if (av_channel_layout_copy(&audioEncCtx->ch_layout, best) < 0)
				{
					av_channel_layout_default(&audioEncCtx->ch_layout, targetChannels);
				}
			}
			if (audioEncCtx->ch_layout.nb_channels <= 0)
			{
				av_channel_layout_uninit(&audioEncCtx->ch_layout);
				av_channel_layout_default(&audioEncCtx->ch_layout, 2);
			}
			audioEncCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
			if (audioEnc->sample_fmts)
			{
				bool fltpSupported = false;
				for (const AVSampleFormat* p = audioEnc->sample_fmts; *p != AV_SAMPLE_FMT_NONE; ++p)
				{
					if (*p == AV_SAMPLE_FMT_FLTP)
					{
						fltpSupported = true;
						break;
					}
				}
				if (!fltpSupported)
				{
					audioEncCtx->sample_fmt = audioEnc->sample_fmts[0];
				}
			}
			audioEncCtx->bit_rate = 128000;
			audioEncCtx->time_base = AVRational{1, audioEncCtx->sample_rate};
			audioEncCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
			if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
			{
				audioEncCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			}
			ret = avcodec_open2(audioEncCtx, audioEnc, nullptr);
			if (ret < 0)
			{
				char errbuf[AV_ERROR_MAX_STRING_SIZE];
				av_strerror(ret, errbuf, sizeof(errbuf));
				appendLogLine(QStringLiteral("[屏幕推流] 打开音频编码器失败: %1; sr=%2 ch=%3 fmt=%4")
					              .arg(QString::fromUtf8(errbuf))
					              .arg(audioEncCtx->sample_rate)
					              .arg(audioEncCtx->ch_layout.nb_channels)
					              .arg(static_cast<int>(audioEncCtx->sample_fmt)));
				exitCode = ret;
				setLastError(QStringLiteral("打开音频编码器失败"));
				goto cleanup;
			}
			outAudioStream = avformat_new_stream(ofmt, nullptr);
			if (!outAudioStream)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建输出音频流失败"));
				goto cleanup;
			}
			ret = avcodec_parameters_from_context(outAudioStream->codecpar, audioEncCtx);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("写入输出音频参数失败"));
				goto cleanup;
			}
			outAudioStream->codecpar->codec_tag = 0;
			outAudioStream->time_base = audioEncCtx->time_base;
			const AVChannelLayout* inLayout = nullptr;
			if (useWasapiAudio)
			{
				inLayout = wasapiAudio.channelLayout().nb_channels > 0 ? &wasapiAudio.channelLayout() : nullptr;
			}
			else
			{
				inLayout = audioDecCtx->ch_layout.nb_channels > 0 ? &audioDecCtx->ch_layout : nullptr;
			}
			AVChannelLayout defaultInLayout;
			if (!inLayout)
			{
				av_channel_layout_default(&defaultInLayout, 2);
				inLayout = &defaultInLayout;
			}
			const AVSampleFormat inSampleFmt = useWasapiAudio ? wasapiAudio.sampleFmt() : audioDecCtx->sample_fmt;
			const int inSampleRate2 = useWasapiAudio ? inSampleRate : audioDecCtx->sample_rate;
			ret = swr_alloc_set_opts2(&audioSwr, &audioEncCtx->ch_layout, audioEncCtx->sample_fmt, audioEncCtx->sample_rate,
			                          inLayout, inSampleFmt, inSampleRate2, 0, nullptr);
			if (!inLayout || inLayout == &defaultInLayout)
			{
				av_channel_layout_uninit(&defaultInLayout);
			}
			if (ret < 0 || !audioSwr)
			{
				exitCode = ret < 0 ? ret : AVERROR(EINVAL);
				setLastError(QStringLiteral("创建音频重采样器失败"));
				goto cleanup;
			}
			ret = swr_init(audioSwr);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("初始化音频重采样器失败"));
				goto cleanup;
			}
			const int fifoInitSamples = qMax(audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024, 1024) * 8;
			audioFifo = av_audio_fifo_alloc(audioEncCtx->sample_fmt, audioEncCtx->ch_layout.nb_channels, fifoInitSamples);
			if (!audioFifo)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建音频FIFO失败"));
				goto cleanup;
			}
			if ((audioDecCtx2 && audioPkt2 && audioDecFrame2) || useWasapiAudio2)
			{
				const AVChannelLayout* inLayout2 = nullptr;
				if (useWasapiAudio2)
				{
					inLayout2 = wasapiAudio2.channelLayout().nb_channels > 0 ? &wasapiAudio2.channelLayout() : nullptr;
				}
				else
				{
					inLayout2 = audioDecCtx2->ch_layout.nb_channels > 0 ? &audioDecCtx2->ch_layout : nullptr;
				}
				AVChannelLayout defaultInLayout2;
				if (!inLayout2)
				{
					av_channel_layout_default(&defaultInLayout2, 2);
					inLayout2 = &defaultInLayout2;
				}
				const AVSampleFormat inSampleFmt2 = useWasapiAudio2 ? wasapiAudio2.sampleFmt() : audioDecCtx2->sample_fmt;
				const int inSampleRate3 = useWasapiAudio2
					                          ? (wasapiAudio2.sampleRate() > 0 ? wasapiAudio2.sampleRate() : audioEncCtx->sample_rate)
					                          : audioDecCtx2->sample_rate;
				ret = swr_alloc_set_opts2(&audioSwr2, &audioEncCtx->ch_layout, audioEncCtx->sample_fmt, audioEncCtx->sample_rate,
				                          inLayout2, inSampleFmt2, inSampleRate3, 0, nullptr);
				if (!inLayout2 || inLayout2 == &defaultInLayout2)
				{
					av_channel_layout_uninit(&defaultInLayout2);
				}
				if (ret >= 0 && audioSwr2 && swr_init(audioSwr2) >= 0)
				{
					audioFifo2 = av_audio_fifo_alloc(audioEncCtx->sample_fmt, audioEncCtx->ch_layout.nb_channels, fifoInitSamples);
				}
				if (!audioFifo2)
				{
					if (audioSwr2)
					{
						swr_free(&audioSwr2);
					}
					appendLogLine(QStringLiteral("[摄像头预览推流] 副音频源混音初始化失败，降级单音频"));
				}
			}
		}
	}

	if (!(ofmt->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open2(&ofmt->pb, outPath, AVIO_FLAG_WRITE, nullptr, nullptr);
		if (ret < 0)
		{
			exitCode = ret;
			setLastError(QStringLiteral("打开推流输出地址失败"));
			goto cleanup;
		}
	}
	ret = avformat_write_header(ofmt, nullptr);
	if (ret < 0)
	{
		exitCode = ret;
		setLastError(QStringLiteral("写入推流头失败"));
		goto cleanup;
	}
	wroteHeader = true;

	encFrame->format = encCtx->pix_fmt;
	encFrame->width = encCtx->width;
	encFrame->height = encCtx->height;
	ret = av_frame_get_buffer(encFrame, 32);
	if (ret < 0)
	{
		exitCode = ret;
		setLastError(QStringLiteral("分配编码帧缓冲失败"));
		goto cleanup;
	}
	avStatWindowStart = std::chrono::steady_clock::now();
	avStatVideoPkts = 0;
	avStatAudioPkts = 0;
	if (audioActive && outAudioStream && audioSwr && audioEncCtx && audioOutPkt && audioEncFrame && audioFifo)
	{
		audioThreadStarted = true;
		audioWorker = std::thread([&]() {
			const int encSamples = audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024;
			const bool dualMixEnabled = audioFifo2 && audioSwr2 &&
				(useWasapiAudio2 || (audioIfmt2 && audioPkt2 && audioDecCtx2 && audioDecFrame2 && audioIndex2 >= 0));
			const auto writeEncodedPackets = [&]() -> bool {
				while (avcodec_receive_packet(audioEncCtx, audioOutPkt) == 0)
				{
					av_packet_rescale_ts(audioOutPkt, audioEncCtx->time_base, outAudioStream->time_base);
					audioOutPkt->stream_index = outAudioStream->index;
					int aw = 0;
					{
						std::lock_guard<std::mutex> lk(muxWriteMutex);
						aw = av_interleaved_write_frame(ofmt, audioOutPkt);
					}
					if (aw < 0)
					{
						char werr[AV_ERROR_MAX_STRING_SIZE];
						av_strerror(aw, werr, sizeof(werr));
						appendLogLine(QStringLiteral("[摄像头推流] 音频写包失败: %1 pts=%2 dts=%3 dur=%4 size=%5 tb=%6/%7 sr=%8 ch=%9")
							              .arg(QString::fromUtf8(werr))
							              .arg(audioOutPkt->pts)
							              .arg(audioOutPkt->dts)
							              .arg(audioOutPkt->duration)
							              .arg(audioOutPkt->size)
							              .arg(outAudioStream ? outAudioStream->time_base.num : 0)
							              .arg(outAudioStream ? outAudioStream->time_base.den : 0)
							              .arg(audioEncCtx ? audioEncCtx->sample_rate : 0)
							              .arg(audioEncCtx ? audioEncCtx->ch_layout.nb_channels : 0));
						audioThreadErr.store(aw, std::memory_order_relaxed);
						audioThreadStop.store(true, std::memory_order_relaxed);
						av_packet_unref(audioOutPkt);
						return false;
					}
					++avStatAudioPkts;
					av_packet_unref(audioOutPkt);
				}
				return true;
			};
			const auto enqueueFromWasapi =
				[&](fplayer::windows_api::WasapiLoopbackCapture& cap, SwrContext* swr, AVAudioFifo* fifo) -> bool {
				std::vector<uint8_t> captured;
				int capturedSamples = 0;
				if (!cap.readInterleaved(captured, capturedSamples) || capturedSamples <= 0)
				{
					return false;
				}
				const uint8_t* inData[1] = {captured.data()};
				const int srcRate = qMax(1, cap.sampleRate());
				const int dstSamples = av_rescale_rnd(
					swr_get_delay(swr, srcRate) + capturedSamples,
					audioEncCtx->sample_rate,
					srcRate,
					AV_ROUND_UP);
				if (dstSamples <= 0)
				{
					return false;
				}
				uint8_t** dstData = nullptr;
				int dstLinesize = 0;
				if (av_samples_alloc_array_and_samples(&dstData, &dstLinesize, audioEncCtx->ch_layout.nb_channels, dstSamples,
				                                       audioEncCtx->sample_fmt, 0) < 0)
				{
					return false;
				}
				const int converted = swr_convert(swr, dstData, dstSamples, inData, capturedSamples);
				bool ok = false;
				if (converted > 0)
				{
					const int need = av_audio_fifo_size(fifo) + converted;
					ok = av_audio_fifo_realloc(fifo, need) >= 0 &&
					     av_audio_fifo_write(fifo, reinterpret_cast<void**>(dstData), converted) >= converted;
				}
				av_freep(&dstData[0]);
				av_freep(&dstData);
				return ok;
			};
			const auto enqueueFromDshow =
				[&](AVFormatContext* ifmt, AVPacket* pkt, AVCodecContext* decCtx, AVFrame* decFrame, int index, SwrContext* swr,
				    AVAudioFifo* fifo) -> bool {
				const int audioRet = av_read_frame(ifmt, pkt);
				if (audioRet < 0)
				{
					return false;
				}
				bool wrote = false;
				if (pkt->stream_index == index)
				{
					const int decSendRet = avcodec_send_packet(decCtx, pkt);
					if (decSendRet >= 0)
					{
						while (avcodec_receive_frame(decCtx, decFrame) == 0)
						{
							const int dstSamples = av_rescale_rnd(
								swr_get_delay(swr, decCtx->sample_rate) + decFrame->nb_samples,
								audioEncCtx->sample_rate,
								decCtx->sample_rate,
								AV_ROUND_UP);
							if (dstSamples <= 0)
							{
								av_frame_unref(decFrame);
								continue;
							}
							uint8_t** dstData = nullptr;
							int dstLinesize = 0;
							if (av_samples_alloc_array_and_samples(&dstData, &dstLinesize, audioEncCtx->ch_layout.nb_channels, dstSamples,
							                                       audioEncCtx->sample_fmt, 0) >= 0)
							{
								const int converted = swr_convert(swr, dstData, dstSamples,
								                                  (const uint8_t* const*)decFrame->data, decFrame->nb_samples);
								if (converted > 0)
								{
									const int need = av_audio_fifo_size(fifo) + converted;
									if (av_audio_fifo_realloc(fifo, need) >= 0 &&
									    av_audio_fifo_write(fifo, reinterpret_cast<void**>(dstData), converted) >= converted)
									{
										wrote = true;
									}
								}
								av_freep(&dstData[0]);
								av_freep(&dstData);
							}
							av_frame_unref(decFrame);
						}
					}
				}
				av_packet_unref(pkt);
				return wrote;
			};
			while (!m_stopRequest.load(std::memory_order_relaxed) && !audioThreadStop.load(std::memory_order_relaxed))
			{
				bool gotMain = false;
				bool gotSecond = false;
				if (useWasapiAudio)
				{
					gotMain = enqueueFromWasapi(wasapiAudio, audioSwr, audioFifo);
				}
				else if (audioIfmt && audioPkt && audioDecCtx)
				{
					gotMain = enqueueFromDshow(audioIfmt, audioPkt, audioDecCtx, audioDecFrame, audioIndex, audioSwr, audioFifo);
				}
				if (dualMixEnabled)
				{
					if (useWasapiAudio2)
					{
						gotSecond = enqueueFromWasapi(wasapiAudio2, audioSwr2, audioFifo2);
					}
					else if (audioIfmt2 && audioPkt2 && audioDecCtx2 && audioDecFrame2 && audioIndex2 >= 0)
					{
						gotSecond = enqueueFromDshow(audioIfmt2, audioPkt2, audioDecCtx2, audioDecFrame2, audioIndex2, audioSwr2, audioFifo2);
					}
				}
				while (av_audio_fifo_size(audioFifo) >= encSamples)
				{
					if (dualMixEnabled && av_audio_fifo_size(audioFifo2) < encSamples)
					{
						break;
					}
					if (!fillAudioEncFrameFromFifos(audioEncFrame, audioEncCtx, audioFifo, audioFifo2, encSamples, dualMixEnabled))
					{
						break;
					}
					audioEncFrame->pts = audioPts;
					audioPts += encSamples;
					const int sendRet = avcodec_send_frame(audioEncCtx, audioEncFrame);
					if (sendRet < 0)
					{
						char errbuf[AV_ERROR_MAX_STRING_SIZE];
						av_strerror(sendRet, errbuf, sizeof(errbuf));
						appendLogLine(QStringLiteral("[摄像头推流] 音频送编码失败: %1").arg(QString::fromUtf8(errbuf)));
						break;
					}
					if (!writeEncodedPackets())
					{
						return;
					}
				}
				if (!gotMain && !gotSecond)
				{
					QThread::msleep(2);
				}
			}
			if (audioEncCtx && outAudioStream && audioOutPkt)
			{
				avcodec_send_frame(audioEncCtx, nullptr);
				(void)writeEncodedPackets();
			}
		});
	}

	while (!m_stopRequest.load(std::memory_order_relaxed))
	{
		const int threadAudioErr = audioThreadErr.load(std::memory_order_relaxed);
		if (threadAudioErr < 0)
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(threadAudioErr, errbuf, sizeof(errbuf));
			exitCode = threadAudioErr;
			setLastError(QStringLiteral("写入音频包失败: %1").arg(QString::fromUtf8(errbuf)));
			break;
		}
		frame = fplayer::CameraFrameBus::instance().snapshot();
		if (!frame.valid || frame.serial == lastSerial)
		{
			QThread::msleep(5);
			continue;
		}
		lastSerial = frame.serial;
		if (frame.width != encCtx->width || frame.height != encCtx->height)
		{
			setLastError(QStringLiteral("摄像头预览分辨率发生变化，请停止后重新推流"));
			exitCode = AVERROR(EINVAL);
			break;
		}
		if (av_frame_make_writable(encFrame) < 0)
		{
			continue;
		}
		const int uvWidth = (frame.width + 1) / 2;
		const int uvHeight = (frame.height + 1) / 2;
		for (int r = 0; r < frame.height; ++r)
		{
			std::memcpy(encFrame->data[0] + r * encFrame->linesize[0], frame.y.constData() + r * frame.yStride, frame.width);
		}
		for (int r = 0; r < uvHeight; ++r)
		{
			std::memcpy(encFrame->data[1] + r * encFrame->linesize[1], frame.u.constData() + r * frame.uStride, uvWidth);
			std::memcpy(encFrame->data[2] + r * encFrame->linesize[2], frame.v.constData() + r * frame.vStride, uvWidth);
		}
		encFrame->pts = framePts++;
		ret = avcodec_send_frame(encCtx, encFrame);
		if (ret < 0)
		{
			continue;
		}
		while (avcodec_receive_packet(encCtx, outPkt) == 0)
		{
			av_packet_rescale_ts(outPkt, encCtx->time_base, ofmt->streams[0]->time_base);
			outPkt->stream_index = 0;
			int vw = 0;
			{
				std::lock_guard<std::mutex> lk(muxWriteMutex);
				vw = av_interleaved_write_frame(ofmt, outPkt);
			}
			if (vw >= 0)
			{
				++avStatVideoPkts;
			}
			av_packet_unref(outPkt);
		}
		const auto statNow = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(statNow - avStatWindowStart).count() >= 1000)
		{
			appendLogLine(QStringLiteral("[摄像头推流] 发包统计 1s: video=%1 audio=%2").arg(avStatVideoPkts).arg(avStatAudioPkts));
			avStatVideoPkts = 0;
			avStatAudioPkts = 0;
			avStatWindowStart = statNow;
		}
	}
	audioThreadStop.store(true, std::memory_order_relaxed);
	if (audioThreadStarted && audioWorker.joinable())
	{
		audioWorker.join();
	}

	avcodec_send_frame(encCtx, nullptr);
	while (avcodec_receive_packet(encCtx, outPkt) == 0)
	{
		av_packet_rescale_ts(outPkt, encCtx->time_base, ofmt->streams[0]->time_base);
		outPkt->stream_index = 0;
		{
			std::lock_guard<std::mutex> lk(muxWriteMutex);
			av_interleaved_write_frame(ofmt, outPkt);
		}
		av_packet_unref(outPkt);
	}
	if (wroteHeader)
	{
		av_write_trailer(ofmt);
	}

cleanup:
	if (encFrame)
	{
		av_frame_free(&encFrame);
	}
	if (outPkt)
	{
		av_packet_free(&outPkt);
	}
	if (encCtx)
	{
		avcodec_free_context(&encCtx);
	}
	if (audioPkt)
	{
		av_packet_free(&audioPkt);
	}
	if (audioOutPkt)
	{
		av_packet_free(&audioOutPkt);
	}
	if (audioPkt2)
	{
		av_packet_free(&audioPkt2);
	}
	if (audioDecFrame)
	{
		av_frame_free(&audioDecFrame);
	}
	if (audioDecFrame2)
	{
		av_frame_free(&audioDecFrame2);
	}
	if (audioEncFrame)
	{
		av_frame_free(&audioEncFrame);
	}
	if (audioSwr)
	{
		swr_free(&audioSwr);
	}
	if (audioSwr2)
	{
		swr_free(&audioSwr2);
	}
	if (audioFifo)
	{
		av_audio_fifo_free(audioFifo);
		audioFifo = nullptr;
	}
	if (audioFifo2)
	{
		av_audio_fifo_free(audioFifo2);
		audioFifo2 = nullptr;
	}
	if (audioEncCtx)
	{
		avcodec_free_context(&audioEncCtx);
	}
	if (audioDecCtx)
	{
		avcodec_free_context(&audioDecCtx);
	}
	if (audioDecCtx2)
	{
		avcodec_free_context(&audioDecCtx2);
	}
	if (audioIfmt)
	{
		avformat_close_input(&audioIfmt);
	}
	if (audioIfmt2)
	{
		avformat_close_input(&audioIfmt2);
	}
	if (ofmt)
	{
		if (!(ofmt->oformat->flags & AVFMT_NOFILE) && ofmt->pb)
		{
			avio_closep(&ofmt->pb);
		}
		avformat_free_context(ofmt);
	}
	{
		QMutexLocker locker(&m_mutex);
		m_lastExitCode = exitCode;
	}
	m_completedSession.store(true, std::memory_order_relaxed);
	m_running.store(false, std::memory_order_relaxed);
}

void fplayer::StreamFFmpeg::pushCameraLoop(const QString& outputUrl, const QString& captureSpec)
{
	AVFormatContext* ifmt = nullptr;
	AVFormatContext* ofmt = nullptr;
	AVCodecContext* decCtx = nullptr;
	AVCodecContext* encCtx = nullptr;
	AVPacket* inPkt = nullptr;
	AVPacket* outPkt = nullptr;
	AVFrame* decFrame = nullptr;
	AVFrame* encFrame = nullptr;
	SwsContext* sws = nullptr;
	int exitCode = 0;
	int ret = 0;
	bool wroteHeader = false;
	int64_t framePts = 0;
	int videoIndex = -1;
	AVFormatContext* audioIfmt = nullptr;
	AVCodecContext* audioDecCtx = nullptr;
	AVFormatContext* audioIfmt2 = nullptr;
	AVCodecContext* audioDecCtx2 = nullptr;
	AVCodecContext* audioEncCtx = nullptr;
	SwrContext* audioSwr = nullptr;
	SwrContext* audioSwr2 = nullptr;
	AVAudioFifo* audioFifo = nullptr;
	AVAudioFifo* audioFifo2 = nullptr;
	AVPacket* audioPkt = nullptr;
	AVPacket* audioPkt2 = nullptr;
	AVPacket* audioOutPkt = nullptr;
	AVFrame* audioDecFrame = nullptr;
	AVFrame* audioDecFrame2 = nullptr;
	AVFrame* audioEncFrame = nullptr;
	int audioIndex = -1;
	int audioIndex2 = -1;
	AVStream* outAudioStream = nullptr;
	int64_t audioPts = 0;
	fplayer::windows_api::WasapiLoopbackCapture wasapiAudio;
	fplayer::windows_api::WasapiLoopbackCapture wasapiAudio2;
	bool useWasapiAudio = false;
	bool useWasapiAudio2 = false;
	std::mutex muxWriteMutex;
	std::atomic<bool> audioThreadStop{false};
	std::atomic<int> audioThreadErr{0};
	std::thread audioWorker;
	bool audioThreadStarted = false;

	const QByteArray outUtf8 = outputUrl.toUtf8();
	const char* outPath = outUtf8.constData();
	const CaptureParams params = parseCaptureParams(captureSpec);
	const QByteArray deviceUtf8 = params.device.toUtf8();
	const char* devicePath = deviceUtf8.constData();
	const int targetFps = qMax(1, params.fps);
	const QString requestedAudioOut = params.audioOutputSource.trimmed();
	const QString requestedAudioIn = params.audioInputSource.trimmed();
	const bool hasIn = !requestedAudioIn.isEmpty() && requestedAudioIn != QStringLiteral("off");
	const bool hasOut = !requestedAudioOut.isEmpty() && requestedAudioOut != QStringLiteral("off");
	const bool dualAudioRequested = hasIn && hasOut && requestedAudioIn != requestedAudioOut;
	const QString audioPrimarySource = dualAudioRequested ? requestedAudioIn : QString();
	const QString audioSecondarySource = dualAudioRequested ? requestedAudioOut : QString();
	const QString resolvedAudioSource = hasOut ? requestedAudioOut : (hasIn ? requestedAudioIn : QString());
	const bool enableAudio = !resolvedAudioSource.isEmpty() && resolvedAudioSource != QStringLiteral("off");
	avdevice_register_all();

	bool audioActive = enableAudio;
	if (audioActive)
	{
		QString openedAudioDevice;
		QString openDetail;
		const QString selectedPrimary = dualAudioRequested ? audioPrimarySource : resolvedAudioSource;
		if (!fplayer::windows_api::openDshowAudioInputWithFallback(selectedPrimary, m_stopRequest, audioIfmt, openedAudioDevice, openDetail))
		{
			QString wasapiErr;
			if (selectedPrimary == QStringLiteral("system") && wasapiAudio.init(wasapiErr))
			{
				useWasapiAudio = true;
				openedAudioDevice = QStringLiteral("native-wasapi-loopback(default)");
				appendLogLine(QStringLiteral("[摄像头采集推流] dshow音频不可用，已切换系统API回采: %1").arg(openDetail));
			}
			else
			{
				appendLogLine(QStringLiteral("[摄像头采集推流] 音频采集初始化失败，已自动降级为无声推流: %1; wasapi-native=%2")
					              .arg(openDetail)
					              .arg(wasapiErr));
				audioActive = false;
			}
		}
		if (audioActive)
		{
			if (audioIfmt)
			{
				audioIfmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
				audioIfmt->interrupt_callback.opaque = &m_stopRequest;
			}
			appendLogLine(QStringLiteral("[摄像头采集推流] 音频主设备=%1").arg(openedAudioDevice));
		}
		if (audioActive && audioIfmt)
		{
			ret = avformat_find_stream_info(audioIfmt, nullptr);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("读取音频流信息失败"));
				goto cleanup;
			}
			for (unsigned i = 0; i < audioIfmt->nb_streams; ++i)
			{
				if (audioIfmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
				{
					audioIndex = static_cast<int>(i);
					break;
				}
			}
			if (audioIndex < 0)
			{
				exitCode = AVERROR_STREAM_NOT_FOUND;
				setLastError(QStringLiteral("未找到可用音频流"));
				goto cleanup;
			}
			const AVCodecParameters* inAudioPar = audioIfmt->streams[audioIndex]->codecpar;
			const AVCodec* audioDec = avcodec_find_decoder(inAudioPar->codec_id);
			if (!audioDec)
			{
				exitCode = AVERROR_DECODER_NOT_FOUND;
				setLastError(QStringLiteral("未找到音频解码器"));
				goto cleanup;
			}
			audioDecCtx = avcodec_alloc_context3(audioDec);
			if (!audioDecCtx)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建音频解码器上下文失败"));
				goto cleanup;
			}
			ret = avcodec_parameters_to_context(audioDecCtx, inAudioPar);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("加载音频输入参数失败"));
				goto cleanup;
			}
			ret = avcodec_open2(audioDecCtx, audioDec, nullptr);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("打开音频解码器失败"));
				goto cleanup;
			}
		}
		audioPkt = av_packet_alloc();
		audioOutPkt = av_packet_alloc();
		audioDecFrame = av_frame_alloc();
		audioEncFrame = av_frame_alloc();
		if (!audioPkt || !audioOutPkt || !audioDecFrame || !audioEncFrame)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("分配音频编码资源失败"));
			goto cleanup;
		}
		if (audioActive && dualAudioRequested)
		{
			QString openedAudioDevice2;
			QString openDetail2;
			if (!fplayer::windows_api::openDshowAudioInputWithFallback(audioSecondarySource, m_stopRequest, audioIfmt2, openedAudioDevice2, openDetail2))
			{
				QString wasapiErr2;
				if (audioSecondarySource == QStringLiteral("system") && wasapiAudio2.init(wasapiErr2))
				{
					useWasapiAudio2 = true;
					openedAudioDevice2 = QStringLiteral("native-wasapi-loopback(default)");
				}
				else
				{
					appendLogLine(QStringLiteral("[摄像头采集推流] 副音频源不可用，退化为单音频: %1; wasapi-native=%2")
						              .arg(openDetail2)
						              .arg(wasapiErr2));
				}
			}
			if (audioIfmt2)
			{
				audioIfmt2->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
				audioIfmt2->interrupt_callback.opaque = &m_stopRequest;
				const int ret2 = avformat_find_stream_info(audioIfmt2, nullptr);
				if (ret2 >= 0)
				{
					for (unsigned i = 0; i < audioIfmt2->nb_streams; ++i)
					{
						if (audioIfmt2->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
						{
							audioIndex2 = static_cast<int>(i);
							break;
						}
					}
					if (audioIndex2 >= 0)
					{
						const AVCodecParameters* inAudioPar2 = audioIfmt2->streams[audioIndex2]->codecpar;
						const AVCodec* audioDec2 = avcodec_find_decoder(inAudioPar2->codec_id);
						if (audioDec2)
						{
							audioDecCtx2 = avcodec_alloc_context3(audioDec2);
							if (audioDecCtx2 && avcodec_parameters_to_context(audioDecCtx2, inAudioPar2) >= 0 &&
							    avcodec_open2(audioDecCtx2, audioDec2, nullptr) >= 0)
							{
								audioPkt2 = av_packet_alloc();
								audioDecFrame2 = av_frame_alloc();
								appendLogLine(QStringLiteral("[摄像头采集推流] 音频副设备=%1").arg(openedAudioDevice2));
							}
						}
					}
				}
			}
			else if (useWasapiAudio2)
			{
				appendLogLine(QStringLiteral("[摄像头采集推流] 音频副设备=native-wasapi-loopback(default)"));
			}
		}
	}

	inPkt = av_packet_alloc();
	outPkt = av_packet_alloc();
	decFrame = av_frame_alloc();
	encFrame = av_frame_alloc();
	if (!inPkt || !outPkt || !decFrame || !encFrame)
	{
		exitCode = AVERROR(ENOMEM);
		setLastError(QStringLiteral("分配编码资源失败"));
		goto cleanup;
	}

	{
		AVDictionary* inOpts = nullptr;
		av_dict_set(&inOpts, "framerate", QByteArray::number(targetFps).constData(), 0);
		if (params.width > 0 && params.height > 0)
		{
			const QByteArray sizeText = QByteArray::number(params.width) + "x" + QByteArray::number(params.height);
			av_dict_set(&inOpts, "video_size", sizeText.constData(), 0);
		}
		const AVInputFormat* inFmt = av_find_input_format("dshow");
		ifmt = avformat_alloc_context();
		if (!ifmt)
		{
			av_dict_free(&inOpts);
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("创建摄像头采集上下文失败"));
			goto cleanup;
		}
		ifmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
		ifmt->interrupt_callback.opaque = &m_stopRequest;
		ret = avformat_open_input(&ifmt, devicePath, inFmt, &inOpts);
		av_dict_free(&inOpts);
		if (ret < 0)
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			exitCode = ret;
			setLastError(QStringLiteral("打开摄像头采集失败: %1").arg(QString::fromUtf8(errbuf)));
			goto cleanup;
		}
	}
	ret = avformat_find_stream_info(ifmt, nullptr);
	if (ret < 0)
	{
		exitCode = ret;
		setLastError(QStringLiteral("读取摄像头流信息失败"));
		goto cleanup;
	}
	for (unsigned i = 0; i < ifmt->nb_streams; ++i)
	{
		if (ifmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			videoIndex = static_cast<int>(i);
			break;
		}
	}
	if (videoIndex < 0)
	{
		exitCode = AVERROR_STREAM_NOT_FOUND;
		setLastError(QStringLiteral("未找到摄像头视频流"));
		goto cleanup;
	}
	{
		const AVCodec* dec = avcodec_find_decoder(ifmt->streams[videoIndex]->codecpar->codec_id);
		if (!dec)
		{
			exitCode = AVERROR_DECODER_NOT_FOUND;
			setLastError(QStringLiteral("未找到摄像头解码器"));
			goto cleanup;
		}
		decCtx = avcodec_alloc_context3(dec);
		if (!decCtx)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("创建解码器上下文失败"));
			goto cleanup;
		}
		avcodec_parameters_to_context(decCtx, ifmt->streams[videoIndex]->codecpar);
		ret = avcodec_open2(decCtx, dec, nullptr);
		if (ret < 0)
		{
			exitCode = ret;
			setLastError(QStringLiteral("打开摄像头解码器失败"));
			goto cleanup;
		}
	}

	ret = avformat_alloc_output_context2(&ofmt, nullptr, "flv", outPath);
	if (ret < 0 || !ofmt)
	{
		exitCode = ret ? ret : AVERROR_UNKNOWN;
		setLastError(QStringLiteral("创建推流输出上下文失败"));
		goto cleanup;
	}
	ofmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
	ofmt->interrupt_callback.opaque = &m_stopRequest;

	{
		const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_H264);
		if (!enc)
		{
			enc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
		}
		if (!enc)
		{
			exitCode = AVERROR_ENCODER_NOT_FOUND;
			setLastError(QStringLiteral("未找到可用视频编码器(H264/MPEG4)"));
			goto cleanup;
		}
		encCtx = avcodec_alloc_context3(enc);
		if (!encCtx)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("创建编码器上下文失败"));
			goto cleanup;
		}
		encCtx->width = decCtx->width;
		encCtx->height = decCtx->height;
		encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
		encCtx->time_base = AVRational{1, targetFps};
		encCtx->framerate = AVRational{targetFps, 1};
		encCtx->gop_size = targetFps * 2;
		encCtx->max_b_frames = 0;
		const int bitrateKbps = params.bitrateKbps > 0
			                        ? params.bitrateKbps
			                        : estimateBitrateKbps(encCtx->width, encCtx->height, targetFps);
		encCtx->bit_rate = static_cast<int64_t>(bitrateKbps) * 1000;
		encCtx->rc_max_rate = encCtx->bit_rate;
		encCtx->rc_buffer_size = encCtx->bit_rate * 2;
		if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
		{
			encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		}
		if (enc->id == AV_CODEC_ID_H264 && encCtx->priv_data)
		{
			av_opt_set(encCtx->priv_data, "preset", "superfast", 0);
			av_opt_set(encCtx->priv_data, "tune", "zerolatency", 0);
		}
		appendLogLine(QStringLiteral("[摄像头采集推流] 设备=%1 输入=%2x%3 输出=%4x%5 FPS=%6 码率=%7kbps")
		              .arg(params.device)
		              .arg(decCtx->width)
		              .arg(decCtx->height)
		              .arg(encCtx->width)
		              .arg(encCtx->height)
		              .arg(targetFps)
		              .arg(bitrateKbps));
		ret = avcodec_open2(encCtx, enc, nullptr);
		if (ret < 0)
		{
			exitCode = ret;
			setLastError(QStringLiteral("打开编码器失败"));
			goto cleanup;
		}
		AVStream* outStream = avformat_new_stream(ofmt, nullptr);
		if (!outStream)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("创建输出视频流失败"));
			goto cleanup;
		}
		outStream->time_base = encCtx->time_base;
		avcodec_parameters_from_context(outStream->codecpar, encCtx);
		if (audioActive && ((audioIfmt && audioIndex >= 0) || useWasapiAudio))
		{
			const AVCodec* audioEnc = avcodec_find_encoder(AV_CODEC_ID_AAC);
			if (!audioEnc)
			{
				exitCode = AVERROR_ENCODER_NOT_FOUND;
				setLastError(QStringLiteral("未找到 AAC 音频编码器，无法推送声音"));
				goto cleanup;
			}
			audioEncCtx = avcodec_alloc_context3(audioEnc);
			if (!audioEncCtx)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建音频编码器上下文失败"));
				goto cleanup;
			}
			const int inSampleRate = useWasapiAudio
				                         ? (wasapiAudio.sampleRate() > 0 ? wasapiAudio.sampleRate() : 48000)
				                         : (audioDecCtx && audioDecCtx->sample_rate > 0 ? audioDecCtx->sample_rate : 48000);
			audioEncCtx->sample_rate = inSampleRate;
			if (audioEnc->supported_samplerates)
			{
				int bestRate = audioEnc->supported_samplerates[0];
				int bestDiff = qAbs(bestRate - audioEncCtx->sample_rate);
				for (const int* p = audioEnc->supported_samplerates; *p != 0; ++p)
				{
					const int diff = qAbs(*p - audioEncCtx->sample_rate);
					if (diff < bestDiff)
					{
						bestRate = *p;
						bestDiff = diff;
					}
				}
				audioEncCtx->sample_rate = bestRate;
			}
			const int srcChannels = useWasapiAudio
				                        ? wasapiAudio.channelLayout().nb_channels
				                        : (audioDecCtx ? audioDecCtx->ch_layout.nb_channels : 0);
			const int targetChannels = (srcChannels == 1) ? 1 : 2;
			av_channel_layout_uninit(&audioEncCtx->ch_layout);
			av_channel_layout_default(&audioEncCtx->ch_layout, targetChannels);
			if (audioEnc->ch_layouts && audioEnc->ch_layouts[0].nb_channels > 0)
			{
				const AVChannelLayout* best = &audioEnc->ch_layouts[0];
				int bestDiff = qAbs(best->nb_channels - targetChannels);
				for (const AVChannelLayout* p = audioEnc->ch_layouts; p && p->nb_channels > 0; ++p)
				{
					const int diff = qAbs(p->nb_channels - targetChannels);
					if (diff < bestDiff)
					{
						best = p;
						bestDiff = diff;
					}
				}
				av_channel_layout_uninit(&audioEncCtx->ch_layout);
				if (av_channel_layout_copy(&audioEncCtx->ch_layout, best) < 0)
				{
					av_channel_layout_default(&audioEncCtx->ch_layout, targetChannels);
				}
			}
			if (audioEncCtx->ch_layout.nb_channels <= 0)
			{
				av_channel_layout_uninit(&audioEncCtx->ch_layout);
				av_channel_layout_default(&audioEncCtx->ch_layout, 2);
			}
			audioEncCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
			if (audioEnc->sample_fmts)
			{
				bool fltpSupported = false;
				for (const AVSampleFormat* p = audioEnc->sample_fmts; *p != AV_SAMPLE_FMT_NONE; ++p)
				{
					if (*p == AV_SAMPLE_FMT_FLTP)
					{
						fltpSupported = true;
						break;
					}
				}
				if (!fltpSupported)
				{
					audioEncCtx->sample_fmt = audioEnc->sample_fmts[0];
				}
			}
			audioEncCtx->bit_rate = 128000;
			audioEncCtx->time_base = AVRational{1, audioEncCtx->sample_rate};
			audioEncCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
			if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
			{
				audioEncCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			}
			ret = avcodec_open2(audioEncCtx, audioEnc, nullptr);
			if (ret < 0)
			{
				char errbuf[AV_ERROR_MAX_STRING_SIZE];
				av_strerror(ret, errbuf, sizeof(errbuf));
				appendLogLine(QStringLiteral("[屏幕推流] 打开音频编码器失败: %1; sr=%2 ch=%3 fmt=%4")
					              .arg(QString::fromUtf8(errbuf))
					              .arg(audioEncCtx->sample_rate)
					              .arg(audioEncCtx->ch_layout.nb_channels)
					              .arg(static_cast<int>(audioEncCtx->sample_fmt)));
				exitCode = ret;
				setLastError(QStringLiteral("打开音频编码器失败"));
				goto cleanup;
			}
			outAudioStream = avformat_new_stream(ofmt, nullptr);
			if (!outAudioStream)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建输出音频流失败"));
				goto cleanup;
			}
			ret = avcodec_parameters_from_context(outAudioStream->codecpar, audioEncCtx);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("写入输出音频参数失败"));
				goto cleanup;
			}
			outAudioStream->codecpar->codec_tag = 0;
			outAudioStream->time_base = audioEncCtx->time_base;
			const AVChannelLayout* inLayout = nullptr;
			if (useWasapiAudio)
			{
				inLayout = wasapiAudio.channelLayout().nb_channels > 0 ? &wasapiAudio.channelLayout() : nullptr;
			}
			else
			{
				inLayout = audioDecCtx->ch_layout.nb_channels > 0 ? &audioDecCtx->ch_layout : nullptr;
			}
			AVChannelLayout defaultInLayout;
			if (!inLayout)
			{
				av_channel_layout_default(&defaultInLayout, 2);
				inLayout = &defaultInLayout;
			}
			const AVSampleFormat inSampleFmt = useWasapiAudio ? wasapiAudio.sampleFmt() : audioDecCtx->sample_fmt;
			const int inSampleRate2 = useWasapiAudio ? inSampleRate : audioDecCtx->sample_rate;
			ret = swr_alloc_set_opts2(&audioSwr, &audioEncCtx->ch_layout, audioEncCtx->sample_fmt, audioEncCtx->sample_rate,
			                          inLayout, inSampleFmt, inSampleRate2, 0, nullptr);
			if (!inLayout || inLayout == &defaultInLayout)
			{
				av_channel_layout_uninit(&defaultInLayout);
			}
			if (ret < 0 || !audioSwr)
			{
				exitCode = ret < 0 ? ret : AVERROR(EINVAL);
				setLastError(QStringLiteral("创建音频重采样器失败"));
				goto cleanup;
			}
			ret = swr_init(audioSwr);
			if (ret < 0)
			{
				exitCode = ret;
				setLastError(QStringLiteral("初始化音频重采样器失败"));
				goto cleanup;
			}
			const int fifoInitSamples = qMax(audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024, 1024) * 8;
			audioFifo = av_audio_fifo_alloc(audioEncCtx->sample_fmt, audioEncCtx->ch_layout.nb_channels, fifoInitSamples);
			if (!audioFifo)
			{
				exitCode = AVERROR(ENOMEM);
				setLastError(QStringLiteral("创建音频FIFO失败"));
				goto cleanup;
			}
			if ((audioDecCtx2 && audioPkt2 && audioDecFrame2) || useWasapiAudio2)
			{
				const AVChannelLayout* inLayout2 = nullptr;
				if (useWasapiAudio2)
				{
					inLayout2 = wasapiAudio2.channelLayout().nb_channels > 0 ? &wasapiAudio2.channelLayout() : nullptr;
				}
				else
				{
					inLayout2 = audioDecCtx2->ch_layout.nb_channels > 0 ? &audioDecCtx2->ch_layout : nullptr;
				}
				AVChannelLayout defaultInLayout2;
				if (!inLayout2)
				{
					av_channel_layout_default(&defaultInLayout2, 2);
					inLayout2 = &defaultInLayout2;
				}
				const AVSampleFormat inSampleFmt2 = useWasapiAudio2 ? wasapiAudio2.sampleFmt() : audioDecCtx2->sample_fmt;
				const int inSampleRate3 = useWasapiAudio2
					                          ? (wasapiAudio2.sampleRate() > 0 ? wasapiAudio2.sampleRate() : audioEncCtx->sample_rate)
					                          : audioDecCtx2->sample_rate;
				ret = swr_alloc_set_opts2(&audioSwr2, &audioEncCtx->ch_layout, audioEncCtx->sample_fmt, audioEncCtx->sample_rate,
				                          inLayout2, inSampleFmt2, inSampleRate3, 0, nullptr);
				if (!inLayout2 || inLayout2 == &defaultInLayout2)
				{
					av_channel_layout_uninit(&defaultInLayout2);
				}
				if (ret >= 0 && audioSwr2 && swr_init(audioSwr2) >= 0)
				{
					audioFifo2 = av_audio_fifo_alloc(audioEncCtx->sample_fmt, audioEncCtx->ch_layout.nb_channels, fifoInitSamples);
				}
				if (!audioFifo2)
				{
					if (audioSwr2)
					{
						swr_free(&audioSwr2);
					}
					appendLogLine(QStringLiteral("[摄像头采集推流] 副音频源混音初始化失败，降级单音频"));
				}
			}
		}
	}

	if (!(ofmt->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open2(&ofmt->pb, outPath, AVIO_FLAG_WRITE, nullptr, nullptr);
		if (ret < 0)
		{
			exitCode = ret;
			setLastError(QStringLiteral("打开推流输出地址失败"));
			goto cleanup;
		}
	}
	ret = avformat_write_header(ofmt, nullptr);
	if (ret < 0)
	{
		exitCode = ret;
		setLastError(QStringLiteral("写入推流头失败"));
		goto cleanup;
	}
	wroteHeader = true;

	encFrame->format = encCtx->pix_fmt;
	encFrame->width = encCtx->width;
	encFrame->height = encCtx->height;
	ret = av_frame_get_buffer(encFrame, 32);
	if (ret < 0)
	{
		exitCode = ret;
		setLastError(QStringLiteral("分配编码帧缓冲失败"));
		goto cleanup;
	}
	sws = sws_getContext(decCtx->width, decCtx->height, decCtx->pix_fmt, encCtx->width, encCtx->height, encCtx->pix_fmt,
	                     SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!sws)
	{
		exitCode = AVERROR(EINVAL);
		setLastError(QStringLiteral("创建像素格式转换器失败"));
		goto cleanup;
	}
	if (audioActive && outAudioStream && audioSwr && audioEncCtx && audioOutPkt && audioEncFrame && audioFifo)
	{
		audioThreadStarted = true;
		audioWorker = std::thread([&]() {
			const int encSamples = audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024;
			const bool dualMixEnabled = audioFifo2 && audioSwr2 &&
				(useWasapiAudio2 || (audioIfmt2 && audioPkt2 && audioDecCtx2 && audioDecFrame2 && audioIndex2 >= 0));
			const auto writeEncodedPackets = [&]() -> bool {
				while (avcodec_receive_packet(audioEncCtx, audioOutPkt) == 0)
				{
					av_packet_rescale_ts(audioOutPkt, audioEncCtx->time_base, outAudioStream->time_base);
					audioOutPkt->stream_index = outAudioStream->index;
					int aw = 0;
					{
						std::lock_guard<std::mutex> lk(muxWriteMutex);
						aw = av_interleaved_write_frame(ofmt, audioOutPkt);
					}
					if (aw < 0)
					{
						audioThreadErr.store(aw, std::memory_order_relaxed);
						audioThreadStop.store(true, std::memory_order_relaxed);
						av_packet_unref(audioOutPkt);
						return false;
					}
					av_packet_unref(audioOutPkt);
				}
				return true;
			};
			const auto enqueueFromWasapi =
				[&](fplayer::windows_api::WasapiLoopbackCapture& cap, SwrContext* swr, AVAudioFifo* fifo) -> bool {
				std::vector<uint8_t> captured;
				int capturedSamples = 0;
				if (!cap.readInterleaved(captured, capturedSamples) || capturedSamples <= 0)
				{
					return false;
				}
				const uint8_t* inData[1] = {captured.data()};
				const int srcRate = qMax(1, cap.sampleRate());
				const int dstSamples = av_rescale_rnd(
					swr_get_delay(swr, srcRate) + capturedSamples,
					audioEncCtx->sample_rate,
					srcRate,
					AV_ROUND_UP);
				if (dstSamples <= 0)
				{
					return false;
				}
				uint8_t** dstData = nullptr;
				int dstLinesize = 0;
				if (av_samples_alloc_array_and_samples(&dstData, &dstLinesize, audioEncCtx->ch_layout.nb_channels, dstSamples,
				                                       audioEncCtx->sample_fmt, 0) < 0)
				{
					return false;
				}
				const int converted = swr_convert(swr, dstData, dstSamples, inData, capturedSamples);
				bool ok = false;
				if (converted > 0)
				{
					const int need = av_audio_fifo_size(fifo) + converted;
					ok = av_audio_fifo_realloc(fifo, need) >= 0 &&
					     av_audio_fifo_write(fifo, reinterpret_cast<void**>(dstData), converted) >= converted;
				}
				av_freep(&dstData[0]);
				av_freep(&dstData);
				return ok;
			};
			const auto enqueueFromDshow =
				[&](AVFormatContext* ifmt, AVPacket* pkt, AVCodecContext* decCtx, AVFrame* decFrame, int index, SwrContext* swr,
				    AVAudioFifo* fifo) -> bool {
				const int audioRet = av_read_frame(ifmt, pkt);
				if (audioRet < 0)
				{
					return false;
				}
				bool wrote = false;
				if (pkt->stream_index == index)
				{
					const int decSendRet = avcodec_send_packet(decCtx, pkt);
					if (decSendRet >= 0)
					{
						while (avcodec_receive_frame(decCtx, decFrame) == 0)
						{
							const int dstSamples = av_rescale_rnd(
								swr_get_delay(swr, decCtx->sample_rate) + decFrame->nb_samples,
								audioEncCtx->sample_rate,
								decCtx->sample_rate,
								AV_ROUND_UP);
							if (dstSamples <= 0)
							{
								av_frame_unref(decFrame);
								continue;
							}
							uint8_t** dstData = nullptr;
							int dstLinesize = 0;
							if (av_samples_alloc_array_and_samples(&dstData, &dstLinesize, audioEncCtx->ch_layout.nb_channels, dstSamples,
							                                       audioEncCtx->sample_fmt, 0) >= 0)
							{
								const int converted = swr_convert(swr, dstData, dstSamples,
								                                  (const uint8_t* const*)decFrame->data, decFrame->nb_samples);
								if (converted > 0)
								{
									const int need = av_audio_fifo_size(fifo) + converted;
									if (av_audio_fifo_realloc(fifo, need) >= 0 &&
									    av_audio_fifo_write(fifo, reinterpret_cast<void**>(dstData), converted) >= converted)
									{
										wrote = true;
									}
								}
								av_freep(&dstData[0]);
								av_freep(&dstData);
							}
							av_frame_unref(decFrame);
						}
					}
				}
				av_packet_unref(pkt);
				return wrote;
			};
			while (!m_stopRequest.load(std::memory_order_relaxed) && !audioThreadStop.load(std::memory_order_relaxed))
			{
				bool gotMain = false;
				bool gotSecond = false;
				if (useWasapiAudio)
				{
					gotMain = enqueueFromWasapi(wasapiAudio, audioSwr, audioFifo);
				}
				else if (audioIfmt && audioPkt && audioDecCtx)
				{
					gotMain = enqueueFromDshow(audioIfmt, audioPkt, audioDecCtx, audioDecFrame, audioIndex, audioSwr, audioFifo);
				}
				if (dualMixEnabled)
				{
					if (useWasapiAudio2)
					{
						gotSecond = enqueueFromWasapi(wasapiAudio2, audioSwr2, audioFifo2);
					}
					else if (audioIfmt2 && audioPkt2 && audioDecCtx2 && audioDecFrame2 && audioIndex2 >= 0)
					{
						gotSecond = enqueueFromDshow(audioIfmt2, audioPkt2, audioDecCtx2, audioDecFrame2, audioIndex2, audioSwr2, audioFifo2);
					}
				}
				while (av_audio_fifo_size(audioFifo) >= encSamples)
				{
					if (dualMixEnabled && av_audio_fifo_size(audioFifo2) < encSamples)
					{
						break;
					}
					if (!fillAudioEncFrameFromFifos(audioEncFrame, audioEncCtx, audioFifo, audioFifo2, encSamples, dualMixEnabled))
					{
						break;
					}
					audioEncFrame->pts = audioPts;
					audioPts += encSamples;
					if (avcodec_send_frame(audioEncCtx, audioEncFrame) < 0)
					{
						break;
					}
					if (!writeEncodedPackets())
					{
						return;
					}
				}
				if (!gotMain && !gotSecond)
				{
					QThread::msleep(2);
				}
			}
			if (audioEncCtx && outAudioStream && audioOutPkt)
			{
				avcodec_send_frame(audioEncCtx, nullptr);
				(void)writeEncodedPackets();
			}
		});
	}

	while (!m_stopRequest.load(std::memory_order_relaxed))
	{
		const int threadAudioErr = audioThreadErr.load(std::memory_order_relaxed);
		if (threadAudioErr < 0)
		{
			exitCode = threadAudioErr;
			setLastError(QStringLiteral("写入音频包失败"));
			break;
		}
		ret = av_read_frame(ifmt, inPkt);
		if (ret < 0)
		{
			if (ret == AVERROR_EOF)
			{
				break;
			}
			if (ret == AVERROR(EAGAIN))
			{
				continue;
			}
			exitCode = ret;
			setLastError(QStringLiteral("读取摄像头帧失败"));
			break;
		}
		if (inPkt->stream_index != videoIndex)
		{
			av_packet_unref(inPkt);
			continue;
		}
		ret = avcodec_send_packet(decCtx, inPkt);
		av_packet_unref(inPkt);
		if (ret < 0)
		{
			continue;
		}
		while (avcodec_receive_frame(decCtx, decFrame) == 0)
		{
			av_frame_make_writable(encFrame);
			sws_scale(sws, decFrame->data, decFrame->linesize, 0, decCtx->height, encFrame->data, encFrame->linesize);
			encFrame->pts = framePts++;
			ret = avcodec_send_frame(encCtx, encFrame);
			if (ret < 0)
			{
				continue;
			}
			while (avcodec_receive_packet(encCtx, outPkt) == 0)
			{
				av_packet_rescale_ts(outPkt, encCtx->time_base, ofmt->streams[0]->time_base);
				outPkt->stream_index = 0;
				{
					std::lock_guard<std::mutex> lk(muxWriteMutex);
					av_interleaved_write_frame(ofmt, outPkt);
				}
				av_packet_unref(outPkt);
			}
			av_frame_unref(decFrame);
		}
	}
	audioThreadStop.store(true, std::memory_order_relaxed);
	if (audioThreadStarted && audioWorker.joinable())
	{
		audioWorker.join();
	}

	avcodec_send_frame(encCtx, nullptr);
	while (avcodec_receive_packet(encCtx, outPkt) == 0)
	{
		av_packet_rescale_ts(outPkt, encCtx->time_base, ofmt->streams[0]->time_base);
		outPkt->stream_index = 0;
		{
			std::lock_guard<std::mutex> lk(muxWriteMutex);
			av_interleaved_write_frame(ofmt, outPkt);
		}
		av_packet_unref(outPkt);
	}
	if (wroteHeader)
	{
		av_write_trailer(ofmt);
	}

cleanup:
	if (sws)
	{
		sws_freeContext(sws);
	}
	if (decFrame)
	{
		av_frame_free(&decFrame);
	}
	if (inPkt)
	{
		av_packet_free(&inPkt);
	}
	if (encFrame)
	{
		av_frame_free(&encFrame);
	}
	if (outPkt)
	{
		av_packet_free(&outPkt);
	}
	if (encCtx)
	{
		avcodec_free_context(&encCtx);
	}
	if (decCtx)
	{
		avcodec_free_context(&decCtx);
	}
	if (ofmt)
	{
		if (!(ofmt->oformat->flags & AVFMT_NOFILE) && ofmt->pb)
		{
			avio_closep(&ofmt->pb);
		}
		avformat_free_context(ofmt);
	}
	if (ifmt)
	{
		avformat_close_input(&ifmt);
	}
	{
		QMutexLocker locker(&m_mutex);
		m_lastExitCode = exitCode;
	}
	m_completedSession.store(true, std::memory_order_relaxed);
	m_running.store(false, std::memory_order_relaxed);
}
