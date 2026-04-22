#include <fplayer/backend/media_ffmpeg/screencaptureffmpeg.h>

#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <cstdlib>
#include <logger/logger.h>

#include <fplayer/common/fglwidget/fglwidget.h>

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

fplayer::ScreenCaptureFFmpeg::ScreenCaptureFFmpeg()
{
	m_backendType = MediaBackendType::FFmpeg;
	avdevice_register_all();
}

fplayer::ScreenCaptureFFmpeg::~ScreenCaptureFFmpeg()
{
	stopCapture();
	cleanup();
}

void fplayer::ScreenCaptureFFmpeg::refreshScreens()
{
	auto alignEvenFloor = [](const int v) -> int {
		return (v >= 0) ? (v & ~1) : -(((-v) + 1) & ~1);
	};
	auto alignEvenSize = [](const int v) -> int {
		return qMax(2, v) & ~1;
	};
	m_descriptions.clear();
	const auto screens = QGuiApplication::screens();
	for (auto* screen : screens)
	{
		if (!screen)
		{
			continue;
		}
		ScreenDescription d;
		d.name = screen->name().isEmpty() ? QStringLiteral("屏幕") : screen->name();
		d.isPrimary = screen == QGuiApplication::primaryScreen();
		const QSize logical = screen->geometry().size();
		const qreal dpr = screen->devicePixelRatio();
		d.width = alignEvenSize(qRound(logical.width() * dpr));
		d.height = alignEvenSize(qRound(logical.height() * dpr));
		d.x = alignEvenFloor(qRound(screen->geometry().x() * dpr));
		d.y = alignEvenFloor(qRound(screen->geometry().y() * dpr));
		m_descriptions.push_back(d);
	}
}

QList<fplayer::ScreenDescription> fplayer::ScreenCaptureFFmpeg::getDescriptions()
{
	if (m_descriptions.isEmpty())
	{
		refreshScreens();
	}
	return m_descriptions;
}

bool fplayer::ScreenCaptureFFmpeg::openInputForSelectedScreen()
{
	if (m_screenIndex < 0 || m_screenIndex >= m_descriptions.size())
	{
		return false;
	}
	const auto screen = m_descriptions[m_screenIndex];
	AVDictionary* options = nullptr;
	av_dict_set(&options, "framerate", QString::number(m_fps).toUtf8().constData(), 0);
	av_dict_set(&options, "draw_mouse", m_captureCursor ? "1" : "0", 0);
	// 单显示器场景下让 gdigrab 走默认“整桌面”参数，避免 DPI/坐标换算边界导致区域越界。
	// 多显示器下才显式传 offset/video_size 做分屏抓取。
	if (m_descriptions.size() > 1)
	{
		av_dict_set(&options, "offset_x", QString::number(screen.x).toUtf8().constData(), 0);
		av_dict_set(&options, "offset_y", QString::number(screen.y).toUtf8().constData(), 0);
		av_dict_set(&options, "video_size", QString("%1x%2").arg(screen.width).arg(screen.height).toUtf8().constData(), 0);
	}
	const AVInputFormat* inputFmt = nullptr;
	QByteArray inputUrl;
#if defined(_WIN32)
	inputFmt = av_find_input_format("gdigrab");
	inputUrl = "desktop";
#elif defined(__linux__)
	inputFmt = av_find_input_format("x11grab");
	const char* displayEnv = std::getenv("DISPLAY");
	const char* waylandEnv = std::getenv("WAYLAND_DISPLAY");
	if ((!displayEnv || !*displayEnv) && waylandEnv && *waylandEnv)
	{
		av_dict_free(&options);
		LOG_WARN("[screen][ffmpeg]", "wayland detected but DISPLAY is empty; x11grab unavailable on this session");
		return false;
	}
	const QString display = (displayEnv && *displayEnv) ? QString::fromUtf8(displayEnv) : QStringLiteral(":0.0");
	inputUrl = QString("%1+%2,%3").arg(display).arg(screen.x).arg(screen.y).toUtf8();
	av_dict_set(&options, "video_size", QString("%1x%2").arg(screen.width).arg(screen.height).toUtf8().constData(), 0);
#endif
	if (!inputFmt)
	{
		av_dict_free(&options);
		return false;
	}
	const int ret = avformat_open_input(&m_formatContext, inputUrl.constData(), inputFmt, &options);
	av_dict_free(&options);
	if (ret < 0)
	{
		char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
		av_strerror(ret, errbuf, sizeof(errbuf));
		LOG_WARN("[screen][ffmpeg]", "open screen input failed, index=", m_screenIndex, " screens=", m_descriptions.size(),
		         " err=", errbuf);
		return false;
	}
	if (avformat_find_stream_info(m_formatContext, nullptr) < 0)
	{
		LOG_WARN("[screen][ffmpeg]", "find stream info failed");
		return false;
	}
	int videoStreamIndex = -1;
	for (unsigned int i = 0; i < m_formatContext->nb_streams; ++i)
	{
		if (m_formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			videoStreamIndex = static_cast<int>(i);
			break;
		}
	}
	if (videoStreamIndex < 0)
	{
		return false;
	}
	m_stream = m_formatContext->streams[videoStreamIndex];
	const AVCodec* codec = avcodec_find_decoder(m_stream->codecpar->codec_id);
	if (!codec)
	{
		return false;
	}
	m_codecContext = avcodec_alloc_context3(codec);
	if (!m_codecContext)
	{
		return false;
	}
	if (avcodec_parameters_to_context(m_codecContext, m_stream->codecpar) < 0)
	{
		return false;
	}
	if (avcodec_open2(m_codecContext, codec, nullptr) < 0)
	{
		return false;
	}
	return true;
}

