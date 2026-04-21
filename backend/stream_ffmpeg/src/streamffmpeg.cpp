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
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <QList>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QThread>
#include <QUrl>
#if __has_include(<QAudioFormat>) && __has_include(<QAudioSink>) && __has_include(<QMediaDevices>) && __has_include(<QIODevice>)
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QIODevice>
#define FPLAYER_PREVIEW_AUDIO_WITH_QT 1
#else
#define FPLAYER_PREVIEW_AUDIO_WITH_QT 0
#endif
#include <fplayer/common/cameraframebus/cameraframebus.h>
#include <fplayer/common/screenframebus/screenframebus.h>
#include "audio_pipeline.h"
#include "streamffmpeg_helpers.h"
#include "platform/windows/audioinputprobe.h"
#include "platform/windows/wasapiloopbackcapture.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>
#include <QVector>
#include <QRect>

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

	// 组合合成只向 CPU 侧 YUV420P 三平面缓冲写入；硬编若选 NV12 则 V 平面缺失(data[2]==nullptr)，memset/draw 会直接崩溃。
	bool composeEncoderSupportsYuv420p(const AVCodec* codec)
	{
		if (!codec)
		{
			return false;
		}
		if (!codec->pix_fmts)
		{
			return true;
		}
		for (const AVPixelFormat* p = codec->pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
		{
			if (*p == AV_PIX_FMT_YUV420P)
			{
				return true;
			}
		}
		return false;
	}
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
		else if (route.kind == PushInputKind::ComposeScene)
		{
			m_worker = std::make_unique<std::thread>([this, outputUrl, route]() {
				pushComposeSceneLoop(outputUrl, route.spec);
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
	if (inputUrl.trimmed().isEmpty())
	{
		setLastError(QStringLiteral("拉流输入地址为空"));
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
	if (!startPullWorker(inputUrl, outputUrl.trimmed()))
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
		const bool udpOutput = outputUrl.startsWith(QStringLiteral("udp://"), Qt::CaseInsensitive);
		const bool rtmpOutput = outputUrl.startsWith(QStringLiteral("rtmp://"), Qt::CaseInsensitive);
		QString normalizedOutput = outputUrl;
		if (udpOutput && !normalizedOutput.contains(QStringLiteral("pkt_size="), Qt::CaseInsensitive))
		{
			normalizedOutput += normalizedOutput.contains(QLatin1Char('?')) ? QStringLiteral("&pkt_size=1316")
			                                                               : QStringLiteral("?pkt_size=1316");
		}
		m_worker = std::make_unique<std::thread>([this, inputUrl, normalizedOutput, udpOutput, rtmpOutput]() {
			remuxLoop(inputUrl, normalizedOutput, udpOutput ? "mpegts" : (rtmpOutput ? "flv" : nullptr));
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

void fplayer::StreamFFmpeg::setPreviewPaused(const bool paused)
{
	m_previewPaused.store(paused, std::memory_order_relaxed);
}

bool fplayer::StreamFFmpeg::previewPaused() const
{
	return m_previewPaused.load(std::memory_order_relaxed);
}

void fplayer::StreamFFmpeg::setPreviewVolume(const float volume)
{
	m_previewVolume.store(std::clamp(volume, 0.0f, 2.0f), std::memory_order_relaxed);
}

float fplayer::StreamFFmpeg::previewVolume() const
{
	return m_previewVolume.load(std::memory_order_relaxed);
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
	AVCodecContext* previewDecCtx = nullptr;
	SwsContext* previewSws = nullptr;
	AVFrame* previewDecFrame = nullptr;
	AVFrame* previewYuvFrame = nullptr;
	int previewVideoStreamIndex = -1;
	AVCodecContext* previewAudioDecCtx = nullptr;
	SwrContext* previewAudioSwr = nullptr;
	AVFrame* previewAudioDecFrame = nullptr;
	AVFrame* previewAudioOutFrame = nullptr;
	int previewAudioStreamIndex = -1;
#if FPLAYER_PREVIEW_AUDIO_WITH_QT
	QAudioSink* previewAudioSink = nullptr;
	QIODevice* previewAudioIo = nullptr;
#endif
	bool previewAudioReady = false;
	int previewAudioDecodedFrames = 0;
	int previewAudioResampledSamples = 0;
	int previewAudioWrittenBytes = 0;
	auto previewAudioStatWindowStart = std::chrono::steady_clock::now();
	int ret = 0;
	int exitCode = 0;
	bool wroteHeader = false;
	bool firstPacketArrived = false;
	const QUrl inputParsedUrl(inputUrl);
	const QString inputHost = inputParsedUrl.host().trimmed().toLower();
	const bool udpBindListener = inputUrl.startsWith(QStringLiteral("udp://0.0.0.0:"), Qt::CaseInsensitive);
	const bool listenerMode = inputUrl.contains(QStringLiteral("listen=1"), Qt::CaseInsensitive) ||
		inputUrl.contains(QStringLiteral("mode=listener"), Qt::CaseInsensitive) || udpBindListener ||
		inputHost == QStringLiteral("0.0.0.0");
	const bool isRtmpInput = inputUrl.startsWith(QStringLiteral("rtmp://"), Qt::CaseInsensitive);
	const bool isRtspInput = inputUrl.startsWith(QStringLiteral("rtsp://"), Qt::CaseInsensitive);
	bool waitingLogged = false;
	int openRetryCount = 0;
	int streamInfoRetryCount = 0;
	const char* effectiveOutputShortName = outputShortName;
	const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
	qint64 connectedMs = 0;
	qint64 firstPacketMs = 0;

	const QByteArray inUtf8 = inputUrl.toUtf8();
	QString effectiveOutputUrl = outputUrl.trimmed();
	const bool previewOnlyMode = effectiveOutputUrl.isEmpty();
	if (previewOnlyMode)
	{
#if defined(_WIN32)
		effectiveOutputUrl = QStringLiteral("NUL");
#else
		effectiveOutputUrl = QStringLiteral("/dev/null");
#endif
		effectiveOutputShortName = "null";
	}
	const QByteArray outUtf8 = effectiveOutputUrl.toUtf8();
	const char* inPath = inUtf8.constData();
	const char* outPath = outUtf8.constData();
	appendLogLine(QStringLiteral("[拉流] remuxLoop start"));
	appendLogLine(QStringLiteral("[拉流] 输入地址: %1").arg(inputUrl));
	appendLogLine(QStringLiteral("[拉流] 输出地址: %1").arg(outputUrl.isEmpty() ? QStringLiteral("<预览模式，不保存文件>") : outputUrl));
	appendLogLine(QStringLiteral("[拉流] 监听模式: %1").arg(listenerMode ? QStringLiteral("yes") : QStringLiteral("no")));
	const bool outputIsFile = !previewOnlyMode && !outputUrl.contains(QStringLiteral("://"));
	if (outputIsFile)
	{
		const QFileInfo outInfo(outputUrl);
		const QDir outDir = outInfo.dir();
		if (!outDir.exists())
		{
			if (QDir().mkpath(outDir.absolutePath()))
			{
				appendLogLine(QStringLiteral("[拉流] 已创建输出目录: %1").arg(outDir.absolutePath()));
			}
			else
			{
				appendLogLine(QStringLiteral("[拉流] 创建输出目录失败: %1").arg(outDir.absolutePath()));
			}
		}
	}

	pkt = av_packet_alloc();
	if (!pkt)
	{
		exitCode = AVERROR(ENOMEM);
		setLastError(QStringLiteral("分配 AVPacket 失败"));
		goto cleanup;
	}

	while (!m_stopRequest.load(std::memory_order_relaxed))
	{
		ifmt = avformat_alloc_context();
		if (!ifmt)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("分配输入 AVFormatContext 失败"));
			goto cleanup;
		}
		ifmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
		ifmt->interrupt_callback.opaque = &m_stopRequest;

		AVDictionary* inOpts = nullptr;
		if (listenerMode && (isRtmpInput || isRtspInput))
		{
			// 对 RTMP/RTSP 更稳定的方式：通过 demuxer 选项启用 listen，而不是仅靠 URL query。
			av_dict_set(&inOpts, "listen", "1", 0);
		}
		ret = avformat_open_input(&ifmt, inPath, nullptr, &inOpts);
		if (inOpts)
		{
			av_dict_free(&inOpts);
		}
		if (ret < 0)
		{
			exitCode = ret;
			++openRetryCount;
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			appendLogLine(QStringLiteral("[拉流][open_input] ret=%1 msg=%2 retry=%3")
			              .arg(ret)
			              .arg(QString::fromUtf8(errbuf))
			              .arg(openRetryCount));
			if (listenerMode)
			{
				if (!waitingLogged)
				{
					appendLogLine(QStringLiteral("[拉流] 等待推流端连接..."));
					waitingLogged = true;
				}
				if (ret != AVERROR_EXIT)
				{
					setLastError(QStringLiteral("等待推流连接中: %1").arg(QString::fromUtf8(errbuf)));
				}
				if (ifmt)
				{
					avformat_free_context(ifmt);
					ifmt = nullptr;
				}
				// 监听模式下 -138(AVERROR_EXIT) 常见于可中断等待过程，不应视为终止失败。
				if (ret == AVERROR_EXIT)
				{
					exitCode = 0;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				continue;
			}
			setLastError(QStringLiteral("打开输入失败: %1").arg(QString::fromUtf8(errbuf)));
			if (ifmt)
			{
				avformat_free_context(ifmt);
				ifmt = nullptr;
			}
			goto cleanup;
		}
		connectedMs = QDateTime::currentMSecsSinceEpoch();
		appendLogLine(QStringLiteral("[拉流] 输入连接建立，耗时=%1ms 重试=%2")
		              .arg(connectedMs - startMs)
		              .arg(openRetryCount));

		ret = avformat_find_stream_info(ifmt, nullptr);
		if (ret < 0)
		{
			exitCode = ret;
			++streamInfoRetryCount;
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			appendLogLine(QStringLiteral("[拉流][find_stream_info] ret=%1 msg=%2 retry=%3")
			              .arg(ret)
			              .arg(QString::fromUtf8(errbuf))
			              .arg(streamInfoRetryCount));
			if (listenerMode)
			{
				if (ret != AVERROR_EXIT)
				{
					setLastError(QStringLiteral("等待推流连接中: %1").arg(QString::fromUtf8(errbuf)));
				}
				avformat_close_input(&ifmt);
				ifmt = nullptr;
				if (ret == AVERROR_EXIT)
				{
					exitCode = 0;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				continue;
			}
			setLastError(QStringLiteral("解析流信息失败: %1").arg(QString::fromUtf8(errbuf)));
			goto cleanup;
		}
		appendLogLine(QStringLiteral("[拉流] 流信息解析成功，重试=%1").arg(streamInfoRetryCount));
		appendLogLine(QStringLiteral("[拉流] 输入流数量=%1").arg(ifmt ? static_cast<int>(ifmt->nb_streams) : 0));
		break;
	}
	if (!ifmt)
	{
		goto cleanup;
	}

	ret = avformat_alloc_output_context2(&ofmt, nullptr, effectiveOutputShortName, outPath);
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
	for (unsigned i = 0; i < ifmt->nb_streams; ++i)
	{
		if (ifmt->streams[i] && ifmt->streams[i]->codecpar &&
			ifmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			previewVideoStreamIndex = static_cast<int>(i);
			break;
		}
	}
	for (unsigned i = 0; i < ifmt->nb_streams; ++i)
	{
		if (ifmt->streams[i] && ifmt->streams[i]->codecpar &&
			ifmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			previewAudioStreamIndex = static_cast<int>(i);
			break;
		}
	}
	if (previewVideoStreamIndex >= 0)
	{
		const AVCodecParameters* vpar = ifmt->streams[previewVideoStreamIndex]->codecpar;
		const AVCodec* dec = avcodec_find_decoder(vpar->codec_id);
		if (dec)
		{
			previewDecCtx = avcodec_alloc_context3(dec);
			if (previewDecCtx && avcodec_parameters_to_context(previewDecCtx, vpar) >= 0 &&
				avcodec_open2(previewDecCtx, dec, nullptr) >= 0)
			{
				previewDecFrame = av_frame_alloc();
				previewYuvFrame = av_frame_alloc();
				if (previewDecFrame && previewYuvFrame)
				{
					appendLogLine(QStringLiteral("[拉流] 预览解码器已启用"));
				}
			}
		}
	}
	if (previewOnlyMode && previewAudioStreamIndex >= 0)
	{
		const AVCodecParameters* apar = ifmt->streams[previewAudioStreamIndex]->codecpar;
		const AVCodec* adec = avcodec_find_decoder(apar->codec_id);
		if (adec)
		{
			previewAudioDecCtx = avcodec_alloc_context3(adec);
			if (previewAudioDecCtx && avcodec_parameters_to_context(previewAudioDecCtx, apar) >= 0 &&
				avcodec_open2(previewAudioDecCtx, adec, nullptr) >= 0)
			{
				previewAudioDecFrame = av_frame_alloc();
				previewAudioOutFrame = av_frame_alloc();
				appendLogLine(QStringLiteral("[拉流] 预览音频解码器已启用"));
			}
		}
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
	appendLogLine(QStringLiteral("[拉流] 输出头写入成功"));

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
		if (previewDecCtx && previewDecFrame && previewYuvFrame && pkt->stream_index == previewVideoStreamIndex)
		{
			if (avcodec_send_packet(previewDecCtx, pkt) == 0)
			{
				while (avcodec_receive_frame(previewDecCtx, previewDecFrame) == 0)
				{
					if (!previewSws ||
						previewYuvFrame->width != previewDecFrame->width ||
						previewYuvFrame->height != previewDecFrame->height)
					{
						if (previewSws)
						{
							sws_freeContext(previewSws);
							previewSws = nullptr;
						}
						av_frame_unref(previewYuvFrame);
						previewYuvFrame->format = AV_PIX_FMT_YUV420P;
						previewYuvFrame->width = previewDecFrame->width;
						previewYuvFrame->height = previewDecFrame->height;
						if (av_frame_get_buffer(previewYuvFrame, 32) < 0)
						{
							break;
						}
						previewSws = sws_getContext(previewDecFrame->width, previewDecFrame->height,
						                            static_cast<AVPixelFormat>(previewDecFrame->format),
						                            previewDecFrame->width, previewDecFrame->height, AV_PIX_FMT_YUV420P,
						                            SWS_BILINEAR, nullptr, nullptr, nullptr);
					}
					if (previewSws && av_frame_make_writable(previewYuvFrame) >= 0)
					{
						sws_scale(previewSws,
						          previewDecFrame->data,
						          previewDecFrame->linesize,
						          0,
						          previewDecFrame->height,
						          previewYuvFrame->data,
						          previewYuvFrame->linesize);
						const int h = previewYuvFrame->height;
						const int uvH = (h + 1) / 2;
						const int ySize = previewYuvFrame->linesize[0] * h;
						const int uSize = previewYuvFrame->linesize[1] * uvH;
						const int vSize = previewYuvFrame->linesize[2] * uvH;
						QByteArray y(reinterpret_cast<const char*>(previewYuvFrame->data[0]), ySize);
						QByteArray u(reinterpret_cast<const char*>(previewYuvFrame->data[1]), uSize);
						QByteArray v(reinterpret_cast<const char*>(previewYuvFrame->data[2]), vSize);
						if (!m_previewPaused.load(std::memory_order_relaxed))
						{
							fplayer::ScreenFrameBus::instance().publish(y, u, v,
							                                            previewYuvFrame->width, previewYuvFrame->height,
							                                            previewYuvFrame->linesize[0],
							                                            previewYuvFrame->linesize[1],
							                                            previewYuvFrame->linesize[2],
							                                            QStringLiteral("pull_preview"));
						}
					}
				}
			}
		}
		if (!firstPacketArrived)
		{
			firstPacketArrived = true;
			firstPacketMs = QDateTime::currentMSecsSinceEpoch();
			appendLogLine(QStringLiteral("[拉流] 检测到上游推流连接"));
			appendLogLine(QStringLiteral("[拉流] 首包到达耗时=%1ms").arg(firstPacketMs - startMs));
		}
		if (previewOnlyMode && previewAudioDecCtx && previewAudioDecFrame && previewAudioOutFrame &&
			pkt->stream_index == previewAudioStreamIndex)
		{
			if (avcodec_send_packet(previewAudioDecCtx, pkt) == 0)
			{
				while (avcodec_receive_frame(previewAudioDecCtx, previewAudioDecFrame) == 0)
				{
#if FPLAYER_PREVIEW_AUDIO_WITH_QT
					if (!previewAudioReady)
					{
						QAudioFormat format = QMediaDevices::defaultAudioOutput().preferredFormat();
						if (format.sampleRate() <= 0)
						{
							format.setSampleRate(48000);
						}
						format.setChannelCount(2);
						format.setSampleFormat(QAudioFormat::Int16);
						previewAudioSink = new QAudioSink(format);
						previewAudioSink->setBufferSize(256 * 1024);
						previewAudioIo = previewAudioSink->start();
						if (!previewAudioIo)
						{
							appendLogLine(QStringLiteral("[拉流][音频] QAudioSink start 失败"));
							delete previewAudioSink;
							previewAudioSink = nullptr;
						}
						else
						{
							AVChannelLayout outLayout;
							av_channel_layout_default(&outLayout, format.channelCount());
							av_sample_fmt_is_planar(AV_SAMPLE_FMT_S16);
							swr_alloc_set_opts2(&previewAudioSwr,
							                    &outLayout,
							                    AV_SAMPLE_FMT_S16,
							                    format.sampleRate(),
							                    &previewAudioDecCtx->ch_layout,
							                    previewAudioDecCtx->sample_fmt,
							                    previewAudioDecCtx->sample_rate,
							                    0,
							                    nullptr);
							av_channel_layout_uninit(&outLayout);
							if (!previewAudioSwr || swr_init(previewAudioSwr) < 0)
							{
								appendLogLine(QStringLiteral("[拉流][音频] 重采样器初始化失败"));
								if (previewAudioSwr)
								{
									swr_free(&previewAudioSwr);
								}
							}
							else
							{
								previewAudioReady = true;
								appendLogLine(QStringLiteral("[拉流] 预览音频输出已启用 rate=48000 ch=2 fmt=s16"));
							}
						}
					}
					if (previewAudioReady && previewAudioSwr && previewAudioIo)
					{
						++previewAudioDecodedFrames;
						const int outSamples = av_rescale_rnd(
							swr_get_delay(previewAudioSwr, previewAudioDecCtx->sample_rate) + previewAudioDecFrame->nb_samples,
							48000,
							previewAudioDecCtx->sample_rate,
							AV_ROUND_UP);
						av_frame_unref(previewAudioOutFrame);
						previewAudioOutFrame->format = AV_SAMPLE_FMT_S16;
						previewAudioOutFrame->sample_rate = 48000;
						av_channel_layout_default(&previewAudioOutFrame->ch_layout, 2);
						previewAudioOutFrame->nb_samples = outSamples;
						if (av_frame_get_buffer(previewAudioOutFrame, 0) == 0)
						{
							const int conv = swr_convert(previewAudioSwr,
							                             previewAudioOutFrame->data,
							                             outSamples,
							                             const_cast<const uint8_t**>(previewAudioDecFrame->data),
							                             previewAudioDecFrame->nb_samples);
							if (conv > 0)
							{
								previewAudioResampledSamples += conv;
								const int outBytes = av_samples_get_buffer_size(nullptr, 2, conv, AV_SAMPLE_FMT_S16, 1);
								if (outBytes > 0)
								{
										float vol = m_previewVolume.load(std::memory_order_relaxed);
										if (m_previewPaused.load(std::memory_order_relaxed))
										{
											vol = 0.0f;
										}
										if (std::abs(vol - 1.0f) > 0.001f)
										{
											auto* pcm = reinterpret_cast<int16_t*>(previewAudioOutFrame->data[0]);
											const int samples = outBytes / static_cast<int>(sizeof(int16_t));
											for (int i = 0; i < samples; ++i)
											{
												const float normalized = static_cast<float>(pcm[i]) / 32768.0f;
												const float scaled = normalized * vol;
												const float limited = scaled / (1.0f + std::abs(scaled)); // soft limiter
												const int outSample = static_cast<int>(std::lrint(limited * 32767.0f));
												pcm[i] = static_cast<int16_t>(std::clamp(outSample, -32768, 32767));
											}
										}
										const qint64 written = previewAudioIo->write(reinterpret_cast<const char*>(previewAudioOutFrame->data[0]), outBytes);
									if (written > 0)
									{
										previewAudioWrittenBytes += static_cast<int>(written);
									}
								}
							}
						}
					}
#else
					if (!previewAudioReady)
					{
						appendLogLine(QStringLiteral("[拉流] 预览音频输出未启用：当前构建未包含 QtMultimedia，暂仅视频预览"));
						previewAudioReady = true;
					}
#endif
					const auto audioStatNow = std::chrono::steady_clock::now();
					if (std::chrono::duration_cast<std::chrono::milliseconds>(audioStatNow - previewAudioStatWindowStart).count() >= 1000)
					{
						appendLogLine(QStringLiteral("[拉流][音频] 1s: decFrames=%1 resampledSamples=%2 writtenBytes=%3")
						              .arg(previewAudioDecodedFrames)
						              .arg(previewAudioResampledSamples)
						              .arg(previewAudioWrittenBytes));
						previewAudioDecodedFrames = 0;
						previewAudioResampledSamples = 0;
						previewAudioWrittenBytes = 0;
						previewAudioStatWindowStart = audioStatNow;
					}
				}
			}
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
	if (m_stopRequest.load(std::memory_order_relaxed) && exitCode == AVERROR_EXIT)
	{
		exitCode = 0;
	}
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
	if (previewSws)
	{
		sws_freeContext(previewSws);
	}
	if (previewDecFrame)
	{
		av_frame_free(&previewDecFrame);
	}
	if (previewYuvFrame)
	{
		av_frame_free(&previewYuvFrame);
	}
	if (previewDecCtx)
	{
		avcodec_free_context(&previewDecCtx);
	}
	if (previewAudioOutFrame)
	{
		av_frame_free(&previewAudioOutFrame);
	}
	if (previewAudioDecFrame)
	{
		av_frame_free(&previewAudioDecFrame);
	}
	if (previewAudioSwr)
	{
		swr_free(&previewAudioSwr);
	}
	if (previewAudioDecCtx)
	{
		avcodec_free_context(&previewAudioDecCtx);
	}
	#if FPLAYER_PREVIEW_AUDIO_WITH_QT
	if (previewAudioSink)
	{
		previewAudioSink->stop();
		delete previewAudioSink;
		previewAudioSink = nullptr;
		previewAudioIo = nullptr;
	}
	#endif

	{
		QMutexLocker locker(&m_mutex);
		m_lastExitCode = exitCode;
	}
	appendLogLine(QStringLiteral("[拉流] remuxLoop 结束 exitCode=%1 openRetry=%2 infoRetry=%3")
	              .arg(exitCode)
	              .arg(openRetryCount)
	              .arg(streamInfoRetryCount));
	m_completedSession.store(true, std::memory_order_relaxed);
	m_running.store(false, std::memory_order_relaxed);
}

void fplayer::StreamFFmpeg::pushComposeSceneLoop(const QString& outputUrl, const QString& sceneSpec)
{
	struct ComposeItem
	{
		enum class Kind
		{
			Camera,
			Screen,
			File
		};
		Kind kind = Kind::Screen;
		QString sourceId;
		QRect rect;
	};

	const CaptureParams params = parseCaptureParams(sceneSpec);
	const int targetFps = qMax(1, params.fps);
	int sceneW = 0;
	int sceneH = 0;
	QVector<ComposeItem> items;
	{
		const QStringList parts = sceneSpec.split(';', Qt::SkipEmptyParts);
		for (const QString& raw : parts)
		{
			const int eq = raw.indexOf('=');
			if (eq <= 0)
			{
				continue;
			}
			const QString key = raw.left(eq).trimmed().toLower();
			const QString value = raw.mid(eq + 1).trimmed();
			if (key == QStringLiteral("scene_w"))
			{
				sceneW = value.toInt();
				continue;
			}
			if (key == QStringLiteral("scene_h"))
			{
				sceneH = value.toInt();
				continue;
			}
			if (!key.startsWith(QStringLiteral("src")))
			{
				continue;
			}
			const QStringList seg = value.split(',', Qt::SkipEmptyParts);
			if (seg.size() < 5)
			{
				continue;
			}
			ComposeItem it;
			const QString k = seg.at(0).trimmed().toLower();
			if (k == QStringLiteral("camera"))
			{
				it.kind = ComposeItem::Kind::Camera;
			}
			else if (k == QStringLiteral("file"))
			{
				it.kind = ComposeItem::Kind::File;
			}
			else
			{
				it.kind = ComposeItem::Kind::Screen;
			}
			if (seg.size() >= 6)
			{
				it.sourceId = seg.at(1).trimmed();
				it.rect = QRect(seg.at(2).toInt(), seg.at(3).toInt(), qMax(1, seg.at(4).toInt()), qMax(1, seg.at(5).toInt()));
			}
			else
			{
				it.sourceId = QStringLiteral("default");
				it.rect = QRect(seg.at(1).toInt(), seg.at(2).toInt(), qMax(1, seg.at(3).toInt()), qMax(1, seg.at(4).toInt()));
			}
			if (it.sourceId.isEmpty())
			{
				it.sourceId = QStringLiteral("default");
			}
			items.push_back(it);
		}
	}

	int outW = params.outWidth > 0 ? params.outWidth : sceneW;
	int outH = params.outHeight > 0 ? params.outHeight : sceneH;
	if (outW <= 0 || outH <= 0)
	{
		outW = 1280;
		outH = 720;
	}
	outW = qMax(2, outW) & ~1;
	outH = qMax(2, outH) & ~1;
	if (sceneW <= 0 || sceneH <= 0)
	{
		sceneW = outW;
		sceneH = outH;
	}
	// 预览画布 scene 与编码 out 若宽高比不一致，应对画布做等比缩放后居中（黑边），
	// 不得对 X/Y 轴用不同比例映射，否则拉流端会出现非等比拉伸（常见为「比预览更高/更扁」）。
	double composeScale = 1.0;
	int composeOffX = 0;
	int composeOffY = 0;
	{
		const int sw = qMax(1, sceneW);
		const int sh = qMax(1, sceneH);
		const double sx = static_cast<double>(outW) / static_cast<double>(sw);
		const double sy = static_cast<double>(outH) / static_cast<double>(sh);
		composeScale = qMin(sx, sy);
		const int innerW = qMax(2, static_cast<int>(std::lround(static_cast<double>(sw) * composeScale)) & ~1);
		const int innerH = qMax(2, static_cast<int>(std::lround(static_cast<double>(sh) * composeScale)) & ~1);
		composeOffX = (outW - innerW) / 2;
		composeOffY = (outH - innerH) / 2;
	}
	if (items.isEmpty())
	{
		setLastError(QStringLiteral("组合场景为空"));
		m_running.store(false, std::memory_order_relaxed);
		return;
	}
	{
		QStringList layoutItems;
		for (int i = 0; i < items.size(); ++i)
		{
			const auto& it = items.at(i);
			const QString kind = (it.kind == ComposeItem::Kind::Camera)
				                     ? QStringLiteral("camera")
				                     : (it.kind == ComposeItem::Kind::Screen ? QStringLiteral("screen") : QStringLiteral("file"));
			layoutItems << QStringLiteral("#%1[%2:%3](%4,%5,%6x%7)")
				               .arg(i)
				               .arg(kind)
				               .arg(it.sourceId)
				               .arg(it.rect.x())
				               .arg(it.rect.y())
				               .arg(it.rect.width())
				               .arg(it.rect.height());
		}
		// appendLogLine(QStringLiteral("[组合推流] 场景层级顺序=%1").arg(layoutItems.join(" -> ")));
	}

	AVFormatContext* ofmt = nullptr;
	AVCodecContext* encCtx = nullptr;
	AVFormatContext* audioIfmt = nullptr;
	AVFormatContext* audioIfmt2 = nullptr;
	AVCodecContext* audioDecCtx = nullptr;
	AVCodecContext* audioDecCtx2 = nullptr;
	AVCodecContext* audioEncCtx = nullptr;
	SwrContext* audioSwr = nullptr;
	SwrContext* audioSwr2 = nullptr;
	AVAudioFifo* audioFifo = nullptr;
	AVAudioFifo* audioFifo2 = nullptr;
	AVPacket* outPkt = av_packet_alloc();
	AVPacket* audioPkt = nullptr;
	AVPacket* audioPkt2 = nullptr;
	AVPacket* audioOutPkt = nullptr;
	AVFrame* encFrame = av_frame_alloc();
	AVFrame* audioDecFrame = nullptr;
	AVFrame* audioDecFrame2 = nullptr;
	AVFrame* audioEncFrame = nullptr;
	AVStream* outAudioStream = nullptr;
	int exitCode = 0;
	int ret = 0;
	bool wroteHeader = false;
	int audioIndex = -1;
	int audioIndex2 = -1;
	int64_t audioPts = 0;
	int audioInputSampleRate = 48000;
	bool audioEnabled = false;
	int64_t pts = 0;
	int64_t frameIntervalUs = qMax<int64_t>(1000, 1000000LL / targetFps);
	int64_t startUs = 0;
	int64_t nextFrameUs = 0;
	int noFrameCount = 0;
	QVector<int> sourceNoFrame;
	QVector<quint64> sourceLastSerial;
	QVector<QSize> sourceLastSize;
	QVector<QString> sourceLastDiag;
	QByteArray outUtf8;
	const char* outPath = nullptr;
	fplayer::windows_api::WasapiLoopbackCapture wasapiAudio;
	fplayer::windows_api::WasapiLoopbackCapture wasapiAudio2;
	bool useWasapiAudio = false;
	bool useWasapiAudio2 = false;
	std::mutex muxWriteMutex;
	std::atomic<bool> audioThreadStop{false};
	std::atomic<int> audioThreadErr{0};
	std::thread audioWorker;
	bool audioThreadStarted = false;
	auto drawPlaneNearest = [](uint8_t* dst, const int dstStride, const int dstW, const int dstH,
	                           const QByteArray& srcPlane, const int srcStride, const int srcW, const int srcH) {
		if (!dst || srcPlane.isEmpty() || dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0 || srcStride <= 0)
		{
			return;
		}
		const auto* src = reinterpret_cast<const uint8_t*>(srcPlane.constData());
		for (int y = 0; y < dstH; ++y)
		{
			const int sy = qBound(0, (y * srcH) / qMax(1, dstH), srcH - 1);
			uint8_t* d = dst + y * dstStride;
			const uint8_t* s = src + sy * srcStride;
			for (int x = 0; x < dstW; ++x)
			{
				const int sx = qBound(0, (x * srcW) / qMax(1, dstW), srcW - 1);
				d[x] = s[sx];
			}
		}
	};
	auto planeBytesEnough = [](const QByteArray& buf, const int stride, const int w, const int h) -> bool {
		if (stride <= 0 || w <= 0 || h <= 0 || buf.isEmpty())
		{
			return false;
		}
		const qint64 need = static_cast<qint64>(stride) * static_cast<qint64>(h - 1) + static_cast<qint64>(w);
		return need > 0 && static_cast<qint64>(buf.size()) >= need;
	};
	// 须在任意 goto cleanup 之前定义（MinGW 禁止跨过带初始化的 lambda）。
	auto drawYuv420pContainInSlot = [&](const int slotX, const int slotY, const int slotW, const int slotH,
	                                    const QByteArray& yBuf, const QByteArray& uBuf, const QByteArray& vBuf,
	                                    const int ys, const int us, const int vs, const int srcW, const int srcH) {
		if (!encFrame || !encFrame->data[0] || slotW <= 0 || slotH <= 0 || srcW <= 0 || srcH <= 0)
		{
			return;
		}
		const double scale = (std::min)(static_cast<double>(slotW) / static_cast<double>(srcW),
		                               static_cast<double>(slotH) / static_cast<double>(srcH));
		int paintW = qMax(2, static_cast<int>(std::lround(static_cast<double>(srcW) * scale)) & ~1);
		int paintH = qMax(2, static_cast<int>(std::lround(static_cast<double>(srcH) * scale)) & ~1);
		paintW = qMin(paintW, slotW) & ~1;
		paintH = qMin(paintH, slotH) & ~1;
		const int ox = slotX + (slotW - paintW) / 2;
		const int oy = slotY + (slotH - paintH) / 2;
		const int srcUvW = (srcW + 1) / 2;
		const int srcUvH = (srcH + 1) / 2;
		const int paintUvW = paintW / 2;
		const int paintUvH = paintH / 2;
		drawPlaneNearest(encFrame->data[0] + oy * encFrame->linesize[0] + ox, encFrame->linesize[0], paintW, paintH,
		                 yBuf, ys, srcW, srcH);
		drawPlaneNearest(encFrame->data[1] + (oy / 2) * encFrame->linesize[1] + (ox / 2), encFrame->linesize[1], paintUvW, paintUvH,
		                 uBuf, us, srcUvW, srcUvH);
		drawPlaneNearest(encFrame->data[2] + (oy / 2) * encFrame->linesize[2] + (ox / 2), encFrame->linesize[2], paintUvW, paintUvH,
		                 vBuf, vs, srcUvW, srcUvH);
	};
	if (!outPkt || !encFrame)
	{
		exitCode = AVERROR(ENOMEM);
		setLastError(QStringLiteral("组合推流资源分配失败"));
		goto cleanup;
	}

	outUtf8 = outputUrl.toUtf8();
	outPath = outUtf8.constData();
	ret = avformat_alloc_output_context2(&ofmt, nullptr, "flv", outPath);
	if (ret < 0 || !ofmt)
	{
		exitCode = ret < 0 ? ret : AVERROR_UNKNOWN;
		setLastError(QStringLiteral("组合推流创建输出上下文失败"));
		goto cleanup;
	}
	ofmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
	ofmt->interrupt_callback.opaque = &m_stopRequest;

	{
		QList<VideoEncoderChoice> candidates = pickVideoEncoderCandidates(params.videoEncoder);
		const QString preferEncoder = params.videoEncoder.trimmed().toLower();
		// 用户显式选择硬编时，失败后自动回退 CPU，保证组合推流不断流。
		if (preferEncoder == QStringLiteral("nvenc") || preferEncoder == QStringLiteral("amf"))
		{
			const QList<VideoEncoderChoice> cpuFallback = pickVideoEncoderCandidates(QStringLiteral("cpu"));
			for (const auto& c : cpuFallback)
			{
				bool exists = false;
				for (const auto& it : candidates)
				{
					if (it.codec == c.codec)
					{
						exists = true;
						break;
					}
				}
				if (!exists)
				{
					candidates.push_back(c);
				}
			}
		}
		const AVCodec* enc = nullptr;
		QString lastOpenError;
		QString firstTriedEncoder;
		for (const auto& c : candidates)
		{
			if (!c.codec)
			{
				continue;
			}
			// appendLogLine(QStringLiteral("[组合推流] 尝试编码器=%1").arg(QString::fromLatin1(c.codec->name ? c.codec->name : "unknown")));
			if (encCtx)
			{
				avcodec_free_context(&encCtx);
			}
			encCtx = avcodec_alloc_context3(c.codec);
			if (!encCtx)
			{
				continue;
			}
			encCtx->width = outW;
			encCtx->height = outH;
			if (!composeEncoderSupportsYuv420p(c.codec))
			{
				// appendLogLine(QStringLiteral("[组合推流] 编码器 %1 不支持 YUV420P，跳过（组合链路仅支持三平面写入）")
				// 	              .arg(QString::fromLatin1(c.codec->name ? c.codec->name : "unknown")));
				avcodec_free_context(&encCtx);
				encCtx = nullptr;
				continue;
			}
			encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
			encCtx->sample_aspect_ratio = AVRational{1, 1};
			encCtx->time_base = AVRational{1, targetFps};
			encCtx->framerate = AVRational{targetFps, 1};
			encCtx->gop_size = targetFps * 2;
			encCtx->max_b_frames = 0;
			const int bitrateKbps = params.bitrateKbps > 0 ? params.bitrateKbps : estimateBitrateKbps(outW, outH, targetFps);
			encCtx->bit_rate = static_cast<int64_t>(bitrateKbps) * 1000;
			encCtx->rc_max_rate = encCtx->bit_rate;
			encCtx->rc_buffer_size = encCtx->bit_rate * 2;
			if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
			{
				encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			}
			if (encCtx->priv_data)
			{
				const QString codecName = QString::fromLatin1(c.codec->name ? c.codec->name : "").toLower();
				if (codecName == QStringLiteral("h264_nvenc"))
				{
					av_opt_set(encCtx->priv_data, "preset", "p1", 0);
					av_opt_set(encCtx->priv_data, "tune", "ll", 0);
					av_opt_set(encCtx->priv_data, "rc", "cbr", 0);
					av_opt_set(encCtx->priv_data, "zerolatency", "1", 0);
				}
				else if (codecName == QStringLiteral("h264_amf"))
				{
					// AMF 不接受 x264 preset/tune 文本，保持默认参数避免选项解析失败。
				}
				else if (c.codec->id == AV_CODEC_ID_H264)
				{
					av_opt_set(encCtx->priv_data, "preset", "superfast", 0);
					av_opt_set(encCtx->priv_data, "tune", "zerolatency", 0);
				}
			}
			ret = avcodec_open2(encCtx, c.codec, nullptr);
			if (ret >= 0)
			{
				enc = c.codec;
				if (!firstTriedEncoder.isEmpty() && firstTriedEncoder != QString::fromLatin1(enc->name ? enc->name : ""))
				{
					// appendLogLine(QStringLiteral("[组合推流] 硬编不可用，已自动回退到 %1")
					// 	              .arg(QString::fromLatin1(enc->name ? enc->name : "unknown")));
				}
				break;
			}
			if (firstTriedEncoder.isEmpty())
			{
				firstTriedEncoder = QString::fromLatin1(c.codec->name ? c.codec->name : "");
			}
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			// appendLogLine(QStringLiteral("[组合推流] 编码器打开失败=%1 err=%2")
			// 	              .arg(QString::fromLatin1(c.codec->name ? c.codec->name : "unknown"))
			// 	              .arg(QString::fromUtf8(errbuf)));
			lastOpenError = QString::fromUtf8(errbuf);
		}
		if (!enc)
		{
			exitCode = ret < 0 ? ret : AVERROR_ENCODER_NOT_FOUND;
			setLastError(QStringLiteral("组合推流打开编码器失败: %1").arg(lastOpenError));
			goto cleanup;
		}
		AVStream* outStream = avformat_new_stream(ofmt, nullptr);
		if (!outStream)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("组合推流创建输出流失败"));
			goto cleanup;
		}
		outStream->time_base = encCtx->time_base;
		avcodec_parameters_from_context(outStream->codecpar, encCtx);
		outStream->codecpar->sample_aspect_ratio.num = 1;
		outStream->codecpar->sample_aspect_ratio.den = 1;
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
					appendLogLine(QStringLiteral("[组合推流] dshow音频不可用，已切换系统API回采: %1").arg(openDetail));
				}
				else
				{
					appendLogLine(QStringLiteral("[组合推流] 音频采集初始化失败，已自动降级为无声推流: %1; wasapi-native=%2")
						              .arg(openDetail)
						              .arg(wasapiErr));
					audioActive = false;
				}
			}
			if (audioIfmt)
			{
				audioIfmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
				audioIfmt->interrupt_callback.opaque = &m_stopRequest;
			}
			if (audioActive && ((audioIfmt && avformat_find_stream_info(audioIfmt, nullptr) >= 0) || useWasapiAudio))
			{
				if (audioIfmt)
				{
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
						audioActive = false;
					}
					else
					{
						const AVCodecParameters* inAudioPar = audioIfmt->streams[audioIndex]->codecpar;
						const AVCodec* audioDec = avcodec_find_decoder(inAudioPar->codec_id);
						if (!audioDec)
						{
							audioActive = false;
						}
						else
						{
							audioDecCtx = avcodec_alloc_context3(audioDec);
							if (!(audioDecCtx && avcodec_parameters_to_context(audioDecCtx, inAudioPar) >= 0 &&
							      avcodec_open2(audioDecCtx, audioDec, nullptr) >= 0))
							{
								if (audioDecCtx)
								{
									avcodec_free_context(&audioDecCtx);
								}
								audioActive = false;
							}
						}
					}
				}
			}
			else if (audioActive)
			{
				audioActive = false;
			}
			if (audioActive)
			{
				audioPkt = av_packet_alloc();
				audioPkt2 = av_packet_alloc();
				audioOutPkt = av_packet_alloc();
				audioDecFrame = av_frame_alloc();
				audioDecFrame2 = av_frame_alloc();
				audioEncFrame = av_frame_alloc();
				if (!audioPkt || !audioOutPkt || !audioDecFrame || !audioEncFrame || !audioPkt2 || !audioDecFrame2)
				{
					audioActive = false;
				}
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
					}
				}
				if (audioIfmt2)
				{
					audioIfmt2->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
					audioIfmt2->interrupt_callback.opaque = &m_stopRequest;
					if (avformat_find_stream_info(audioIfmt2, nullptr) >= 0)
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
			if (audioActive)
			{
				const AVCodec* audioEnc = avcodec_find_encoder(AV_CODEC_ID_AAC);
				if (!audioEnc)
				{
					audioActive = false;
				}
				else
				{
					audioEncCtx = avcodec_alloc_context3(audioEnc);
					if (!audioEncCtx)
					{
						audioActive = false;
					}
					else
					{
						audioInputSampleRate = useWasapiAudio
							                         ? (wasapiAudio.sampleRate() > 0 ? wasapiAudio.sampleRate() : 48000)
							                         : (audioDecCtx && audioDecCtx->sample_rate > 0 ? audioDecCtx->sample_rate : 48000);
						audioEncCtx->sample_rate = audioInputSampleRate;
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
						if (avcodec_open2(audioEncCtx, audioEnc, nullptr) < 0)
						{
							audioActive = false;
						}
					}
				}
			}
			if (audioActive)
			{
				outAudioStream = avformat_new_stream(ofmt, nullptr);
				if (!outAudioStream || avcodec_parameters_from_context(outAudioStream->codecpar, audioEncCtx) < 0)
				{
					audioActive = false;
				}
			}
			if (audioActive)
			{
				outAudioStream->codecpar->codec_tag = 0;
				outAudioStream->time_base = audioEncCtx->time_base;
				const AVChannelLayout* inLayout = nullptr;
				if (useWasapiAudio)
				{
					inLayout = wasapiAudio.channelLayout().nb_channels > 0 ? &wasapiAudio.channelLayout() : nullptr;
				}
				else
				{
					inLayout = audioDecCtx && audioDecCtx->ch_layout.nb_channels > 0 ? &audioDecCtx->ch_layout : nullptr;
				}
				AVChannelLayout defaultInLayout;
				if (!inLayout)
				{
					av_channel_layout_default(&defaultInLayout, 2);
					inLayout = &defaultInLayout;
				}
				const AVSampleFormat inSampleFmt = useWasapiAudio ? wasapiAudio.sampleFmt() : audioDecCtx->sample_fmt;
				const int inSampleRate2 = useWasapiAudio ? audioInputSampleRate : audioDecCtx->sample_rate;
				ret = swr_alloc_set_opts2(&audioSwr, &audioEncCtx->ch_layout, audioEncCtx->sample_fmt, audioEncCtx->sample_rate,
				                          inLayout, inSampleFmt, inSampleRate2, 0, nullptr);
				if (!inLayout || inLayout == &defaultInLayout)
				{
					av_channel_layout_uninit(&defaultInLayout);
				}
				if (ret < 0 || !audioSwr || swr_init(audioSwr) < 0)
				{
					audioActive = false;
				}
			}
			if (audioActive)
			{
				const int fifoInitSamples = qMax(audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024, 1024) * 8;
				audioFifo = av_audio_fifo_alloc(audioEncCtx->sample_fmt, audioEncCtx->ch_layout.nb_channels, fifoInitSamples);
				if (!audioFifo)
				{
					audioActive = false;
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
					}
				}
			}
			audioEnabled = audioActive;
		}
		// appendLogLine(QStringLiteral("[组合推流] 使用编码器=%1 输出=%2x%3 fps=%4")
		// 	              .arg(QString::fromLatin1(enc->name ? enc->name : "unknown"))
		// 	              .arg(encCtx->width)
		// 	              .arg(encCtx->height)
		// 	              .arg(targetFps));
	}

	if (!(ofmt->oformat->flags & AVFMT_NOFILE))
	{
	ret = avio_open2(&ofmt->pb, outPath, AVIO_FLAG_WRITE, nullptr, nullptr);
		if (ret < 0)
		{
			exitCode = ret;
			setLastError(QStringLiteral("组合推流打开输出失败"));
			goto cleanup;
		}
	}
	ret = avformat_write_header(ofmt, nullptr);
	if (ret < 0)
	{
		exitCode = ret;
		setLastError(QStringLiteral("组合推流写输出头失败"));
		goto cleanup;
	}
	wroteHeader = true;
	if (audioEnabled && outAudioStream && audioSwr && audioEncCtx && audioOutPkt && audioEncFrame && audioFifo)
	{
		audioThreadStarted = true;
		audioWorker = std::thread([&]() {
			const int encSamples = audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024;
			const int64_t audioFrameIntervalUs = qMax<int64_t>(
				1000,
				(static_cast<int64_t>(encSamples) * 1000000LL) / qMax(1, audioEncCtx->sample_rate));
			int64_t nextAudioEncodeUs = av_gettime_relative();
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
			const auto enqueueFromDshow = [&](AVFormatContext* ifmt, AVPacket* pkt, AVCodecContext* decCtx, AVFrame* decFrame, int index,
			                                  SwrContext* swr, AVAudioFifo* fifo) -> bool {
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
							if (dstSamples > 0)
							{
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
				else if (audioIfmt && audioPkt && audioDecCtx && audioDecFrame && audioIndex >= 0)
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
				int64_t nowUs = av_gettime_relative();
				if (nowUs > nextAudioEncodeUs + audioFrameIntervalUs * 3)
				{
					nextAudioEncodeUs = nowUs;
				}
				while (nowUs >= nextAudioEncodeUs)
				{
					const int mainSamples = av_audio_fifo_size(audioFifo);
					const int secondSamples = dualMixEnabled ? av_audio_fifo_size(audioFifo2) : 0;
					const bool canMixFullFrame = !dualMixEnabled || secondSamples >= encSamples;
					bool frameReady = false;
					if (mainSamples >= encSamples && canMixFullFrame)
					{
						frameReady = fillAudioEncFrameFromFifos(audioEncFrame, audioEncCtx, audioFifo, audioFifo2, encSamples, dualMixEnabled);
					}
					else if (mainSamples > 0)
					{
						const int readSamples = qMin(mainSamples, encSamples);
						// 次路不足时不阻塞主路，直接按主路补静音保证实时连续性。
						frameReady = fillAudioEncFrameFromFifosWithPadding(
							audioEncFrame,
							audioEncCtx,
							audioFifo,
							canMixFullFrame ? audioFifo2 : nullptr,
							readSamples,
							encSamples,
							canMixFullFrame);
					}
					else
					{
						av_frame_unref(audioEncFrame);
						audioEncFrame->nb_samples = encSamples;
						audioEncFrame->format = audioEncCtx->sample_fmt;
						audioEncFrame->sample_rate = audioEncCtx->sample_rate;
						av_channel_layout_copy(&audioEncFrame->ch_layout, &audioEncCtx->ch_layout);
						if (av_frame_get_buffer(audioEncFrame, 0) >= 0)
						{
							av_samples_set_silence(audioEncFrame->data, 0, encSamples, audioEncCtx->ch_layout.nb_channels,
							                       audioEncCtx->sample_fmt);
							frameReady = true;
						}
					}
					if (!frameReady)
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
					nextAudioEncodeUs += audioFrameIntervalUs;
					nowUs = av_gettime_relative();
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

	encFrame->format = encCtx->pix_fmt;
	encFrame->width = encCtx->width;
	encFrame->height = encCtx->height;
	ret = av_frame_get_buffer(encFrame, 32);
	if (ret < 0)
	{
		exitCode = ret;
		setLastError(QStringLiteral("组合推流分配编码帧失败"));
		goto cleanup;
	}
	if (!encFrame->data[0] || !encFrame->data[1] || !encFrame->data[2] || encCtx->pix_fmt != AV_PIX_FMT_YUV420P)
	{
		exitCode = AVERROR(EINVAL);
		setLastError(QStringLiteral("组合推流编码帧需为 YUV420P 且含 Y/U/V 三平面"));
		goto cleanup;
	}
	// appendLogLine(QStringLiteral("[组合推流] 场景=%1x%2 输出=%3x%4 源数量=%5").arg(sceneW).arg(sceneH).arg(outW).arg(outH).arg(items.size()));
	if (!params.audioOutputSource.trimmed().isEmpty() && params.audioOutputSource.trimmed() != QStringLiteral("off"))
	{
		// appendLogLine(QStringLiteral("[组合推流] 音频链路已启用，按 audio_in/audio_out 配置采集。"));
	}
	// appendLogLine(QStringLiteral("[组合推流] 诊断: frameIntervalUs=%1").arg(frameIntervalUs));

	startUs = av_gettime_relative();
	nextFrameUs = startUs;
	sourceNoFrame = QVector<int>(items.size(), 0);
	sourceLastSerial = QVector<quint64>(items.size(), 0);
	sourceLastSize = QVector<QSize>(items.size(), QSize());
	sourceLastDiag = QVector<QString>(items.size(), QStringLiteral("n/a"));
	while (!m_stopRequest.load(std::memory_order_relaxed))
	{
		const int threadAudioErr = audioThreadErr.load(std::memory_order_relaxed);
		if (threadAudioErr < 0)
		{
			exitCode = threadAudioErr;
			setLastError(QStringLiteral("组合推流写入音频包失败"));
			break;
		}
		const int64_t nowUs = av_gettime_relative();
		if (nowUs < nextFrameUs)
		{
			av_usleep(static_cast<unsigned>(nextFrameUs - nowUs));
		}
		ret = av_frame_make_writable(encFrame);
		if (ret < 0)
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			// appendLogLine(QStringLiteral("[组合推流] av_frame_make_writable 失败 err=%1").arg(QString::fromUtf8(errbuf)));
			exitCode = ret;
			setLastError(QStringLiteral("组合推流编码帧不可写"));
			goto cleanup;
		}
		for (int y = 0; y < encCtx->height; ++y)
		{
			memset(encFrame->data[0] + y * encFrame->linesize[0], 16, encCtx->width);
		}
		for (int y = 0; y < encCtx->height / 2; ++y)
		{
			memset(encFrame->data[1] + y * encFrame->linesize[1], 128, encCtx->width / 2);
			memset(encFrame->data[2] + y * encFrame->linesize[2], 128, encCtx->width / 2);
		}

		int composedCount = 0;
		for (int si = 0; si < items.size(); ++si)
		{
			const ComposeItem& it = items.at(si);
			int dx = composeOffX + static_cast<int>(std::lround(static_cast<double>(it.rect.x()) * composeScale));
			int dy = composeOffY + static_cast<int>(std::lround(static_cast<double>(it.rect.y()) * composeScale));
			int dw = qMax(2, static_cast<int>(std::lround(static_cast<double>(it.rect.width()) * composeScale)) & ~1);
			int dh = qMax(2, static_cast<int>(std::lround(static_cast<double>(it.rect.height()) * composeScale)) & ~1);
			if (dx < 0 || dy < 0 || dx + dw > outW || dy + dh > outH)
			{
				dx = qBound(0, dx, qMax(0, outW - 2));
				dy = qBound(0, dy, qMax(0, outH - 2));
				dw = qMin(dw, outW - dx) & ~1;
				dh = qMin(dh, outH - dy) & ~1;
			}
			if (dw <= 0 || dh <= 0)
			{
				continue;
			}
			if (it.kind == ComposeItem::Kind::Camera)
			{
				const auto cam = fplayer::CameraFrameBus::instance().snapshot(it.sourceId);
				if (!cam.valid || cam.width <= 0 || cam.height <= 0)
				{
					++noFrameCount;
					++sourceNoFrame[si];
					continue;
				}
				if (cam.yStride <= 0 || cam.uStride <= 0 || cam.vStride <= 0 || cam.y.isEmpty() || cam.u.isEmpty() || cam.v.isEmpty())
				{
					// appendLogLine(QStringLiteral("[组合推流] source#%1 camera帧异常 serial=%2 size=%3x%4 stride=%5/%6/%7 bytes=%8/%9/%10")
					// 	              .arg(si)
					// 	              .arg(cam.serial)
					// 	              .arg(cam.width)
					// 	              .arg(cam.height)
					// 	              .arg(cam.yStride)
					// 	              .arg(cam.uStride)
					// 	              .arg(cam.vStride)
					// 	              .arg(cam.y.size())
					// 	              .arg(cam.u.size())
					// 	              .arg(cam.v.size()));
					continue;
				}
				const int camUvW = (cam.width + 1) / 2;
				const int camUvH = (cam.height + 1) / 2;
				if (!planeBytesEnough(cam.y, cam.yStride, cam.width, cam.height) ||
				    !planeBytesEnough(cam.u, cam.uStride, camUvW, camUvH) ||
				    !planeBytesEnough(cam.v, cam.vStride, camUvW, camUvH))
				{
					// appendLogLine(QStringLiteral("[组合推流] source#%1 camera平面长度不足 serial=%2 size=%3x%4 stride=%5/%6/%7 bytes=%8/%9/%10")
					// 	              .arg(si)
					// 	              .arg(cam.serial)
					// 	              .arg(cam.width)
					// 	              .arg(cam.height)
					// 	              .arg(cam.yStride)
					// 	              .arg(cam.uStride)
					// 	              .arg(cam.vStride)
					// 	              .arg(cam.y.size())
					// 	              .arg(cam.u.size())
					// 	              .arg(cam.v.size()));
					continue;
				}
				sourceNoFrame[si] = 0;
				sourceLastSerial[si] = cam.serial;
				sourceLastSize[si] = QSize(cam.width, cam.height);
				sourceLastDiag[si] = QStringLiteral("camera serial=%1 %2x%3 stride=%4/%5/%6")
					                     .arg(cam.serial)
					                     .arg(cam.width)
					                     .arg(cam.height)
					                     .arg(cam.yStride)
					                     .arg(cam.uStride)
					                     .arg(cam.vStride);
				drawYuv420pContainInSlot(dx, dy, dw, dh, cam.y, cam.u, cam.v, cam.yStride, cam.uStride, cam.vStride, cam.width, cam.height);
				++composedCount;
				continue;
			}
			// 屏幕与文件素材均走 ScreenFrameBus（按 sourceId 分通道），槽位内等比 contain 由 drawYuv420pContainInSlot 统一处理。
			if (it.kind == ComposeItem::Kind::Screen || it.kind == ComposeItem::Kind::File)
			{
				const auto scr = fplayer::ScreenFrameBus::instance().snapshot(it.sourceId);
				if (!scr.valid || scr.width <= 0 || scr.height <= 0)
				{
					++noFrameCount;
					++sourceNoFrame[si];
					continue;
				}
				if (scr.yStride <= 0 || scr.uStride <= 0 || scr.vStride <= 0 || scr.y.isEmpty() || scr.u.isEmpty() || scr.v.isEmpty())
				{
					// appendLogLine(QStringLiteral("[组合推流] source#%1 screen帧异常 sid=%2 serial=%3 size=%4x%5 stride=%6/%7/%8 bytes=%9/%10/%11")
					// 	              .arg(si)
					// 	              .arg(it.sourceId)
					// 	              .arg(scr.serial)
					// 	              .arg(scr.width)
					// 	              .arg(scr.height)
					// 	              .arg(scr.yStride)
					// 	              .arg(scr.uStride)
					// 	              .arg(scr.vStride)
					// 	              .arg(scr.y.size())
					// 	              .arg(scr.u.size())
					// 	              .arg(scr.v.size()));
					continue;
				}
				const int scrUvW = (scr.width + 1) / 2;
				const int scrUvH = (scr.height + 1) / 2;
				if (!planeBytesEnough(scr.y, scr.yStride, scr.width, scr.height) ||
				    !planeBytesEnough(scr.u, scr.uStride, scrUvW, scrUvH) ||
				    !planeBytesEnough(scr.v, scr.vStride, scrUvW, scrUvH))
				{
					// appendLogLine(QStringLiteral("[组合推流] source#%1 screen平面长度不足 sid=%2 serial=%3 size=%4x%5 stride=%6/%7/%8 bytes=%9/%10/%11")
					// 	              .arg(si)
					// 	              .arg(it.sourceId)
					// 	              .arg(scr.serial)
					// 	              .arg(scr.width)
					// 	              .arg(scr.height)
					// 	              .arg(scr.yStride)
					// 	              .arg(scr.uStride)
					// 	              .arg(scr.vStride)
					// 	              .arg(scr.y.size())
					// 	              .arg(scr.u.size())
					// 	              .arg(scr.v.size()));
					continue;
				}
				sourceNoFrame[si] = 0;
				sourceLastSerial[si] = scr.serial;
				sourceLastSize[si] = QSize(scr.width, scr.height);
				const QString busTag = (it.kind == ComposeItem::Kind::File) ? QStringLiteral("file") : QStringLiteral("screen");
				sourceLastDiag[si] = QStringLiteral("%1 sid=%2 serial=%3 %4x%5 stride=%6/%7/%8")
					                     .arg(busTag)
					                     .arg(it.sourceId)
					                     .arg(scr.serial)
					                     .arg(scr.width)
					                     .arg(scr.height)
					                     .arg(scr.yStride)
					                     .arg(scr.uStride)
					                     .arg(scr.vStride);
				drawYuv420pContainInSlot(dx, dy, dw, dh, scr.y, scr.u, scr.v, scr.yStride, scr.uStride, scr.vStride, scr.width, scr.height);
				++composedCount;
			}
		}

		encFrame->pts = av_rescale_q(nextFrameUs - startUs, AVRational{1, 1000000}, encCtx->time_base);
		ret = avcodec_send_frame(encCtx, encFrame);
		if (ret < 0)
		{
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			// appendLogLine(QStringLiteral("[组合推流] avcodec_send_frame失败 pts=%1 err=%2")
			// 	              .arg(encFrame->pts)
			// 	              .arg(QString::fromUtf8(errbuf)));
			(void)errbuf;
		}
		if (ret >= 0)
		{
			while (avcodec_receive_packet(encCtx, outPkt) == 0)
			{
				av_packet_rescale_ts(outPkt, encCtx->time_base, ofmt->streams[0]->time_base);
				outPkt->stream_index = 0;
				int wret = 0;
				{
					std::lock_guard<std::mutex> lk(muxWriteMutex);
					wret = av_interleaved_write_frame(ofmt, outPkt);
				}
				if (wret < 0)
				{
					char errbuf[AV_ERROR_MAX_STRING_SIZE];
					av_strerror(wret, errbuf, sizeof(errbuf));
					// appendLogLine(QStringLiteral("[组合推流] 写包失败 pts=%1 dts=%2 size=%3 err=%4")
					// 	              .arg(outPkt->pts)
					// 	              .arg(outPkt->dts)
					// 	              .arg(outPkt->size)
					// 	              .arg(QString::fromUtf8(errbuf)));
					(void)errbuf;
				}
				av_packet_unref(outPkt);
			}
		}
		++pts;
		nextFrameUs += frameIntervalUs;
		if (nextFrameUs < av_gettime_relative() - frameIntervalUs * 3)
		{
			nextFrameUs = av_gettime_relative();
		}
#if 0
		// 组合推流心跳与逐源诊断（排障时可打开）
		static int heartbeat = 0;
		++heartbeat;
		if (heartbeat % qMax(1, targetFps * 2) == 0)
		{
			appendLogLine(QStringLiteral("[组合推流] 心跳 composed=%1/%2 noFrame=%3 pts=%4")
				              .arg(composedCount)
				              .arg(items.size())
				              .arg(noFrameCount)
				              .arg(pts));
			for (int i = 0; i < items.size(); ++i)
			{
				const auto& it = items.at(i);
				const QString kind = (it.kind == ComposeItem::Kind::Camera)
					                     ? QStringLiteral("camera")
					                     : (it.kind == ComposeItem::Kind::Screen ? QStringLiteral("screen") : QStringLiteral("file"));
				appendLogLine(QStringLiteral("[组合推流] source#%1 kind=%2 sid=%3 noFrameStreak=%4 lastSerial=%5 lastSize=%6x%7")
					              .arg(i)
					              .arg(kind)
					              .arg(it.sourceId)
					              .arg(sourceNoFrame[i])
					              .arg(sourceLastSerial[i])
					              .arg(sourceLastSize[i].width())
					              .arg(sourceLastSize[i].height()));
				appendLogLine(QStringLiteral("[组合推流] source#%1 diag=%2").arg(i).arg(sourceLastDiag[i]));
			}
			noFrameCount = 0;
		}
#else
		(void)composedCount;
#endif
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
	audioThreadStop.store(true, std::memory_order_relaxed);
	if (audioThreadStarted && audioWorker.joinable())
	{
		audioWorker.join();
	}
	if (wroteHeader)
	{
		av_write_trailer(ofmt);
	}

cleanup:
	audioThreadStop.store(true, std::memory_order_relaxed);
	if (audioThreadStarted && audioWorker.joinable())
	{
		audioWorker.join();
	}
	if (audioEncFrame)
	{
		av_frame_free(&audioEncFrame);
	}
	if (audioDecFrame2)
	{
		av_frame_free(&audioDecFrame2);
	}
	if (audioDecFrame)
	{
		av_frame_free(&audioDecFrame);
	}
	if (audioOutPkt)
	{
		av_packet_free(&audioOutPkt);
	}
	if (audioPkt2)
	{
		av_packet_free(&audioPkt2);
	}
	if (audioPkt)
	{
		av_packet_free(&audioPkt);
	}
	if (audioFifo2)
	{
		av_audio_fifo_free(audioFifo2);
	}
	if (audioFifo)
	{
		av_audio_fifo_free(audioFifo);
	}
	if (audioSwr2)
	{
		swr_free(&audioSwr2);
	}
	if (audioSwr)
	{
		swr_free(&audioSwr);
	}
	if (audioEncCtx)
	{
		avcodec_free_context(&audioEncCtx);
	}
	if (audioDecCtx2)
	{
		avcodec_free_context(&audioDecCtx2);
	}
	if (audioDecCtx)
	{
		avcodec_free_context(&audioDecCtx);
	}
	if (audioIfmt2)
	{
		avformat_close_input(&audioIfmt2);
	}
	if (audioIfmt)
	{
		avformat_close_input(&audioIfmt);
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
			if (encCtx->priv_data)
			{
				const QString codecName = QString::fromLatin1(enc->name ? enc->name : "").toLower();
				if (codecName == QStringLiteral("h264_nvenc"))
				{
					av_opt_set(encCtx->priv_data, "preset", "p1", 0);
					av_opt_set(encCtx->priv_data, "tune", "ll", 0);
					av_opt_set(encCtx->priv_data, "rc", "cbr", 0);
					av_opt_set(encCtx->priv_data, "zerolatency", "1", 0);
				}
				else if (codecName == QStringLiteral("h264_amf"))
				{
					// AMF 不接受 x264 preset/tune 文本，保持默认参数避免选项解析失败。
				}
				else if (enc->id == AV_CODEC_ID_H264)
				{
					av_opt_set(encCtx->priv_data, "preset", "ultrafast", 0);
					av_opt_set(encCtx->priv_data, "tune", "zerolatency", 0);
				}
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
			const int64_t audioFrameIntervalUs = qMax<int64_t>(
				1000,
				(static_cast<int64_t>(encSamples) * 1000000LL) / qMax(1, audioEncCtx->sample_rate));
			int64_t nextAudioEncodeUs = av_gettime_relative();
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
				int64_t nowUs = av_gettime_relative();
				if (nowUs > nextAudioEncodeUs + audioFrameIntervalUs * 3)
				{
					nextAudioEncodeUs = nowUs;
				}
				while (nowUs >= nextAudioEncodeUs)
				{
					const int mainSamples = av_audio_fifo_size(audioFifo);
					const int secondSamples = dualMixEnabled ? av_audio_fifo_size(audioFifo2) : 0;
					const bool canMixFullFrame = !dualMixEnabled || secondSamples >= encSamples;
					bool frameReady = false;
					if (mainSamples >= encSamples && canMixFullFrame)
					{
						frameReady = fillAudioEncFrameFromFifos(audioEncFrame, audioEncCtx, audioFifo, audioFifo2, encSamples, dualMixEnabled);
					}
					else if (mainSamples > 0)
					{
						const int readSamples = qMin(mainSamples, encSamples);
						frameReady = fillAudioEncFrameFromFifosWithPadding(
							audioEncFrame,
							audioEncCtx,
							audioFifo,
							canMixFullFrame ? audioFifo2 : nullptr,
							readSamples,
							encSamples,
							canMixFullFrame);
					}
					else
					{
						av_frame_unref(audioEncFrame);
						audioEncFrame->nb_samples = encSamples;
						audioEncFrame->format = audioEncCtx->sample_fmt;
						audioEncFrame->sample_rate = audioEncCtx->sample_rate;
						av_channel_layout_copy(&audioEncFrame->ch_layout, &audioEncCtx->ch_layout);
						if (av_frame_get_buffer(audioEncFrame, 0) >= 0)
						{
							av_samples_set_silence(audioEncFrame->data, 0, encSamples, audioEncCtx->ch_layout.nb_channels,
							                       audioEncCtx->sample_fmt);
							frameReady = true;
						}
					}
					if (!frameReady)
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
					nextAudioEncodeUs += audioFrameIntervalUs;
					nowUs = av_gettime_relative();
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

	// 等待当前屏幕预览帧可用，避免推流线程空跑。（暂停后总线不再更新 serial，需回退 snapshot 取最后一帧。）
	for (int i = 0; i < 300 && !m_stopRequest.load(std::memory_order_relaxed); ++i)
	{
		if (fplayer::ScreenFrameBus::instance().snapshotIfNew(lastSerial, frame) && frame.width > 0 && frame.height > 0)
		{
			break;
		}
		frame = fplayer::ScreenFrameBus::instance().snapshot();
		if (frame.valid && frame.width > 0 && frame.height > 0)
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
			if (encCtx->priv_data)
			{
				const QString codecName = QString::fromLatin1(enc->name ? enc->name : "").toLower();
				if (codecName == QStringLiteral("h264_nvenc"))
				{
					av_opt_set(encCtx->priv_data, "preset", "p1", 0);
					av_opt_set(encCtx->priv_data, "tune", "ll", 0);
					av_opt_set(encCtx->priv_data, "rc", "cbr", 0);
					av_opt_set(encCtx->priv_data, "zerolatency", "1", 0);
				}
				else if (codecName == QStringLiteral("h264_amf"))
				{
					// AMF 不接受 x264 preset/tune 文本，保持默认参数避免选项解析失败。
				}
				else if (enc->id == AV_CODEC_ID_H264)
				{
					av_opt_set(encCtx->priv_data, "preset", "ultrafast", 0);
					av_opt_set(encCtx->priv_data, "tune", "zerolatency", 0);
				}
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
			const int encSamples = audioEncCtx->frame_size > 0 ? audioEncCtx->frame_size : 1024;
			const int64_t audioFrameIntervalUs = qMax<int64_t>(
				1000,
				(static_cast<int64_t>(encSamples) * 1000000LL) / qMax(1, audioEncCtx->sample_rate));
			int64_t nextAudioEncodeUs = av_gettime_relative();
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
					int64_t nowUs = av_gettime_relative();
					if (nowUs > nextAudioEncodeUs + audioFrameIntervalUs * 3)
					{
						nextAudioEncodeUs = nowUs;
					}
					while (nowUs >= nextAudioEncodeUs)
					{
						const bool enableMix = dualMixEnabled && av_audio_fifo_size(audioFifo2) >= encSamples;
						const int mainSamples = av_audio_fifo_size(audioFifo);
						bool frameReady = false;
						if (mainSamples >= encSamples && (!dualMixEnabled || enableMix))
						{
							frameReady = fillAudioEncFrameFromFifos(audioEncFrame, audioEncCtx, audioFifo, audioFifo2, encSamples, enableMix);
						}
						else if (mainSamples > 0)
						{
							const int readSamples = qMin(mainSamples, encSamples);
							frameReady = fillAudioEncFrameFromFifosWithPadding(
								audioEncFrame,
								audioEncCtx,
								audioFifo,
								enableMix ? audioFifo2 : nullptr,
								readSamples,
								encSamples,
								enableMix);
						}
						else
						{
							av_frame_unref(audioEncFrame);
							audioEncFrame->nb_samples = encSamples;
							audioEncFrame->format = audioEncCtx->sample_fmt;
							audioEncFrame->sample_rate = audioEncCtx->sample_rate;
							av_channel_layout_copy(&audioEncFrame->ch_layout, &audioEncCtx->ch_layout);
							if (av_frame_get_buffer(audioEncFrame, 0) >= 0)
							{
								av_samples_set_silence(audioEncFrame->data, 0, encSamples, audioEncCtx->ch_layout.nb_channels,
								                       audioEncCtx->sample_fmt);
								frameReady = true;
							}
						}
						if (!frameReady)
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
						nextAudioEncodeUs += audioFrameIntervalUs;
						nowUs = av_gettime_relative();
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
									int64_t nowUs = av_gettime_relative();
									if (nowUs > nextAudioEncodeUs + audioFrameIntervalUs * 3)
									{
										nextAudioEncodeUs = nowUs;
									}
									while (nowUs >= nextAudioEncodeUs)
									{
										const bool enableMix = dualMixEnabled && av_audio_fifo_size(audioFifo2) >= encSamples;
										const int mainSamples = av_audio_fifo_size(audioFifo);
										bool frameReady = false;
										if (mainSamples >= encSamples && (!dualMixEnabled || enableMix))
										{
											frameReady = fillAudioEncFrameFromFifos(audioEncFrame, audioEncCtx, audioFifo, audioFifo2, encSamples,
											                                        enableMix);
										}
										else if (mainSamples > 0)
										{
											const int readSamples = qMin(mainSamples, encSamples);
											frameReady = fillAudioEncFrameFromFifosWithPadding(
												audioEncFrame,
												audioEncCtx,
												audioFifo,
												enableMix ? audioFifo2 : nullptr,
												readSamples,
												encSamples,
												enableMix);
										}
										else
										{
											av_frame_unref(audioEncFrame);
											audioEncFrame->nb_samples = encSamples;
											audioEncFrame->format = audioEncCtx->sample_fmt;
											audioEncFrame->sample_rate = audioEncCtx->sample_rate;
											av_channel_layout_copy(&audioEncFrame->ch_layout, &audioEncCtx->ch_layout);
											if (av_frame_get_buffer(audioEncFrame, 0) >= 0)
											{
												av_samples_set_silence(audioEncFrame->data, 0, encSamples, audioEncCtx->ch_layout.nb_channels,
												                       audioEncCtx->sample_fmt);
												frameReady = true;
											}
										}
										if (!frameReady)
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
										nextAudioEncodeUs += audioFrameIntervalUs;
										nowUs = av_gettime_relative();
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
		const bool gotNew = fplayer::ScreenFrameBus::instance().snapshotIfNew(lastSerial, frame);
		if (!gotNew)
		{
			// 预览暂停时 serial 不递增：用最后一帧按帧率重复编码，拉流端才能持续收到视频而非一直缓冲。
			frame = fplayer::ScreenFrameBus::instance().snapshot();
			if (!frame.valid || frame.width <= 0 || frame.height <= 0)
			{
				QThread::msleep(2);
				continue;
			}
		}
		else
		{
			lastSerial = frame.serial;
		}
		const auto now = std::chrono::steady_clock::now();
		if (now < nextEncodeAt)
		{
			QThread::msleep(1);
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
	std::chrono::steady_clock::time_point nextCamHoldEncodeAt{};
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
	const int camHoldFrameIntervalMs = qMax(1, 1000 / targetFps);
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
			const int64_t audioFrameIntervalUs = qMax<int64_t>(
				1000,
				(static_cast<int64_t>(encSamples) * 1000000LL) / qMax(1, audioEncCtx->sample_rate));
			int64_t nextAudioEncodeUs = av_gettime_relative();
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
				int64_t nowUs = av_gettime_relative();
				if (nowUs > nextAudioEncodeUs + audioFrameIntervalUs * 3)
				{
					nextAudioEncodeUs = nowUs;
				}
				while (nowUs >= nextAudioEncodeUs)
				{
					const int mainSamples = av_audio_fifo_size(audioFifo);
					const int secondSamples = dualMixEnabled ? av_audio_fifo_size(audioFifo2) : 0;
					const bool canMixFullFrame = !dualMixEnabled || secondSamples >= encSamples;
					bool frameReady = false;
					if (mainSamples >= encSamples && canMixFullFrame)
					{
						frameReady = fillAudioEncFrameFromFifos(audioEncFrame, audioEncCtx, audioFifo, audioFifo2, encSamples, dualMixEnabled);
					}
					else if (mainSamples > 0)
					{
						const int readSamples = qMin(mainSamples, encSamples);
						frameReady = fillAudioEncFrameFromFifosWithPadding(
							audioEncFrame,
							audioEncCtx,
							audioFifo,
							canMixFullFrame ? audioFifo2 : nullptr,
							readSamples,
							encSamples,
							canMixFullFrame);
					}
					else
					{
						av_frame_unref(audioEncFrame);
						audioEncFrame->nb_samples = encSamples;
						audioEncFrame->format = audioEncCtx->sample_fmt;
						audioEncFrame->sample_rate = audioEncCtx->sample_rate;
						av_channel_layout_copy(&audioEncFrame->ch_layout, &audioEncCtx->ch_layout);
						if (av_frame_get_buffer(audioEncFrame, 0) >= 0)
						{
							av_samples_set_silence(audioEncFrame->data, 0, encSamples, audioEncCtx->ch_layout.nb_channels,
							                       audioEncCtx->sample_fmt);
							frameReady = true;
						}
					}
					if (!frameReady)
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
					nextAudioEncodeUs += audioFrameIntervalUs;
					nowUs = av_gettime_relative();
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

	nextCamHoldEncodeAt = std::chrono::steady_clock::now();
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
		if (!frame.valid || frame.width <= 0 || frame.height <= 0)
		{
			QThread::msleep(5);
			continue;
		}
		const bool newCamSerial = (frame.serial != lastSerial);
		if (newCamSerial)
		{
			lastSerial = frame.serial;
		}
		else
		{
			// 预览暂停：serial 不变，按目标帧率重复编码最后一帧，否则拉流端无视频包。
			const auto nowHold = std::chrono::steady_clock::now();
			if (nowHold < nextCamHoldEncodeAt)
			{
				QThread::msleep(1);
				continue;
			}
			nextCamHoldEncodeAt = nowHold + std::chrono::milliseconds(camHoldFrameIntervalMs);
		}
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
			const int64_t audioFrameIntervalUs = qMax<int64_t>(
				1000,
				(static_cast<int64_t>(encSamples) * 1000000LL) / qMax(1, audioEncCtx->sample_rate));
			int64_t nextAudioEncodeUs = av_gettime_relative();
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
				int64_t nowUs = av_gettime_relative();
				if (nowUs > nextAudioEncodeUs + audioFrameIntervalUs * 3)
				{
					nextAudioEncodeUs = nowUs;
				}
				while (nowUs >= nextAudioEncodeUs)
				{
					const int mainSamples = av_audio_fifo_size(audioFifo);
					const int secondSamples = dualMixEnabled ? av_audio_fifo_size(audioFifo2) : 0;
					const bool canMixFullFrame = !dualMixEnabled || secondSamples >= encSamples;
					bool frameReady = false;
					if (mainSamples >= encSamples && canMixFullFrame)
					{
						frameReady = fillAudioEncFrameFromFifos(audioEncFrame, audioEncCtx, audioFifo, audioFifo2, encSamples, dualMixEnabled);
					}
					else if (mainSamples > 0)
					{
						const int readSamples = qMin(mainSamples, encSamples);
						frameReady = fillAudioEncFrameFromFifosWithPadding(
							audioEncFrame,
							audioEncCtx,
							audioFifo,
							canMixFullFrame ? audioFifo2 : nullptr,
							readSamples,
							encSamples,
							canMixFullFrame);
					}
					else
					{
						av_frame_unref(audioEncFrame);
						audioEncFrame->nb_samples = encSamples;
						audioEncFrame->format = audioEncCtx->sample_fmt;
						audioEncFrame->sample_rate = audioEncCtx->sample_rate;
						av_channel_layout_copy(&audioEncFrame->ch_layout, &audioEncCtx->ch_layout);
						if (av_frame_get_buffer(audioEncFrame, 0) >= 0)
						{
							av_samples_set_silence(audioEncFrame->data, 0, encSamples, audioEncCtx->ch_layout.nb_channels,
							                       audioEncCtx->sample_fmt);
							frameReady = true;
						}
					}
					if (!frameReady)
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
					nextAudioEncodeUs += audioFrameIntervalUs;
					nowUs = av_gettime_relative();
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
