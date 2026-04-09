#include <fplayer/api/media/icamera.h>
fplayer::ICamera::~ICamera() = default;

bool fplayer::ICamera::isPlaying() const
{
	return m_isPlaying;
}

fplayer::MediaBackendType fplayer::ICamera::getBackendType() const
{
	return m_backend;
}