#include <fplayer/backend/net_ffmpeg/streamffmpeg.h>

extern "C"
{
#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <QMutexLocker>
#include <QThread>
#include <fplayer/common/cameraframebus/cameraframebus.h>
#include <chrono>
#include <cstring>
#include <mutex>

namespace
{
	struct CaptureParams
	{
		QString device;
		QString source;
		int fps = 30;
		int width = 0;
		int height = 0;
		int outWidth = 0;
		int outHeight = 0;
		int x = 0;
		int y = 0;
		int bitrateKbps = 0;
	};

	void appendLimited(QString& buf, const QString& line)
	{
		buf += line;
		if (buf.size() > 16000)
		{
			buf = buf.right(16000);
		}
	}

	CaptureParams parseCaptureParams(const QString& spec)
	{
		CaptureParams params;
		const QStringList parts = spec.split(';', Qt::SkipEmptyParts);
		for (const QString& raw : parts)
		{
			const QString part = raw.trimmed();
			const int eq = part.indexOf('=');
			if (eq <= 0)
			{
				continue;
			}
			const QString key = part.left(eq).trimmed().toLower();
			const QString value = part.mid(eq + 1).trimmed();
			if (key == QStringLiteral("video"))
			{
				params.device = QStringLiteral("video=") + value;
				continue;
			}
			if (key == QStringLiteral("device"))
			{
				params.device = value;
				continue;
			}
			if (key == QStringLiteral("src64"))
			{
				params.source = QString::fromUtf8(QByteArray::fromBase64(value.toUtf8()));
				continue;
			}
			if (key == QStringLiteral("fps"))
			{
				bool ok = false;
				const int fps = value.toInt(&ok);
				if (ok && fps > 0)
				{
					params.fps = fps;
				}
				continue;
			}
			if (key == QStringLiteral("size"))
			{
				const QStringList wh = value.split('x', Qt::SkipEmptyParts);
				if (wh.size() == 2)
				{
					bool wOk = false;
					bool hOk = false;
					const int w = wh.at(0).trimmed().toInt(&wOk);
					const int h = wh.at(1).trimmed().toInt(&hOk);
					if (wOk && hOk && w > 0 && h > 0)
					{
						params.width = w;
						params.height = h;
					}
				}
				continue;
			}
			if (key == QStringLiteral("outsize"))
			{
				const QStringList wh = value.split('x', Qt::SkipEmptyParts);
				if (wh.size() == 2)
				{
					bool wOk = false;
					bool hOk = false;
					const int w = wh.at(0).trimmed().toInt(&wOk);
					const int h = wh.at(1).trimmed().toInt(&hOk);
					if (wOk && hOk && w > 0 && h > 0)
					{
						params.outWidth = w;
						params.outHeight = h;
					}
				}
				continue;
			}
			if (key == QStringLiteral("x"))
			{
				bool ok = false;
				const int v = value.toInt(&ok);
				if (ok)
				{
					params.x = v;
				}
				continue;
			}
			if (key == QStringLiteral("y"))
			{
				bool ok = false;
				const int v = value.toInt(&ok);
				if (ok)
				{
					params.y = v;
				}
				continue;
			}
			if (key == QStringLiteral("bitrate"))
			{
				bool ok = false;
				const int kbps = value.toInt(&ok);
				if (ok && kbps > 0)
				{
					params.bitrateKbps = kbps;
				}
			}
		}
		return params;
	}

	int estimateBitrateKbps(int width, int height, int fps)
	{
		if (width <= 0 || height <= 0 || fps <= 0)
		{
			return 3500;
		}
		// 经验公式：按像素吞吐估算实时推流码率，优先避免高分辨率下模糊。
		const double bitsPerPixelPerFrame = 0.08;
		const double kbps = (static_cast<double>(width) * static_cast<double>(height) * static_cast<double>(fps) *
		                     bitsPerPixelPerFrame) / 1000.0;
		return qBound(1200, static_cast<int>(kbps), 50000);
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
	const QString screenPrefix = QStringLiteral("__screen_capture__:");
	const QString cameraPrefix = QStringLiteral("__camera_capture__:");
	const QString cameraPreviewPrefix = QStringLiteral("__camera_preview__:");
	const QString fileTranscodePrefix = QStringLiteral("__file_transcode__:");
	const bool isScreenCapture = (inputUrl == QStringLiteral("__screen_capture__")) || inputUrl.startsWith(screenPrefix);
	const bool isCameraCapture = inputUrl.startsWith(cameraPrefix);
	const bool isCameraPreview = inputUrl.startsWith(cameraPreviewPrefix);
	const bool isFileTranscode = inputUrl.startsWith(fileTranscodePrefix);
	const QString screenSpec = inputUrl.startsWith(screenPrefix) ? inputUrl.mid(screenPrefix.size()).trimmed() : QString();
	const QString cameraSpec = isCameraCapture ? inputUrl.mid(cameraPrefix.size()).trimmed() : QString();
	const QString cameraPreviewSpec = isCameraPreview ? inputUrl.mid(cameraPreviewPrefix.size()).trimmed() : QString();
	const QString fileTranscodeSpec = isFileTranscode ? inputUrl.mid(fileTranscodePrefix.size()).trimmed() : QString();
	const CaptureParams cameraParams = parseCaptureParams(cameraSpec);
	if (isCameraCapture && cameraParams.device.isEmpty())
	{
		m_running.store(false, std::memory_order_relaxed);
		setLastError(QStringLiteral("摄像头推流设备参数为空"));
		return false;
	}
	try
	{
		if (isScreenCapture)
		{
			m_worker = std::make_unique<std::thread>([this, outputUrl, screenSpec]() {
				pushScreenLoop(outputUrl, screenSpec);
			});
		}
		else if (isCameraCapture)
		{
			m_worker = std::make_unique<std::thread>([this, outputUrl, cameraSpec]() {
				pushCameraLoop(outputUrl, cameraSpec);
			});
		}
		else if (isCameraPreview)
		{
			m_worker = std::make_unique<std::thread>([this, outputUrl, cameraPreviewSpec]() {
				pushCameraPreviewLoop(outputUrl, cameraPreviewSpec);
			});
		}
		else if (isFileTranscode)
		{
			m_worker = std::make_unique<std::thread>([this, outputUrl, fileTranscodeSpec]() {
				transcodeFileLoop(outputUrl, fileTranscodeSpec);
			});
		}
		else
		{
			m_worker = std::make_unique<std::thread>([this, inputUrl, outputUrl]() {
				remuxLoop(inputUrl, outputUrl, "flv");
			});
		}
	}
	catch (...)
	{
		m_running.store(false, std::memory_order_relaxed);
		setLastError(QStringLiteral("无法启动推流线程"));
		return false;
	}
	if (isScreenCapture)
	{
		appendLogLine(QStringLiteral("[推流] 已启动（桌面采集直推）"));
	}
	else if (isCameraCapture)
	{
		appendLogLine(QStringLiteral("[推流] 已启动（摄像头采集直推）"));
	}
	else if (isCameraPreview)
	{
		appendLogLine(QStringLiteral("[推流] 已启动（当前摄像头预览帧推流）"));
	}
	else if (isFileTranscode)
	{
		appendLogLine(QStringLiteral("[推流] 已启动（文件转码推流）"));
	}
	else
	{
		appendLogLine(QStringLiteral("[推流] 已启动（libavformat 转封装 copy）"));
	}
	return true;
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
	try
	{
		m_worker = std::make_unique<std::thread>([this, inputUrl, outputUrl]() {
			remuxLoop(inputUrl, outputUrl, nullptr);
		});
	}
	catch (...)
	{
		m_running.store(false, std::memory_order_relaxed);
		setLastError(QStringLiteral("无法启动拉流线程"));
		return false;
	}
	appendLogLine(QStringLiteral("[拉流] 已启动（libavformat 转封装 copy）"));
	return true;
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
		encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
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
			exitCode = ret;
			setLastError(QStringLiteral("打开编码器失败"));
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
		encCtx->width = params.outWidth > 0 ? params.outWidth : decCtx->width;
		encCtx->height = params.outHeight > 0 ? params.outHeight : decCtx->height;
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
			av_opt_set(encCtx->priv_data, "preset", "ultrafast", 0);
			av_opt_set(encCtx->priv_data, "tune", "zerolatency", 0);
		}
		appendLogLine(QStringLiteral("[屏幕推流] 采集区域=%1,%2 %3x%4 输出=%5x%6 FPS=%7 码率=%8kbps")
		              .arg(params.x)
		              .arg(params.y)
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
	while (!m_stopRequest.load(std::memory_order_relaxed))
	{
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
	fplayer::CameraFrame frame;

	const QByteArray outUtf8 = outputUrl.toUtf8();
	const char* outPath = outUtf8.constData();
	const CaptureParams params = parseCaptureParams(captureSpec);
	const int targetFps = qMax(1, params.fps);
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

	while (!m_stopRequest.load(std::memory_order_relaxed))
	{
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
			av_interleaved_write_frame(ofmt, outPkt);
			av_packet_unref(outPkt);
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

	const QByteArray outUtf8 = outputUrl.toUtf8();
	const char* outPath = outUtf8.constData();
	const CaptureParams params = parseCaptureParams(captureSpec);
	const QByteArray deviceUtf8 = params.device.toUtf8();
	const char* devicePath = deviceUtf8.constData();
	const int targetFps = qMax(1, params.fps);
	avdevice_register_all();

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

	while (!m_stopRequest.load(std::memory_order_relaxed))
	{
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