bool fplayer::ScreenCaptureFFmpeg::selectScreen(int index)
{
	if (m_descriptions.isEmpty())
	{
		refreshScreens();
	}
	if (index < 0 || index >= m_descriptions.size())
	{
		return false;
	}
	const bool wasActive = m_active.load();
	stopCapture();
	cleanup();
	m_screenIndex = index;
	if (!openInputForSelectedScreen())
	{
		return false;
	}
	if (wasActive)
	{
		setActive(true);
	}
	return true;
}

int fplayer::ScreenCaptureFFmpeg::getIndex() const
{
	return m_screenIndex;
}

void fplayer::ScreenCaptureFFmpeg::setPreviewTarget(const PreviewTarget& target)
{
	if (!target.backend_hint)
	{
		return;
	}
	m_glWidget = static_cast<FGLWidget*>(target.backend_hint);
}

void fplayer::ScreenCaptureFFmpeg::setActive(bool active)
{
	if (active == m_active.load())
	{
		return;
	}
	m_active.store(active);
	if (!active)
	{
		stopCapture();
		return;
	}
	if (!m_codecContext && !selectScreen(m_screenIndex))
	{
		m_active.store(false);
		return;
	}
	m_capturing.store(true);
	m_captureThread = QThread::create([this]() {
		captureLoop();
	});
	m_captureThread->start();
}

bool fplayer::ScreenCaptureFFmpeg::isActive() const
{
	return m_active.load();
}

bool fplayer::ScreenCaptureFFmpeg::setCursorCaptureEnabled(bool enabled)
{
	const bool changed = (m_captureCursor != enabled);
	m_captureCursor = enabled;
	if (changed && m_active.load())
	{
		selectScreen(m_screenIndex);
	}
	return true;
}

bool fplayer::ScreenCaptureFFmpeg::canControlCursorCapture() const
{
	return true;
}

bool fplayer::ScreenCaptureFFmpeg::setFrameRate(int fps)
{
	if (fps <= 0)
	{
		return false;
	}
	const bool changed = (m_fps != fps);
	m_fps = fps;
	if (changed && m_active.load())
	{
		selectScreen(m_screenIndex);
	}
	return true;
}

