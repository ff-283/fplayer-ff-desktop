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
class QSlider;
class QLabel;
class QTimer;
class QComboBox;
class QMenuBar;
class QToolButton;
class QResizeEvent;
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
	void resizeEvent(QResizeEvent* event) override;
	Ui::CaptureWindow* ui;
	// QVideoWidget* m_view = nullptr;
	// QMediaCaptureSession* m_session = nullptr;
	// QCamera* m_camera = nullptr;
	// QList<QCameraDevice> m_devices;

	fplayer::Service* m_service = nullptr;
	void togglePlayPause();
	bool chooseAndPlayFile();
	void updateFileProgressUi();
	QString formatTimeMs(qint64 ms) const;
	void updateDebugStatsUi();
	void updateTitleMarqueeText();
	void relocateTitleWidget();
	bool m_isFileMode = false;
	fplayer::MediaBackendType m_cameraBackendType = fplayer::MediaBackendType::Qt6;
	QMenuBar* m_modeMenuBar = nullptr;
	QToolButton* m_fileTitleButton = nullptr;
	QTimer* m_titleMarqueeTimer = nullptr;
	QString m_currentFileTitle;
	int m_titleMarqueeOffset = 0;
	QSlider* m_fileProgress = nullptr;
	QLabel* m_fileProgressLabel = nullptr;
	QLabel* m_debugStatsLabel = nullptr;
	QComboBox* m_speedCombo = nullptr;
	QTimer* m_fileProgressTimer = nullptr;
	QTimer* m_debugStatsTimer = nullptr;
	bool m_progressDragging = false;
};


#endif //FPLAYER_DESKETOP_CAPTUREWINDOW_H