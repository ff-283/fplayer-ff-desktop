#include <fplayer/runtime/runtime.h>

#include <fplayer/backend/media_ffmpeg/cameraffmpeg.h>
#include <fplayer/backend/media_ffmpeg/playerffmpeg.h>
#include <fplayer/backend/media_ffmpeg/screencaptureffmpeg.h>
#if defined(_WIN32) && defined(FPLAYER_WITH_SCREEN_DXGI)
#include <fplayer/backend/desktopcapture_dxgi/screencapturedxgi.h>
#endif
#if defined(FPLAYER_WITH_NET_FFMPEG)
#include <fplayer/backend/net_ffmpeg/streamffmpeg.h>
#endif

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
	case MediaBackendType::Dxgi:
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
	case MediaBackendType::Dxgi:
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
		// 显式请求 Qt6 屏幕采集后端。
		m_screenCapture = std::make_shared<fplayer::ScreenCaptureQt6>();
		break;
	case MediaBackendType::FFmpeg:
		// 显式请求 FFmpeg 屏幕采集后端（Windows 下为 gdigrab）。
		m_screenCapture = std::make_shared<fplayer::ScreenCaptureFFmpeg>();
		break;
	case MediaBackendType::Dxgi:
#if defined(_WIN32) && defined(FPLAYER_WITH_SCREEN_DXGI)
		// Windows + DXGI 模块可用：走 Desktop Duplication 后端。
		m_screenCapture = std::make_shared<fplayer::ScreenCaptureDxgi>();
#else
		// DXGI 不可用时安全回退到 FFmpeg，避免功能不可用。
		m_screenCapture = std::make_shared<fplayer::ScreenCaptureFFmpeg>();
#endif
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
#if defined(FPLAYER_WITH_NET_FFMPEG)
		m_stream = std::make_shared<fplayer::StreamFFmpeg>();
#endif
		break;
	case MediaBackendType::Dxgi:
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