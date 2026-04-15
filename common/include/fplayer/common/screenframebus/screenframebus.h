#ifndef FPLAYER_DESKTOP_SCREENFRAMEBUS_H
#define FPLAYER_DESKTOP_SCREENFRAMEBUS_H

#include <atomic>
#include <QByteArray>
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
		             int yStride, int uStride, int vStride);

		ScreenFrame snapshot() const;
		bool snapshotIfNew(quint64 lastSerial, ScreenFrame& outFrame) const;
		void setPublishTargetSize(int width, int height);
		void publishTargetSize(int& width, int& height) const;

	private:
		ScreenFrameBus() = default;

	private:
		mutable QMutex m_mutex;
		ScreenFrame m_frame;
		quint64 m_serial = 0;
		std::atomic<quint64> m_latestSerial{0};
		int m_targetWidth = 0;
		int m_targetHeight = 0;
	};
}

#endif // FPLAYER_DESKTOP_SCREENFRAMEBUS_H
