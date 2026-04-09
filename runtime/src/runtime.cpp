#include <fplayer/runtime/runtime.h>

#include <fplayer/backend/media_ffmpeg/cameraffmpeg.h>

#include <fplayer/backend/media_qt6/cameraqt6.h>

std::shared_ptr<fplayer::ICamera> fplayer::RunTime::createCamera(MediaBackendType backend)
{
	m_camera.reset();
	switch (backend)
	{
	case MediaBackendType::Qt6:
		m_camera = std::make_shared<fplayer::CameraQt6>();
		break;
	case MediaBackendType::FFmpeg:
		m_camera = std::make_unique<fplayer::CameraFFmpeg>();
		break;
	default:
		break;
	}
	return m_camera;
}

void fplayer::RunTime::bindCameraPreview(const fplayer::PreviewTarget& target)
{
	if (!m_camera)
	{
		return;
	}
	this->m_camera->setPreviewTarget(target);
}