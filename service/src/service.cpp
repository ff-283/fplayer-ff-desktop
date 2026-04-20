#include <fplayer/service/service.h>

#include <QtMultimediaWidgets/QVideoWidget>
#include <QRegularExpression>
#include <fplayer/common/maplist/maplist.hpp>
#include <logger/logger.h>

namespace
{
	bool parseCameraFormatText(const QString& formatText, int& width, int& height, int& fps)
	{
		static const QRegularExpression re(R"((\d+)\s*x\s*(\d+)\s+(\d+)\s*fps)", QRegularExpression::CaseInsensitiveOption);
		const QRegularExpressionMatch m = re.match(formatText);
		if (!m.hasMatch())
		{
			return false;
		}
		width = m.captured(1).toInt();
		height = m.captured(2).toInt();
		fps = m.captured(3).toInt();
		return width > 0 && height > 0 && fps > 0;
	}
}

fplayer::Service::Service() : m_runtime(new RunTime()), m_cameraIndex(0)
{
};

fplayer::Service::~Service() = default;

void fplayer::Service::initCamera(MediaBackendType backend)
{
	this->m_camera = m_runtime->createCamera(backend);
	if (this->m_camera == nullptr)
	{
		LOG_WARN("fplayer::Service::initCamera(MediaBackend backend) ==> 摄像头获取失败");
	}
}

void fplayer::Service::initPlayer(MediaBackendType backend)
{
	this->m_player = m_runtime->createPlayer(backend);
	if (this->m_player == nullptr)
	{
		LOG_WARN("fplayer::Service::initPlayer(MediaBackend backend) ==> 播放器获取失败");
	}
}

void fplayer::Service::initScreenCapture(MediaBackendType backend)
{
	this->m_screenCapture = m_runtime->createScreenCapture(backend);
	if (this->m_screenCapture == nullptr)
	{
		LOG_WARN("fplayer::Service::initScreenCapture(MediaBackend backend) ==> 屏幕采集获取失败");
	}
}

void fplayer::Service::initStream(MediaBackendType backend)
{
	this->m_stream = m_runtime->createStream(backend);
	if (this->m_stream == nullptr)
	{
		LOG_WARN("fplayer::Service::initStream(MediaBackend backend) ==> 推拉流获取失败");
		m_streamInitErrorHint = QStringLiteral("stream backend unavailable: ensure CMake option FPLAYER_BUILD_STREAM_FFMPEG=ON then reconfigure/rebuild");
	}
	else
	{
		m_streamInitErrorHint.clear();
	}
}

void fplayer::Service::bindCameraPreview(fplayer::IFVideoView* videoView)
{
	if (!videoView || !m_runtime)
	{
		return;
	}

	const auto target = videoView->previewTarget();
	m_runtime->bindCameraPreview(target);

	// switch (this->m_camera->getBackendType())
	// {
	// case MediaBackendType::Qt6:
	// 	this->bindCameraPreviewQt6(widget);
	// 	break;
	// case MediaBackendType::FFmpeg:
	// 	this->bindCameraPreviewFFmpeg(widget);
	// 	break;
	// }
}

void fplayer::Service::bindPlayerPreview(fplayer::IFVideoView* videoView)
{
	if (!videoView || !m_runtime)
	{
		return;
	}
	const auto target = videoView->previewTarget();
	m_runtime->bindPlayerPreview(target);
}

void fplayer::Service::bindScreenPreview(fplayer::IFVideoView* videoView)
{
	if (!videoView || !m_runtime)
	{
		return;
	}
	const auto target = videoView->previewTarget();
	m_runtime->bindScreenPreview(target);
}

bool fplayer::Service::openMediaFile(const QString& filePath)
{
	if (!m_player)
	{
		return false;
	}
	return m_player->openFile(filePath);
}

void fplayer::Service::selectCamera(int index)
{
	m_cameraIndex = -1;
	if (m_camera->selectCamera(index))
	{
		m_cameraIndex = index;
	}
}

void fplayer::Service::selectCameraFormat(int index)
{
	if (!m_camera->selectCameraFormat(index))
	{
		LOG_DEBUG("fplayer::Service::selectCameraFormat(int index) ==> 摄像头格式切换失败");
		return;
	}
	LOG_DEBUG("fplayer::Service::selectCameraFormat(int index) ==> 摄像头格式切换成功");
}

