#include "ui_CaptureWindow.h"

#include <fplayer/widget/capturewindow.h>
#include <fplayer/service/service.h>
#include <fplayer/widget/fvideoview.h>

#include <QVideoWidget>
#include <QVBoxLayout>
#include <QCamera>
#include <QMediaCaptureSession>
#include <QMediaDevices>
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
#include <QToolButton>
#include <QFileInfo>
#include <QResizeEvent>

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
	auto* actionGroup = new QActionGroup(this);
	actionGroup->setExclusive(true);
	auto* actionCameraMode = modeMenu->addAction(tr("摄像头模式"));
	actionCameraMode->setCheckable(true);
	auto* actionFileMode = modeMenu->addAction(tr("文件播放模式"));
	actionFileMode->setCheckable(true);
	actionGroup->addAction(actionCameraMode);
	actionGroup->addAction(actionFileMode);
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

	m_fileProgress = new QSlider(Qt::Horizontal, this);
	m_fileProgress->setMinimum(0);
	m_fileProgress->setMaximum(0);
	m_fileProgress->setVisible(false);
	m_fileProgress->setMinimumWidth(320);
	m_fileProgress->setMaximumWidth(16777215);
	m_fileProgress->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	m_fileProgressLabel = new QLabel(tr("00:00 / 00:00"), this);
	m_fileProgressLabel->setVisible(false);
	m_debugStatsLabel = new QLabel(tr("VQ:0 AQ:0 DropV/2s:0 AudFrm/2s:0 AudKB/2s:0"), this);
	m_debugStatsLabel->setVisible(false);
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
	this->ui->wgtView->setBackendType(backendType);

	// 2) 绑定预览窗口
	m_service->bindCameraPreview(this->ui->wgtView);
	m_service->bindPlayerPreview(this->ui->wgtView);

	// 3) 获取摄像头列表
	auto cameraList = m_service->getCameraList();
	QStringList list;
	list << cameraList;
	this->ui->cmbDevices->addItems(list);

	// 4) 连接信号槽
	// 摄像头变更
	connect(this->ui->cmbDevices, &QComboBox::currentIndexChanged, [this](int index) {
		if (index < 0)
		{
			return;
		}

		this->m_service->selectCamera(index);
		QStringList formats(this->m_service->getCameraFormats(index));
		this->ui->cmbFormats->clear();
		this->ui->cmbFormats->addItems(formats);
		this->ui->cmbFormats->setCurrentIndex(0);

	});

	// 摄像头格式变更
	connect(this->ui->cmbFormats, &QComboBox::currentIndexChanged, [this](int index) {
		if (index < 0)
		{
			return;
		}

		this->m_service->selectCameraFormat(index);

	});


	// 5) 选择第一个摄像头（此时预览已经设置好了）
	if (!list.isEmpty())
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
		this->ui->wgtDevices->setVisible(true);
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
		this->ui->btnPlay->setIcon(QIcon::fromTheme(
			this->m_service->cameraIsPlaying() ? QIcon::ThemeIcon::MediaPlaybackPause : QIcon::ThemeIcon::MediaPlaybackStart));
	};
	auto switchToFileMode = [this]() -> bool {
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
	if (m_isFileMode)
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