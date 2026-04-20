#include <fplayer/backend/stream_ffmpeg/streamffmpeg.h>

extern "C"
{
#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
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
#include "platform/windows/audioinputprobe.h"
#include "platform/windows/wasapiloopbackcapture.h"
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
	struct CaptureParams
	{
		QString device;
		QString source;
		QString audioSource = QStringLiteral("off");
		QString videoEncoder = QStringLiteral("auto");
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
				continue;
			}
			if (key == QStringLiteral("audio"))
			{
				params.audioSource = value;
				continue;
			}
			if (key == QStringLiteral("encoder"))
			{
				params.videoEncoder = value.toLower();
			}
		}
		return params;
	}

	struct VideoEncoderChoice
	{
		const AVCodec* codec = nullptr;
		QString name;
		bool isHardware = false;
	};

	void appendNamedEncoder(QList<VideoEncoderChoice>& out, const char* encoderName, const bool isHw, const QString& displayName)
	{
		const AVCodec* enc = avcodec_find_encoder_by_name(encoderName);
		if (!enc)
		{
			return;
		}
		VideoEncoderChoice choice;
		choice.codec = enc;
		choice.name = displayName;
		choice.isHardware = isHw;
		out.push_back(choice);
	}

	bool codecLooksLikeHardware(const AVCodec* enc)
	{
		if (!enc || !enc->name)
		{
			return false;
		}
		const QString n = QString::fromLatin1(enc->name).toLower();
		return n.contains(QStringLiteral("nvenc")) || n.contains(QStringLiteral("amf")) || n.contains(QStringLiteral("qsv")) ||
		       n.contains(QStringLiteral("vaapi")) || n.contains(QStringLiteral("videotoolbox")) || n.contains(QStringLiteral("mediacodec"));
	}

	/// 按用户选项给出有序备选；自动模式在 avcodec_open2 失败时应依次尝试下一项（NVENC→AMF→libx264）。
	QList<VideoEncoderChoice> pickVideoEncoderCandidates(const QString& prefer)
	{
		QList<VideoEncoderChoice> list;
		const QString pref = prefer.trimmed().toLower();
		if (pref == QStringLiteral("nvenc"))
		{
			appendNamedEncoder(list, "h264_nvenc", true, QStringLiteral("h264_nvenc"));
			return list;
		}
		if (pref == QStringLiteral("amf"))
		{
			appendNamedEncoder(list, "h264_amf", true, QStringLiteral("h264_amf"));
			return list;
		}
		if (pref == QStringLiteral("cpu"))
		{
			appendNamedEncoder(list, "libx264", false, QStringLiteral("libx264"));
			if (list.isEmpty())
			{
				appendNamedEncoder(list, "libopenh264", false, QStringLiteral("libopenh264"));
			}
			return list;
		}
		appendNamedEncoder(list, "h264_nvenc", true, QStringLiteral("h264_nvenc"));
		appendNamedEncoder(list, "h264_amf", true, QStringLiteral("h264_amf"));
		appendNamedEncoder(list, "libx264", false, QStringLiteral("libx264"));
		if (!list.isEmpty())
		{
			return list;
		}
		const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_H264);
		if (!enc)
		{
			enc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
		}
		if (enc)
		{
			VideoEncoderChoice c;
			c.codec = enc;
			c.name = QString::fromLatin1(enc->name ? enc->name : "unknown");
			c.isHardware = codecLooksLikeHardware(enc);
			list.push_back(c);
		}
		return list;
	}

	bool canCreateD3D11HwDevice()
	{
		AVBufferRef* hwDev = nullptr;
		const int ret = av_hwdevice_ctx_create(&hwDev, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
		if (ret >= 0 && hwDev)
		{
			av_buffer_unref(&hwDev);
			return true;
		}
		av_buffer_unref(&hwDev);
		return false;
	}

	QString encoderHwFramesHint(const AVCodec* enc)
	{
		if (!enc)
		{
			return QStringLiteral("unknown");
		}
		bool hasD3D11VA = false;
		bool hasCuda = false;
		for (int i = 0;; ++i)
		{
			const AVCodecHWConfig* cfg = avcodec_get_hw_config(enc, i);
			if (!cfg)
			{
				break;
			}
			if (cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA)
			{
				hasD3D11VA = true;
			}
			if (cfg->device_type == AV_HWDEVICE_TYPE_CUDA)
			{
				hasCuda = true;
			}
		}
		return QStringLiteral("d3d11va=%1 cuda=%2")
				.arg(hasD3D11VA ? QStringLiteral("yes") : QStringLiteral("no"))
				.arg(hasCuda ? QStringLiteral("yes") : QStringLiteral("no"));
	}

	AVPixelFormat pickEncoderPixelFormat(const AVCodec* enc, const bool preferHardware)
	{
		if (!enc || !enc->pix_fmts)
		{
			return AV_PIX_FMT_YUV420P;
		}
		if (preferHardware)
		{
			for (const AVPixelFormat* p = enc->pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
			{
				if (*p == AV_PIX_FMT_NV12)
				{
					return *p;
				}
			}
		}
		for (const AVPixelFormat* p = enc->pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
		{
			if (*p == AV_PIX_FMT_YUV420P)
			{
				return *p;
			}
		}
		return enc->pix_fmts[0];
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
	const QString screenPrefix = QStringLiteral("__screen_capture__:");
	const QString screenPreviewPrefix = QStringLiteral("__screen_preview__:");
	const QString cameraPrefix = QStringLiteral("__camera_capture__:");
	const QString cameraPreviewPrefix = QStringLiteral("__camera_preview__:");
	const QString fileTranscodePrefix = QStringLiteral("__file_transcode__:");
	const bool isScreenCapture = (inputUrl == QStringLiteral("__screen_capture__")) || inputUrl.startsWith(screenPrefix);
	const bool isScreenPreview = inputUrl.startsWith(screenPreviewPrefix);
	const bool isCameraCapture = inputUrl.startsWith(cameraPrefix);
	const bool isCameraPreview = inputUrl.startsWith(cameraPreviewPrefix);
	const bool isFileTranscode = inputUrl.startsWith(fileTranscodePrefix);
	const QString screenSpec = inputUrl.startsWith(screenPrefix) ? inputUrl.mid(screenPrefix.size()).trimmed() : QString();
	const QString screenPreviewSpec = isScreenPreview ? inputUrl.mid(screenPreviewPrefix.size()).trimmed() : QString();
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
		else if (isScreenPreview)
		{
			m_worker = std::make_unique<std::thread>([this, outputUrl, screenPreviewSpec]() {
				pushScreenPreviewLoop(outputUrl, screenPreviewSpec);
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
	else if (isScreenPreview)
	{
		appendLogLine(QStringLiteral("[推流] 已启动（DXGI 预览帧推流）"));
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
	AVCodecContext* audioEncCtx = nullptr;
	SwrContext* audioSwr = nullptr;
	AVPacket* audioPkt = nullptr;
	AVPacket* audioOutPkt = nullptr;
	AVFrame* audioDecFrame = nullptr;
	AVFrame* audioEncFrame = nullptr;
	int audioIndex = -1;
	AVStream* outAudioStream = nullptr;
	int64_t audioPts = 0;
	fplayer::windows_api::WasapiLoopbackCapture wasapiAudio;
	bool useWasapiAudio = false;

	const bool enableAudio = !params.audioSource.isEmpty() && params.audioSource != QStringLiteral("off");
	bool audioActive = enableAudio;
	if (audioActive)
	{
		QString openedAudioDevice;
		QString openDetail;
		if (!fplayer::windows_api::openDshowAudioInputWithFallback(params.audioSource, m_stopRequest, audioIfmt, openedAudioDevice, openDetail))
		{
			appendLogLine(QStringLiteral("[屏幕推流] 音频采集初始化失败，已自动降级为无声推流: %1")
				              .arg(openDetail));
			audioActive = false;
		}
		if (audioActive)
		{
			audioIfmt->interrupt_callback.callback = &StreamFFmpeg::interruptCallback;
			audioIfmt->interrupt_callback.opaque = &m_stopRequest;
			appendLogLine(QStringLiteral("[屏幕推流] 音频设备=%1").arg(openedAudioDevice));
		}
		if (!audioActive || !audioIfmt)
		{
			goto audio_init_done;
		}
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
		audioOutPkt = av_packet_alloc();
		audioDecFrame = av_frame_alloc();
		audioEncFrame = av_frame_alloc();
		if (!audioPkt || !audioOutPkt || !audioDecFrame || !audioEncFrame)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("分配音频编码资源失败"));
			goto cleanup;
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
			appendLogLine(QStringLiteral("[屏幕推流] 音频来源=%1").arg(params.audioSource));
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
		if (audioActive && audioIfmt && audioPkt && outAudioStream)
		{
			const int audioRet = av_read_frame(audioIfmt, audioPkt);
			if (audioRet >= 0)
			{
				if (audioPkt->stream_index == audioIndex)
				{
					ret = avcodec_send_packet(audioDecCtx, audioPkt);
					if (ret >= 0)
					{
						while (avcodec_receive_frame(audioDecCtx, audioDecFrame) == 0)
						{
							const int dstSamples = av_rescale_rnd(
								swr_get_delay(audioSwr, audioDecCtx->sample_rate) + audioDecFrame->nb_samples,
								audioEncCtx->sample_rate,
								audioDecCtx->sample_rate,
								AV_ROUND_UP);
							av_frame_unref(audioEncFrame);
							audioEncFrame->nb_samples = qMax(1, dstSamples);
							audioEncFrame->format = audioEncCtx->sample_fmt;
							audioEncFrame->sample_rate = audioEncCtx->sample_rate;
							av_channel_layout_copy(&audioEncFrame->ch_layout, &audioEncCtx->ch_layout);
							if (av_frame_get_buffer(audioEncFrame, 0) >= 0)
							{
								swr_convert(audioSwr, audioEncFrame->data, audioEncFrame->nb_samples,
								            (const uint8_t* const*)audioDecFrame->data, audioDecFrame->nb_samples);
								audioEncFrame->pts = audioPts;
								audioPts += audioEncFrame->nb_samples;
								if (avcodec_send_frame(audioEncCtx, audioEncFrame) >= 0)
								{
									while (avcodec_receive_packet(audioEncCtx, audioOutPkt) == 0)
									{
										av_packet_rescale_ts(audioOutPkt, audioEncCtx->time_base, outAudioStream->time_base);
										audioOutPkt->stream_index = outAudioStream->index;
										av_interleaved_write_frame(ofmt, audioOutPkt);
										av_packet_unref(audioOutPkt);
									}
								}
							}
							av_frame_unref(audioDecFrame);
						}
					}
				}
				av_packet_unref(audioPkt);
			}
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
	if (audioActive && audioEncCtx && outAudioStream && audioOutPkt)
	{
		avcodec_send_frame(audioEncCtx, nullptr);
		while (avcodec_receive_packet(audioEncCtx, audioOutPkt) == 0)
		{
			av_packet_rescale_ts(audioOutPkt, audioEncCtx->time_base, outAudioStream->time_base);
			audioOutPkt->stream_index = outAudioStream->index;
			av_interleaved_write_frame(ofmt, audioOutPkt);
			av_packet_unref(audioOutPkt);
		}
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
	fplayer::ScreenFrame frame;

	const QByteArray outUtf8 = outputUrl.toUtf8();
	const char* outPath = outUtf8.constData();
	const CaptureParams params = parseCaptureParams(captureSpec);
	const int targetFps = qMax(1, params.fps);
	const int frameIntervalMs = qMax(1, 1000 / targetFps);

	avdevice_register_all();
	AVFormatContext* audioIfmt = nullptr;
	AVCodecContext* audioDecCtx = nullptr;
	AVCodecContext* audioEncCtx = nullptr;
	SwrContext* audioSwr = nullptr;
	AVPacket* audioPkt = nullptr;
	AVPacket* audioOutPkt = nullptr;
	AVFrame* audioDecFrame = nullptr;
	AVFrame* audioEncFrame = nullptr;
	int audioIndex = -1;
	AVStream* outAudioStream = nullptr;
	int64_t audioPts = 0;
	fplayer::windows_api::WasapiLoopbackCapture wasapiAudio;
	bool useWasapiAudio = false;

	const bool enableAudio = !params.audioSource.isEmpty() && params.audioSource != QStringLiteral("off");
	bool audioActive = enableAudio;
	if (audioActive)
	{
		QString openedAudioDevice;
		QString openDetail;
		if (!fplayer::windows_api::openDshowAudioInputWithFallback(params.audioSource, m_stopRequest, audioIfmt, openedAudioDevice, openDetail))
		{
			QString wasapiErr;
			if (params.audioSource == QStringLiteral("system") && wasapiAudio.init(wasapiErr))
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
		audioOutPkt = av_packet_alloc();
		audioDecFrame = av_frame_alloc();
		audioEncFrame = av_frame_alloc();
		if (!audioPkt || !audioOutPkt || !audioDecFrame || !audioEncFrame)
		{
			exitCode = AVERROR(ENOMEM);
			setLastError(QStringLiteral("分配音频编码资源失败"));
			goto cleanup;
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
			appendLogLine(QStringLiteral("[屏幕推流] 音频来源=%1").arg(params.audioSource));
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
	auto avStatWindowStart = startClock;
	int avStatVideoPkts = 0;
	int avStatAudioPkts = 0;

	while (!m_stopRequest.load(std::memory_order_relaxed))
	{
		if (audioActive && useWasapiAudio && outAudioStream && audioSwr && audioEncCtx && audioOutPkt && audioEncFrame)
		{
			std::vector<uint8_t> captured;
			int capturedSamples = 0;
			if (wasapiAudio.readInterleaved(captured, capturedSamples) && capturedSamples > 0)
			{
				const uint8_t* inData[1] = {captured.data()};
				const int dstSamples = av_rescale_rnd(
								swr_get_delay(audioSwr, wasapiAudio.sampleRate()) + capturedSamples,
					audioEncCtx->sample_rate,
								wasapiAudio.sampleRate(),
					AV_ROUND_UP);
				av_frame_unref(audioEncFrame);
				audioEncFrame->nb_samples = qMax(1, dstSamples);
				audioEncFrame->format = audioEncCtx->sample_fmt;
				audioEncFrame->sample_rate = audioEncCtx->sample_rate;
				av_channel_layout_copy(&audioEncFrame->ch_layout, &audioEncCtx->ch_layout);
				if (av_frame_get_buffer(audioEncFrame, 0) >= 0)
				{
					swr_convert(audioSwr, audioEncFrame->data, audioEncFrame->nb_samples, inData, capturedSamples);
					audioEncFrame->pts = audioPts;
					audioPts += audioEncFrame->nb_samples;
					if (avcodec_send_frame(audioEncCtx, audioEncFrame) >= 0)
					{
						while (avcodec_receive_packet(audioEncCtx, audioOutPkt) == 0)
						{
							av_packet_rescale_ts(audioOutPkt, audioEncCtx->time_base, outAudioStream->time_base);
							audioOutPkt->stream_index = outAudioStream->index;
							const int aw = av_interleaved_write_frame(ofmt, audioOutPkt);
							if (aw >= 0)
							{
								++avStatAudioPkts;
							}
							av_packet_unref(audioOutPkt);
						}
					}
				}
			}
		}
		else if (audioActive && audioIfmt && audioPkt && outAudioStream && audioDecCtx && audioSwr && audioEncCtx && audioOutPkt)
		{
			const int maxAudioPacketsPerTick = 4;
			for (int audioDrain = 0; audioDrain < maxAudioPacketsPerTick; ++audioDrain)
			{
				const int audioRet = av_read_frame(audioIfmt, audioPkt);
				if (audioRet < 0)
				{
					if (audioRet != AVERROR(EAGAIN) && audioRet != AVERROR_EOF)
					{
						char errbuf[AV_ERROR_MAX_STRING_SIZE];
						av_strerror(audioRet, errbuf, sizeof(errbuf));
						appendLogLine(QStringLiteral("[屏幕推流] 音频读取中断: %1").arg(QString::fromUtf8(errbuf)));
					}
					break;
				}
				if (audioPkt->stream_index == audioIndex)
				{
					ret = avcodec_send_packet(audioDecCtx, audioPkt);
					if (ret >= 0)
					{
						while (avcodec_receive_frame(audioDecCtx, audioDecFrame) == 0)
						{
							const int dstSamples = av_rescale_rnd(
								swr_get_delay(audioSwr, audioDecCtx->sample_rate) + audioDecFrame->nb_samples,
								audioEncCtx->sample_rate,
								audioDecCtx->sample_rate,
								AV_ROUND_UP);
							av_frame_unref(audioEncFrame);
							audioEncFrame->nb_samples = qMax(1, dstSamples);
							audioEncFrame->format = audioEncCtx->sample_fmt;
							audioEncFrame->sample_rate = audioEncCtx->sample_rate;
							av_channel_layout_copy(&audioEncFrame->ch_layout, &audioEncCtx->ch_layout);
							if (av_frame_get_buffer(audioEncFrame, 0) >= 0)
							{
								swr_convert(audioSwr, audioEncFrame->data, audioEncFrame->nb_samples,
								            (const uint8_t* const*)audioDecFrame->data, audioDecFrame->nb_samples);
								audioEncFrame->pts = audioPts;
								audioPts += audioEncFrame->nb_samples;
								if (avcodec_send_frame(audioEncCtx, audioEncFrame) >= 0)
								{
									while (avcodec_receive_packet(audioEncCtx, audioOutPkt) == 0)
									{
										av_packet_rescale_ts(audioOutPkt, audioEncCtx->time_base, outAudioStream->time_base);
										audioOutPkt->stream_index = outAudioStream->index;
										const int aw = av_interleaved_write_frame(ofmt, audioOutPkt);
										if (aw >= 0)
										{
											++avStatAudioPkts;
										}
										av_packet_unref(audioOutPkt);
									}
								}
							}
							av_frame_unref(audioDecFrame);
						}
					}
				}
				av_packet_unref(audioPkt);
			}
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
			ret = av_interleaved_write_frame(ofmt, outPkt);
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
			appendLogLine(QStringLiteral("[屏幕推流] 发包统计 1s: video=%1 audio=%2").arg(avStatVideoPkts).arg(avStatAudioPkts));
			avStatVideoPkts = 0;
			avStatAudioPkts = 0;
			avStatWindowStart = statNow;
		}
	}

	if (!writeFailed)
	{
		avcodec_send_frame(encCtx, nullptr);
		while (avcodec_receive_packet(encCtx, outPkt) == 0)
		{
			av_packet_rescale_ts(outPkt, encCtx->time_base, ofmt->streams[0]->time_base);
			outPkt->stream_index = 0;
			ret = av_interleaved_write_frame(ofmt, outPkt);
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
	if (!writeFailed && audioActive && audioEncCtx && outAudioStream && audioOutPkt)
	{
		avcodec_send_frame(audioEncCtx, nullptr);
		while (avcodec_receive_packet(audioEncCtx, audioOutPkt) == 0)
		{
			av_packet_rescale_ts(audioOutPkt, audioEncCtx->time_base, outAudioStream->time_base);
			audioOutPkt->stream_index = outAudioStream->index;
			av_interleaved_write_frame(ofmt, audioOutPkt);
			av_packet_unref(audioOutPkt);
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
	fplayer::CameraFrame frame;
	AVFormatContext* audioIfmt = nullptr;
	AVCodecContext* audioDecCtx = nullptr;
	AVCodecContext* audioEncCtx = nullptr;
	SwrContext* audioSwr = nullptr;
	AVPacket* audioPkt = nullptr;
	AVPacket* audioOutPkt = nullptr;
	AVFrame* audioDecFrame = nullptr;
	AVFrame* audioEncFrame = nullptr;
	int audioIndex = -1;
	AVStream* outAudioStream = nullptr;
	int64_t audioPts = 0;
	fplayer::windows_api::WasapiLoopbackCapture wasapiAudio;
	bool useWasapiAudio = false;

	const QByteArray outUtf8 = outputUrl.toUtf8();
	const char* outPath = outUtf8.constData();
	const CaptureParams params = parseCaptureParams(captureSpec);
	const int targetFps = qMax(1, params.fps);
	const bool enableAudio = !params.audioSource.isEmpty() && params.audioSource != QStringLiteral("off");
	bool audioActive = enableAudio;
	if (audioActive)
	{
		QString openedAudioDevice;
		QString openDetail;
		if (!fplayer::windows_api::openDshowAudioInputWithFallback(params.audioSource, m_stopRequest, audioIfmt, openedAudioDevice, openDetail))
		{
			QString wasapiErr;
			if (params.audioSource == QStringLiteral("system") && wasapiAudio.init(wasapiErr))
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
			appendLogLine(QStringLiteral("[摄像头预览推流] 音频设备=%1").arg(openedAudioDevice));
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
	auto avStatWindowStart = std::chrono::steady_clock::now();
	int avStatVideoPkts = 0;
	int avStatAudioPkts = 0;

	while (!m_stopRequest.load(std::memory_order_relaxed))
	{
		if (audioActive && useWasapiAudio && outAudioStream && audioSwr && audioEncCtx && audioOutPkt && audioEncFrame)
		{
			std::vector<uint8_t> captured;
			int capturedSamples = 0;
			if (wasapiAudio.readInterleaved(captured, capturedSamples) && capturedSamples > 0)
			{
				const uint8_t* inData[1] = {captured.data()};
				const int dstSamples = av_rescale_rnd(
					swr_get_delay(audioSwr, wasapiAudio.sampleRate()) + capturedSamples,
					audioEncCtx->sample_rate,
					wasapiAudio.sampleRate(),
					AV_ROUND_UP);
				av_frame_unref(audioEncFrame);
				audioEncFrame->nb_samples = qMax(1, dstSamples);
				audioEncFrame->format = audioEncCtx->sample_fmt;
				audioEncFrame->sample_rate = audioEncCtx->sample_rate;
				av_channel_layout_copy(&audioEncFrame->ch_layout, &audioEncCtx->ch_layout);
				if (av_frame_get_buffer(audioEncFrame, 0) >= 0)
				{
					swr_convert(audioSwr, audioEncFrame->data, audioEncFrame->nb_samples, inData, capturedSamples);
					audioEncFrame->pts = audioPts;
					audioPts += audioEncFrame->nb_samples;
					if (avcodec_send_frame(audioEncCtx, audioEncFrame) >= 0)
					{
						while (avcodec_receive_packet(audioEncCtx, audioOutPkt) == 0)
						{
							av_packet_rescale_ts(audioOutPkt, audioEncCtx->time_base, outAudioStream->time_base);
							audioOutPkt->stream_index = outAudioStream->index;
							const int aw = av_interleaved_write_frame(ofmt, audioOutPkt);
							if (aw >= 0)
							{
								++avStatAudioPkts;
							}
							av_packet_unref(audioOutPkt);
						}
					}
				}
			}
		}
		else if (audioActive && audioIfmt && audioPkt && outAudioStream && audioDecCtx && audioSwr && audioEncCtx && audioOutPkt)
		{
			const int maxAudioPacketsPerTick = 4;
			for (int audioDrain = 0; audioDrain < maxAudioPacketsPerTick; ++audioDrain)
			{
				const int audioRet = av_read_frame(audioIfmt, audioPkt);
				if (audioRet < 0)
				{
					if (audioRet != AVERROR(EAGAIN) && audioRet != AVERROR_EOF)
					{
						char errbuf[AV_ERROR_MAX_STRING_SIZE];
						av_strerror(audioRet, errbuf, sizeof(errbuf));
						appendLogLine(QStringLiteral("[摄像头推流] 音频读取中断: %1").arg(QString::fromUtf8(errbuf)));
					}
					break;
				}
				if (audioPkt->stream_index == audioIndex)
				{
					ret = avcodec_send_packet(audioDecCtx, audioPkt);
					if (ret >= 0)
					{
						while (avcodec_receive_frame(audioDecCtx, audioDecFrame) == 0)
						{
							const int dstSamples = av_rescale_rnd(
								swr_get_delay(audioSwr, audioDecCtx->sample_rate) + audioDecFrame->nb_samples,
								audioEncCtx->sample_rate,
								audioDecCtx->sample_rate,
								AV_ROUND_UP);
							av_frame_unref(audioEncFrame);
							audioEncFrame->nb_samples = qMax(1, dstSamples);
							audioEncFrame->format = audioEncCtx->sample_fmt;
							audioEncFrame->sample_rate = audioEncCtx->sample_rate;
							av_channel_layout_copy(&audioEncFrame->ch_layout, &audioEncCtx->ch_layout);
							if (av_frame_get_buffer(audioEncFrame, 0) >= 0)
							{
								swr_convert(audioSwr, audioEncFrame->data, audioEncFrame->nb_samples,
								            (const uint8_t* const*)audioDecFrame->data, audioDecFrame->nb_samples);
								audioEncFrame->pts = audioPts;
								audioPts += audioEncFrame->nb_samples;
								if (avcodec_send_frame(audioEncCtx, audioEncFrame) >= 0)
								{
									while (avcodec_receive_packet(audioEncCtx, audioOutPkt) == 0)
									{
										av_packet_rescale_ts(audioOutPkt, audioEncCtx->time_base, outAudioStream->time_base);
										audioOutPkt->stream_index = outAudioStream->index;
										const int aw = av_interleaved_write_frame(ofmt, audioOutPkt);
										if (aw >= 0)
										{
											++avStatAudioPkts;
										}
										av_packet_unref(audioOutPkt);
									}
								}
							}
							av_frame_unref(audioDecFrame);
						}
					}
				}
				av_packet_unref(audioPkt);
			}
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
			const int vw = av_interleaved_write_frame(ofmt, outPkt);
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

	avcodec_send_frame(encCtx, nullptr);
	while (avcodec_receive_packet(encCtx, outPkt) == 0)
	{
		av_packet_rescale_ts(outPkt, encCtx->time_base, ofmt->streams[0]->time_base);
		outPkt->stream_index = 0;
		av_interleaved_write_frame(ofmt, outPkt);
		av_packet_unref(outPkt);
	}
	if (audioActive && audioEncCtx && outAudioStream && audioOutPkt)
	{
		avcodec_send_frame(audioEncCtx, nullptr);
		while (avcodec_receive_packet(audioEncCtx, audioOutPkt) == 0)
		{
			av_packet_rescale_ts(audioOutPkt, audioEncCtx->time_base, outAudioStream->time_base);
			audioOutPkt->stream_index = outAudioStream->index;
			av_interleaved_write_frame(ofmt, audioOutPkt);
			av_packet_unref(audioOutPkt);
		}
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
	AVCodecContext* audioEncCtx = nullptr;
	SwrContext* audioSwr = nullptr;
	AVPacket* audioPkt = nullptr;
	AVPacket* audioOutPkt = nullptr;
	AVFrame* audioDecFrame = nullptr;
	AVFrame* audioEncFrame = nullptr;
	int audioIndex = -1;
	AVStream* outAudioStream = nullptr;
	int64_t audioPts = 0;
	fplayer::windows_api::WasapiLoopbackCapture wasapiAudio;
	bool useWasapiAudio = false;

	const QByteArray outUtf8 = outputUrl.toUtf8();
	const char* outPath = outUtf8.constData();
	const CaptureParams params = parseCaptureParams(captureSpec);
	const QByteArray deviceUtf8 = params.device.toUtf8();
	const char* devicePath = deviceUtf8.constData();
	const int targetFps = qMax(1, params.fps);
	const bool enableAudio = !params.audioSource.isEmpty() && params.audioSource != QStringLiteral("off");
	avdevice_register_all();

	bool audioActive = enableAudio;
	if (audioActive)
	{
		QString openedAudioDevice;
		QString openDetail;
		if (!fplayer::windows_api::openDshowAudioInputWithFallback(params.audioSource, m_stopRequest, audioIfmt, openedAudioDevice, openDetail))
		{
			QString wasapiErr;
			if (params.audioSource == QStringLiteral("system") && wasapiAudio.init(wasapiErr))
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
			appendLogLine(QStringLiteral("[摄像头采集推流] 音频设备=%1").arg(openedAudioDevice));
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
