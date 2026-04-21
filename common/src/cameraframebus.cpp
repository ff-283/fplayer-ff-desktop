#include <fplayer/common/cameraframebus/cameraframebus.h>

#include <QMutexLocker>

fplayer::CameraFrameBus& fplayer::CameraFrameBus::instance()
{
	static CameraFrameBus bus;
	return bus;
}

void fplayer::CameraFrameBus::publish(const QByteArray& y, const QByteArray& u, const QByteArray& v, int width, int height,
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
	ch.frame.y = QByteArray(y.constData(), y.size());
	ch.frame.u = QByteArray(u.constData(), u.size());
	ch.frame.v = QByteArray(v.constData(), v.size());
	ch.frame.width = width;
	ch.frame.height = height;
	ch.frame.yStride = yStride;
	ch.frame.uStride = uStride;
	ch.frame.vStride = vStride;
	ch.frame.serial = ++ch.serial;
	ch.frame.valid = true;
}

fplayer::CameraFrame fplayer::CameraFrameBus::snapshot(const QString& sourceId) const
{
	QMutexLocker locker(&m_mutex);
	const QString sid = sourceId.trimmed().isEmpty() ? QStringLiteral("default") : sourceId.trimmed();
	const auto it = m_channels.constFind(sid);
	return (it == m_channels.constEnd()) ? CameraFrame{} : it.value().frame;
}
