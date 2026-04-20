#include "ui_CaptureWindow.h"

#include <fplayer/widget/capturewindow.h>
#include <fplayer/service/service.h>
#include <fplayer/widget/fvideoview.h>

#include <QVBoxLayout>
#include <QCamera>
#include <QMediaDevices>
#include <QAudioDevice>
#include <logger/logger.h>
#include <QDebug>
#include <qicon.h>
#include <QApplication>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QAbstractSpinBox>
#include <QShortcut>
#include <QFileDialog>
#include <QMenuBar>
#include <QMenu>
#include <QActionGroup>
#include <QSlider>
#include <QLabel>
#include <QTimer>
#include <QComboBox>
#include <QCheckBox>
#include <QToolButton>
#include <QFileInfo>
#include <QResizeEvent>
#include <QFontMetrics>
#include <QAbstractItemView>
#include <QGuiApplication>
#include <QScreen>
#include <QSet>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QPushButton>
#include <QMessageBox>
#include <QTextEdit>
#include <QSpinBox>
#include <QRegularExpression>
#include <QStandardItemModel>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#endif

namespace
{
const char* screenBackendName(const fplayer::MediaBackendType backend)
{
	switch (backend)
	{
	case fplayer::MediaBackendType::Qt6:
		return "Qt6";
	case fplayer::MediaBackendType::FFmpeg:
		return "FFmpeg(gdigrab)";
	case fplayer::MediaBackendType::Dxgi:
		return "DXGI";
	default:
		return "Unknown";
	}
}

#if defined(_WIN32)
template<typename T>
void safeDxgiRelease(T*& p)
{
	if (p)
	{
		p->Release();
		p = nullptr;
	}
}

bool isHdrEnabledForScreenIndex(const int screenIndex)
{
	const auto screens = QGuiApplication::screens();
	if (screenIndex < 0 || screenIndex >= screens.size() || !screens.at(screenIndex))
	{
		return false;
	}
	const auto* targetScreen = screens.at(screenIndex);
	const QRect logical = targetScreen->geometry();
	const qreal dpr = targetScreen->devicePixelRatio();
	const QRect expected(
		qRound(logical.x() * dpr),
		qRound(logical.y() * dpr),
		qRound(logical.width() * dpr),
		qRound(logical.height() * dpr));
	constexpr int kTolerance = 2;

	IDXGIFactory1* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
	{
		return false;
	}
	bool hdrEnabled = false;
	for (UINT ai = 0; !hdrEnabled; ++ai)
	{
		IDXGIAdapter1* adapter = nullptr;
		if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND)
		{
			break;
		}
		for (UINT oi = 0; !hdrEnabled; ++oi)
		{
			IDXGIOutput* output = nullptr;
			if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND)
			{
				break;
			}
			DXGI_OUTPUT_DESC od{};
			if (FAILED(output->GetDesc(&od)))
			{
				safeDxgiRelease(output);
				continue;
			}
			const QRect outRect(
				od.DesktopCoordinates.left,
				od.DesktopCoordinates.top,
				od.DesktopCoordinates.right - od.DesktopCoordinates.left,
				od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
			const bool match =
				(qAbs(outRect.x() - expected.x()) <= kTolerance) &&
				(qAbs(outRect.y() - expected.y()) <= kTolerance) &&
				(qAbs(outRect.width() - expected.width()) <= kTolerance) &&
				(qAbs(outRect.height() - expected.height()) <= kTolerance);
			if (!match)
			{
				safeDxgiRelease(output);
				continue;
			}
			IDXGIOutput6* output6 = nullptr;
			if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6))) && output6)
			{
				DXGI_OUTPUT_DESC1 od1{};
				if (SUCCEEDED(output6->GetDesc1(&od1)))
				{
					const DXGI_COLOR_SPACE_TYPE cs = od1.ColorSpace;
					const bool hdrByColorSpace =
						(cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) ||
						(cs == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020) ||
						(cs == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
					const bool hdrByBitDepth = od1.BitsPerColor > 8;
					hdrEnabled = hdrByColorSpace || hdrByBitDepth;
				}
			}
			safeDxgiRelease(output6);
			safeDxgiRelease(output);
		}
		safeDxgiRelease(adapter);
	}
	safeDxgiRelease(factory);
	return hdrEnabled;
}
#endif
}

CaptureWindow::CaptureWindow(QWidget* parent, fplayer::MediaBackendType backendType) :
	QWidget(parent),
	ui(new Ui::CaptureWindow),
	m_cameraBackendType(backendType)
{
	ui->setupUi(this);
	m_service = new fplayer::Service();
	m_modeMenuBar = new QMenuBar(this);
	ui->verticalLayout->setMenuBar(m_modeMenuBar);
	auto* modeMenu = m_modeMenuBar->addMenu(tr("模式"));
	auto* streamMenu = m_modeMenuBar->addMenu(tr("推拉流"));
	auto* actionGroup = new QActionGroup(this);
	actionGroup->setExclusive(true);
	auto* actionCameraMode = modeMenu->addAction(tr("摄像头模式"));
	actionCameraMode->setCheckable(true);
	auto* actionFileMode = modeMenu->addAction(tr("文件播放模式"));
	actionFileMode->setCheckable(true);
	auto* actionScreenMode = modeMenu->addAction(tr("屏幕捕获模式"));
	actionScreenMode->setCheckable(true);
	actionGroup->addAction(actionCameraMode);
	actionGroup->addAction(actionFileMode);
	actionGroup->addAction(actionScreenMode);
	auto* actionPushStream = streamMenu->addAction(tr("推流"));
	auto* actionPullStream = streamMenu->addAction(tr("拉流"));
	actionCameraMode->setChecked(true);

	m_fileTitleButton = new QToolButton(m_modeMenuBar);
	m_fileTitleButton->setText(tr("点击选择文件"));
	m_fileTitleButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
	m_fileTitleButton->setAutoRaise(true);
	m_fileTitleButton->setCursor(Qt::PointingHandCursor);
	m_fileTitleButton->setFixedWidth(420);
	m_fileTitleButton->setFixedHeight(24);
	m_fileTitleButton->show();
	m_fileTitleButton->raise();

	m_titleMarqueeTimer = new QTimer(this);
	m_titleMarqueeTimer->setInterval(180);
	connect(m_titleMarqueeTimer, &QTimer::timeout, this, &CaptureWindow::updateTitleMarqueeText);
	// 防止按钮点击后持有键盘焦点，导致空格再次触发同一按钮 click。
	this->ui->btnPlay->setFocusPolicy(Qt::NoFocus);
	this->ui->btnCut->setFocusPolicy(Qt::NoFocus);
	this->ui->btnCast->setFocusPolicy(Qt::NoFocus);
	this->ui->btnSettings->setFocusPolicy(Qt::NoFocus);
	this->ui->btnFullscreen->setFocusPolicy(Qt::NoFocus);
	this->ui->chkCaptureCursor->setVisible(false);
	this->ui->cmbScreenFps->setVisible(false);

	m_fileProgress = new QSlider(Qt::Horizontal, this);
	m_fileProgress->setMinimum(0);
	m_fileProgress->setMaximum(0);
	m_fileProgress->setVisible(false);
	m_fileProgress->setMinimumWidth(320);
	m_fileProgress->setMaximumWidth(16777215);
	m_fileProgress->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	m_fileProgressLabel = new QLabel(tr("00:00 / 00:00"), this);
	m_fileProgressLabel->setVisible(false);
	m_fileProgressLabel->setFixedWidth(120);
	m_debugStatsLabel = new QLabel(tr("VQ:0 AQ:0 DropV/2s:0 AudFrm/2s:0 AudKB/2s:0"), this);
	m_debugStatsLabel->setVisible(false);
	m_debugStatsLabel->setFixedWidth(300);
	m_debugStatsLabel->setTextFormat(Qt::PlainText);
	m_speedCombo = new QComboBox(this);
	m_speedCombo->addItem(tr("1倍速"), 1.0);
	m_speedCombo->addItem(tr("1.25倍速"), 1.25);
	m_speedCombo->addItem(tr("1.5倍速"), 1.5);
	m_speedCombo->addItem(tr("2倍速"), 2.0);
	m_speedCombo->setCurrentIndex(0);
	m_speedCombo->setVisible(false);
	ui->horizontalLayout->insertWidget(1, m_fileProgress, 1);
	ui->horizontalLayout->insertWidget(2, m_fileProgressLabel, 0);
	ui->horizontalLayout->insertWidget(3, m_speedCombo, 0);
	ui->horizontalLayout->insertWidget(4, m_debugStatsLabel, 0);
	m_fileProgressTimer = new QTimer(this);
	m_fileProgressTimer->setInterval(200);
	connect(m_fileProgressTimer, &QTimer::timeout, this, &CaptureWindow::updateFileProgressUi);
	connect(m_fileProgress, &QSlider::sliderPressed, this, [this]() { m_progressDragging = true; });
	connect(m_fileProgress, &QSlider::sliderReleased, this, [this]() {
		m_progressDragging = false;
		this->m_service->playerSeekMs(m_fileProgress->value());
		this->updateFileProgressUi();
	});
	connect(m_speedCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (index < 0)
		{
			return;
		}
		const double rate = m_speedCombo->itemData(index).toDouble();
		this->m_service->playerSetPlaybackRate(rate);
	});
	m_debugStatsTimer = new QTimer(this);
	m_debugStatsTimer->setInterval(500);
	connect(m_debugStatsTimer, &QTimer::timeout, this, &CaptureWindow::updateDebugStatsUi);
	auto refreshFullscreenButton = [this]() {
		if (this->isFullScreen())
		{
			this->ui->btnFullscreen->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::ViewRestore));
		}
		else
		{
			this->ui->btnFullscreen->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::ViewFullscreen));
		}
	};

	// 1) 初始化摄像头
	m_service->initCamera(backendType);
	// 文件播放模块当前仅实现 FFmpeg 后端，固定用 FFmpeg 初始化播放器。
	m_service->initPlayer(fplayer::MediaBackendType::FFmpeg);
	// 屏幕捕获后端选择：
	// - Windows 且构建启用了 DXGI 模块时：使用 DXGI（Desktop Duplication）
	// - 其他情况：回退到 FFmpeg（gdigrab）路径
