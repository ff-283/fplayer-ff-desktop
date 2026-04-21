#ifndef FPLAYER_DESKTOP_SCREENFRAMEBUS_H
#define FPLAYER_DESKTOP_SCREENFRAMEBUS_H

#include <atomic>
#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <fplayer/common/export.h>

namespace fplayer
{
	struct FPLAYER_COMMON_EXPORT ScreenFrame
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

	class FPLAYER_COMMON_EXPORT ScreenFrameBus
	{
	public:
		static ScreenFrameBus& instance();

		void publish(const QByteArray& y, const QByteArray& u, const QByteArray& v, int width, int height,
		             int yStride, int uStride, int vStride, const QString& sourceId = QStringLiteral("default"));

		ScreenFrame snapshot(const QString& sourceId = QStringLiteral("default")) const;
		bool snapshotIfNew(quint64 lastSerial, ScreenFrame& outFrame, const QString& sourceId = QStringLiteral("default")) const;
		void setPublishTargetSize(int width, int height, const QString& sourceId = QStringLiteral("default"));
		void publishTargetSize(int& width, int& height, const QString& sourceId = QStringLiteral("default")) const;

	private:
		ScreenFrameBus() = default;

	private:
		mutable QMutex m_mutex;
		struct Channel
		{
			ScreenFrame frame;
			quint64 serial = 0;
			quint64 latestSerial = 0;
			int targetWidth = 0;
			int targetHeight = 0;
		};
		mutable QHash<QString, Channel> m_channels;
	};
}

#endif // FPLAYER_DESKTOP_SCREENFRAMEBUS_H
