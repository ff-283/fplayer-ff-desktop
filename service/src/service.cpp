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
	this->m_camera->pause();
}

void fplayer::Service::cameraResume()
{
	this->m_camera->resume();
}

bool fplayer::Service::cameraIsPlaying()
{
	return this->m_camera->isPlaying();
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