#include <QDebug>
#include <QThread>
#include <QPainter>
#include <QImage>
#include <QWidget>
#include <QRegularExpression>
#include <atomic>

#include <fplayer/backend/media_ffmpeg/cameraffmpeg.h>
#include <fplayer/common/cameraframebus/cameraframebus.h>

#include "fplayer/backend/media_ffmpeg/camerainfofetcher.h"
#include "fplayer/common/fglwidget/fglwidget.h"

#include <logger/logger.h>

// FFmpeg 设备相关头文件
extern "C" {
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
}

namespace fplayer
{
	// FFmpeg 阻塞读中断回调：
	// 当 isCapturing=false 时返回 1，可打断 av_read_frame 的阻塞等待，
	// 让切换/停止流程更快结束，避免线程卡死。
	static int ffmpegInterruptCallback(void* opaque)
	{
		auto* capturing = static_cast<std::atomic<bool>*>(opaque);
		if (!capturing)
		{
			return 0;
		}
		// 返回非0会中断阻塞 IO（如 av_read_frame）
		return capturing->load() ? 0 : 1;
	}

	static bool parseCameraFormatText(const QString& formatText, int& width, int& height, int& fps)
	{
		// 兼容 "1920x1080 30fps"（可带空格/大小写）
		static const QRegularExpression re(R"((\d+)\s*x\s*(\d+)\s+(\d+)\s*fps)", QRegularExpression::CaseInsensitiveOption);
		QRegularExpressionMatch m = re.match(formatText);
		if (!m.hasMatch())
		{
			return false;
		}
		width = m.captured(1).toInt();
		height = m.captured(2).toInt();
		fps = m.captured(3).toInt();
		return width > 0 && height > 0 && fps > 0;
	}

	// 可选：为线程安全做准备
	static std::mutex log_mutex;

	static void loggerCallback(void* ptr, int level, const char* fmt, va_list vargs)
	{
		// 过滤等级（关键）
		if (level > av_log_get_level())
		{
			return;
		}
		static char message[2048];
		std::lock_guard<std::mutex> lock(log_mutex);

		vsnprintf(message, sizeof(message), fmt, vargs);

		// 可选：过滤掉结尾的换行
		std::string str(message);
		if (!str.empty() && str.back() == '\n')
		{
			str.pop_back();
		}

		// 将 FFmpeg 的日志级别转换为你自己的日志级别
		if (level <= AV_LOG_PANIC || level == AV_LOG_FATAL)
		{
			LOG_CRITI("[ffmpeg]", str);
		}
		else if (level <= AV_LOG_ERROR)
		{
			LOG_ERROR("[ffmpeg]", str);
		}
		else if (level <= AV_LOG_WARNING)
		{
			LOG_WARN("[ffmpeg]", str);
		}
		else if (level <= AV_LOG_INFO)
		{
			LOG_INFO("[ffmpeg]", str);
		}
		else if (level <= AV_LOG_VERBOSE)
		{
			LOG_DEBUG("[ffmpeg]", str);
		}
		else if (level <= AV_LOG_DEBUG)
		{
			LOG_DEBUG("[ffmpeg]", str);
		}
		else
		{
			LOG_TRACE("[ffmpeg]", str);
		}
	}


	struct CameraFFmpeg::Impl
	{
		// 输入/解码主链路对象
		AVFormatContext* formatContext = nullptr;
		AVCodecContext* codecContext = nullptr;
		AVStream* stream = nullptr;
		// 像素格式转换资源：用于把 NV12/MJPEG 等统一转成 YUV420P
		SwsContext* swsContext = nullptr;
		AVFrame* swsFrame = nullptr;
		int swsWidth = 0;
		int swsHeight = 0;
		AVPixelFormat swsSrcFormat = AV_PIX_FMT_NONE;
		QThread* captureThread = nullptr;
		std::atomic<bool> isCapturing{false};
		PreviewTarget previewTarget;
		FGLWidget* fGLWieget = nullptr;
		QString frameBusSourceId = QStringLiteral("default");

