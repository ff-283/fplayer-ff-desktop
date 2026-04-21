#ifndef FPLAYER_DESKTOP_CAMERAFRAMEBUS_H
#define FPLAYER_DESKTOP_CAMERAFRAMEBUS_H

#include <QByteArray>
#include <QHash>
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
		             int yStride, int uStride, int vStride, const QString& sourceId = QStringLiteral("default"));

		CameraFrame snapshot(const QString& sourceId = QStringLiteral("default")) const;

	private:
		CameraFrameBus() = default;

	private:
		mutable QMutex m_mutex;
		struct Channel
		{
			CameraFrame frame;
			quint64 serial = 0;
		};
		mutable QHash<QString, Channel> m_channels;
	};
}

#endif // FPLAYER_DESKTOP_CAMERAFRAMEBUS_H