int fplayer::ScreenCaptureFFmpeg::frameRate() const
{
	return m_fps;
}

bool fplayer::ScreenCaptureFFmpeg::canControlFrameRate() const
{
	return true;
}

void fplayer::ScreenCaptureFFmpeg::dispatchFrameToView(const QByteArray& yData, const QByteArray& uData, const QByteArray& vData,
                                                       int width, int height, int yStride, int uStride, int vStride)
{
	if (!m_glWidget)
	{
		return;
	}
	const QByteArray yCopy(yData.constData(), yData.size());
	const QByteArray uCopy(uData.constData(), uData.size());
	const QByteArray vCopy(vData.constData(), vData.size());
	QPointer<FGLWidget> target = m_glWidget;
	QMetaObject::invokeMethod(m_glWidget, [target, yCopy, uCopy, vCopy, width, height, yStride, uStride, vStride]() {
		if (!target)
		{
			return;
		}
		target->updateYUVFrame(yCopy, uCopy, vCopy, width, height, yStride, uStride, vStride);
	}, Qt::QueuedConnection);
}

void fplayer::ScreenCaptureFFmpeg::captureLoop()
{
	AVPacket* packet = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	SwsContext* swsContext = nullptr;
	AVFrame* yuvFrame = av_frame_alloc();
	while (m_capturing.load())
	{
		if (av_read_frame(m_formatContext, packet) < 0)
		{
			QThread::msleep(5);
			continue;
		}
		if (packet->stream_index != m_stream->index)
		{
			av_packet_unref(packet);
			continue;
		}
		if (avcodec_send_packet(m_codecContext, packet) < 0)
		{
			av_packet_unref(packet);
			continue;
		}
		while (avcodec_receive_frame(m_codecContext, frame) == 0)
		{
			if (!swsContext)
			{
				swsContext = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
				                            frame->width, frame->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
				yuvFrame->format = AV_PIX_FMT_YUV420P;
				yuvFrame->width = frame->width;
				yuvFrame->height = frame->height;
				av_frame_get_buffer(yuvFrame, 32);
			}
			sws_scale(swsContext, frame->data, frame->linesize, 0, frame->height, yuvFrame->data, yuvFrame->linesize);
			const int w = yuvFrame->width;
			const int h = yuvFrame->height;
			QByteArray y(reinterpret_cast<const char*>(yuvFrame->data[0]), yuvFrame->linesize[0] * h);
			QByteArray u(reinterpret_cast<const char*>(yuvFrame->data[1]), yuvFrame->linesize[1] * (h / 2));
			QByteArray v(reinterpret_cast<const char*>(yuvFrame->data[2]), yuvFrame->linesize[2] * (h / 2));
			dispatchFrameToView(y, u, v, w, h, yuvFrame->linesize[0], yuvFrame->linesize[1], yuvFrame->linesize[2]);
			av_frame_unref(frame);
		}
		av_packet_unref(packet);
	}
	if (swsContext)
	{
		sws_freeContext(swsContext);
	}
	av_frame_free(&yuvFrame);
	av_frame_free(&frame);
	av_packet_free(&packet);
}

void fplayer::ScreenCaptureFFmpeg::stopCapture()
{
	m_capturing.store(false);
	m_active.store(false);
	if (m_captureThread && m_captureThread->isRunning())
	{
		m_captureThread->quit();
		m_captureThread->wait(1200);
	}
	delete m_captureThread;
	m_captureThread = nullptr;
}

void fplayer::ScreenCaptureFFmpeg::cleanup()
{
	if (m_codecContext)
	{
		avcodec_free_context(&m_codecContext);
		m_codecContext = nullptr;
	}
	if (m_formatContext)
	{
		avformat_close_input(&m_formatContext);
		m_formatContext = nullptr;
	}
	m_stream = nullptr;
}
