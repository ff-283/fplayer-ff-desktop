#include <fplayer/api/media/iplayer.h>

fplayer::IPlayer::~IPlayer() = default;

bool fplayer::IPlayer::isPlaying() const
{
	return m_isPlaying;
}

fplayer::MediaBackendType fplayer::IPlayer::getBackendType() const
{
	return m_backend;
}