#if defined(_WIN32) && defined(FPLAYER_WITH_SCREEN_DXGI)
	m_screenBackendType = fplayer::MediaBackendType::Dxgi;
#else
	m_screenBackendType = fplayer::MediaBackendType::FFmpeg;
#endif
	m_service->initScreenCapture(m_screenBackendType);
	m_service->initStream(fplayer::MediaBackendType::FFmpeg);
	this->ui->wgtView->setBackendType(backendType);

	// 2) 绑定预览窗口
	m_service->bindCameraPreview(this->ui->wgtView);
	m_service->bindPlayerPreview(this->ui->wgtView);
	m_service->bindScreenPreview(this->ui->wgtView);

	// 3) 获取摄像头列表
	this->refreshCameraDeviceUi();

	// 4) 连接信号槽
	// 摄像头变更
	connect(this->ui->cmbDevices, &QComboBox::currentIndexChanged, [this](int index) {
		if (index < 0)
		{
			return;
		}
		if (m_captureMode == CaptureMode::Screen)
		{
			this->selectScreen(index);
			return;
		}
		this->m_service->selectCamera(index);
		QStringList formats(this->m_service->getCameraFormats(index));
		this->ui->cmbFormats->clear();
		this->ui->cmbFormats->addItems(formats);
		this->ui->cmbFormats->setCurrentIndex(0);

	});
	connect(this->ui->chkCaptureCursor, &QCheckBox::toggled, this, [this](const bool checked) {
		if (m_captureMode != CaptureMode::Screen)
		{
			return;
		}
		if (!m_service->screenSetCursorCaptureEnabled(checked))
		{
			this->ui->chkCaptureCursor->setToolTip(tr("当前屏幕采集后端不支持切换鼠标指针捕获。"));
		}
		else
		{
			this->updateCaptureCursorCheckToolTip();
		}
	});
	connect(this->ui->cmbScreenFps, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (m_captureMode != CaptureMode::Screen || index < 0)
		{
			return;
		}
		const int fps = this->ui->cmbScreenFps->itemData(index).toInt();
		const int currentScreenIndex = this->ui->cmbDevices->currentIndex();
		if (currentScreenIndex >= 0)
		{
			m_screenFpsOverrides[currentScreenIndex] = fps;
		}
		if (!m_service->screenSetFrameRate(fps))
		{
			this->ui->cmbScreenFps->setToolTip(tr("当前屏幕采集后端不支持动态帧率设置。"));
		}
		else
		{
			this->ui->cmbScreenFps->setToolTip(QString());
		}
	});

	// 摄像头格式变更
	connect(this->ui->cmbFormats, &QComboBox::currentIndexChanged, [this](int index) {
		if (m_captureMode != CaptureMode::Camera)
		{
			return;
		}
		if (index < 0)
		{
			return;
		}

		this->m_service->selectCameraFormat(index);

	});


	// 5) 选择第一个摄像头（此时预览已经设置好了）
	if (this->ui->cmbDevices->count() > 0)
	{
		this->ui->cmbDevices->setCurrentIndex(0);
		this->m_service->selectCamera(0);

		QStringList formats(this->m_service->getCameraFormats(0));
		this->ui->cmbFormats->addItems(formats);
		this->ui->cmbFormats->setCurrentIndex(0);
		this->m_service->selectCameraFormat(0);
	}

	connect(this->ui->btnPlay, &QPushButton::clicked, [this]() {
		this->togglePlayPause();
	});

	auto switchToCameraMode = [this]() {
		m_isFileMode = false;
		m_captureMode = CaptureMode::Camera;
		stopScreenCapture();
		this->ui->wgtDevices->setVisible(true);
		this->ui->cmbFormats->setVisible(true);
		this->ui->chkCaptureCursor->setVisible(false);
		this->ui->cmbScreenFps->setVisible(false);
		this->m_fileProgress->setVisible(false);
		this->m_fileProgressLabel->setVisible(false);
		this->m_speedCombo->setVisible(false);
		this->m_debugStatsLabel->setVisible(false);
		this->m_fileProgressTimer->stop();
		this->m_debugStatsTimer->stop();
		this->m_service->playerPause();
		this->m_service->playerSetPlaybackRate(1.0);
		this->m_speedCombo->setCurrentIndex(0);
		this->ui->wgtView->setBackendType(m_cameraBackendType);
		this->m_service->bindCameraPreview(this->ui->wgtView);
		this->refreshCameraDeviceUi();
		if (this->ui->cmbDevices->count() > 0)
		{
			this->ui->cmbDevices->setCurrentIndex(0);
		}
		this->ui->btnPlay->setIcon(QIcon::fromTheme(
			this->m_service->cameraIsPlaying() ? QIcon::ThemeIcon::MediaPlaybackPause : QIcon::ThemeIcon::MediaPlaybackStart));
	};
	auto switchToFileMode = [this]() -> bool {
		m_captureMode = CaptureMode::File;
		stopScreenCapture();
		this->ui->wgtView->setBackendType(fplayer::MediaBackendType::FFmpeg);
		this->m_service->bindPlayerPreview(this->ui->wgtView);
		if (!this->chooseAndPlayFile())
		{
			this->ui->wgtView->setBackendType(m_cameraBackendType);
			this->m_service->bindCameraPreview(this->ui->wgtView);
			return false;
		}
		m_isFileMode = true;
		this->ui->wgtDevices->setVisible(false);
		this->m_fileProgress->setVisible(true);
		this->m_fileProgressLabel->setVisible(true);
		this->m_speedCombo->setVisible(true);
		this->m_debugStatsLabel->setVisible(true);
		this->updateFileProgressUi();
		this->updateDebugStatsUi();
		this->m_fileProgressTimer->start();
		this->m_debugStatsTimer->start();
		this->ui->btnPlay->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::MediaPlaybackPause));
		return true;
	};
	auto switchToScreenMode = [this]() -> bool {
		m_isFileMode = false;
		m_captureMode = CaptureMode::Screen;
		LOG_INFO("[screen]", "switch to screen mode, backend=", screenBackendName(m_screenBackendType));
		this->m_service->playerPause();
		this->m_service->cameraPause();
		this->m_fileProgress->setVisible(false);
		this->m_fileProgressLabel->setVisible(false);
		this->m_speedCombo->setVisible(false);
		this->m_debugStatsLabel->setVisible(false);
		this->m_fileProgressTimer->stop();
		this->m_debugStatsTimer->stop();
		this->ui->wgtDevices->setVisible(true);
		this->ui->cmbFormats->setVisible(false);
		this->ui->chkCaptureCursor->setVisible(true);
		this->ui->cmbScreenFps->setVisible(true);
		this->ui->wgtView->setBackendType(m_screenBackendType);
		this->m_service->bindScreenPreview(this->ui->wgtView);
		const bool canControlFps = this->m_service->screenCanControlFrameRate();
		this->ui->cmbScreenFps->setEnabled(canControlFps);
		if (!canControlFps)
		{
			this->ui->cmbScreenFps->setToolTip(tr("当前屏幕采集后端不支持帧率设置。"));
		}
		else
		{
			this->ui->cmbScreenFps->setToolTip(QString());
		}
		this->refreshScreenDeviceUi();
		if (this->ui->cmbDevices->count() <= 0)
		{
			return false;
		}
		const int preferredIndex = qBound(0, m_lastScreenIndex, this->ui->cmbDevices->count() - 1);
		this->ui->cmbDevices->blockSignals(true);
		this->ui->cmbDevices->setCurrentIndex(preferredIndex);
		this->ui->cmbDevices->blockSignals(false);
		return this->selectScreen(preferredIndex);
	};
	connect(actionCameraMode, &QAction::triggered, this, [actionCameraMode, switchToCameraMode]() {
		actionCameraMode->setChecked(true);
		switchToCameraMode();
	});
	connect(actionFileMode, &QAction::triggered, this, [actionCameraMode, actionFileMode, switchToFileMode]() {
		if (!switchToFileMode())
		{
			actionCameraMode->setChecked(true);
			actionFileMode->setChecked(false);
		}
	});
	connect(actionScreenMode, &QAction::triggered, this,
	        [actionCameraMode, actionScreenMode, switchToScreenMode, switchToCameraMode]() {
		        if (!switchToScreenMode())
		        {
			        actionCameraMode->setChecked(true);
			        actionScreenMode->setChecked(false);
			        switchToCameraMode();
		        }
	        });
	connect(actionPushStream, &QAction::triggered, this, [this]() {
		QDialog dlg(this);
		dlg.setWindowTitle(tr("推流配置"));
		auto* layout = new QFormLayout(&dlg);
		layout->setVerticalSpacing(10);
		layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
		auto addRecent = [](QStringList& list, const QString& value) {
			const QString v = value.trimmed();
			if (v.isEmpty())
			{
				return;
			}
			list.removeAll(v);
			list.prepend(v);
			while (list.size() > 8)
			{
				list.removeLast();
			}
		};
		auto* cmbProtocol = new QComboBox(&dlg);
		cmbProtocol->addItem(tr("RTMP"), QStringLiteral("rtmp://127.0.0.1:1935/live/stream"));
		cmbProtocol->addItem(tr("RTSP"), QStringLiteral("rtsp://127.0.0.1:8554/live/stream"));
		cmbProtocol->addItem(tr("SRT"), QStringLiteral("srt://127.0.0.1:8890?mode=caller"));
		const bool fileScene = (m_captureMode == CaptureMode::File);
		const bool screenScene = (m_captureMode == CaptureMode::Screen);
		auto* lblInputMode = new QLabel(screenScene
			                                ? tr("来源：屏幕采集后端（由 Service 统一编排）")
			                                : (fileScene ? tr("来源：当前文件模式媒体源") : tr("来源：当前摄像头模式")),
		                            &dlg);
		lblInputMode->setWordWrap(true);
		lblInputMode->setMinimumHeight(lblInputMode->fontMetrics().lineSpacing() * 2 + 6);
		auto* lblInputValue = new QLabel(&dlg);
		lblInputValue->setWordWrap(true);
		lblInputValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
		lblInputValue->setText(m_currentFilePath.trimmed().isEmpty() ? tr("未打开文件") : m_currentFilePath.trimmed());
		auto* cmbOutput = new QComboBox(&dlg);
		cmbOutput->setEditable(true);
		{
			QStringList outputItems = m_recentPushOutputs;
			if (outputItems.isEmpty())
			{
				outputItems << QStringLiteral("rtmp://127.0.0.1:1935/live/stream");
				outputItems << QStringLiteral("rtsp://127.0.0.1:8554/live/stream");
				outputItems << QStringLiteral("srt://127.0.0.1:8890?mode=caller");
			}
			cmbOutput->addItems(outputItems);
		}
		cmbOutput->setCurrentText(QStringLiteral(""));
		cmbOutput->lineEdit()->setPlaceholderText(tr("输出地址，例如 rtmp://127.0.0.1:1935/live/stream"));
		auto* spFps = new QSpinBox(&dlg);
		spFps->setRange(0, 240);
		spFps->setSpecialValueText(tr("跟随当前"));
		auto* cmbSize = new QComboBox(&dlg);
		cmbSize->setEditable(true);
		cmbSize->setInsertPolicy(QComboBox::NoInsert);
		cmbSize->lineEdit()->setPlaceholderText(tr("跟随当前 / 例如 1920x1080"));
		cmbSize->addItem(tr("跟随当前"), QString());
		auto appendSizeOption = [cmbSize](const int w, const int h) {
			if (w <= 0 || h <= 0)
			{
				return;
			}
			const QString key = QStringLiteral("%1x%2").arg(w).arg(h);
			if (cmbSize->findData(key) < 0)
			{
				cmbSize->addItem(key, key);
			}
		};
		auto appendPresetSizes = [appendSizeOption](const int maxW, const int maxH) {
			const QList<QSize> presets{
				QSize(7680, 4320), QSize(5120, 2880), QSize(3840, 2160), QSize(3440, 1440),
				QSize(2560, 1440), QSize(2560, 1080), QSize(1920, 1200), QSize(1920, 1080),
				QSize(1600, 900), QSize(1366, 768), QSize(1280, 720), QSize(960, 540),
				QSize(854, 480), QSize(640, 360)
			};
			for (const QSize& s : presets)
			{
				if (maxW > 0 && maxH > 0)
				{
					if (s.width() > maxW || s.height() > maxH)
					{
						continue;
					}
				}
				appendSizeOption(s.width(), s.height());
			}
		};
		auto* spBitrate = new QSpinBox(&dlg);
		spBitrate->setRange(0, 50000);
		spBitrate->setSpecialValueText(tr("跟随当前"));
		spBitrate->setValue(0);
		spBitrate->setSuffix(tr(" kbps"));
		auto* cmbEncoder = new QComboBox(&dlg);
		cmbEncoder->addItem(tr("自动（优先NVENC/AMF）"), QStringLiteral("auto"));
		cmbEncoder->addItem(tr("CPU（x264）"), QStringLiteral("cpu"));
		cmbEncoder->addItem(tr("NVIDIA NVENC"), QStringLiteral("nvenc"));
		cmbEncoder->addItem(tr("AMD AMF"), QStringLiteral("amf"));
		{
			const QStringList availableList = this->m_service->streamAvailableVideoEncoders();
			const QSet<QString> available(availableList.begin(), availableList.end());
			auto* model = qobject_cast<QStandardItemModel*>(cmbEncoder->model());
			if (model)
			{
				const auto disableByData = [cmbEncoder, model](const QString& dataValue, const QString& disabledText) {
					const int idx = cmbEncoder->findData(dataValue);
					if (idx < 0)
					{
						return;
					}
					if (QStandardItem* item = model->item(idx))
					{
						item->setEnabled(false);
						item->setToolTip(disabledText);
					}
				};
				if (!available.contains(QStringLiteral("nvenc")))
				{
					disableByData(QStringLiteral("nvenc"), tr("当前 FFmpeg/驱动环境不可用：h264_nvenc"));
				}
				if (!available.contains(QStringLiteral("amf")))
				{
					disableByData(QStringLiteral("amf"), tr("当前 FFmpeg/驱动环境不可用：h264_amf"));
				}
			}
			if (!available.contains(QStringLiteral("nvenc")) && !available.contains(QStringLiteral("amf")))
			{
				cmbEncoder->setToolTip(tr("当前仅检测到 CPU 编码可用"));
			}
		}
		auto* cmbAudioInput = new QComboBox(&dlg);
		cmbAudioInput->addItem(tr("关闭输入设备"), QStringLiteral("off"));
		{
			const auto audioInputs = QMediaDevices::audioInputs();
			QSet<QString> dedup;
			for (const auto& dev : audioInputs)
			{
				const QString name = dev.description().trimmed();
				if (name.isEmpty() || dedup.contains(name))
				{
					continue;
				}
				dedup.insert(name);
				cmbAudioInput->addItem(tr("麦克风：%1").arg(name), name);
			}
		}
		cmbAudioInput->setCurrentIndex(0);
		auto* cmbAudioOutput = new QComboBox(&dlg);
		cmbAudioOutput->addItem(tr("关闭输出设备"), QStringLiteral("off"));
		cmbAudioOutput->addItem(tr("系统声音（实验）"), QStringLiteral("system"));
		{
			const auto audioOutputs = QMediaDevices::audioOutputs();
			QSet<QString> dedup;
			for (const auto& dev : audioOutputs)
			{
				const QString name = dev.description().trimmed();
				if (name.isEmpty() || dedup.contains(name))
				{
					continue;
				}
				dedup.insert(name);
				cmbAudioOutput->addItem(tr("扬声器：%1").arg(name), name);
			}
		}
		cmbAudioOutput->setCurrentIndex(0);
		cmbAudioInput->setEnabled(!fileScene);
		cmbAudioOutput->setEnabled(!fileScene);
		auto* chkKeepAspect = new QCheckBox(tr("保持宽高比"), &dlg);
		chkKeepAspect->setChecked(true);
		if (screenScene)
		{
			const int curFps = this->m_service ? this->m_service->screenFrameRate() : 30;
			spFps->setValue(curFps);
			if (this->m_service)
			{
				const auto screens = this->m_service->getScreenList();
				const int idx = this->ui->cmbDevices ? this->ui->cmbDevices->currentIndex() : -1;
				if (idx >= 0 && idx < screens.size())
				{
					const QString text = screens.at(idx);
					const QRegularExpression re(R"(\((?:主屏|副屏),\s*(\d+)x(\d+)\))");
					const auto m = re.match(text);
					if (m.hasMatch())
					{
						const int w = m.captured(1).toInt();
						const int h = m.captured(2).toInt();
						appendPresetSizes(w, h);
						appendSizeOption(w, h);
						cmbSize->setCurrentText(QStringLiteral("%1x%2").arg(w).arg(h));
					}
				}
			}
		}
		else if (!fileScene)
		{
			const QString fmtText = this->ui->cmbFormats ? this->ui->cmbFormats->currentText().trimmed() : QString();
			const QRegularExpression re(R"((\d+)\s*x\s*(\d+)\s+(\d+)\s*fps)", QRegularExpression::CaseInsensitiveOption);
			const auto m = re.match(fmtText);
			if (m.hasMatch())
			{
				const int w = m.captured(1).toInt();
				const int h = m.captured(2).toInt();
				const int fps = m.captured(3).toInt();
				spFps->setValue(fps);
				appendPresetSizes(w, h);
				appendSizeOption(w, h);
				cmbSize->setCurrentText(QStringLiteral("%1x%2").arg(w).arg(h));
			}
			else
			{
				appendPresetSizes(3840, 2160);
			}
		}
		else
		{
			appendPresetSizes(3840, 2160);
		}
		if (fileScene)
		{
			spFps->setEnabled(false);
			cmbSize->setEnabled(false);
			spBitrate->setEnabled(true);
		}
		auto* lblPushParams = new QLabel(&dlg);
		lblPushParams->setWordWrap(true);
		lblPushParams->setTextInteractionFlags(Qt::TextSelectableByMouse);
		lblPushParams->setMinimumHeight(lblPushParams->fontMetrics().lineSpacing() * 4 + 8);
		auto refreshPushParams = [this, lblPushParams, lblInputValue, fileScene, screenScene]() {
			if (!lblPushParams)
			{
				return;
			}
			if (screenScene)
			{
				const QString screenText = this->ui->cmbDevices ? this->ui->cmbDevices->currentText().trimmed() : QString();
				const int fps = this->m_service ? this->m_service->screenFrameRate() : 30;
				lblPushParams->setText(tr("模式：屏幕\n后端：%1\n来源：%2\n帧率：%3 FPS\n编码：H264（不可用时 MPEG4）")
				                       .arg(QString::fromLatin1(screenBackendName(this->m_screenBackendType)))
				                       .arg(screenText.isEmpty() ? tr("当前屏幕") : screenText)
				                       .arg(fps));
				return;
			}
			if (fileScene)
			{
				const QString input = lblInputValue ? lblInputValue->text().trimmed() : QString();
				lblPushParams->setText(tr("模式：文件\n来源：%1\n策略：默认 copy；设置参数时转码\n编码：copy/重编码（按参数）")
				                       .arg(input.isEmpty() ? tr("未指定") : input));
				return;
			}
			const QString cameraName = this->ui->cmbDevices ? this->ui->cmbDevices->currentText().trimmed() : QString();
			const QString formatText = this->ui->cmbFormats ? this->ui->cmbFormats->currentText().trimmed() : QString();
			lblPushParams->setText(tr("模式：摄像头\n设备：%1\n格式：%2\n编码：H264（不可用时 MPEG4）")
			                       .arg(cameraName.isEmpty() ? tr("未选择") : cameraName)
			                       .arg(formatText.isEmpty() ? tr("默认") : formatText));
		};
		auto* lblStatus = new QLabel(tr("状态：未启动"), &dlg);
		auto* txtLog = new QTextEdit(&dlg);
		txtLog->setReadOnly(true);
		txtLog->setMinimumHeight(120);
		auto* logTimer = new QTimer(&dlg);
		logTimer->setInterval(500);
		connect(logTimer, &QTimer::timeout, &dlg, [this, lblStatus, txtLog]() {
			if (this->m_service->streamIsRunning())
			{
				lblStatus->setText(tr("状态：运行中"));
			}
			else if (this->m_service->streamHasCompletedSession())
			{
				lblStatus->setText(tr("状态：已停止，退出码=%1").arg(this->m_service->streamLastExitCode()));
			}
			else
			{
				lblStatus->setText(tr("状态：当前无推流任务"));
			}
			const QString latestLog = this->m_service->streamRecentLog();
			// 用户正在选择文本时不重刷，避免复制被打断。
			if (!txtLog->textCursor().hasSelection())
			{
				const QString currentLog = txtLog->toPlainText();
				if (latestLog.startsWith(currentLog))
				{
					if (latestLog.size() > currentLog.size())
					{
						txtLog->moveCursor(QTextCursor::End);
						txtLog->insertPlainText(latestLog.mid(currentLog.size()));
						txtLog->moveCursor(QTextCursor::End);
					}
				}
				else if (currentLog != latestLog)
				{
					// 日志被清空或滚动裁剪时，回退到整段同步一次。
					txtLog->setPlainText(latestLog);
					txtLog->moveCursor(QTextCursor::End);
				}
			}
		});
		logTimer->start();
		connect(cmbProtocol, &QComboBox::currentTextChanged, &dlg, [cmbProtocol, cmbOutput]() {
			if (cmbOutput->currentText().trimmed().isEmpty())
			{
				cmbOutput->setCurrentText(cmbProtocol->currentData().toString());
			}
		});
		layout->addRow(lblInputMode);
		if (fileScene)
		{
			layout->addRow(tr("输入源"), lblInputValue);
		}
		layout->addRow(tr("当前参数"), lblPushParams);
		layout->addRow(tr("帧率"), spFps);
		layout->addRow(tr("尺寸"), cmbSize);
		layout->addRow(QString(), chkKeepAspect);
		layout->addRow(tr("码率"), spBitrate);
		layout->addRow(tr("视频编码器"), cmbEncoder);
		layout->addRow(tr("输入设备"), cmbAudioInput);
		layout->addRow(tr("输出设备"), cmbAudioOutput);
		layout->addRow(tr("协议模板"), cmbProtocol);
		layout->addRow(tr("输出"), cmbOutput);
		layout->addRow(lblStatus);
		layout->addRow(txtLog);
		auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
		auto* btnStart = new QPushButton(tr("开始推流"), &dlg);
		auto* btnStop = new QPushButton(tr("停止推流"), &dlg);
		buttons->addButton(btnStart, QDialogButtonBox::AcceptRole);
		buttons->addButton(btnStop, QDialogButtonBox::ActionRole);
		layout->addRow(buttons);
		connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		connect(btnStop, &QPushButton::clicked, &dlg,
		        [this, btnStart, cmbProtocol, cmbOutput, spFps, cmbSize, spBitrate, cmbEncoder, cmbAudioInput, cmbAudioOutput,
		         fileScene, screenScene]() {
			this->m_service->streamStop();
			btnStart->setEnabled(true);
			cmbProtocol->setEnabled(true);
			cmbOutput->setEnabled(true);
			spFps->setEnabled(!fileScene);
			cmbSize->setEnabled(!fileScene);
			spBitrate->setEnabled(true);
			cmbEncoder->setEnabled(true);
			cmbAudioInput->setEnabled(!fileScene);
			cmbAudioOutput->setEnabled(!fileScene);
		});
		connect(btnStart, &QPushButton::clicked, &dlg,
		        [this, btnStart, cmbProtocol, cmbOutput, spFps, cmbSize, spBitrate, cmbEncoder, cmbAudioInput, cmbAudioOutput,
		         chkKeepAspect, fileScene, screenScene, addRecent]() {
			const QString pushOutput = cmbOutput->currentText().trimmed();
			if (pushOutput.isEmpty())
			{
				QMessageBox::warning(this, tr("推流失败"), tr("输出地址不能为空。"));
				return;
			}
			fplayer::Service::PushScene pushScene = fplayer::Service::PushScene::Camera;
			QString sceneInput;
			if (screenScene)
			{
				pushScene = fplayer::Service::PushScene::Screen;
			}
			else if (fileScene)
			{
				pushScene = fplayer::Service::PushScene::File;
				sceneInput = m_currentFilePath.trimmed();
				if (sceneInput.isEmpty())
				{
					QMessageBox::warning(this, tr("推流失败"), tr("文件模式下未找到可用输入源，请先打开文件后再推流。"));
					return;
				}
			}
			else
			{
				sceneInput = this->ui->cmbDevices ? this->ui->cmbDevices->currentText().trimmed() : QString();
			}
			fplayer::Service::PushOptions options;
			options.fps = spFps->value();
			options.bitrateKbps = spBitrate->value();
			options.keepAspectRatio = chkKeepAspect->isChecked();
			options.videoEncoder = cmbEncoder->currentData().toString();
			options.audioInputSource = cmbAudioInput->currentData().toString();
			options.audioOutputSource = cmbAudioOutput->currentData().toString();
			QString sizeText = cmbSize->currentText().trimmed();
			if (sizeText.isEmpty() || sizeText == tr("跟随当前"))
			{
				sizeText = cmbSize->currentData().toString().trimmed();
			}
			if (!sizeText.isEmpty())
			{
				const QRegularExpression re(R"((\d+)\s*x\s*(\d+))", QRegularExpression::CaseInsensitiveOption);
				const auto m = re.match(sizeText);
				if (m.hasMatch())
				{
					options.width = m.captured(1).toInt();
					options.height = m.captured(2).toInt();
				}
				else
				{
					QMessageBox::warning(this, tr("推流失败"), tr("尺寸格式无效，请使用 WxH，例如 1920x1080。"));
					return;
				}
			}
			if (!this->m_service->streamStartPushByScene(pushScene, pushOutput, sceneInput, options))
			{
				QMessageBox::warning(this, tr("推流失败"), this->m_service->streamLastError());
				return;
			}
			addRecent(m_recentPushOutputs, pushOutput);
			btnStart->setEnabled(false);
			cmbProtocol->setEnabled(false);
			cmbOutput->setEnabled(false);
			spFps->setEnabled(false);
			cmbSize->setEnabled(false);
			spBitrate->setEnabled(false);
			cmbEncoder->setEnabled(false);
			cmbAudioInput->setEnabled(false);
			cmbAudioOutput->setEnabled(false);
		});
		connect(logTimer, &QTimer::timeout, &dlg,
		        [this, btnStart, cmbProtocol, cmbOutput, spFps, cmbSize, spBitrate, cmbEncoder, cmbAudioInput, cmbAudioOutput,
		         fileScene, screenScene]() {
			const bool running = this->m_service->streamIsRunning();
			if (!running)
			{
				btnStart->setEnabled(true);
				cmbProtocol->setEnabled(true);
				cmbOutput->setEnabled(true);
				spFps->setEnabled(!fileScene);
				cmbSize->setEnabled(!fileScene);
				spBitrate->setEnabled(true);
				cmbEncoder->setEnabled(true);
				cmbAudioInput->setEnabled(!fileScene);
				cmbAudioOutput->setEnabled(!fileScene);
			}
		});
		refreshPushParams();
		dlg.exec();
	});
	connect(actionPullStream, &QAction::triggered, this, [this]() {
		QDialog dlg(this);
		dlg.setWindowTitle(tr("拉流配置"));
		auto* layout = new QFormLayout(&dlg);
		auto addRecent = [](QStringList& list, const QString& value) {
			const QString v = value.trimmed();
			if (v.isEmpty())
			{
				return;
			}
			list.removeAll(v);
			list.prepend(v);
			while (list.size() > 8)
			{
				list.removeLast();
			}
		};
		auto* cmbProtocol = new QComboBox(&dlg);
		cmbProtocol->addItem(tr("RTMP"), QStringLiteral("rtmp://127.0.0.1:1935/live/stream"));
		cmbProtocol->addItem(tr("RTSP"), QStringLiteral("rtsp://127.0.0.1:8554/live/stream"));
		cmbProtocol->addItem(tr("SRT"), QStringLiteral("srt://127.0.0.1:8890?mode=listener"));
		auto* cmbInput = new QComboBox(&dlg);
		cmbInput->setEditable(true);
		{
			QStringList inputItems = m_recentPullInputs;
			if (inputItems.isEmpty())
			{
				inputItems << QStringLiteral("rtmp://127.0.0.1:1935/live/stream");
				inputItems << QStringLiteral("rtsp://127.0.0.1:8554/live/stream");
				inputItems << QStringLiteral("srt://127.0.0.1:8890?mode=listener");
			}
			cmbInput->addItems(inputItems);
		}
		auto* cmbOutput = new QComboBox(&dlg);
		cmbOutput->setEditable(true);
		{
			QStringList outputItems = m_recentPullOutputs;
			if (outputItems.isEmpty())
			{
				outputItems << QStringLiteral("D:/temp/pull.mp4");
				outputItems << QStringLiteral("pull_output.mp4");
			}
			cmbOutput->addItems(outputItems);
		}
		auto* chkPreview = new QCheckBox(tr("直接预览到播放窗口"), &dlg);
		cmbInput->lineEdit()->setPlaceholderText(tr("输入流地址，例如 rtmp://127.0.0.1:1935/live/stream"));
		cmbOutput->lineEdit()->setPlaceholderText(tr("输出文件，例如 D:/temp/pull.mp4"));
		auto* btnBrowseOutput = new QPushButton(tr("选择输出文件"), &dlg);
		connect(btnBrowseOutput, &QPushButton::clicked, &dlg, [cmbOutput, this]() {
			const QString outPath = QFileDialog::getSaveFileName(this, tr("选择拉流输出文件"), QString(),
			                                                     tr("Media Files (*.mp4 *.mkv *.flv);;All Files (*.*)"));
			if (!outPath.isEmpty())
			{
				cmbOutput->setCurrentText(outPath);
			}
		});
		auto* lblStatus = new QLabel(tr("状态：未启动"), &dlg);
		auto* txtLog = new QTextEdit(&dlg);
		txtLog->setReadOnly(true);
		txtLog->setMinimumHeight(120);
		auto* logTimer = new QTimer(&dlg);
		logTimer->setInterval(500);
		connect(logTimer, &QTimer::timeout, &dlg, [this, lblStatus, txtLog]() {
			if (this->m_service->streamIsRunning())
			{
				lblStatus->setText(tr("状态：运行中"));
			}
			else if (this->m_service->streamHasCompletedSession())
			{
				lblStatus->setText(tr("状态：已停止，退出码=%1").arg(this->m_service->streamLastExitCode()));
			}
			else
			{
				lblStatus->setText(tr("状态：当前无拉流任务"));
			}
			const QString latestLog = this->m_service->streamRecentLog();
			// 用户正在选择文本时不重刷，避免复制被打断。
			if (!txtLog->textCursor().hasSelection())
			{
				const QString currentLog = txtLog->toPlainText();
				if (latestLog.startsWith(currentLog))
				{
					if (latestLog.size() > currentLog.size())
					{
						txtLog->moveCursor(QTextCursor::End);
						txtLog->insertPlainText(latestLog.mid(currentLog.size()));
						txtLog->moveCursor(QTextCursor::End);
					}
				}
				else if (currentLog != latestLog)
				{
					// 日志被清空或滚动裁剪时，回退到整段同步一次。
					txtLog->setPlainText(latestLog);
					txtLog->moveCursor(QTextCursor::End);
				}
			}
		});
		logTimer->start();
		connect(cmbProtocol, &QComboBox::currentTextChanged, &dlg, [cmbProtocol, cmbInput]() {
			if (cmbInput->currentText().trimmed().isEmpty())
			{
				cmbInput->setCurrentText(cmbProtocol->currentData().toString());
			}
		});
		layout->addRow(tr("输入"), cmbInput);
		layout->addRow(tr("协议模板"), cmbProtocol);
		layout->addRow(tr("输出"), cmbOutput);
		layout->addRow(QString(), btnBrowseOutput);
		layout->addRow(chkPreview);
		layout->addRow(lblStatus);
		layout->addRow(txtLog);
		auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
		auto* btnStop = new QPushButton(tr("停止拉流"), &dlg);
		buttons->addButton(btnStop, QDialogButtonBox::ActionRole);
		layout->addRow(buttons);
		connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		connect(btnStop, &QPushButton::clicked, &dlg, [this]() {
			this->m_service->streamStop();
		});
		if (dlg.exec() != QDialog::Accepted)
		{
			return;
		}
		const QString pullInput = cmbInput->currentText().trimmed();
		const QString pullOutput = cmbOutput->currentText().trimmed();
		if (chkPreview->isChecked())
		{
			this->ui->wgtView->setBackendType(fplayer::MediaBackendType::FFmpeg);
			this->m_service->bindPlayerPreview(this->ui->wgtView);
			if (!this->m_service->openMediaFile(pullInput))
			{
				QMessageBox::warning(this, tr("拉流预览失败"), tr("无法打开输入流地址"));
				return;
			}
			m_captureMode = CaptureMode::File;
			m_isFileMode = true;
			this->ui->wgtDevices->setVisible(false);
			this->m_fileProgress->setVisible(true);
			this->m_fileProgressLabel->setVisible(true);
			this->m_speedCombo->setVisible(true);
			this->m_debugStatsLabel->setVisible(true);
			this->m_fileProgressTimer->start();
			this->m_debugStatsTimer->start();
			this->ui->btnPlay->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::MediaPlaybackPause));
			addRecent(m_recentPullInputs, pullInput);
			return;
		}
		if (!this->m_service->streamStartPull(pullInput, pullOutput))
		{
			QMessageBox::warning(this, tr("拉流失败"), this->m_service->streamLastError());
			return;
		}
		addRecent(m_recentPullInputs, pullInput);
		addRecent(m_recentPullOutputs, pullOutput);
	});
	connect(m_fileTitleButton, &QToolButton::clicked, this, [this, actionFileMode]() {
		if (!m_isFileMode)
		{
			actionFileMode->trigger();
			return;
		}
		if (this->chooseAndPlayFile())
		{
			this->updateFileProgressUi();
		}
	});

	connect(this->ui->btnFullscreen, &QPushButton::clicked, [this, refreshFullscreenButton]() {
		if (this->isFullScreen())
		{
			this->showNormal();
		}
		else
		{
			this->showFullScreen();
		}
		refreshFullscreenButton();
	});

	refreshFullscreenButton();

	auto canUseGlobalHotkey = []() -> bool {
		QWidget* fw = QApplication::focusWidget();
		if (!fw)
		{
			return true;
		}
		// 文本输入类控件聚焦时，不拦截按键，避免影响输入体验。
		if (qobject_cast<QLineEdit*>(fw) ||
			qobject_cast<QTextEdit*>(fw) ||
			qobject_cast<QPlainTextEdit*>(fw) ||
			qobject_cast<QAbstractSpinBox*>(fw))
		{
			return false;
		}
		return true;
	};

	// 使用快捷键而不是 keyPressEvent，避免被子控件（如下拉框）吞键。
	auto* scPlayPause = new QShortcut(QKeySequence(Qt::Key_Space), this);
	scPlayPause->setContext(Qt::ApplicationShortcut);
	connect(scPlayPause, &QShortcut::activated, this, [this, canUseGlobalHotkey]() {
		if (!this->isActiveWindow() || !canUseGlobalHotkey())
		{
			return;
		}
		this->togglePlayPause();
	});

	auto* scFullscreen = new QShortcut(QKeySequence(Qt::Key_F), this);
	scFullscreen->setContext(Qt::ApplicationShortcut);
	connect(scFullscreen, &QShortcut::activated, this, [this, refreshFullscreenButton, canUseGlobalHotkey]() {
		if (!this->isActiveWindow() || !canUseGlobalHotkey())
		{
			return;
		}
		if (this->isFullScreen())
		{
			this->showNormal();
		}
		else
		{
			this->showFullScreen();
		}
		refreshFullscreenButton();
	});

	auto* scExitFullscreen = new QShortcut(QKeySequence(Qt::Key_Escape), this);
	scExitFullscreen->setContext(Qt::ApplicationShortcut);
	connect(scExitFullscreen, &QShortcut::activated, this, [this, refreshFullscreenButton]() {
		if (!this->isActiveWindow())
		{
			return;
		}
		if (this->isFullScreen())
		{
			this->showNormal();
			refreshFullscreenButton();
		}
	});

	relocateTitleWidget();
}