		~Impl()
		{
			stopCapture();
			cleanup();
		}

		void cleanup()
		{
			// cleanup 仅释放 FFmpeg 媒体资源；
			// 线程对象生命周期由 stopCapture() 统一处理。
			if (swsFrame)
			{
				av_frame_free(&swsFrame);
				swsFrame = nullptr;
			}
			if (swsContext)
			{
				sws_freeContext(swsContext);
				swsContext = nullptr;
			}
			swsWidth = 0;
			swsHeight = 0;
			swsSrcFormat = AV_PIX_FMT_NONE;

			if (codecContext)
			{
				avcodec_free_context(&codecContext);
				codecContext = nullptr;
			}
			if (formatContext)
			{
				avformat_close_input(&formatContext);
				formatContext = nullptr;
			}
		}

		void stopCapture()
		{
			// 1) 协作式停止信号（供回调与循环判断）
			isCapturing.store(false);

			if (captureThread && captureThread->isRunning())
			{
				// 2) QThread::create 无默认事件循环时 quit() 无操作；协作式退出靠 isCapturing + 中断回调
				captureThread->quit();
				captureThread->wait(500);// 等待最多0.5秒
				if (captureThread->isRunning())
				{
					// 3) 仍阻塞时，关闭输入打断 av_read_frame
					if (formatContext)
					{
						avformat_close_input(&formatContext);
						formatContext = nullptr;
					}
					captureThread->quit();
					captureThread->wait(1500);
				}
				if (captureThread->isRunning())
				{
					qDebug() << "[CameraFFmpeg] capture thread did not stop in time.";
					// 绝不 delete 正在运行的 QThread（会触发崩溃）
					captureThread = nullptr;
					return;
				}
				delete captureThread;
				captureThread = nullptr;
			}
		}
	};

	CameraFFmpeg::CameraFFmpeg() : m_impl(new Impl)
	{
		m_backend = MediaBackendType::FFmpeg;
		// 注册 FFmpeg 设备
		avdevice_register_all();

		av_log_set_level(AV_LOG_FATAL);
		av_log_set_callback(loggerCallback);// 指定ffmpeg日志输出到logger
	}

	CameraFFmpeg::~CameraFFmpeg()
	{
		delete m_impl;
	}

	void CameraFFmpeg::refreshCameras()
	{
		m_descriptions.clear();

		m_descriptions = CameraDescriptionFetcher::getDescriptions();
	}

	QList<CameraDescription> CameraFFmpeg::getDescriptions()
	{
		if (m_descriptions.empty())
		{
			refreshCameras();
		}
		return m_descriptions;
	}

	int CameraFFmpeg::getIndex()
	{
		return m_cameraIndex;
	}

