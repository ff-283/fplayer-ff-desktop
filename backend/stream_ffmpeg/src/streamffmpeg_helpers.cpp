#include "streamffmpeg_helpers.h"

extern "C"
{
#include <libavutil/hwcontext.h>
}

#include <QByteArray>

namespace fplayer::streamffmpeg_helpers
{
	namespace
	{
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
			       n.contains(QStringLiteral("vaapi")) || n.contains(QStringLiteral("videotoolbox")) ||
			       n.contains(QStringLiteral("mediacodec"));
		}
	}

	void appendLimited(QString& buf, const QString& line)
	{
		buf += line;
		if (buf.size() > 16000)
		{
			buf = buf.right(16000);
		}
	}

	PushInputRoute parsePushInputRoute(const QString& inputUrl)
	{
		const QString screenPrefix = QStringLiteral("__screen_capture__:");
		const QString screenPreviewPrefix = QStringLiteral("__screen_preview__:");
		const QString cameraPrefix = QStringLiteral("__camera_capture__:");
		const QString cameraPreviewPrefix = QStringLiteral("__camera_preview__:");
		const QString fileTranscodePrefix = QStringLiteral("__file_transcode__:");

		PushInputRoute route;
		if ((inputUrl == QStringLiteral("__screen_capture__")) || inputUrl.startsWith(screenPrefix))
		{
			route.kind = PushInputKind::ScreenCapture;
			route.spec = inputUrl.startsWith(screenPrefix) ? inputUrl.mid(screenPrefix.size()).trimmed() : QString();
			return route;
		}
		if (inputUrl.startsWith(screenPreviewPrefix))
		{
			route.kind = PushInputKind::ScreenPreview;
			route.spec = inputUrl.mid(screenPreviewPrefix.size()).trimmed();
			return route;
		}
		if (inputUrl.startsWith(cameraPrefix))
		{
			route.kind = PushInputKind::CameraCapture;
			route.spec = inputUrl.mid(cameraPrefix.size()).trimmed();
			return route;
		}
		if (inputUrl.startsWith(cameraPreviewPrefix))
		{
			route.kind = PushInputKind::CameraPreview;
			route.spec = inputUrl.mid(cameraPreviewPrefix.size()).trimmed();
			return route;
		}
		if (inputUrl.startsWith(fileTranscodePrefix))
		{
			route.kind = PushInputKind::FileTranscode;
			route.spec = inputUrl.mid(fileTranscodePrefix.size()).trimmed();
			return route;
		}
		route.kind = PushInputKind::Remux;
		route.spec.clear();
		return route;
	}

	QString pushStartedLog(const PushInputKind kind)
	{
		switch (kind)
		{
		case PushInputKind::ScreenCapture:
			return QStringLiteral("[推流] 已启动（桌面采集直推）");
		case PushInputKind::ScreenPreview:
			return QStringLiteral("[推流] 已启动（DXGI 预览帧推流）");
		case PushInputKind::CameraCapture:
			return QStringLiteral("[推流] 已启动（摄像头采集直推）");
		case PushInputKind::CameraPreview:
			return QStringLiteral("[推流] 已启动（当前摄像头预览帧推流）");
		case PushInputKind::FileTranscode:
			return QStringLiteral("[推流] 已启动（文件转码推流）");
		case PushInputKind::Remux:
		default:
			return QStringLiteral("[推流] 已启动（libavformat 转封装 copy）");
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
			if (key == QStringLiteral("audio_in"))
			{
				params.audioInputSource = value;
				continue;
			}
			if (key == QStringLiteral("audio_out"))
			{
				params.audioOutputSource = value;
				continue;
			}
			if (key == QStringLiteral("encoder"))
			{
				params.videoEncoder = value.toLower();
			}
		}
		return params;
	}

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

	int estimateBitrateKbps(const int width, const int height, const int fps)
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