void CaptureWindow::togglePlayPause()
{
	if (m_captureMode == CaptureMode::File)
	{
		if (this->m_service->playerIsPlaying())
		{
			this->m_service->playerPause();
			this->ui->btnPlay->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::MediaPlaybackStart));
		}
		else
		{
			this->m_service->playerResume();
			this->ui->btnPlay->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::MediaPlaybackPause));
		}
		return;
	}
	if (m_captureMode == CaptureMode::Screen)
	{
		if (!m_service)
		{
			return;
		}
		const bool active = m_service->screenIsActive();
		m_service->screenSetActive(!active);
		this->ui->btnPlay->setIcon(QIcon::fromTheme(
			!active ? QIcon::ThemeIcon::MediaPlaybackPause : QIcon::ThemeIcon::MediaPlaybackStart));
		return;
	}

	if (this->m_service->cameraIsPlaying())
	{
		this->m_service->cameraPause();
		this->ui->btnPlay->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::MediaPlaybackStart));
	}
	else
	{
		this->m_service->cameraResume();
		this->ui->btnPlay->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::MediaPlaybackPause));
	}
}

bool CaptureWindow::chooseAndPlayFile()
{
	const QString filePath = QFileDialog::getOpenFileName(
		this,
		tr("选择媒体文件"),
		QString(),
		tr("Media Files (*.mp4 *.mkv *.mov *.avi *.flv *.wmv *.mp3 *.aac *.wav *.flac);;All Files (*.*)")
	);
	if (filePath.isEmpty())
	{
		return false;
	}

	this->m_service->cameraPause();
	if (this->m_service->openMediaFile(filePath))
	{
		m_currentFileTitle = QFileInfo(filePath).fileName();
		m_currentFilePath = filePath;
		m_titleMarqueeOffset = 0;
		updateTitleMarqueeText();
		if (m_titleMarqueeTimer && !m_titleMarqueeTimer->isActive())
		{
			m_titleMarqueeTimer->start();
		}
		return true;
	}
	return false;
}

