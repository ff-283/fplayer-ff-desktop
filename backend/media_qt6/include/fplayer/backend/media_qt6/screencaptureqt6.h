#ifndef FPLAYER_DESKETOP_SCREENCAPTUREQT6_H
#define FPLAYER_DESKETOP_SCREENCAPTUREQT6_H

#include <QObject>
#include <QList>

#include <fplayer/backend/media_qt6/export.h>
#include <fplayer/api/media/iscreencapture.h>

class QMediaCaptureSession;
class QScreenCapture;

namespace fplayer
{
	class FPLAYER_BACKEND_MEDIA_QT6_EXPORT ScreenCaptureQt6 : public QObject, public IScreenCapture
	{
	public:
		ScreenCaptureQt6();
		~ScreenCaptureQt6() override = default;

		void refreshScreens() override;
		QList<ScreenDescription> getDescriptions() override;
		bool selectScreen(int index) override;
		int getIndex() const override;
		void setPreviewTarget(const PreviewTarget& target) override;
		void setActive(bool active) override;
		bool isActive() const override;
		bool setCursorCaptureEnabled(bool enabled) override;
		bool canControlCursorCapture() const override;

	private:
		QMediaCaptureSession* m_session = nullptr;
		QScreenCapture* m_screenCapture = nullptr;
		QList<ScreenDescription> m_descriptions;
	};
}

#endif //FPLAYER_DESKETOP_SCREENCAPTUREQT6_H