QList<QString> fplayer::Service::getCameraList() const
{
	QList<QString> cameraList;
	if (this->m_camera == nullptr)
	{
		return cameraList;
	}

	const auto cameraDescriptions = this->m_camera->getDescriptions();
	cameraList = mapList(cameraDescriptions, [](const CameraDescription& description) {
		return description.description;
	});

	return cameraList;
}

QList<QString> fplayer::Service::getCameraFormats(int index) const
{
	if (this->m_camera == nullptr)
	{
		return {};
	}

	const auto cameraDescriptions = this->m_camera->getDescriptions();
	if (index < 0 || index >= cameraDescriptions.size())
	{
		return {};
	}
	return cameraDescriptions.at(index).formats;
}

void fplayer::Service::cameraPause()
{
	if (this->m_camera)
	{
		this->m_camera->pause();
	}
}

void fplayer::Service::cameraResume()
{
	if (this->m_camera)
	{
		this->m_camera->resume();
	}
}

bool fplayer::Service::cameraIsPlaying()
{
	return this->m_camera && this->m_camera->isPlaying();
}

void fplayer::Service::playerPause()
{
	if (m_player)
	{
		m_player->pause();
	}
}

void fplayer::Service::playerResume()
{
	if (m_player)
	{
		m_player->play();
	}
}

void fplayer::Service::playerStop()
{
	if (m_player)
	{
		m_player->stop();
	}
}

bool fplayer::Service::playerIsPlaying()
{
	return m_player && m_player->isPlaying();
}

qint64 fplayer::Service::playerDurationMs() const
{
	return m_player ? m_player->durationMs() : 0;
}

qint64 fplayer::Service::playerPositionMs() const
{
	return m_player ? m_player->positionMs() : 0;
}

bool fplayer::Service::playerSeekMs(qint64 positionMs)
{
	return m_player && m_player->seekMs(positionMs);
}

void fplayer::Service::playerSetPlaybackRate(double rate)
{
	if (m_player)
	{
		m_player->setPlaybackRate(rate);
	}
}

double fplayer::Service::playerPlaybackRate() const
{
	return m_player ? m_player->playbackRate() : 1.0;
}

QString fplayer::Service::playerDebugStats() const
{
	return m_player ? m_player->debugStats() : QStringLiteral("n/a");
}

QList<QString> fplayer::Service::getScreenList() const
{
	QList<QString> screenList;
	if (!m_screenCapture)
	{
		return screenList;
	}
	const auto descriptions = m_screenCapture->getDescriptions();
	for (const auto& d : descriptions)
	{
		screenList.push_back(QString("%1 (%2, %3x%4)")
		                     .arg(d.name, d.isPrimary ? QStringLiteral("主屏") : QStringLiteral("副屏"))
		                     .arg(d.width)
		                     .arg(d.height));
	}
	return screenList;
}

bool fplayer::Service::selectScreen(int index)
{
	return m_screenCapture && m_screenCapture->selectScreen(index);
}

void fplayer::Service::screenSetActive(bool active)
{
	if (m_screenCapture)
	{
		m_screenCapture->setActive(active);
	}
}

bool fplayer::Service::screenIsActive() const
{
	return m_screenCapture && m_screenCapture->isActive();
}

bool fplayer::Service::screenSetCursorCaptureEnabled(bool enabled)
{
	return m_screenCapture && m_screenCapture->setCursorCaptureEnabled(enabled);
}

bool fplayer::Service::screenCanControlCursorCapture() const
{
	return m_screenCapture && m_screenCapture->canControlCursorCapture();
}

bool fplayer::Service::screenSetFrameRate(int fps)
{
	return m_screenCapture && m_screenCapture->setFrameRate(fps);
}

int fplayer::Service::screenFrameRate() const
{
	return m_screenCapture ? m_screenCapture->frameRate() : 30;
}

bool fplayer::Service::screenCanControlFrameRate() const
{
	return m_screenCapture && m_screenCapture->canControlFrameRate();
}

fplayer::MediaBackendType fplayer::Service::screenBackendType() const
{
	return m_screenCapture ? m_screenCapture->backendType() : MediaBackendType::Qt6;
}

bool fplayer::Service::streamStartPush(const QString& inputUrl, const QString& outputUrl)
{
	return m_stream && m_stream->startPush(inputUrl, outputUrl);
}

bool fplayer::Service::streamStartPushByScene(PushScene scene, const QString& outputUrl, const QString& sceneInput)
{
	return streamStartPushByScene(scene, outputUrl, sceneInput, PushOptions{});
}

