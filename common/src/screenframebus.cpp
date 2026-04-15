#include <fplayer/common/screenframebus/screenframebus.h>

#include <QMutexLocker>

fplayer::ScreenFrameBus& fplayer::ScreenFrameBus::instance()
{
	static ScreenFrameBus bus;
	return bus;
}

void fplayer::ScreenFrameBus::publish(const QByteArray& y, const QByteArray& u, const QByteArray& v, int width, int height,
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

fplayer::ScreenFrame fplayer::ScreenFrameBus::snapshot() const
{
	QMutexLocker locker(&m_mutex);
	return m_frame;
}

void fplayer::ScreenFrameBus::setPublishTargetSize(int width, int height)
{
	QMutexLocker locker(&m_mutex);
	m_targetWidth = width > 0 ? width : 0;
	m_targetHeight = height > 0 ? height : 0;
}

void fplayer::ScreenFrameBus::publishTargetSize(int& width, int& height) const
{
	QMutexLocker locker(&m_mutex);
	width = m_targetWidth;
	height = m_targetHeight;
}
