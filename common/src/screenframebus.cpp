#include <fplayer/common/screenframebus/screenframebus.h>

#include <QMutexLocker>

fplayer::ScreenFrameBus& fplayer::ScreenFrameBus::instance()
{
	static ScreenFrameBus bus;
	return bus;
}

void fplayer::ScreenFrameBus::publish(const QByteArray& y, const QByteArray& u, const QByteArray& v, int width, int height,
                                      int yStride, int uStride, int vStride, const QString& sourceId)
{
	QMutexLocker locker(&m_mutex);
	const QString sid = sourceId.trimmed().isEmpty() ? QStringLiteral("default") : sourceId.trimmed();
	auto it = m_channels.find(sid);
	if (it == m_channels.end())
	{
		it = m_channels.insert(sid, Channel{});
	}
	Channel& ch = it.value();
	ch.frame.y = y;
	ch.frame.u = u;
	ch.frame.v = v;
	ch.frame.width = width;
	ch.frame.height = height;
	ch.frame.yStride = yStride;
	ch.frame.uStride = uStride;
	ch.frame.vStride = vStride;
	ch.frame.serial = ++ch.serial;
	ch.frame.valid = true;
	ch.latestSerial = ch.frame.serial;
}

fplayer::ScreenFrame fplayer::ScreenFrameBus::snapshot(const QString& sourceId) const
{
	QMutexLocker locker(&m_mutex);
	const QString sid = sourceId.trimmed().isEmpty() ? QStringLiteral("default") : sourceId.trimmed();
	const auto it = m_channels.constFind(sid);
	return (it == m_channels.constEnd()) ? ScreenFrame{} : it.value().frame;
}

bool fplayer::ScreenFrameBus::snapshotIfNew(quint64 lastSerial, ScreenFrame& outFrame, const QString& sourceId) const
{
	QMutexLocker locker(&m_mutex);
	const QString sid = sourceId.trimmed().isEmpty() ? QStringLiteral("default") : sourceId.trimmed();
	const auto it = m_channels.constFind(sid);
	if (it == m_channels.constEnd())
	{
		return false;
	}
	const Channel& ch = it.value();
	const quint64 latest = ch.latestSerial;
	if (latest <= lastSerial)
	{
		return false;
	}
	if (!ch.frame.valid || ch.frame.serial == lastSerial)
	{
		return false;
	}
	outFrame = ch.frame;
	return true;
}

void fplayer::ScreenFrameBus::setPublishTargetSize(int width, int height, const QString& sourceId)
{
	QMutexLocker locker(&m_mutex);
	const QString sid = sourceId.trimmed().isEmpty() ? QStringLiteral("default") : sourceId.trimmed();
	auto it = m_channels.find(sid);
	if (it == m_channels.end())
	{
		it = m_channels.insert(sid, Channel{});
	}
	Channel& ch = it.value();
	ch.targetWidth = width > 0 ? width : 0;
	ch.targetHeight = height > 0 ? height : 0;
}

void fplayer::ScreenFrameBus::publishTargetSize(int& width, int& height, const QString& sourceId) const
{
	QMutexLocker locker(&m_mutex);
	const QString sid = sourceId.trimmed().isEmpty() ? QStringLiteral("default") : sourceId.trimmed();
	const auto it = m_channels.constFind(sid);
	if (it == m_channels.constEnd())
	{
		width = 0;
		height = 0;
		return;
	}
	width = it.value().targetWidth;
	height = it.value().targetHeight;
}