bool fplayer::Service::streamStartPushByScene(PushScene scene, const QString& outputUrl, const QString& sceneInput,
                                              const PushOptions& options)
{
	auto applyAspectRatio = [](int srcW, int srcH, int& outW, int& outH, const bool keepAspect) {
		if (!keepAspect || srcW <= 0 || srcH <= 0 || outW <= 0 || outH <= 0)
		{
			return;
		}
		const double srcRatio = static_cast<double>(srcW) / static_cast<double>(srcH);
		const double dstRatio = static_cast<double>(outW) / static_cast<double>(outH);
		if (dstRatio > srcRatio)
		{
			outW = qMax(2, static_cast<int>(outH * srcRatio)) & ~1;
		}
		else
		{
			outH = qMax(2, static_cast<int>(outW / srcRatio)) & ~1;
		}
	};

	if (!m_stream)
	{
		return false;
	}
	switch (scene)
	{
	case PushScene::Screen:
	{
		const int fps = options.fps > 0 ? options.fps : qMax(1, screenFrameRate());
		const bool useScreenPreview = (screenBackendType() == MediaBackendType::Dxgi);
		QString screenSpec = QStringLiteral("%1:fps=%2")
			                     .arg(useScreenPreview ? QStringLiteral("__screen_preview__") : QStringLiteral("__screen_capture__"))
			                     .arg(fps);
		if (m_screenCapture)
		{
			const auto screens = m_screenCapture->getDescriptions();
			const int index = m_screenCapture->getIndex();
			if (index >= 0 && index < screens.size())
			{
				const auto& s = screens.at(index);
				// size 表示采集区域（整屏）；outsize 才是编码输出尺寸（缩放目标）。
				screenSpec += QStringLiteral(";x=%1;y=%2;size=%3x%4").arg(s.x).arg(s.y).arg(s.width).arg(s.height);
				int w = options.width > 0 ? options.width : s.width;
				int h = options.height > 0 ? options.height : s.height;
				applyAspectRatio(s.width, s.height, w, h, options.keepAspectRatio);
				if (w != s.width || h != s.height)
				{
					screenSpec += QStringLiteral(";outsize=%1x%2").arg(w).arg(h);
				}
			}
		}
		if (options.bitrateKbps > 0)
		{
			screenSpec += QStringLiteral(";bitrate=%1").arg(options.bitrateKbps);
		}
		if (!options.videoEncoder.trimmed().isEmpty())
		{
			screenSpec += QStringLiteral(";encoder=%1").arg(options.videoEncoder.trimmed().toLower());
		}
		if (!options.audioInputSource.trimmed().isEmpty())
		{
			screenSpec += QStringLiteral(";audio_in=%1").arg(options.audioInputSource.trimmed());
		}
		if (!options.audioOutputSource.trimmed().isEmpty())
		{
			screenSpec += QStringLiteral(";audio_out=%1").arg(options.audioOutputSource.trimmed());
		}
		return m_stream->startPush(screenSpec, outputUrl);
	}
	case PushScene::File:
	{
		// 文件模式：未指定参数时走 remux/copy；指定码率/尺寸/帧率时走转码推流。
		const bool needTranscode = options.bitrateKbps > 0 || options.fps > 0 || (options.width > 0 && options.height > 0);
		if (!needTranscode)
		{
			return m_stream->startPush(sceneInput, outputUrl);
		}
		QStringList parts;
		parts << QStringLiteral("src64=%1").arg(QString::fromUtf8(sceneInput.toUtf8().toBase64()));
		if (options.fps > 0)
		{
			parts << QStringLiteral("fps=%1").arg(options.fps);
		}
		if (options.width > 0 && options.height > 0)
		{
			parts << QStringLiteral("size=%1x%2").arg(options.width).arg(options.height);
		}
		if (options.bitrateKbps > 0)
		{
			parts << QStringLiteral("bitrate=%1").arg(options.bitrateKbps);
		}
		return m_stream->startPush(QStringLiteral("__file_transcode__:") + parts.join(';'), outputUrl);
	}
	case PushScene::Camera:
	{
		QString cameraSpec = sceneInput.trimmed();
		if (cameraSpec.isEmpty())
		{
			const QList<QString> cameras = getCameraList();
			if (m_cameraIndex >= 0 && m_cameraIndex < cameras.size())
			{
				cameraSpec = cameras.at(m_cameraIndex).trimmed();
			}
		}
		if (cameraSpec.isEmpty())
		{
			QStringList fallbackParams;
			if (!options.audioInputSource.trimmed().isEmpty())
			{
				fallbackParams << QStringLiteral("audio_in=%1").arg(options.audioInputSource.trimmed());
			}
			if (!options.audioOutputSource.trimmed().isEmpty())
			{
				fallbackParams << QStringLiteral("audio_out=%1").arg(options.audioOutputSource.trimmed());
			}
			const QString tail = fallbackParams.isEmpty() ? QString() : fallbackParams.join(';');
			return m_stream->startPush(QStringLiteral("__camera_capture__:") + tail, outputUrl);
		}
		if (!cameraSpec.startsWith(QStringLiteral("video=")))
		{
			cameraSpec = QStringLiteral("video=") + cameraSpec;
		}
		int width = 0;
		int height = 0;
		int fps = 0;
		if (m_camera)
		{
			const auto descriptions = m_camera->getDescriptions();
			if (m_cameraIndex >= 0 && m_cameraIndex < descriptions.size())
			{
				const auto& cur = descriptions.at(m_cameraIndex);
				if (cur.formatIndex >= 0 && cur.formatIndex < cur.formats.size())
				{
					parseCameraFormatText(cur.formats.at(cur.formatIndex), width, height, fps);
				}
			}
		}
		QStringList params;
		params << cameraSpec;
		if (options.fps > 0)
		{
			params << QStringLiteral("fps=%1").arg(options.fps);
		}
		else if (fps > 0)
		{
			params << QStringLiteral("fps=%1").arg(fps);
		}
		int outW = options.width > 0 ? options.width : width;
		int outH = options.height > 0 ? options.height : height;
		applyAspectRatio(width, height, outW, outH, options.keepAspectRatio);
		if (outW > 0 && outH > 0)
		{
			params << QStringLiteral("size=%1x%2").arg(outW).arg(outH);
		}
		if (options.bitrateKbps > 0)
		{
			params << QStringLiteral("bitrate=%1").arg(options.bitrateKbps);
		}
		if (!options.audioInputSource.trimmed().isEmpty())
		{
			params << QStringLiteral("audio_in=%1").arg(options.audioInputSource.trimmed());
		}
		if (!options.audioOutputSource.trimmed().isEmpty())
		{
			params << QStringLiteral("audio_out=%1").arg(options.audioOutputSource.trimmed());
		}
		return m_stream->startPush(QStringLiteral("__camera_preview__:") + params.join(';'), outputUrl);
	}
	default:
		return false;
	}
}

