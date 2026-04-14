#include <fplayer/api/media/iscreencapture.h>

fplayer::IScreenCapture::~IScreenCapture() = default;

fplayer::MediaBackendType fplayer::IScreenCapture::backendType() const
{
	return m_backendType;
}