CaptureWindow::~CaptureWindow()
{
	stopScreenCapture();
	if (m_service)
	{
		if (m_fileProgressTimer)
		{
			m_fileProgressTimer->stop();
		}
		if (m_debugStatsTimer)
		{
			m_debugStatsTimer->stop();
		}
		if (m_titleMarqueeTimer)
		{
			m_titleMarqueeTimer->stop();
		}
		m_service->playerStop();
		delete m_service;
		m_service = nullptr;
	}
	delete ui;
}

void CaptureWindow::updateFileProgressUi()
{
	if (!m_isFileMode || !m_fileProgress || !m_fileProgressLabel)
	{
		return;
	}
	const qint64 duration = m_service->playerDurationMs();
	const qint64 position = m_service->playerPositionMs();
	const int maxValue = duration > 0 ? static_cast<int>(duration) : 0;
	m_fileProgress->setMaximum(maxValue);
	if (!m_progressDragging)
	{
		const int value = maxValue > 0 ? static_cast<int>(qMin(position, duration)) : 0;
		m_fileProgress->setValue(value);
	}
	m_fileProgressLabel->setText(QString("%1 / %2").arg(formatTimeMs(position), formatTimeMs(duration)));
}

QString CaptureWindow::formatTimeMs(qint64 ms) const
{
	if (ms < 0)
	{
		ms = 0;
	}
	const qint64 totalSec = ms / 1000;
	const qint64 min = totalSec / 60;
	const qint64 sec = totalSec % 60;
	return QString("%1:%2").arg(min, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
}

void CaptureWindow::updateTitleMarqueeText()
{
	if (!m_fileTitleButton)
	{
		return;
	}
	if (m_currentFileTitle.isEmpty())
	{
		m_fileTitleButton->setText(tr("点击选择文件"));
		return;
	}
	const QString source = m_currentFileTitle + "    ";
	if (source.size() <= 24)
	{
		m_fileTitleButton->setText(source.trimmed());
		return;
	}
	if (m_titleMarqueeOffset >= source.size())
	{
		m_titleMarqueeOffset = 0;
	}
	const QString loop = source + source;
	m_fileTitleButton->setText(loop.mid(m_titleMarqueeOffset, 24));
	m_titleMarqueeOffset = (m_titleMarqueeOffset + 1) % source.size();
}

void CaptureWindow::updateDebugStatsUi()
{
	if (!m_isFileMode || !m_debugStatsLabel)
	{
		return;
	}
	m_debugStatsLabel->setText(m_service->playerDebugStats());
}

void CaptureWindow::relocateTitleWidget()
{
	if (!m_modeMenuBar || !m_fileTitleButton)
	{
		return;
	}
	const int centerX = (m_modeMenuBar->width() - m_fileTitleButton->width()) / 2;
	const int centerY = (m_modeMenuBar->height() - m_fileTitleButton->height()) / 2;
	m_fileTitleButton->move(qMax(0, centerX), qMax(0, centerY));
}

void CaptureWindow::resizeEvent(QResizeEvent* event)
{
	QWidget::resizeEvent(event);
	relocateTitleWidget();
}

void CaptureWindow::refreshCameraDeviceUi()
{
	this->ui->cmbDevices->blockSignals(true);
	this->ui->cmbFormats->blockSignals(true);
	this->ui->cmbDevices->clear();
	this->ui->cmbFormats->clear();
	this->ui->cmbDevices->addItems(QStringList(this->m_service->getCameraList()));
	this->ui->cmbDevices->blockSignals(false);
	this->ui->cmbFormats->blockSignals(false);
}

void CaptureWindow::refreshScreenDeviceUi()
{
	this->ui->cmbDevices->blockSignals(true);
	this->ui->cmbDevices->clear();
	const auto items = m_service ? m_service->getScreenList() : QList<QString>{};
	for (const auto& item : items)
	{
		this->ui->cmbDevices->addItem(item);
	}
	this->ui->cmbDevices->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	int maxTextWidth = 0;
	const QFontMetrics fm(this->ui->cmbDevices->font());
	for (const auto& item : items)
	{
		maxTextWidth = qMax(maxTextWidth, fm.horizontalAdvance(item));
	}
	const int expectWidth = maxTextWidth + 72;
	this->ui->cmbDevices->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	this->ui->cmbDevices->setMinimumContentsLength(qMax(24, expectWidth / qMax(1, fm.averageCharWidth())));
	this->ui->cmbDevices->setMinimumWidth(expectWidth);
	this->ui->cmbDevices->setMaximumWidth(expectWidth + 24);
	if (this->ui->cmbDevices->view())
	{
		this->ui->cmbDevices->view()->setMinimumWidth(expectWidth + 40);
	}
	this->ui->cmbDevices->blockSignals(false);
}

void CaptureWindow::refreshScreenFpsUi(int screenIndex)
{
	this->ui->cmbScreenFps->blockSignals(true);
	this->ui->cmbScreenFps->clear();
	const auto screens = QGuiApplication::screens();
	qreal refreshRate = 60.0;
	if (screenIndex >= 0 && screenIndex < screens.size() && screens.at(screenIndex))
	{
		refreshRate = screens.at(screenIndex)->refreshRate();
	}
	const QList<int> baseFps{15, 24, 25, 30, 45, 50, 60, 75, 90, 100, 120, 144, 165, 180, 200, 240};
	QList<int> candidates;
	for (const int fps : baseFps)
	{
		if (fps <= static_cast<int>(refreshRate + 0.5))
		{
			candidates.push_back(fps);
		}
	}
	if (candidates.isEmpty())
	{
		candidates.push_back(qMax(15, static_cast<int>(refreshRate + 0.5)));
	}
	const int recommended = preferredFpsForScreen(screenIndex);
	QSet<int> dedup;
	for (const int fps : candidates)
	{
		if (dedup.contains(fps))
		{
			continue;
		}
		dedup.insert(fps);
		const QString text = (fps == recommended) ? tr("%1 FPS (推荐)").arg(fps) : tr("%1 FPS").arg(fps);
		this->ui->cmbScreenFps->addItem(text, fps);
	}
	this->ui->cmbScreenFps->blockSignals(false);
}

bool CaptureWindow::selectScreen(int index)
{
	if (!m_service || index < 0 || index >= this->ui->cmbDevices->count())
	{
		return false;
	}
	auto fallbackToFfmpeg = [this, index]() -> bool {
		LOG_WARN("[screen]", "fallback to FFmpeg(gdigrab), index=", index);
		m_service->initScreenCapture(fplayer::MediaBackendType::FFmpeg);
		m_screenBackendType = fplayer::MediaBackendType::FFmpeg;
		this->ui->wgtView->setBackendType(m_screenBackendType);
		this->m_service->bindScreenPreview(this->ui->wgtView);
		this->refreshScreenDeviceUi();
		if (this->ui->cmbDevices->count() <= 0)
		{
			return false;
		}
		const int fallbackIndex = qBound(0, index, this->ui->cmbDevices->count() - 1);
		refreshScreenFpsUi(fallbackIndex);
		const int fallbackFps = m_screenFpsOverrides.value(fallbackIndex, preferredFpsForScreen(fallbackIndex));
		const int fallbackFpsComboIndex = this->ui->cmbScreenFps->findData(fallbackFps);
		this->ui->cmbScreenFps->blockSignals(true);
		this->ui->cmbScreenFps->setCurrentIndex(fallbackFpsComboIndex >= 0
			                                        ? fallbackFpsComboIndex
			                                        : (this->ui->cmbScreenFps->count() > 0 ? 0 : -1));
		this->ui->cmbScreenFps->blockSignals(false);
		const bool fallbackCanControlFps = this->m_service->screenCanControlFrameRate();
		this->ui->cmbScreenFps->setEnabled(fallbackCanControlFps);
		if (!fallbackCanControlFps)
		{
			this->ui->cmbScreenFps->setToolTip(tr("当前屏幕采集后端不支持帧率设置。"));
		}
		else
		{
			this->ui->cmbScreenFps->setToolTip(QString());
		}
		if (!m_service->selectScreen(fallbackIndex))
		{
			return false;
		}
		this->m_service->screenSetFrameRate(fallbackFps > 0 ? fallbackFps : 30);
		m_service->screenSetActive(true);
		if (!m_service->screenSetCursorCaptureEnabled(this->ui->chkCaptureCursor->isChecked()))
		{
			this->ui->chkCaptureCursor->setChecked(false);
			this->ui->chkCaptureCursor->setEnabled(false);
			this->ui->chkCaptureCursor->setToolTip(tr("当前屏幕采集后端不支持捕获鼠标指针。"));
		}
		else
		{
			this->ui->chkCaptureCursor->setEnabled(true);
			this->updateCaptureCursorCheckToolTip();
		}
		this->ui->btnPlay->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::MediaPlaybackPause));
		return true;
	};
	LOG_INFO("[screen]", "start capture, backend=", screenBackendName(m_screenBackendType), " index=", index);
	m_lastScreenIndex = index;
	this->ui->wgtView->setBackendType(m_screenBackendType);
	this->m_service->bindScreenPreview(this->ui->wgtView);
	refreshScreenFpsUi(index);
	const int fps = m_screenFpsOverrides.value(index, preferredFpsForScreen(index));
	const int fpsComboIndex = this->ui->cmbScreenFps->findData(fps);
	if (fpsComboIndex >= 0)
	{
		this->ui->cmbScreenFps->blockSignals(true);
		this->ui->cmbScreenFps->setCurrentIndex(fpsComboIndex);
		this->ui->cmbScreenFps->blockSignals(false);
	}
	else
	{
		this->ui->cmbScreenFps->blockSignals(true);
		this->ui->cmbScreenFps->setCurrentIndex(this->ui->cmbScreenFps->count() > 0 ? 0 : -1);
		this->ui->cmbScreenFps->blockSignals(false);
	}
