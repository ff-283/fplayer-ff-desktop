#include <fplayer/runtime/runtime.h>

#include <fplayer/backend/media_ffmpeg/cameraffmpeg.h>
#include <fplayer/backend/media_ffmpeg/playerffmpeg.h>
#include <fplayer/backend/media_ffmpeg/screencaptureffmpeg.h>
#include <fplayer/backend/net_ffmpeg/streamffmpeg.h>

#include <fplayer/backend/media_qt6/cameraqt6.h>
#include <fplayer/backend/media_qt6/screencaptureqt6.h>

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

std::shared_ptr<fplayer::IPlayer> fplayer::RunTime::createPlayer(MediaBackendType backend)
{
	m_player.reset();
	switch (backend)
	{
	case MediaBackendType::Qt6:
		break;
	case MediaBackendType::FFmpeg:
		m_player = std::make_shared<fplayer::PlayerFFmpeg>();
		break;
	default:
		break;
	}
	return m_player;
}

std::shared_ptr<fplayer::IScreenCapture> fplayer::RunTime::createScreenCapture(MediaBackendType backend)
{
	m_screenCapture.reset();
	switch (backend)
	{
	case MediaBackendType::Qt6:
		m_screenCapture = std::make_shared<fplayer::ScreenCaptureQt6>();
		break;
	case MediaBackendType::FFmpeg:
		m_screenCapture = std::make_shared<fplayer::ScreenCaptureFFmpeg>();
		break;
	default:
		break;
	}
	return m_screenCapture;
}

std::shared_ptr<fplayer::IStream> fplayer::RunTime::createStream(MediaBackendType backend)
{
	m_stream.reset();
	switch (backend)
	{
	case MediaBackendType::FFmpeg:
		m_stream = std::make_shared<fplayer::StreamFFmpeg>();
		break;
	default:
		break;
	}
	return m_stream;
}

void fplayer::RunTime::bindCameraPreview(const fplayer::PreviewTarget& target)
{
	if (!m_camera)
	{
		return;
	}
	this->m_camera->setPreviewTarget(target);
}

void fplayer::RunTime::bindPlayerPreview(const fplayer::PreviewTarget& target)
{
	if (!m_player)
	{
		return;
	}
	this->m_player->setPreviewTarget(target);
}

void fplayer::RunTime::bindScreenPreview(const fplayer::PreviewTarget& target)
{
	if (!m_screenCapture)
	{
		return;
	}
	this->m_screenCapture->setPreviewTarget(target);
}