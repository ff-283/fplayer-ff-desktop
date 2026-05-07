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
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QList>
#include <QSize>
#include <fplayer/widget/export.h>
#include <fplayer/api/media/mediabackendtype.h>


namespace fplayer
{
	enum class MediaBackendType;
	class Service;
}

class QCameraDevice;
class QCamera;
class QSlider;
class QLabel;
class QTimer;
class QComboBox;
class QMenuBar;
class QToolButton;
class QResizeEvent;
class QCloseEvent;
class QSplitter;
class QMdiArea;
class QWidget;
class QListWidget;
class QPushButton;
class QDialog;
class QTextEdit;
class QSlider;
class QTcpServer;
class QMdiSubWindow;
class QSystemTrayIcon;
class QPoint;
class QRect;
class ImagePoolSidebar;
namespace fplayer
{
	class FVideoView;
}
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
	enum class CaptureMode
	{
		Camera,
		File,
		Screen,
	};
	void closeEvent(QCloseEvent* event) override;
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
	void refreshCameraDeviceUi();
	void refreshScreenDeviceUi();
	void refreshScreenFpsUi(int screenIndex);
	bool selectScreen(int index);
	void stopScreenCapture();
	int preferredFpsForScreen(int screenIndex) const;
	void updateCaptureCursorCheckToolTip();
	void ensureComposeWorkspace();
	/** 离开组合模式时暂停/关闭所有素材采集与播放，保留列表与子窗口以便再次进入时恢复布局（冻结）。 */
	void suspendComposeSourcesForBackground();
	void setComposeMode(bool enabled);
	void clearComposeSources();
	void addComposeFileSource();
	void addComposeCameraSource();
	void addComposeScreenSource();
	void removeComposeSourceAt(int index);
	void refreshComposeSourceListSelection();
	void refreshComposeSourceListItems();
	void updateComposeSelectionHighlight();
	void bringComposeSourceToFront(int index);
	void sendComposeSourceToBack(int index);
	void moveComposeSourceUp(int index);
	void moveComposeSourceDown(int index);
	void setComposeCropMode(int index, bool enabled);
	void requestComposeSourceContextMenu(const QPoint& globalPos, int index);
	void syncComposeControlPanel();
	/** 根据当前选中的组合素材行，刷新屏幕采集激活状态（仅允许一路 DXGI 等采集实例处于激活）。 */
	void refreshComposeScreenCaptureState(int selectedComposeIndex, int preferScreenRow = -1, int excludeScreenRow = -1);
	bool composeSourceIsPlaying(int index) const;
	void updateComposePlaybackIcons();
	void toggleComposeSourcePlayPauseAt(int index);
	void applyComposeZOrder();
	void applyComposeAspectRatio();
	void resizeWindowForComposeAspect();
	void remapComposeSourcesToViewport(const QRect& oldBounds, const QRect& newBounds);
	void forceRefreshComposePreview();
	bool buildComposeScreenCaptureSpec(QString& spec, int fps, int outW, int outH, int bitrateKbps, const QString& encoder,
	                                   const QString& audioIn, const QString& audioOut) const;
	void refreshComposeOutputSizeOptions();
	void loadCapturePreferences();
	void saveCapturePreferences() const;
	void applyTheme();
	void refreshThemeIcons();
	void openCaptureSettingsDialog(QWidget* parent = nullptr);
	QString makeScreenshotFilePath(const QString& prefix = QStringLiteral("capture")) const;
	QString makeRecordingFilePath(const QString& prefix = QStringLiteral("record")) const;
	QSize preferredComposeExportSize() const;
	QImage buildComposeSnapshotImage(const QSize& outSize) const;
	void updateRecordButtonUi();
	void updatePullRecordButtonUi();
	void handleMainCaptureScreenshot();
	void handleMainCaptureRecordToggle();
	void handleMainCaptureSettings();
	void setupTrayIcon();
	void showFromTray();
	void quitFromTray();
	bool m_isFileMode = false;
	bool m_isComposeMode = false;
	CaptureMode m_captureMode = CaptureMode::Camera;
	fplayer::MediaBackendType m_cameraBackendType = fplayer::MediaBackendType::Qt6;
	QMenuBar* m_modeMenuBar = nullptr;
	QToolButton* m_fileTitleButton = nullptr;
	QTimer* m_titleMarqueeTimer = nullptr;
	QString m_currentFileTitle;
	QString m_currentFilePath;
	int m_titleMarqueeOffset = 0;
	QSlider* m_fileProgress = nullptr;
	QLabel* m_fileProgressLabel = nullptr;
	QLabel* m_debugStatsLabel = nullptr;
	QComboBox* m_speedCombo = nullptr;
	QTimer* m_fileProgressTimer = nullptr;
	QTimer* m_debugStatsTimer = nullptr;
	bool m_progressDragging = false;
	int m_lastScreenIndex = 0;
	QHash<int, int> m_screenFpsOverrides;
	QSet<int> m_hdrPromptedScreenIndexes;
	QStringList m_recentPushInputs;
	QStringList m_recentPushOutputs;
	QStringList m_recentPullInputs;
	QStringList m_recentPullOutputs;
	int m_pullReservedPort = 1935;
	QDialog* m_pullMonitorDialog = nullptr;
	QDialog* m_pullPreviewDialog = nullptr;
	fplayer::FVideoView* m_pullPreviewView = nullptr;
	QPushButton* m_pullStartButton = nullptr;
	QPushButton* m_pullStopButton = nullptr;
	QTextEdit* m_pullLogView = nullptr;
	QSlider* m_pullVolumeSlider = nullptr;
	QPushButton* m_pullPreviewShotButton = nullptr;
	QPushButton* m_pullPreviewRecordButton = nullptr;
	QPushButton* m_pullPreviewSettingsButton = nullptr;
	QPushButton* m_pullPreviewPauseButton = nullptr;
	QPushButton* m_pullPreviewRefreshButton = nullptr;
	QPushButton* m_pullPreviewFullscreenButton = nullptr;
	QPushButton* m_pullPreviewImagePoolButton = nullptr;
	QLabel* m_pullPreviewRecordDurationLabel = nullptr;
	fplayer::Service* m_pullRecordService = nullptr;
	fplayer::MediaBackendType m_screenBackendType = fplayer::MediaBackendType::Qt6;
	fplayer::MediaBackendType m_filePlaybackBackend = fplayer::MediaBackendType::Qt6;
	QString m_capturePrefPath;
	QString m_screenshotSaveDir;
	QString m_recordSaveDir;
	QString m_pushGateway;
	QString m_pushServiceApp;
	QString m_pushServiceStream;
	QString m_pullGateway;
	QString m_pullServiceApp;
	QString m_pullServiceStream;
	QString m_pushRouteMode;
	QString m_pushServiceMode;
	QString m_pushProtocolTemplate;
	int m_pushFps = 0;
	QString m_pushSize;
	int m_pushBitrateKbps = 0;
	QString m_pushEncoder;
	QString m_pushAudioInput;
	QString m_pushAudioOutput;
	bool m_pushKeepAspect = true;
	bool m_closeToTrayOnClose = true;
	QString m_composeOutputSize;
	QString m_aiEndpoint;
	QString m_aiApiKey;
	QString m_aiModel;
	QString m_aiUserBubbleColor = QStringLiteral("#2997ff");
	QString m_aiAiBubbleColor = QStringLiteral("#1c1c1e");
	QString m_aiChatBgColor = QStringLiteral("#0d0d0f");
	QString m_aiFontFamily;
	int m_aiFontSize = 14;
	QString m_aiTextColor = QStringLiteral("#f5f5f7");
	QString m_userTextColor = QStringLiteral("#ffffff");
	QString m_aiSystemBubbleColor = QStringLiteral("#1a1a1c");
	QString m_aiSystemTextColor = QStringLiteral("#a1a1a6");
	QString m_aiSenderColor = QStringLiteral("#6e6e73");
	QString m_aiSystemSenderColor = QStringLiteral("#ff9f0a");
	int m_theme = 0;  // 0=dark, 1=light
	QString m_accentColor = QStringLiteral("#2997ff");  // 主题色
	bool m_mainRecording = false;
	bool m_pullRecording = false;
	bool m_pullRecordingViaMainService = false;
	QString m_pullCurrentInputUrl;
	QString m_pullRecordInputUrl;
	QString m_pullRecordOutputPath;
	QTimer* m_mainRecordTimer = nullptr;
	QTimer* m_pullRecordTimer = nullptr;
	qint64 m_mainRecordStartMs = 0;
	qint64 m_pullRecordStartMs = 0;
	qint64 m_pullSessionStartMs = 0;
	QSplitter* m_composeSplitter = nullptr;
	QMdiArea* m_composeMdiArea = nullptr;
	QWidget* m_composePreviewHost = nullptr;
	QListWidget* m_composeSourceList = nullptr;
	QComboBox* m_composeAspectCombo = nullptr;
	QComboBox* m_composeSizeCombo = nullptr;
	QPushButton* m_btnComposeAddFile = nullptr;
	QPushButton* m_btnComposeAddCamera = nullptr;
	QPushButton* m_btnComposeAddScreen = nullptr;
	int m_composeAspectW = 16;
	int m_composeAspectH = 9;
	bool m_adjustingComposeWindowSize = false;
	QTimer* m_composeZOrderGuardTimer = nullptr;
	struct ComposeSourceItem
	{
		enum class SourceKind
		{
			File,
			Camera,
			Screen
		};
		SourceKind kind = SourceKind::File;
		fplayer::Service* service = nullptr;
		QWidget* container = nullptr;
		fplayer::FVideoView* view = nullptr;
		QMdiSubWindow* subWindow = nullptr;
		QString title;
		QString sourceId;
		bool cropMode = false;
		bool keepAspectResize = false;
		int deviceIndex = 0;
		int formatIndex = 0;
		int screenFps = 30;
		bool screenCaptureCursor = false;
	};
	QList<ComposeSourceItem> m_composeSources;
	int m_composeSelectedIndex = -1;
	QSystemTrayIcon* m_trayIcon = nullptr;
	QMenu* m_trayMenu = nullptr;
	QAction* m_trayShowAction = nullptr;
	QAction* m_trayQuitAction = nullptr;
	bool m_quitFromTray = false;
	// Image pool
	QPushButton* m_btnImagePool = nullptr;
	ImagePoolSidebar* m_imagePoolSidebar = nullptr;

signals:
	void screenshotSaved(const QString& filePath);
};


#endif //FPLAYER_DESKETOP_CAPTUREWINDOW_H