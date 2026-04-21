#include <fplayer/api/media/iscreencapture.h>

fplayer::IScreenCapture::~IScreenCapture() = default;

fplayer::MediaBackendType fplayer::IScreenCapture::backendType() const
{
	return m_backendType;
}

void fplayer::IScreenCapture::setFrameBusSourceId(const QString& sourceId)
{
	const QString v = sourceId.trimmed();
	m_frameBusSourceId = v.isEmpty() ? QStringLiteral("default") : v;
}

QString fplayer::IScreenCapture::frameBusSourceId() const
{
	return m_frameBusSourceId;
}
