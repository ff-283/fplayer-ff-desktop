#ifndef FPLAYER_DESKTOP_CAMERAFRAMEBUS_H
#define FPLAYER_DESKTOP_CAMERAFRAMEBUS_H

#include <QByteArray>
#include <QMutex>
#include <fplayer/common/export.h>

namespace fplayer
{
	struct FPLAYER_COMMON_EXPORT CameraFrame
	{
		QByteArray y;
		QByteArray u;
		QByteArray v;
		int width = 0;
		int height = 0;
		int yStride = 0;
		int uStride = 0;
		int vStride = 0;
		quint64 serial = 0;
		bool valid = false;
	};

	class FPLAYER_COMMON_EXPORT CameraFrameBus
	{
	public:
		static CameraFrameBus& instance();

		void publish(const QByteArray& y, const QByteArray& u, const QByteArray& v, int width, int height,
		             int yStride, int uStride, int vStride);

		CameraFrame snapshot() const;

	private:
		CameraFrameBus() = default;

	private:
		mutable QMutex m_mutex;
		CameraFrame m_frame;
		quint64 m_serial = 0;
	};
}

#endif // FPLAYER_DESKTOP_CAMERAFRAMEBUS_H
