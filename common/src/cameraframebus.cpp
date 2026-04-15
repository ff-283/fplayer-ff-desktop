#include <fplayer/common/cameraframebus/cameraframebus.h>

#include <QMutexLocker>

fplayer::CameraFrameBus& fplayer::CameraFrameBus::instance()
{
	static CameraFrameBus bus;
	return bus;
}

void fplayer::CameraFrameBus::publish(const QByteArray& y, const QByteArray& u, const QByteArray& v, int width, int height,
                                      int yStride, int uStride, int vStride)
{
	QMutexLocker locker(&m_mutex);
	m_frame.y = y;
	m_frame.u = u;
	m_frame.v = v;
	m_frame.width = width;
	m_frame.height = height;
	m_frame.yStride = yStride;
	m_frame.uStride = uStride;
	m_frame.vStride = vStride;
	m_frame.serial = ++m_serial;
	m_frame.valid = true;
}

fplayer::CameraFrame fplayer::CameraFrameBus::snapshot() const
{
	QMutexLocker locker(&m_mutex);
	return m_frame;
}