	bool CameraFFmpeg::selectCamera(int index)
	{
		// if (index < 0 || index >= m_impl->cameraDevices.size())
		if (index < 0 || index >= m_descriptions.size())
		{
			//qWarning() << "Invalid camera index:" << index;
			return false;
		}

		// 切换摄像头时采用“先停旧链路，再建新链路”，避免状态串扰。
		m_impl->stopCapture();
		m_impl->cleanup();

		// 打开摄像头
		// const Impl::CameraDeviceInfo& deviceInfo = m_impl->cameraDevices[index];
		const auto deviceInfo = m_descriptions[index];
		// QString devicePath = "video=" + deviceInfo.name;
		QString devicePath = "video=" + deviceInfo.description;

		// 为切换场景缩短探测时长，降低打开延迟。
		if (!m_impl->formatContext)
		{
			m_impl->formatContext = avformat_alloc_context();
		}
		if (!m_impl->formatContext)
		{
			return false;
		}
		m_impl->formatContext->interrupt_callback.callback = ffmpegInterruptCallback;
		m_impl->formatContext->interrupt_callback.opaque = &m_impl->isCapturing;
		m_impl->formatContext->probesize = 32 * 1024;
		m_impl->formatContext->max_analyze_duration = 200 * 1000;// 200ms (us)

		AVDictionary* options = nullptr;
		av_dict_set(&options, "analyzeduration", "200000", 0);
		av_dict_set(&options, "probesize", "32768", 0);
		bool hasExplicitFormat = false;

		// 若已有格式选择，则优先按该分辨率/帧率打开设备。
		if (deviceInfo.formatIndex >= 0 && deviceInfo.formatIndex < deviceInfo.formats.size())
		{
			int fmtW = 0;
			int fmtH = 0;
			int fmtFps = 0;
			if (parseCameraFormatText(deviceInfo.formats[deviceInfo.formatIndex], fmtW, fmtH, fmtFps))
			{
				const QString videoSize = QString("%1x%2").arg(fmtW).arg(fmtH);
				const QString frameRate = QString::number(fmtFps);
				av_dict_set(&options, "video_size", videoSize.toUtf8().constData(), 0);
				av_dict_set(&options, "framerate", frameRate.toUtf8().constData(), 0);
				hasExplicitFormat = true;
				qDebug() << "[CameraFFmpeg] Open with format:" << videoSize << frameRate + "fps";
			}
		}

		int ret = avformat_open_input(&m_impl->formatContext, devicePath.toUtf8().constData(),
		                              av_find_input_format("dshow"), &options);
		av_dict_free(&options);
		if (ret < 0)
		{
			if (hasExplicitFormat)
			{
				qDebug() << "[CameraFFmpeg] Open with explicit format failed, retry with default format.";
				if (m_impl->formatContext)
				{
					avformat_close_input(&m_impl->formatContext);
				}

				AVDictionary* fallbackOptions = nullptr;
				av_dict_set(&fallbackOptions, "analyzeduration", "200000", 0);
				av_dict_set(&fallbackOptions, "probesize", "32768", 0);
				ret = avformat_open_input(&m_impl->formatContext, devicePath.toUtf8().constData(),
				                          av_find_input_format("dshow"), &fallbackOptions);
				av_dict_free(&fallbackOptions);
				if (ret >= 0)
				{
					qDebug() << "[CameraFFmpeg] Fallback open succeeded with device default format.";
				}
			}

			if (ret < 0)
			{
				return false;
			}
		}

		// 读取流信息：后续找视频流和建解码器依赖这一步。
		ret = avformat_find_stream_info(m_impl->formatContext, nullptr);
		if (ret < 0)
		{
			//qWarning() << "Failed to find stream info:" << av_err2str(ret);
			m_impl->cleanup();
			return false;
		}

		// 当前策略：取第一个视频流作为采集目标。
		int videoStreamIndex = -1;
		for (unsigned int i = 0; i < m_impl->formatContext->nb_streams; ++i)
		{
			if (m_impl->formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
			{
				videoStreamIndex = i;
				break;
			}
		}

		if (videoStreamIndex == -1)
		{
			//qWarning() << "No video stream found";
			m_impl->cleanup();
			return false;
		}

		m_impl->stream = m_impl->formatContext->streams[videoStreamIndex];
		AVCodecParameters* codecParams = m_impl->stream->codecpar;

		// 解码链路：find decoder -> alloc context -> copy params -> open。
		const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
		if (!codec)
		{
			//qWarning() << "Failed to find decoder";
			m_impl->cleanup();
			return false;
		}

		// 打开解码器
		m_impl->codecContext = avcodec_alloc_context3(codec);
		if (!m_impl->codecContext)
		{
			//qWarning() << "Failed to allocate codec context";
			m_impl->cleanup();
			return false;
		}

		ret = avcodec_parameters_to_context(m_impl->codecContext, codecParams);
		if (ret < 0)
		{
			//qWarning() << "Failed to copy codec parameters:" << av_err2str(ret);
			m_impl->cleanup();
			return false;
		}

		ret = avcodec_open2(m_impl->codecContext, codec, nullptr);
		if (ret < 0)
		{
			//qWarning() << "Failed to open codec:" << av_err2str(ret);
			m_impl->cleanup();
			return false;
		}

		// 用 QThread::create 在工作线程执行 captureLoop()：默认 connect(started, λ) 可能把槽排到 GUI 线程，
		// 导致采集阻塞主线程，且 QThread::currentThread()->quit() 若落在主线程会直接结束应用事件循环。
		m_impl->isCapturing.store(true);
		m_impl->captureThread = QThread::create([this]() {
			captureLoop();
		});
		m_impl->captureThread->start();

		m_cameraIndex = index;
		m_isPlaying = true;
		//qdebug() << "Selected camera:" << deviceInfo.name;
		return true;
	}

	bool CameraFFmpeg::selectCameraFormat(int index)
	{
		if (m_cameraIndex < 0 || m_cameraIndex >= m_descriptions.size())
		{
			return false;
		}

		auto& desc = m_descriptions[m_cameraIndex];
		if (index < 0 || index >= desc.formats.size())
		{
			return false;
		}

		desc.formatIndex = index;
		qDebug() << "[CameraFFmpeg] Select format index:" << index << desc.formats[index];

		// 切格式策略：复用 selectCamera() 做完整重建，稳定性优先。
		return selectCamera(m_cameraIndex);
	}

	void CameraFFmpeg::pause()
	{
		// m_impl->isPaused = true;
		this->m_isPlaying = false;
		qDebug() << "Camera paused";
	}

	void CameraFFmpeg::resume()
	{
		// m_impl->isPaused = false;
		this->m_isPlaying = true;
		qDebug() << "Camera resumed";
	}

	bool CameraFFmpeg::isPlaying() const
	{
		// return !m_impl->isPaused;
		return m_isPlaying;
	}

	void CameraFFmpeg::setFrameBusSourceId(const QString& sourceId)
	{
		const QString sid = sourceId.trimmed();
		m_impl->frameBusSourceId = sid.isEmpty() ? QStringLiteral("default") : sid;
	}

	QString CameraFFmpeg::frameBusSourceId() const
	{
		return m_impl->frameBusSourceId;
	}

	void CameraFFmpeg::setPreviewTarget(const PreviewTarget& target)
	{
		m_impl->previewTarget = target;
		// 获取渲染窗口
		if (target.backend_hint)
		{
			m_impl->fGLWieget = static_cast<FGLWidget*>(target.backend_hint);
			auto glWidget = static_cast<FGLWidget*>(target.backend_hint);
			// 使用 YUV 直接渲染，更高效
			bool connected = QObject::connect(this, &CameraFFmpeg::yuvFrameReady,
			                                  glWidget, &FGLWidget::updateYUVFrame, Qt::QueuedConnection);
			qDebug() << "[Runtime] YUV signal connection established:" << connected;
		}
		else
		{
			qDebug() << "[Runtime] Failed to connect: ffmpegCamera=" << this << "backend_hint=" << target.backend_hint;
		}
	}

	void CameraFFmpeg::captureLoop()
	{
		// 采集循环标准流程：
		// av_read_frame -> avcodec_send_packet -> avcodec_receive_frame -> 发到渲染层
		AVPacket* packet = av_packet_alloc();
		AVFrame* frame = av_frame_alloc();

		while (m_impl->isCapturing.load())
		{
			if (!m_isPlaying)
			{
				// 暂停只阻塞生产，不销毁链路；恢复播放时可快速继续。
				QThread::msleep(100);
				continue;
			}

			int ret = av_read_frame(m_impl->formatContext, packet);
			if (ret < 0)
			{
				if (ret == AVERROR_EOF)
				{
					// 流结束，重新开始
					avformat_seek_file(m_impl->formatContext, -1, 0, 0, 0, 0);
				}
				else
				{
					QThread::msleep(100);
				}
				continue;
			}

			if (packet->stream_index == m_impl->stream->index)
			{
				ret = avcodec_send_packet(m_impl->codecContext, packet);
				if (ret < 0)
				{
					av_packet_unref(packet);
					continue;
				}

				while (ret >= 0)
				{
					ret = avcodec_receive_frame(m_impl->codecContext, frame);
					if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
					{
						break;
					}
					else if (ret < 0)
					{
						break;
					}

					// 直接发射 YUV 数据，避免 CPU 转换
					AVFrame* renderFrame = frame;
					if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P)
					{
						// 将输入像素格式统一转换为 YUV420P，简化 OpenGL 渲染端处理。
						AVPixelFormat srcFormat = static_cast<AVPixelFormat>(frame->format);
						if (!m_impl->swsContext ||
						    m_impl->swsWidth != frame->width ||
						    m_impl->swsHeight != frame->height ||
						    m_impl->swsSrcFormat != srcFormat)
						{
							if (m_impl->swsContext)
							{
								sws_freeContext(m_impl->swsContext);
								m_impl->swsContext = nullptr;
							}
							if (m_impl->swsFrame)
							{
								av_frame_free(&m_impl->swsFrame);
								m_impl->swsFrame = nullptr;
							}

							m_impl->swsContext = sws_getContext(
									frame->width, frame->height, srcFormat,
									frame->width, frame->height, AV_PIX_FMT_YUV420P,
									SWS_BILINEAR, nullptr, nullptr, nullptr
									);
							if (!m_impl->swsContext)
							{
								qDebug() << "[CameraFFmpeg] Failed to create sws context, src format:" << frame->format;
								av_frame_unref(frame);
								continue;
							}

							m_impl->swsFrame = av_frame_alloc();
							if (!m_impl->swsFrame)
							{
								qDebug() << "[CameraFFmpeg] Failed to allocate sws frame";
								av_frame_unref(frame);
								continue;
							}
							m_impl->swsFrame->format = AV_PIX_FMT_YUV420P;
							m_impl->swsFrame->width = frame->width;
							m_impl->swsFrame->height = frame->height;
							if (av_frame_get_buffer(m_impl->swsFrame, 32) < 0)
							{
								qDebug() << "[CameraFFmpeg] Failed to allocate sws frame buffer";
								av_frame_unref(frame);
								continue;
							}

							m_impl->swsWidth = frame->width;
							m_impl->swsHeight = frame->height;
							m_impl->swsSrcFormat = srcFormat;
							qDebug() << "[CameraFFmpeg] Enable sws convert from format" << frame->format << "to YUV420P";
						}

						av_frame_make_writable(m_impl->swsFrame);
						sws_scale(
								m_impl->swsContext,
								frame->data,
								frame->linesize,
								0,
								frame->height,
								m_impl->swsFrame->data,
								m_impl->swsFrame->linesize
								);
						renderFrame = m_impl->swsFrame;
					}

					const int yStride = renderFrame->linesize[0];
					const int uStride = renderFrame->linesize[1];
					const int vStride = renderFrame->linesize[2];
					const int width = renderFrame->width;
					const int height = renderFrame->height;
					const int uvHeight = height / 2;

					// 这里必须做深拷贝（QByteArray）：
					// 信号是跨线程 queued 传递，AVFrame 原始指针在 unref 后会失效。
					QByteArray yBuffer(reinterpret_cast<const char*>(renderFrame->data[0]), yStride * height);
					QByteArray uBuffer(reinterpret_cast<const char*>(renderFrame->data[1]), uStride * uvHeight);
					QByteArray vBuffer(reinterpret_cast<const char*>(renderFrame->data[2]), vStride * uvHeight);

#if 0
					static int frameCount = 0;
					if (++frameCount % 30 == 0)
					{
						qDebug() << "[CameraFFmpeg] Emitting YUV frame:" << width << "x" << height
								<< "Y stride:" << yStride << "src format:" << frame->format;
					}
#endif
					emit yuvFrameReady(
							yBuffer,
							uBuffer,
							vBuffer,
							width,
							height,
							yStride,
							uStride,
							vStride
							);
					CameraFrameBus::instance().publish(yBuffer, uBuffer, vBuffer, width, height, yStride, uStride, vStride,
					                                  m_impl->frameBusSourceId);

					av_frame_unref(frame);
				}
			}

			av_packet_unref(packet);
		}

		av_frame_free(&frame);
		av_packet_free(&packet);
	}
}