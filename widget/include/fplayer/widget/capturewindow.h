/*************************************************
  * 描述：
  *
  * File：capturewindow.h
  * Date：2026/2/19
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_CAPTUREWINDOW_H
#define FPLAYER_DESKETOP_CAPTUREWINDOW_H


#include <QWidget>
#include <fplayer/widget/export.h>
#include <fplayer/api/media/mediabackendtype.h>


namespace fplayer
{
	enum class MediaBackendType;
	class Service;
}

class QCameraDevice;
class QCamera;
class QMediaCaptureSession;
class QVideoWidget;
QT_BEGIN_NAMESPACE

namespace Ui
{
	class CaptureWindow;
}

QT_END_NAMESPACE

class FPLAYER_WIDGET_EXPORT CaptureWindow : public QWidget
{
	Q_OBJECT

public:
	explicit CaptureWindow(QWidget* parent = nullptr, fplayer::MediaBackendType backendType = fplayer::MediaBackendType::Qt6);

	~CaptureWindow() override;

	// private:
	// 	void refreshCameras();
	// 	bool selectCamera(int index);

private:
	Ui::CaptureWindow* ui;
	// QVideoWidget* m_view = nullptr;
	// QMediaCaptureSession* m_session = nullptr;
	// QCamera* m_camera = nullptr;
	// QList<QCameraDevice> m_devices;

	fplayer::Service* m_service = nullptr;
	void togglePlayPause();
};


#endif //FPLAYER_DESKETOP_CAPTUREWINDOW_H