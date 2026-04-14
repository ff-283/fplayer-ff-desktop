#include <fplayer/service/service.h>

#include <QtMultimediaWidgets/QVideoWidget>
#include <fplayer/common/maplist/maplist.hpp>
#include <logger/logger.h>

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
	return m_stream ? m_stream->lastError() : QStringLiteral("stream not initialized");
}

QString fplayer::Service::streamRecentLog() const
{
	return m_stream ? m_stream->recentLog() : QString();
}

int fplayer::Service::streamLastExitCode() const
{
	return m_stream ? m_stream->lastExitCode() : 0;
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