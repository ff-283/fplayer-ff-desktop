/*************************************************
  * 描述：屏幕采集抽象
  *
  * File：iscreencapture.h
  * ************************************************/
#ifndef FPLAYER_DESKETOP_ISCREENCAPTURE_H
#define FPLAYER_DESKETOP_ISCREENCAPTURE_H

#include <QList>
#include <QString>
#include <fplayer/api/export.h>
#include <fplayer/api/media/mediabackendtype.h>
#include <fplayer/api/media/previewtarget.h>

namespace fplayer
{
	struct ScreenDescription
	{
		QString name;
		bool isPrimary = false;
		int x = 0;
		int y = 0;
		int width = 0;
		int height = 0;
	};

	class FPLAYER_API_EXPORT IScreenCapture
	{
	public:
		virtual ~IScreenCapture();

		virtual void refreshScreens() = 0;
		virtual QList<ScreenDescription> getDescriptions() = 0;
		virtual bool selectScreen(int index) = 0;
		virtual int getIndex() const = 0;

		virtual void setPreviewTarget(const PreviewTarget& target) = 0;
		virtual void setActive(bool active) = 0;
		virtual bool isActive() const = 0;

		virtual bool setCursorCaptureEnabled(bool enabled) = 0;
		virtual bool canControlCursorCapture() const = 0;
		virtual bool setFrameRate(int fps) = 0;
		virtual int frameRate() const = 0;
		virtual bool canControlFrameRate() const = 0;
		virtual void setFrameBusSourceId(const QString& sourceId);
		virtual QString frameBusSourceId() const;

		MediaBackendType backendType() const;

	protected:
		MediaBackendType m_backendType = MediaBackendType::Qt6;
		int m_screenIndex = 0;
		int m_fps = 30;
		QString m_frameBusSourceId = QStringLiteral("default");
	};
}

#endif //FPLAYER_DESKETOP_ISCREENCAPTURE_H