bool fplayer::Service::streamStartPull(const QString& inputUrl, const QString& outputUrl)
{
	return m_stream && m_stream->startPull(inputUrl, outputUrl);
}

void fplayer::Service::streamStop()
{
	if (m_stream)
	{
		m_stream->stop();
	}
}

bool fplayer::Service::streamIsRunning() const
{
	return m_stream && m_stream->isRunning();
}

QString fplayer::Service::streamLastError() const
{
	if (m_stream)
	{
		return m_stream->lastError();
	}
	return m_streamInitErrorHint.isEmpty() ? QStringLiteral("stream not initialized") : m_streamInitErrorHint;
}

QString fplayer::Service::streamRecentLog() const
{
	return m_stream ? m_stream->recentLog() : QString();
}

int fplayer::Service::streamLastExitCode() const
{
	return m_stream ? m_stream->lastExitCode() : 0;
}

QStringList fplayer::Service::streamAvailableVideoEncoders() const
{
	if (!m_stream)
	{
		return {QStringLiteral("cpu")};
	}
	return m_stream->availableVideoEncoders();
}

bool fplayer::Service::streamHasCompletedSession() const
{
	return m_stream && m_stream->hasCompletedStreamSession();
}

// void fplayer::Service::bindCameraPreviewQt6(QWidget* widget)
// {
// 	PreviewTarget target;
// 	if (!widget->layout())
// 	{
// 		auto layout = new QVBoxLayout(widget);
// 		layout->setContentsMargins(0, 0, 0, 0);
// 		layout->setSpacing(0);
// 		widget->setLayout(layout);
// 	}
//
// 	auto view = new QVideoWidget(widget);
// 	widget->layout()->addWidget(view);
// }
//
// void fplayer::Service::bindCameraPreviewFFmpeg(QWidget* widget)
// {
// }