#if defined(_WIN32)
	if (m_screenBackendType == fplayer::MediaBackendType::Dxgi && !m_hdrPromptedScreenIndexes.contains(index))
	{
		if (isHdrEnabledForScreenIndex(index))
		{
			m_hdrPromptedScreenIndexes.insert(index);
			const auto choice = QMessageBox::question(
				this,
				tr("屏幕捕获后端切换"),
				tr("已检测到系统HDR打开，为确保稳定，将屏幕获取后端改为ffmepg。\n是否切换？"),
				QMessageBox::Yes | QMessageBox::No,
				QMessageBox::Yes);
			if (choice == QMessageBox::Yes)
			{
				return fallbackToFfmpeg();
			}
		}
	}
#endif
	if (!m_service->selectScreen(index))
	{
		// DXGI 在部分显卡/DPI/会话组合下可能无法稳定拿到桌面复制流，
		// 这里自动回退到 FFmpeg(gdigrab) 保证屏幕捕获可用。
		if (m_screenBackendType == fplayer::MediaBackendType::Dxgi)
		{
			LOG_WARN("[screen]", "DXGI selectScreen failed");
			return fallbackToFfmpeg();
		}
		return false;
	}
	this->m_service->screenSetFrameRate(fps > 0 ? fps : 30);
	m_service->screenSetActive(true);
	if (!m_service->screenSetCursorCaptureEnabled(this->ui->chkCaptureCursor->isChecked()))
	{
		this->ui->chkCaptureCursor->setChecked(false);
		this->ui->chkCaptureCursor->setEnabled(false);
		this->ui->chkCaptureCursor->setToolTip(tr("当前屏幕采集后端不支持捕获鼠标指针。"));
	}
	else
	{
		this->ui->chkCaptureCursor->setEnabled(true);
		this->updateCaptureCursorCheckToolTip();
	}
	this->ui->btnPlay->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::MediaPlaybackPause));
	return true;
}

