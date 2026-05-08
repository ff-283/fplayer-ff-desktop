#include <fplayer/api/media/iplayer.h>

#include <QImage>

fplayer::IPlayer::~IPlayer() = default;

QImage fplayer::IPlayer::currentFrameImage() const
{
	return {};
}

bool fplayer::IPlayer::isPlaying() const
{
	return m_isPlaying;
}

fplayer::MediaBackendType fplayer::IPlayer::getBackendType() const
{
	return m_backend;
}