void CaptureWindow::updateCaptureCursorCheckToolTip()
{
	if (m_screenBackendType == fplayer::MediaBackendType::Dxgi)
	{
		this->ui->chkCaptureCursor->setToolTip(tr(
			"DXGI 桌面复制在帧内叠加鼠标指针，通常可避免 GDI 全屏抓取导致的系统光标闪烁。"
			"若仍异常可取消勾选（画面中不绘制指针）。"));
		return;
	}
	if (m_screenBackendType == fplayer::MediaBackendType::FFmpeg)
	{
		this->ui->chkCaptureCursor->setToolTip(tr(
			"Windows：FFmpeg 使用 gdigrab（GDI）采集，勾选后会在每帧叠加鼠标指针；与 BitBlt+CAPTUREBLT 及桌面合成（DWM）"
			"叠加时，部分环境下会出现「系统鼠标」在全屏范围高频闪烁，与预览窗口位置无关。"
			"若闪烁请取消勾选（画面中不再绘制指针，系统鼠标仍可见），或尝试降低采集帧率。"));
		return;
	}
	this->ui->chkCaptureCursor->setToolTip(QString());
}

void CaptureWindow::stopScreenCapture()
{
	if (m_service)
	{
		m_service->screenSetActive(false);
	}
}

int CaptureWindow::preferredFpsForScreen(int screenIndex) const
{
	const auto screens = QGuiApplication::screens();
	if (screenIndex < 0 || screenIndex >= screens.size() || !screens.at(screenIndex))
	{
		return 30;
	}
	const auto* screen = screens.at(screenIndex);
	const qreal refreshRate = screen->refreshRate();
	const QSize logical = screen->geometry().size();
	const qreal dpr = screen->devicePixelRatio();
	const qint64 pixels = static_cast<qint64>(logical.width() * dpr) * static_cast<qint64>(logical.height() * dpr);

	int targetByResolution = 60;
	if (pixels <= 1920LL * 1080LL)
	{
		targetByResolution = 120;
	}
	else if (pixels <= 2560LL * 1440LL)
	{
		targetByResolution = 90;
	}
	else if (pixels <= 3840LL * 2160LL)
	{
		targetByResolution = 60;
	}
	else
	{
		targetByResolution = 30;
	}

	const int maxByRefresh = qMax(15, static_cast<int>(refreshRate + 0.5));
	const int upper = qMin(targetByResolution, maxByRefresh);
	if (upper >= 120)
	{
		return 120;
	}
	if (upper >= 90)
	{
		return 90;
	}
	if (upper >= 60)
	{
		return 60;
	}
	if (upper >= 30)
	{
		return 30;
	}
	if (upper >= 24)
	{
		return 24;
	}
	return 15;
}