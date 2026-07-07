#include "ui_capturewindow.h"

#include <fplayer/common/designtokens.h>
#include <fplayer/common/version.h>
#include <fplayer/widget/capturewindow.h>
#include <fplayer/widget/imagepoolsidebar.h>
#include <fplayer/widget/aichatdialog.h>
#include <fplayer/service/service.h>
#include <fplayer/widget/fvideoview.h>
#include <fplayer/common/fglwidget/fglwidget.h>
#include <fplayer/common/screenframebus/screenframebus.h>
#include <fplayer/common/cameraframebus/cameraframebus.h>

#include <QVBoxLayout>
#include <QCamera>
#include <QMediaDevices>
#include <QAudioDevice>
#include <logger/logger.h>
#include <QDebug>
#include <qicon.h>
#include <QApplication>
#include <QCoreApplication>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QAbstractSpinBox>
#include <QShortcut>
#include <QEventLoop>
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
#include <QScrollArea>
#include <QFormLayout>
#include <QPushButton>
#include <QMessageBox>
#include <QSystemTrayIcon>
#include <QSpinBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QRegularExpression>
#include <QStandardItemModel>
#include <QSplitter>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QListWidget>
#include <QFrame>
#include <QAction>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QEvent>
#include <QRubberBand>
#include <QToolTip>
#include <QWindow>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QUuid>
#include <QClipboard>
#include <QColorDialog>
#include <QImage>
#include <QTcpServer>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrl>
#include <QUrlQuery>
#include <QPointer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QCloseEvent>
#include <QWindowStateChangeEvent>
#include <QThread>
#include <functional>
#include <thread>
#include <algorithm>
#include <iterator>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#endif

namespace
{
// ponytail: shared FPS candidate list, was duplicated in syncComposeControlPanel + refreshScreenFpsUi
static const QList<int> kFpsCandidates{15, 24, 25, 30, 45, 50, 60, 75, 90, 100, 120, 144, 165, 180, 200, 240};

class PullPreviewDialog final : public QDialog
{
public:
	explicit PullPreviewDialog(QWidget* parent = nullptr) : QDialog(parent) {}
	std::function<bool()> beforeClose;
	std::function<void(Qt::WindowStates oldState, Qt::WindowStates newState)> onWindowStateChanged;

protected:
	void closeEvent(QCloseEvent* event) override
	{
		if (beforeClose && !beforeClose())
		{
			event->ignore();
			return;
		}
		QDialog::closeEvent(event);
	}

	void changeEvent(QEvent* event) override
	{
		if (event->type() == QEvent::WindowStateChange && onWindowStateChanged)
		{
			auto* stateEvent = static_cast<QWindowStateChangeEvent*>(event);
			onWindowStateChanged(stateEvent->oldState(), windowState());
		}
		QDialog::changeEvent(event);
	}
};

int choosePullListenPort(const int preferredPort)
{
	auto canBindLocalPort = [](const int port) -> bool {
		if (port <= 0 || port > 65535)
		{
			return false;
		}
		QTcpServer probe;
		return probe.listen(QHostAddress::LocalHost, static_cast<quint16>(port));
	};
	if (canBindLocalPort(preferredPort))
	{
		return preferredPort;
	}
	for (int candidate = preferredPort + 1; candidate <= preferredPort + 200; ++candidate)
	{
		if (canBindLocalPort(candidate))
		{
			return candidate;
		}
	}
	QTcpServer probe;
	if (probe.listen(QHostAddress::LocalHost, 0))
	{
		return static_cast<int>(probe.serverPort());
	}
	return preferredPort;
}

QString selectLanHostForPublish()
{
	const QList<QHostAddress> all = QNetworkInterface::allAddresses();
	for (const QHostAddress& addr : all)
	{
		if (addr.protocol() != QAbstractSocket::IPv4Protocol)
		{
			continue;
		}
		if (addr == QHostAddress::LocalHost)
		{
			continue;
		}
		if (addr.isNull())
		{
			continue;
		}
		const QString ip = addr.toString();
		if (ip.startsWith(QStringLiteral("169.254.")))
		{
			continue;
		}
		return ip;
	}
	return QStringLiteral("127.0.0.1");
}

QStringList collectLanIpv4List()
{
	QStringList list;
	const QList<QHostAddress> all = QNetworkInterface::allAddresses();
	for (const QHostAddress& addr : all)
	{
		if (addr.protocol() != QAbstractSocket::IPv4Protocol)
		{
			continue;
		}
		if (addr == QHostAddress::LocalHost || addr.isNull())
		{
			continue;
		}
		const QString ip = addr.toString();
		if (ip.startsWith(QStringLiteral("169.254.")))
		{
			continue;
		}
		list << ip;
	}
	list.removeDuplicates();
	if (list.isEmpty())
	{
		list << QStringLiteral("127.0.0.1");
	}
	return list;
}

void showNonBlockingHint(QWidget* anchor, const QString& text, int durationMs = 2200)
{
	if (!anchor)
	{
		return;
	}
	auto* toast = new QLabel(text);
	toast->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
	toast->setAttribute(Qt::WA_DeleteOnClose, true);
	toast->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	toast->setWordWrap(true);
	toast->setMargin(10);
	toast->setStyleSheet(QStringLiteral(
		"QLabel{background:rgba(30,30,32,0.95);color:#f5f5f7;border:1px solid #2a2a2c;border-radius:12px;font-weight:600;}"));
	toast->setMinimumWidth(320);
	toast->setMaximumWidth(560);
	toast->adjustSize();
	const QRect anchorRect = anchor->rect();
	const QPoint topRight = anchor->mapToGlobal(anchorRect.topRight());
	const int x = topRight.x() - toast->width() - 18;
	const int y = topRight.y() + 18;
	toast->move(x, y);
	toast->show();
	QTimer::singleShot(durationMs, toast, &QWidget::close);
}

// ponytail: qBound(0, v, 255) covers this

QImage i420ToImage(const QByteArray& yPlane,
                   const QByteArray& uPlane,
                   const QByteArray& vPlane,
                   const int width,
                   const int height,
                   const int yStride,
                   const int uStride,
                   const int vStride)
{
	if (width <= 0 || height <= 0 || yStride <= 0 || uStride <= 0 || vStride <= 0)
	{
		return {};
	}
	const int uvW = (width + 1) / 2;
	const int uvH = (height + 1) / 2;
	if (yPlane.size() < yStride * height || uPlane.size() < uStride * uvH || vPlane.size() < vStride * uvH)
	{
		return {};
	}
	QImage image(width, height, QImage::Format_RGB888);
	if (image.isNull())
	{
		return {};
	}
	const auto* yData = reinterpret_cast<const quint8*>(yPlane.constData());
	const auto* uData = reinterpret_cast<const quint8*>(uPlane.constData());
	const auto* vData = reinterpret_cast<const quint8*>(vPlane.constData());
	for (int yy = 0; yy < height; ++yy)
	{
		uchar* rgb = image.scanLine(yy);
		const int uvY = yy / 2;
		for (int xx = 0; xx < width; ++xx)
		{
			const int uvX = xx / 2;
			const int Y = yData[yy * yStride + xx];
			const int U = uData[uvY * uStride + uvX];
			const int V = vData[uvY * vStride + uvX];
			const int c = Y - 16;
			const int d = U - 128;
			const int e = V - 128;
			const int r = (298 * c + 409 * e + 128) >> 8;
			const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
			const int b = (298 * c + 516 * d + 128) >> 8;
			rgb[xx * 3 + 0] = static_cast<uchar>(qBound(0, r, 255));
			rgb[xx * 3 + 1] = static_cast<uchar>(qBound(0, g, 255));
			rgb[xx * 3 + 2] = static_cast<uchar>(qBound(0, b, 255));
		}
	}
	return image;
}


bool requestServiceStreamStart(const QString& gatewayBaseUrl,
                               const QString& app,
                               const QString& stream,
                               const QString& serviceMode,
                               const QJsonObject& publisherMeta,
                               const QJsonObject& sourceMeta,
                               QString& publishRtmp,
                               QString& playHttpFlv,
                               QString& streamId,
                               QString& error)
{
	const QString base = gatewayBaseUrl.trimmed();
	if (base.isEmpty())
	{
		error = QStringLiteral("服务地址为空");
		return false;
	}
	QUrl requestUrl(base);
	if (!requestUrl.isValid())
	{
		error = QStringLiteral("服务地址格式无效");
		return false;
	}
	auto buildApiPath = [](const QString& basePath, const QString& apiSuffix) {
		QString path = basePath.trimmed();
		if (path.isEmpty())
		{
			path = QStringLiteral("/");
		}
		if (!path.startsWith(QLatin1Char('/')))
		{
			path.prepend(QLatin1Char('/'));
		}
		while (path.endsWith(QLatin1Char('/')))
		{
			path.chop(1);
		}
		const QString prefix = QStringLiteral("/api/v1");
		if (path == prefix || path.endsWith(prefix))
		{
			return path + apiSuffix;
		}
		if (path == QStringLiteral("/"))
		{
			return prefix + apiSuffix;
		}
		return path + prefix + apiSuffix;
	};
	requestUrl.setPath(buildApiPath(requestUrl.path(), QStringLiteral("/streams/start")));

	QNetworkRequest req(requestUrl);
	req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

	QJsonObject body{
		{QStringLiteral("app"), app.trimmed().isEmpty() ? QStringLiteral("live") : app.trimmed()},
		{QStringLiteral("stream"), stream.trimmed().isEmpty() ? QStringLiteral("stream001") : stream.trimmed()},
		{QStringLiteral("serviceMode"), serviceMode.trimmed().isEmpty() ? QStringLiteral("httpflv") : serviceMode.trimmed()},
		{QStringLiteral("publisherMeta"), publisherMeta},
		{QStringLiteral("sourceMeta"), sourceMeta}
	};

	QNetworkAccessManager manager;
	QNetworkReply* reply = manager.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
	QEventLoop loop;
	QTimer timeout;
	timeout.setSingleShot(true);
	timeout.setInterval(5000);
	QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
		error = QStringLiteral("请求服务超时（5s）");
		if (reply)
		{
			reply->abort();
		}
		loop.quit();
	});
	QObject::connect(reply, &QNetworkReply::finished, &loop, [&]() {
		loop.quit();
	});
	timeout.start();
	loop.exec();

	if (!timeout.isActive())
	{
		reply->deleteLater();
		return false;
	}
	timeout.stop();

	if (reply->error() != QNetworkReply::NoError)
	{
		error = QStringLiteral("请求服务失败: %1").arg(reply->errorString());
		reply->deleteLater();
		return false;
	}

	const QByteArray data = reply->readAll();
	reply->deleteLater();
	QJsonParseError parseError;
	const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject())
	{
		error = QStringLiteral("服务返回解析失败");
		return false;
	}
	const QJsonObject obj = doc.object();
	publishRtmp = obj.value(QStringLiteral("publishRtmp")).toString().trimmed();
	playHttpFlv = obj.value(QStringLiteral("playHttpFlv")).toString().trimmed();
	streamId = obj.value(QStringLiteral("id")).toString().trimmed();
	if (playHttpFlv.isEmpty() && obj.value(QStringLiteral("playUrls")).isObject())
	{
		playHttpFlv = obj.value(QStringLiteral("playUrls")).toObject().value(QStringLiteral("httpFlv")).toString().trimmed();
	}
	if (publishRtmp.isEmpty())
	{
		error = QStringLiteral("服务未返回 RTMP 推流地址");
		return false;
	}
	return true;
}

bool requestServiceStreamStatus(const QString& gatewayBaseUrl,
                                const QString& app,
                                const QString& stream,
                                QString& preferredPullUrl,
                                QString& playHttpFlv,
                                QString& playRtmp,
                                QString& error)
{
	const QString base = gatewayBaseUrl.trimmed();
	const QString appName = app.trimmed();
	const QString streamName = stream.trimmed();
	if (base.isEmpty())
	{
		error = QStringLiteral("服务地址为空");
		return false;
	}
	if (appName.isEmpty() || streamName.isEmpty())
	{
		error = QStringLiteral("app 和 stream 不能为空");
		return false;
	}
	QUrl requestUrl(base);
	if (!requestUrl.isValid())
	{
		error = QStringLiteral("服务地址格式无效");
		return false;
	}
	auto buildApiPath = [](const QString& basePath, const QString& apiSuffix) {
		QString path = basePath.trimmed();
		if (path.isEmpty())
		{
			path = QStringLiteral("/");
		}
		if (!path.startsWith(QLatin1Char('/')))
		{
			path.prepend(QLatin1Char('/'));
		}
		while (path.endsWith(QLatin1Char('/')))
		{
			path.chop(1);
		}
		const QString prefix = QStringLiteral("/api/v1");
		if (path == prefix || path.endsWith(prefix))
		{
			return path + apiSuffix;
		}
		if (path == QStringLiteral("/"))
		{
			return prefix + apiSuffix;
		}
		return path + prefix + apiSuffix;
	};
	requestUrl.setPath(buildApiPath(requestUrl.path(), QStringLiteral("/streams/resolve")));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("app"), appName);
	query.addQueryItem(QStringLiteral("stream"), streamName);
	requestUrl.setQuery(query);

	QNetworkAccessManager manager;
	QNetworkReply* reply = manager.get(QNetworkRequest(requestUrl));
	QEventLoop loop;
	QTimer timeout;
	timeout.setSingleShot(true);
	timeout.setInterval(5000);
	QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
		error = QStringLiteral("请求服务超时（5s）");
		if (reply)
		{
			reply->abort();
		}
		loop.quit();
	});
	QObject::connect(reply, &QNetworkReply::finished, &loop, [&]() {
		loop.quit();
	});
	timeout.start();
	loop.exec();

	if (!timeout.isActive())
	{
		reply->deleteLater();
		return false;
	}
	timeout.stop();

	if (reply->error() != QNetworkReply::NoError)
	{
		error = QStringLiteral("请求服务失败: %1").arg(reply->errorString());
		reply->deleteLater();
		return false;
	}
	const QByteArray data = reply->readAll();
	reply->deleteLater();
	QJsonParseError parseError;
	const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject())
	{
		error = QStringLiteral("服务返回解析失败");
		return false;
	}
	const QJsonObject obj = doc.object();
	playHttpFlv = obj.value(QStringLiteral("playHttpFlv")).toString().trimmed();
	playRtmp = obj.value(QStringLiteral("publishRtmp")).toString().trimmed();
	if (obj.value(QStringLiteral("playUrls")).isObject())
	{
		const QJsonObject urls = obj.value(QStringLiteral("playUrls")).toObject();
		if (playHttpFlv.isEmpty())
		{
			playHttpFlv = urls.value(QStringLiteral("httpFlv")).toString().trimmed();
		}
		if (playRtmp.isEmpty())
		{
			playRtmp = urls.value(QStringLiteral("rtmp")).toString().trimmed();
		}
	}
	preferredPullUrl = !playHttpFlv.isEmpty() ? playHttpFlv : playRtmp;
	if (preferredPullUrl.isEmpty())
	{
		error = QStringLiteral("服务未返回可用拉流地址");
		return false;
	}
	return true;
}

void syncStreamLogView(QTextEdit* logView, const QString& latestLog)
{
	if (!logView)
	{
		return;
	}
	// 用户正在选择文本时不重刷，避免复制被打断。
	if (logView->textCursor().hasSelection())
	{
		return;
	}
	const QString currentLog = logView->toPlainText();
	if (latestLog.startsWith(currentLog))
	{
		if (latestLog.size() > currentLog.size())
		{
			logView->moveCursor(QTextCursor::End);
			logView->insertPlainText(latestLog.mid(currentLog.size()));
			logView->moveCursor(QTextCursor::End);
		}
	}
	else if (currentLog != latestLog)
	{
		// 日志被清空或滚动裁剪时，回退到整段同步一次。
		logView->setPlainText(latestLog);
		logView->moveCursor(QTextCursor::End);
	}
}

class AspectRatioHostWidget final : public QWidget
{
public:
	explicit AspectRatioHostWidget(QWidget* parent = nullptr) : QWidget(parent)
	{
		setObjectName(QStringLiteral("composePreviewHost"));
		setAttribute(Qt::WA_StyledBackground, true);
	}

	void setAspectRatio(const int w, const int h)
	{
		if (w <= 0 || h <= 0)
		{
			return;
		}
		m_aspectW = w;
		m_aspectH = h;
		updateContentGeometry();
	}

	void attachContent(QWidget* content)
	{
		m_content = content;
		if (m_content)
		{
			m_content->setParent(this);
			m_content->show();
		}
		updateContentGeometry();
	}
	std::function<void(const QRect& oldRect, const QRect& newRect)> onContentGeometryChanged;

protected:
	void resizeEvent(QResizeEvent* event) override
	{
		QWidget::resizeEvent(event);
		updateContentGeometry();
	}

private:
	void updateContentGeometry()
	{
		if (!m_content || m_aspectW <= 0 || m_aspectH <= 0)
		{
			return;
		}
		const QRect r = rect();
		if (r.width() <= 0 || r.height() <= 0)
		{
			return;
		}
		const double ratio = static_cast<double>(m_aspectW) / static_cast<double>(m_aspectH);
		int cw = r.width();
		int ch = static_cast<int>(cw / ratio);
		if (ch > r.height())
		{
			ch = r.height();
			cw = static_cast<int>(ch * ratio);
		}
		const int x = (r.width() - cw) / 2;
		const int y = (r.height() - ch) / 2;
		const QRect nextRect(x, y, qMax(1, cw), qMax(1, ch));
		const QRect oldRect = m_content->geometry();
		if (oldRect == nextRect)
		{
			return;
		}
		m_content->setGeometry(nextRect);
		if (onContentGeometryChanged)
		{
			onContentGeometryChanged(oldRect, nextRect);
		}
	}

	int m_aspectW = 16;
	int m_aspectH = 9;
	QWidget* m_content = nullptr;
};

class ComposeSourceWidget final : public QWidget
{
public:
	enum class DragMode
	{
		None,
		Move,
		ResizeLeft,
		ResizeRight,
		ResizeTop,
		ResizeBottom,
		ResizeTopLeft,
		ResizeTopRight,
		ResizeBottomLeft,
		ResizeBottomRight
	};

	explicit ComposeSourceWidget(QWidget* parent = nullptr) : QWidget(parent)
	{
		setObjectName(QStringLiteral("composeSourceItem"));
		setMouseTracking(true);
		setAttribute(Qt::WA_StyledBackground, true);
		setProperty("composeState", QStringLiteral("normal"));
		auto* layout = new QHBoxLayout(this);
		layout->setContentsMargins(2, 2, 2, 2);
		layout->setSpacing(0);
	}

	void setInnerView(fplayer::FVideoView* view)
	{
		m_view = view;
		if (!layout() || !view)
		{
			return;
		}
		view->installEventFilter(this);
		layout()->addWidget(view);
	}

	void setSelected(const bool selected)
	{
		if (m_selected == selected)
		{
			return;
		}
		m_selected = selected;
		applyVisualStyle();
		update();
	}

	void setCropMode(const bool enabled)
	{
		if (m_cropMode == enabled)
		{
			return;
		}
		m_cropMode = enabled;
		applyVisualStyle();
		updateCursorForPosition(mapFromGlobal(QCursor::pos()));
		update();
	}

	void setAspectResizeEnabled(const bool enabled)
	{
		m_aspectResizeEnabled = enabled;
	}
	static void setUseRubberBandDrag(bool on) { s_useRubberBandDrag = on; }
	static bool useRubberBandDrag() { return s_useRubberBandDrag; }

	bool isDragging() const
	{
		return m_dragInProgress;
	}

	std::function<void()> onSelected;
	std::function<void(const QPoint&)> onContextMenu;
	std::function<void()> onCropFinished;
	std::function<void()> onDragFinished;

protected:
	void paintEvent(QPaintEvent* event) override
	{
		QWidget::paintEvent(event);
		QPainter p(this);
		const QColor border = m_cropMode ? QColor(255, 170, 0) : (m_selected ? QColor(0, 170, 255) : QColor(110, 110, 110));
		const int w = m_selected || m_cropMode ? 2 : 1;
		p.setPen(QPen(border, w));
		p.setBrush(Qt::NoBrush);
		p.drawRect(rect().adjusted(0, 0, -1, -1));
	}

	void contextMenuEvent(QContextMenuEvent* event) override
	{
		if (onContextMenu)
		{
			onContextMenu(event->globalPos());
		}
		event->accept();
	}

	void mousePressEvent(QMouseEvent* event) override
	{
		if (event->button() != Qt::LeftButton)
		{
			QWidget::mousePressEvent(event);
			return;
		}
		if (onSelected)
		{
			onSelected();
		}
		m_dragMode = detectDragMode(event->pos());
		m_dragInProgress = (m_dragMode != DragMode::None);
		m_dragOriginGlobal = event->globalPosition().toPoint();
		m_originGeometry = currentSubWindowGeometry();
		m_previewGeometry = m_originGeometry;
		m_dragChanged = false;
		if (m_dragMode != DragMode::None)
		{
			if (auto* sub = subWindow())
			{
				if (!m_vGuideBand)
				{
					m_vGuideBand = new QRubberBand(QRubberBand::Rectangle, sub->parentWidget());
					m_vGuideBand->setStyleSheet(QStringLiteral("background:rgba(41,151,255,0.25);border:none;"));
				}
				if (!m_hGuideBand)
				{
					m_hGuideBand = new QRubberBand(QRubberBand::Rectangle, sub->parentWidget());
					m_hGuideBand->setStyleSheet(QStringLiteral("background:rgba(41,151,255,0.25);border:none;"));
				}
				m_vGuideBand->hide();
				m_hGuideBand->hide();
				if (s_useRubberBandDrag)
				{
					if (!m_dragRubberBand)
					{
						m_dragRubberBand = new QRubberBand(QRubberBand::Rectangle, sub->parentWidget());
					}
					m_dragRubberBand->setGeometry(sub->geometry());
					m_dragRubberBand->show();
				}
			}
		}
		event->accept();
	}

	void mouseMoveEvent(QMouseEvent* event) override
	{
		updateCursorForPosition(event->pos());
		if (!(event->buttons() & Qt::LeftButton) || m_dragMode == DragMode::None)
		{
			QWidget::mouseMoveEvent(event);
			return;
		}
		const QPoint delta = event->globalPosition().toPoint() - m_dragOriginGlobal;
		QRect g = m_originGeometry;
		const int minW = 120;
		const int minH = 90;
		auto applyResize = [&](const bool left, const bool right, const bool top, const bool bottom) {
			if (left)
			{
				g.setLeft(g.left() + delta.x());
				if (g.width() < minW)
				{
					g.setLeft(g.right() - minW + 1);
				}
			}
			if (right)
			{
				g.setRight(g.right() + delta.x());
				if (g.width() < minW)
				{
					g.setRight(g.left() + minW - 1);
				}
			}
			if (top)
			{
				g.setTop(g.top() + delta.y());
				if (g.height() < minH)
				{
					g.setTop(g.bottom() - minH + 1);
				}
			}
			if (bottom)
			{
				g.setBottom(g.bottom() + delta.y());
				if (g.height() < minH)
				{
					g.setBottom(g.top() + minH - 1);
				}
			}
		};
		switch (m_dragMode)
		{
		case DragMode::Move:
			g.moveTopLeft(m_originGeometry.topLeft() + delta);
			break;
		case DragMode::ResizeLeft:
			applyResize(true, false, false, false);
			break;
		case DragMode::ResizeRight:
			applyResize(false, true, false, false);
			break;
		case DragMode::ResizeTop:
			applyResize(false, false, true, false);
			break;
		case DragMode::ResizeBottom:
			applyResize(false, false, false, true);
			break;
		case DragMode::ResizeTopLeft:
			applyResize(true, false, true, false);
			break;
		case DragMode::ResizeTopRight:
			applyResize(false, true, true, false);
			break;
		case DragMode::ResizeBottomLeft:
			applyResize(true, false, false, true);
			break;
		case DragMode::ResizeBottomRight:
			applyResize(false, true, false, true);
			break;
		case DragMode::None:
			break;
		}
		if (m_aspectResizeEnabled && m_dragMode != DragMode::Move && m_dragMode != DragMode::None)
		{
			const double ratio = m_originGeometry.height() > 0
				                     ? static_cast<double>(m_originGeometry.width()) / static_cast<double>(m_originGeometry.height())
				                     : 1.0;
			const QPoint anchor = [&]() -> QPoint {
				switch (m_dragMode)
				{
				case DragMode::ResizeLeft:
				case DragMode::ResizeTopLeft:
				case DragMode::ResizeBottomLeft:
					return QPoint(m_originGeometry.right(), m_originGeometry.center().y());
				case DragMode::ResizeRight:
				case DragMode::ResizeTopRight:
				case DragMode::ResizeBottomRight:
					return QPoint(m_originGeometry.left(), m_originGeometry.center().y());
				case DragMode::ResizeTop:
					return QPoint(m_originGeometry.center().x(), m_originGeometry.bottom());
				case DragMode::ResizeBottom:
					return QPoint(m_originGeometry.center().x(), m_originGeometry.top());
				default:
					return m_originGeometry.center();
				}
			}();
			int newW = qMax(minW, g.width());
			int newH = qMax(minH, static_cast<int>(newW / ratio));
			if (newH < minH)
			{
				newH = minH;
				newW = qMax(minW, static_cast<int>(newH * ratio));
			}
			QRect ar(QPoint(0, 0), QSize(newW, newH));
			if (m_dragMode == DragMode::ResizeLeft || m_dragMode == DragMode::ResizeTopLeft || m_dragMode == DragMode::ResizeBottomLeft)
			{
				ar.moveRight(anchor.x());
			}
			else if (m_dragMode == DragMode::ResizeRight || m_dragMode == DragMode::ResizeTopRight ||
			         m_dragMode == DragMode::ResizeBottomRight)
			{
				ar.moveLeft(anchor.x());
			}
			else
			{
				ar.moveCenter(g.center());
			}
			if (m_dragMode == DragMode::ResizeTop || m_dragMode == DragMode::ResizeTopLeft || m_dragMode == DragMode::ResizeTopRight)
			{
				ar.moveBottom(m_originGeometry.bottom());
			}
			else if (m_dragMode == DragMode::ResizeBottom || m_dragMode == DragMode::ResizeBottomLeft ||
			         m_dragMode == DragMode::ResizeBottomRight)
			{
				ar.moveTop(m_originGeometry.top());
			}
			g = ar;
		}
		applyMoveSnapAndGuides(g);
		m_previewGeometry = g;
		if (s_useRubberBandDrag && m_dragRubberBand && m_dragRubberBand->isVisible())
		{
			m_dragRubberBand->setGeometry(g);
			m_dragChanged = true;
		}
		else if (auto* sub = subWindow())
		{
			sub->setUpdatesEnabled(false);
			sub->setGeometry(g);
			sub->setUpdatesEnabled(true);
			m_dragChanged = true;
		}
		static qint64 lastTooltipMs = 0;
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		if (now - lastTooltipMs >= 180) {
			QToolTip::showText(event->globalPosition().toPoint(),
			                   QStringLiteral("%1 x %2").arg(g.width()).arg(g.height()),
			                   this);
			lastTooltipMs = now;
		}
		event->accept();
	}

	void mouseReleaseEvent(QMouseEvent* event) override
	{
		const bool shouldFinishCrop = m_cropMode && m_dragChanged;
		const bool shouldNotifyDragFinished = m_dragChanged;
		if (s_useRubberBandDrag && m_dragRubberBand && m_dragRubberBand->isVisible())
		{
			if (auto* sub = subWindow())
			{
				sub->setGeometry(m_dragRubberBand->geometry());
			}
			m_dragRubberBand->hide();
		}
		if (auto* sub = subWindow())
		{
			// 拉伸后强制刷新内部控件尺寸
			resize(sub->contentsRect().size());
			if (layout())
			{
				layout()->update();
				layout()->activate();
			}
			if (m_view)
			{
				m_view->update();
				m_view->repaint();
			}
		}
		if (m_vGuideBand)
		{
			m_vGuideBand->hide();
		}
		if (m_hGuideBand)
		{
			m_hGuideBand->hide();
		}
		QToolTip::hideText();
		m_dragMode = DragMode::None;
		m_dragInProgress = false;
		m_dragChanged = false;
		updateCursorForPosition(event->pos());
		QWidget::mouseReleaseEvent(event);
		if (shouldFinishCrop && onCropFinished)
		{
			onCropFinished();
		}
		if (shouldNotifyDragFinished && onDragFinished)
		{
			onDragFinished();
		}
	}

private:
	bool eventFilter(QObject* watched, QEvent* event) override
	{
		if (watched == m_view)
		{
			if (event->type() == QEvent::MouseButtonPress)
			{
				if (onSelected)
				{
					onSelected();
				}
			}
			else if (event->type() == QEvent::ContextMenu)
			{
				const QPoint globalPos = QCursor::pos();
				if (onContextMenu)
				{
					onContextMenu(globalPos);
					return true;
				}
			}
		}
		return QWidget::eventFilter(watched, event);
	}

	QMdiSubWindow* subWindow() const
	{
		return qobject_cast<QMdiSubWindow*>(parentWidget());
	}

	QRect currentSubWindowGeometry() const
	{
		if (auto* sub = subWindow())
		{
			return sub->geometry();
		}
		return geometry();
	}

	DragMode detectDragMode(const QPoint& pos) const
	{
		constexpr int kHit = 8;
		const QRect r = rect();
		const bool left = pos.x() <= kHit;
		const bool right = pos.x() >= r.width() - kHit;
		const bool top = pos.y() <= kHit;
		const bool bottom = pos.y() >= r.height() - kHit;
		if (top && left)
		{
			return DragMode::ResizeTopLeft;
		}
		if (top && right)
		{
			return DragMode::ResizeTopRight;
		}
		if (bottom && left)
		{
			return DragMode::ResizeBottomLeft;
		}
		if (bottom && right)
		{
			return DragMode::ResizeBottomRight;
		}
		if (left)
		{
			return DragMode::ResizeLeft;
		}
		if (right)
		{
			return DragMode::ResizeRight;
		}
		if (top)
		{
			return DragMode::ResizeTop;
		}
		if (bottom)
		{
			return DragMode::ResizeBottom;
		}
		return DragMode::Move;
	}

	void updateCursorForPosition(const QPoint& pos)
	{
		switch (detectDragMode(pos))
		{
		case DragMode::ResizeLeft:
		case DragMode::ResizeRight:
			setCursor(Qt::SizeHorCursor);
			break;
		case DragMode::ResizeTop:
		case DragMode::ResizeBottom:
			setCursor(Qt::SizeVerCursor);
			break;
		case DragMode::ResizeTopLeft:
		case DragMode::ResizeBottomRight:
			setCursor(Qt::SizeFDiagCursor);
			break;
		case DragMode::ResizeTopRight:
		case DragMode::ResizeBottomLeft:
			setCursor(Qt::SizeBDiagCursor);
			break;
		case DragMode::Move:
			setCursor(Qt::SizeAllCursor);
			break;
		case DragMode::None:
			unsetCursor();
			break;
		}
	}

	void applyMoveSnapAndGuides(QRect& g)
	{
		if (m_dragMode != DragMode::Move)
		{
			if (m_vGuideBand)
			{
				m_vGuideBand->hide();
			}
			if (m_hGuideBand)
			{
				m_hGuideBand->hide();
			}
			return;
		}
		auto* sub = subWindow();
		if (!sub || !sub->parentWidget())
		{
			return;
		}
		constexpr int snapDist = 8;
		const QRect areaRect = sub->parentWidget()->rect();
		struct SnapHit
		{
			int dist = 999999;
			int delta = 0;
			int guide = -1;
			bool hit = false;
		};
		SnapHit xHit;
		SnapHit yHit;
		auto tryAxis = [](SnapHit& hit, const int from, const int target) {
			const int d = target - from;
			const int ad = qAbs(d);
			if (ad < hit.dist)
			{
				hit.dist = ad;
				hit.delta = d;
				hit.guide = target;
				hit.hit = true;
			}
		};

		const int l = g.left();
		const int r = g.right();
		const int cx = g.center().x();
		const int t = g.top();
		const int b = g.bottom();
		const int cy = g.center().y();

		const QList<int> xTargets{areaRect.left(), areaRect.center().x(), areaRect.right()};
		const QList<int> yTargets{areaRect.top(), areaRect.center().y(), areaRect.bottom()};
		for (const int target : xTargets)
		{
			tryAxis(xHit, l, target);
			tryAxis(xHit, cx, target);
			tryAxis(xHit, r, target);
		}
		for (const int target : yTargets)
		{
			tryAxis(yHit, t, target);
			tryAxis(yHit, cy, target);
			tryAxis(yHit, b, target);
		}

		if (auto* area = sub->mdiArea())
		{
			const auto wins = area->subWindowList(QMdiArea::StackingOrder);
			for (QMdiSubWindow* w : wins)
			{
				if (!w || w == sub)
				{
					continue;
				}
				const QRect wr = w->geometry();
				const QList<int> xs{wr.left(), wr.center().x(), wr.right()};
				const QList<int> ys{wr.top(), wr.center().y(), wr.bottom()};
				for (const int target : xs)
				{
					tryAxis(xHit, l, target);
					tryAxis(xHit, cx, target);
					tryAxis(xHit, r, target);
				}
				for (const int target : ys)
				{
					tryAxis(yHit, t, target);
					tryAxis(yHit, cy, target);
					tryAxis(yHit, b, target);
				}
			}
		}

		if (xHit.hit && xHit.dist <= snapDist)
		{
			g.translate(xHit.delta, 0);
			if (m_vGuideBand)
			{
				m_vGuideBand->setGeometry(xHit.guide, areaRect.top(), 1, areaRect.height());
				m_vGuideBand->show();
			}
		}
		else if (m_vGuideBand)
		{
			m_vGuideBand->hide();
		}
		if (yHit.hit && yHit.dist <= snapDist)
		{
			g.translate(0, yHit.delta);
			if (m_hGuideBand)
			{
				m_hGuideBand->setGeometry(areaRect.left(), yHit.guide, areaRect.width(), 1);
				m_hGuideBand->show();
			}
		}
		else if (m_hGuideBand)
		{
			m_hGuideBand->hide();
		}
	}

	void applyVisualStyle()
	{
		QString state = QStringLiteral("normal");
		if (m_cropMode)
		{
			state = QStringLiteral("crop");
		}
		else if (m_selected)
		{
			state = QStringLiteral("selected");
		}
		setProperty("composeState", state);
		style()->unpolish(this);
		style()->polish(this);
	}

	fplayer::FVideoView* m_view = nullptr;
	bool m_selected = false;
	bool m_cropMode = false;
	DragMode m_dragMode = DragMode::None;
	QRect m_originGeometry;
	QRect m_previewGeometry;
	QPoint m_dragOriginGlobal;
	bool m_dragChanged = false;
	QRubberBand* m_dragRubberBand = nullptr;
		QRubberBand* m_vGuideBand = nullptr;
	QRubberBand* m_hGuideBand = nullptr;
	bool m_aspectResizeEnabled = false;
		static bool s_useRubberBandDrag;
	bool m_dragInProgress = false;
};
bool ComposeSourceWidget::s_useRubberBandDrag = false;

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
	setAttribute(Qt::WA_StyledBackground, true);
	setStyleSheet(fplayer::tokens::globalStyleSheet(fplayer::tokens::Theme::Dark));
	qApp->setStyleSheet(fplayer::tokens::globalStyleSheet(fplayer::tokens::Theme::Dark));
	m_service = new fplayer::Service();
	m_capturePrefPath = m_service ? m_service->systemSettingsPath() : QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("system_settings.yaml"));
	loadCapturePreferences();
		applyTheme();
	m_pullReservedPort = choosePullListenPort(m_pullReservedPort);
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
	auto* actionComposeMode = modeMenu->addAction(tr("组合模式"));
	actionComposeMode->setCheckable(true);
	actionGroup->addAction(actionCameraMode);
	actionGroup->addAction(actionFileMode);
	actionGroup->addAction(actionScreenMode);
	actionGroup->addAction(actionComposeMode);
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
	{
		QFont f = m_fileTitleButton->font();
		f.setWeight(QFont::DemiBold);
		m_fileTitleButton->setFont(f);
	}

	m_titleMarqueeTimer = new QTimer(this);
	m_titleMarqueeTimer->setInterval(180);
	connect(m_titleMarqueeTimer, &QTimer::timeout, this, &CaptureWindow::updateTitleMarqueeText);
	// 防止按钮点击后持有键盘焦点，导致空格再次触发同一按钮 click。
	this->ui->btnPlay->setFocusPolicy(Qt::NoFocus);
	this->ui->btnCut->setFocusPolicy(Qt::NoFocus);
	this->ui->btnCast->setFocusPolicy(Qt::NoFocus);
	this->ui->btnSettings->setFocusPolicy(Qt::NoFocus);
	this->ui->btnFullscreen->setFocusPolicy(Qt::NoFocus);
	this->ui->btnImagePool->setFocusPolicy(Qt::NoFocus);
	this->ui->btnCut->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("camera"))));
	this->ui->btnCast->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("video"))));
	this->ui->btnSettings->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("settings"))));
	this->ui->btnImagePool->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("pictures"))));
	for (auto* btn : {static_cast<QPushButton*>(this->ui->btnCut), static_cast<QPushButton*>(this->ui->btnCast)})
	{
		btn->setIconSize(QSize(24, 24));
	}
	this->ui->chkCaptureCursor->setVisible(false);
	this->ui->cmbScreenFps->setVisible(false);
	m_mainRecordTimer = new QTimer(this);
	m_mainRecordTimer->setInterval(500);
	connect(m_mainRecordTimer, &QTimer::timeout, this, &CaptureWindow::updateRecordButtonUi);
	m_pullRecordTimer = new QTimer(this);
	m_pullRecordTimer->setInterval(500);
	connect(m_pullRecordTimer, &QTimer::timeout, this, &CaptureWindow::updatePullRecordButtonUi);

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
		this->ui->btnFullscreen->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("fullScreen"))));
	};

	// 1) 初始化后端服务
	m_service->initCamera(backendType);
	m_service->initPlayer(m_filePlaybackBackend);
#if !(defined(_WIN32) && defined(FPLAYER_WITH_SCREEN_DXGI))
	if (m_screenBackendType == fplayer::MediaBackendType::Dxgi)
	{
		m_screenBackendType = fplayer::MediaBackendType::FFmpeg;
	}
#endif
	m_service->initScreenCapture(m_screenBackendType);
	m_service->initStream(fplayer::MediaBackendType::FFmpeg);
	setupTrayIcon();

	// 2) 绑定预览窗口（默认摄像头模式）
	this->ui->wgtView->setBackendType(backendType);
	m_service->bindCameraPreview(this->ui->wgtView);

	// 3) 获取摄像头列表并选中第一个
	this->refreshCameraDeviceUi();
	if (this->ui->cmbDevices->count() > 0)
	{
		this->ui->cmbDevices->setCurrentIndex(0);
		const auto formats = this->m_service->getCameraFormats(0);
		this->ui->cmbFormats->clear();
		this->ui->cmbFormats->addItems(formats);
		this->ui->cmbFormats->setCurrentIndex(0);
	}

	// 4) 连接信号槽
	// 摄像头变更
	connect(this->ui->cmbDevices, &QComboBox::currentIndexChanged, [this](int index) {
		if (index < 0)
		{
			return;
		}
		if (m_isComposeMode)
		{
			if (m_composeSelectedIndex < 0 || m_composeSelectedIndex >= m_composeSources.size())
			{
				return;
			}
			auto& src = m_composeSources[m_composeSelectedIndex];
			if (!src.service)
			{
				return;
			}
			if (src.kind == CaptureWindow::ComposeSourceItem::SourceKind::Screen)
			{
				src.deviceIndex = index;
				src.service->screenSetActive(false);
				src.service->selectScreen(index);
				const int fps = ui->cmbScreenFps->currentData().toInt();
				src.screenFps = fps > 0 ? fps : src.screenFps;
				if (fps > 0)
				{
					src.service->screenSetFrameRate(fps);
				}
				src.service->screenSetCursorCaptureEnabled(src.screenCaptureCursor);
				refreshComposeScreenCaptureState(m_composeSelectedIndex);
				const QString name = this->ui->cmbDevices->currentText().trimmed();
				if (src.subWindow)
				{
					src.subWindow->setWindowTitle(tr("屏幕：%1").arg(name.isEmpty() ? tr("未知") : name));
				}
				src.title = src.subWindow ? src.subWindow->windowTitle() : src.title;
				refreshComposeSourceListItems();
				ui->cmbScreenFps->setToolTip(tr("当前帧率：%1 FPS").arg(src.service->screenFrameRate()));
				return;
			}
			if (src.kind == CaptureWindow::ComposeSourceItem::SourceKind::Camera)
			{
				src.deviceIndex = index;
				src.service->selectCamera(index);
				QStringList formats(src.service->getCameraFormats(index));
				this->ui->cmbFormats->clear();
				this->ui->cmbFormats->addItems(formats);
				src.formatIndex = formats.isEmpty() ? -1 : qBound(0, src.formatIndex, formats.size() - 1);
				this->ui->cmbFormats->setCurrentIndex(src.formatIndex);
				const QString name = this->ui->cmbDevices->currentText().trimmed();
				if (src.subWindow)
				{
					src.subWindow->setWindowTitle(tr("摄像头：%1").arg(name.isEmpty() ? tr("未知") : name));
				}
				src.title = src.subWindow ? src.subWindow->windowTitle() : src.title;
				refreshComposeSourceListItems();
			}
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
		this->ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme),
			this->m_service->cameraIsPlaying() ? QStringLiteral("pause") : QStringLiteral("play"))));

	});
	connect(this->ui->chkCaptureCursor, &QCheckBox::toggled, this, [this](const bool checked) {
		if (m_isComposeMode)
		{
			if (m_composeSelectedIndex < 0 || m_composeSelectedIndex >= m_composeSources.size())
			{
				return;
			}
			auto& src = m_composeSources[m_composeSelectedIndex];
			if (src.kind != CaptureWindow::ComposeSourceItem::SourceKind::Screen || !src.service)
			{
				return;
			}
			src.screenCaptureCursor = checked;
			src.service->screenSetCursorCaptureEnabled(checked);
			return;
		}
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
		if (index < 0)
		{
			return;
		}
		if (m_isComposeMode)
		{
			if (m_composeSelectedIndex < 0 || m_composeSelectedIndex >= m_composeSources.size())
			{
				return;
			}
			auto& src = m_composeSources[m_composeSelectedIndex];
			if (src.kind != CaptureWindow::ComposeSourceItem::SourceKind::Screen || !src.service)
			{
				return;
			}
			src.screenFps = this->ui->cmbScreenFps->itemData(index).toInt();
			src.service->screenSetFrameRate(src.screenFps);
			ui->cmbScreenFps->setToolTip(tr("当前帧率：%1 FPS").arg(src.service->screenFrameRate()));
			return;
		}
		if (m_captureMode != CaptureMode::Screen)
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
		if (index < 0)
		{
			return;
		}
		if (m_isComposeMode)
		{
			if (m_composeSelectedIndex < 0 || m_composeSelectedIndex >= m_composeSources.size())
			{
				return;
			}
			auto& src = m_composeSources[m_composeSelectedIndex];
			if (src.kind == CaptureWindow::ComposeSourceItem::SourceKind::Camera && src.service)
			{
				src.formatIndex = index;
				src.service->selectCameraFormat(index);
			}
			return;
		}
		if (m_captureMode != CaptureMode::Camera)
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
	this->ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme),
		this->m_service->cameraIsPlaying() ? QStringLiteral("pause") : QStringLiteral("play"))));

	connect(this->ui->btnPlay, &QPushButton::clicked, [this]() {
		this->togglePlayPause();
	});
	connect(this->ui->btnCut, &QPushButton::clicked, this, &CaptureWindow::handleMainCaptureScreenshot);
	connect(this->ui->btnCast, &QPushButton::clicked, this, &CaptureWindow::handleMainCaptureRecordToggle);
	connect(this->ui->btnSettings, &QPushButton::clicked, this, &CaptureWindow::handleMainCaptureSettings);


		// 图池独立窗体
		m_imagePoolSidebar = new ImagePoolSidebar(nullptr);
		m_imagePoolSidebar->setScreenshotDir(m_screenshotSaveDir);
		m_imagePoolSidebar->setWindowTitle(tr("图池"));
		connect(this->ui->btnImagePool, &QPushButton::clicked, [this]() {
			if (m_imagePoolSidebar->isVisible())
			{
				if (m_imagePoolSidebar->isMinimized())
					m_imagePoolSidebar->showNormal();
				m_imagePoolSidebar->raise();
				m_imagePoolSidebar->activateWindow();
			}
			else
			{
				m_imagePoolSidebar->show();
			}
		});
		connect(this, &CaptureWindow::screenshotSaved,
		        m_imagePoolSidebar, &ImagePoolSidebar::onScreenshotSaved);
		// 菜单栏"视图 → 图池"同步
		auto* viewMenu = m_modeMenuBar->addMenu(tr("视图"));
		auto* actionImagePool = viewMenu->addAction(tr("图池"));
		actionImagePool->setCheckable(true);
		connect(actionImagePool, &QAction::toggled, [this](bool checked) {
			if (checked)
				m_imagePoolSidebar->show();
			else
				m_imagePoolSidebar->hide();
		});
		connect(m_imagePoolSidebar, &ImagePoolSidebar::visibilityChanged,
		        actionImagePool, &QAction::setChecked);
		connect(m_imagePoolSidebar, &ImagePoolSidebar::analyzeRequested, [this](const QString& path) {
			auto* dlg = new AiChatDialog(nullptr);
			dlg->setAttribute(Qt::WA_DeleteOnClose);
			fplayer::AiConfig cfg;
			cfg.endpoint = m_aiEndpoint;
			cfg.apiKey = m_aiApiKey;
			cfg.model = m_aiModel;
		cfg.userBubbleColor = m_aiUserBubbleColor;
		cfg.aiBubbleColor = m_aiAiBubbleColor;
		cfg.chatBgColor = m_aiChatBgColor;
				cfg.aiTextColor = m_aiTextColor;
				cfg.userTextColor = m_userTextColor;
		cfg.fontFamily = m_aiFontFamily;
		cfg.fontSize = m_aiFontSize;
			dlg->startChat(path, cfg);
		});

		auto switchToCameraMode = [this]() {
		setComposeMode(false);
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
		this->m_service->playerStop();
		this->m_service->playerSetPlaybackRate(1.0);
		this->m_speedCombo->setCurrentIndex(0);
		m_currentFileTitle.clear();
		m_currentFilePath.clear();
		if (m_titleMarqueeTimer)
		{
			m_titleMarqueeTimer->stop();
		}
		updateTitleMarqueeText();
		this->ui->wgtView->setBackendType(m_cameraBackendType);
		this->m_service->bindCameraPreview(this->ui->wgtView);
		this->refreshCameraDeviceUi();
		if (this->ui->cmbDevices->count() > 0)
		{
			this->ui->cmbDevices->setCurrentIndex(0);
			// 手动填充分辨率列表：因 refreshCameraDeviceUi 内部阻塞信号添加设备项时，
			// Qt 可能已自动选中索引 0，导致后续 setCurrentIndex(0) 不触发信号，
			// cmbFormats 仍为空。
			const auto formats = this->m_service->getCameraFormats(0);
			this->ui->cmbFormats->clear();
			this->ui->cmbFormats->addItems(formats);
			this->ui->cmbFormats->setCurrentIndex(0);
		}
		this->ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme),
			this->m_service->cameraIsPlaying() ? QStringLiteral("pause") : QStringLiteral("play"))));
	};
	auto switchToFileMode = [this]() -> bool {
		setComposeMode(false);
		m_captureMode = CaptureMode::File;
		stopScreenCapture();
		this->ui->wgtView->setBackendType(m_filePlaybackBackend);
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
		this->ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("pause"))));
		return true;
	};
	auto switchToScreenMode = [this]() -> bool {
		setComposeMode(false);
		m_isFileMode = false;
		m_captureMode = CaptureMode::Screen;
		LOG_INFO("[screen]", "switch to screen mode, backend=", screenBackendName(m_screenBackendType));
		this->m_service->playerStop();
		this->m_service->cameraPause();
		this->m_fileProgress->setVisible(false);
		this->m_fileProgressLabel->setVisible(false);
		this->m_speedCombo->setVisible(false);
		this->m_debugStatsLabel->setVisible(false);
		this->m_fileProgressTimer->stop();
		this->m_debugStatsTimer->stop();
		m_currentFileTitle.clear();
		m_currentFilePath.clear();
		if (m_titleMarqueeTimer)
		{
			m_titleMarqueeTimer->stop();
		}
		updateTitleMarqueeText();
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
	connect(actionComposeMode, &QAction::triggered, this, [this]() {
		setComposeMode(true);
	});
	connect(actionPushStream, &QAction::triggered, this, [this]() {
		PullPreviewDialog dlg(this);
		dlg.setWindowTitle(tr("推流配置"));
		dlg.setWindowFlag(Qt::WindowMinimizeButtonHint, true);
		dlg.setWindowFlag(Qt::WindowSystemMenuHint, true);
		auto* mainLayout = new QVBoxLayout(&dlg);
		mainLayout->setContentsMargins(0, 0, 0, 0);
		auto* scrollArea = new QScrollArea(&dlg);
		scrollArea->setWidgetResizable(true);
		scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scrollArea->setFrameShape(QFrame::NoFrame);
		auto* scrollContent = new QWidget(scrollArea);
		dlg.onWindowStateChanged = [this](const Qt::WindowStates oldState, const Qt::WindowStates newState) {
			const bool wasMinimized = oldState.testFlag(Qt::WindowMinimized);
			const bool isMinimized = newState.testFlag(Qt::WindowMinimized);
			if (!wasMinimized && isMinimized)
			{
				this->showMinimized();
			}
		};
		auto* layout = new QFormLayout(scrollContent);
		layout->setVerticalSpacing(10);
		layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
		auto addRecent = [this](QStringList& list, const QString& value) {
			fplayer::Service::addRecentSetting(list, value, 8);
			saveAndApplyTheme();
		};
		auto* cmbProtocol = new QComboBox(&dlg);
		cmbProtocol->addItem(tr("RTMP"), QStringLiteral("rtmp://127.0.0.1:1935/live/stream"));
		cmbProtocol->addItem(tr("RTSP"), QStringLiteral("rtsp://127.0.0.1:8554/live/stream"));
		cmbProtocol->addItem(tr("SRT"), QStringLiteral("srt://127.0.0.1:8890?mode=caller"));
		cmbProtocol->addItem(tr("UDP"), QStringLiteral("udp://127.0.0.1:23000"));
		const bool fileScene = (m_captureMode == CaptureMode::File);
		const bool screenScene = (m_captureMode == CaptureMode::Screen);
		const bool composeScene = m_isComposeMode;
		auto* lblInputMode = new QLabel(composeScene
			                                ? tr("来源：组合模式预览画布（由 Service 统一编排）")
			                                : (screenScene
				                                   ? tr("来源：屏幕采集后端（由 Service 统一编排）")
				                                   : (fileScene ? tr("来源：当前文件模式媒体源") : tr("来源：当前摄像头模式"))),
		                            &dlg);
		lblInputMode->setWordWrap(true);
		lblInputMode->setMinimumHeight(lblInputMode->fontMetrics().lineSpacing() * 2 + 6);
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
		auto* cmbPushRouteMode = new QComboBox(&dlg);
		cmbPushRouteMode->addItem(tr("P2P 直连"), QStringLiteral("p2p"));
		cmbPushRouteMode->addItem(tr("推送服务端"), QStringLiteral("service"));
		{
			const int idx = cmbPushRouteMode->findData(m_pushRouteMode);
			cmbPushRouteMode->setCurrentIndex(idx >= 0 ? idx : 0);
		}
		auto* edtGateway = new QLineEdit(m_pushGateway, &dlg);
		edtGateway->setPlaceholderText(tr("服务地址，例如 http://127.0.0.1:9000"));
		auto* edtServiceApp = new QLineEdit(m_pushServiceApp, &dlg);
		auto* edtServiceStream = new QLineEdit(m_pushServiceStream, &dlg);
		auto* cmbServiceMode = new QComboBox(&dlg);
		cmbServiceMode->addItem(tr("广播分发（RTMP + FLV + HLS）"), QStringLiteral("broadcast"));
		cmbServiceMode->addItem(tr("广播分发 + HTTP-FLV 优先（含 HLS）"), QStringLiteral("httpflv"));
		{
			const int idx = cmbServiceMode->findData(m_pushServiceMode);
			cmbServiceMode->setCurrentIndex(idx >= 0 ? idx : 1);
		}
		auto* lblServicePlayUrl = new QLabel(tr("未创建"), &dlg);
		lblServicePlayUrl->setWordWrap(true);
		lblServicePlayUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
		auto* lblServiceHint = new QLabel(tr("说明：推流前会先向服务端创建会话，再使用返回的 RTMP 地址推流。"), &dlg);
		lblServiceHint->setWordWrap(true);
		auto* lblServiceLoadWarning = new QLabel(
			tr("注意：分辨率和帧率请勿设置过高，防止服务端负载过大影响拉流稳定性，建议不要超过 1920x1080 & 60fps。"),
			&dlg);
		lblServiceLoadWarning->setWordWrap(true);
		lblServiceLoadWarning->setStyleSheet(QStringLiteral("color:#ff453a; font-weight:600;"));
		auto* spFps = new QSpinBox(&dlg);
		spFps->setRange(0, 240);
		spFps->setSpecialValueText(tr("跟随当前"));
		spFps->setValue(qBound(0, m_pushFps, 240));
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
		auto appendComposeSizes = [this, appendSizeOption]() {
			const int aw = qMax(1, m_composeAspectW);
			const int ah = qMax(1, m_composeAspectH);
			const QList<int> widths{3840, 2560, 1920, 1600, 1280, 960, 854, 640};
			for (const int w : widths)
			{
				int h = qMax(2, ((w * ah) / aw) & ~1);
				appendSizeOption(w, h);
			}
		};
		auto* spBitrate = new QSpinBox(&dlg);
		spBitrate->setRange(0, 50000);
		spBitrate->setSpecialValueText(tr("跟随当前"));
		spBitrate->setValue(qBound(0, m_pushBitrateKbps, 50000));
		spBitrate->setSuffix(tr(" kbps"));
		auto* cmbEncoder = new QComboBox(&dlg);
		cmbEncoder->addItem(tr("自动（优先NVENC/AMF）"), QStringLiteral("auto"));
		cmbEncoder->addItem(tr("CPU（h264_mf/libx264/libopenh264）"), QStringLiteral("cpu"));
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
		chkKeepAspect->setChecked(m_pushKeepAspect);
		if (!m_pushProtocolTemplate.trimmed().isEmpty())
		{
			cmbOutput->setCurrentText(m_pushProtocolTemplate);
			const int pidx = cmbProtocol->findData(m_pushProtocolTemplate);
			if (pidx >= 0)
			{
				cmbProtocol->setCurrentIndex(pidx);
			}
		}
		if (!m_pushSize.trimmed().isEmpty())
		{
			cmbSize->setCurrentText(m_pushSize);
		}
		{
			const int eidx = cmbEncoder->findData(m_pushEncoder);
			if (eidx >= 0)
			{
				cmbEncoder->setCurrentIndex(eidx);
			}
		}
		{
			const int aInIdx = cmbAudioInput->findData(m_pushAudioInput);
			if (aInIdx >= 0)
			{
				cmbAudioInput->setCurrentIndex(aInIdx);
			}
		}
		{
			const int aOutIdx = cmbAudioOutput->findData(m_pushAudioOutput);
			if (aOutIdx >= 0)
			{
				cmbAudioOutput->setCurrentIndex(aOutIdx);
			}
		}
		if (composeScene)
		{
			// 组合模式尺寸来源固定为“画布比例 + 预设档位”，完全不依赖素材选择状态。
			cmbSize->clear();
			const int aw = qMax(1, m_composeAspectW);
			const int ah = qMax(1, m_composeAspectH);
			const QList<int> widths{3840, 2560, 1920, 1600, 1280, 960, 854, 640};
			for (const int w : widths)
			{
				const int h = qMax(2, ((w * ah) / aw) & ~1);
				const QString s = QStringLiteral("%1x%2").arg(w).arg(h);
				cmbSize->addItem(s, s);
			}
			QString composeSize = m_composeOutputSize.trimmed();
			if (composeSize.isEmpty() && m_composeSizeCombo && m_composeSizeCombo->count() > 0)
			{
				composeSize = m_composeSizeCombo->currentData().toString().trimmed();
			}
			const int idx = composeSize.isEmpty() ? -1 : cmbSize->findData(composeSize);
			cmbSize->setCurrentIndex(idx >= 0 ? idx : 0);
		}
		else if (screenScene)
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
		auto refreshPushParams = [this, lblPushParams, fileScene, screenScene, composeScene]() {
			if (!lblPushParams)
			{
				return;
			}
			if (composeScene)
			{
				lblPushParams->setText(tr("模式：组合模式\n来源：中间预览窗口内容\n布局：可在窗口中自由拖动缩放素材\n编码：H264（不可用时 MPEG4）"));
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
				const QString input = m_currentFilePath.trimmed().isEmpty() ? QString() : QFileInfo(m_currentFilePath).fileName();
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
			const QString latestLog = this->m_service->streamRecentPushLog();
			syncStreamLogView(txtLog, latestLog);
		});
		logTimer->start();
		connect(cmbProtocol, &QComboBox::currentTextChanged, &dlg, [cmbProtocol, cmbOutput]() {
			if (cmbOutput->currentText().trimmed().isEmpty())
			{
				cmbOutput->setCurrentText(cmbProtocol->currentData().toString());
			}
		});
		layout->addRow(lblInputMode);
		if (fileScene && !composeScene)
		{
			}
		layout->addRow(tr("当前参数"), lblPushParams);
		layout->addRow(tr("帧率"), spFps);
		layout->addRow(tr("尺寸"), cmbSize);
		layout->addRow(QString(), chkKeepAspect);
		layout->addRow(tr("码率"), spBitrate);
		layout->addRow(tr("视频编码器"), cmbEncoder);
		layout->addRow(tr("输入设备"), cmbAudioInput);
		layout->addRow(tr("输出设备"), cmbAudioOutput);
		layout->addRow(tr("推流模式"), cmbPushRouteMode);
		layout->addRow(tr("协议模板"), cmbProtocol);
		layout->addRow(tr("输出"), cmbOutput);
		layout->addRow(tr("服务地址"), edtGateway);
		layout->addRow(tr("服务 app"), edtServiceApp);
		layout->addRow(tr("服务 stream"), edtServiceStream);
		layout->addRow(tr("服务类型"), cmbServiceMode);
		layout->addRow(tr("HTTP-FLV"), lblServicePlayUrl);
		layout->addRow(lblServiceHint);
		layout->addRow(lblServiceLoadWarning);
		layout->addRow(lblStatus);
		layout->addRow(txtLog);
		auto* buttons = new QDialogButtonBox(&dlg);
		auto* btnStart = new QPushButton(tr("开始推流"), &dlg);
		auto* btnStop = new QPushButton(tr("停止推流"), &dlg);
		btnStart->setProperty("role", QStringLiteral("primary"));
		buttons->addButton(btnStart, QDialogButtonBox::AcceptRole);
		buttons->addButton(btnStop, QDialogButtonBox::ActionRole);
		scrollArea->setWidget(scrollContent);
		mainLayout->addWidget(scrollArea, 1);
		mainLayout->addWidget(buttons, 0);
		auto applyPushUiRunningState = [btnStart, btnStop, cmbPushRouteMode, cmbProtocol, cmbOutput, edtGateway, edtServiceApp,
		                                edtServiceStream, cmbServiceMode, spFps, cmbSize, spBitrate, cmbEncoder, cmbAudioInput, cmbAudioOutput,
		                                fileScene](const bool running) {
			btnStart->setEnabled(!running);
			btnStop->setEnabled(running);
			cmbPushRouteMode->setEnabled(!running);
			cmbProtocol->setEnabled(!running);
			cmbOutput->setEnabled(!running);
			edtGateway->setEnabled(!running);
			edtServiceApp->setEnabled(!running);
			edtServiceStream->setEnabled(!running);
			cmbServiceMode->setEnabled(!running);
			spFps->setEnabled(!running && !fileScene);
			cmbSize->setEnabled(!running && !fileScene);
			spBitrate->setEnabled(!running);
			cmbEncoder->setEnabled(!running);
			cmbAudioInput->setEnabled(!running && !fileScene);
			cmbAudioOutput->setEnabled(!running && !fileScene);
		};
		applyPushUiRunningState(this->m_service->streamIsRunning());
		dlg.beforeClose = [this, &dlg]() -> bool {
			if (this->m_service->streamIsRunning())
			{
				const auto answer = QMessageBox::question(&dlg,
				                                          tr("确认关闭"),
				                                          tr("当前正在推流，是否先停止推流再关闭窗口？"),
				                                          QMessageBox::Yes | QMessageBox::No,
				                                          QMessageBox::No);
				if (answer != QMessageBox::Yes)
				{
					return false;
				}
				this->m_service->streamStop();
			}
			return true;
		};
		auto refreshServiceRouteUi = [layout, cmbPushRouteMode, cmbProtocol, cmbOutput, edtGateway, edtServiceApp, edtServiceStream,
		                              cmbServiceMode, lblServicePlayUrl, lblServiceHint, lblServiceLoadWarning]() {
			const bool viaService = cmbPushRouteMode->currentData().toString() == QStringLiteral("service");
			cmbProtocol->setVisible(!viaService);
			cmbOutput->setVisible(!viaService);
			edtGateway->setVisible(viaService);
			edtServiceApp->setVisible(viaService);
			edtServiceStream->setVisible(viaService);
			cmbServiceMode->setVisible(viaService);
			lblServicePlayUrl->setVisible(viaService);
			lblServiceHint->setVisible(viaService);
			lblServiceLoadWarning->setVisible(viaService);
			if (QWidget* label = layout->labelForField(cmbProtocol))
			{
				label->setVisible(!viaService);
			}
			if (QWidget* label = layout->labelForField(cmbOutput))
			{
				label->setVisible(!viaService);
			}
			if (QWidget* label = layout->labelForField(edtGateway))
			{
				label->setVisible(viaService);
			}
			if (QWidget* label = layout->labelForField(edtServiceApp))
			{
				label->setVisible(viaService);
			}
			if (QWidget* label = layout->labelForField(edtServiceStream))
			{
				label->setVisible(viaService);
			}
			if (QWidget* label = layout->labelForField(cmbServiceMode))
			{
				label->setVisible(viaService);
			}
			if (QWidget* label = layout->labelForField(lblServicePlayUrl))
			{
				label->setVisible(viaService);
			}
		};
		connect(cmbPushRouteMode, &QComboBox::currentTextChanged, &dlg, [refreshServiceRouteUi]() {
			refreshServiceRouteUi();
		});
		refreshServiceRouteUi();

		connect(btnStop, &QPushButton::clicked, &dlg,
		        [this, applyPushUiRunningState]() {
			this->m_service->streamStop();
			applyPushUiRunningState(false);
		});
		connect(btnStart, &QPushButton::clicked, &dlg,
		        [this, btnStart, cmbPushRouteMode, cmbProtocol, cmbOutput, edtGateway, edtServiceApp, edtServiceStream, cmbServiceMode,
		         lblServicePlayUrl,
		         spFps, cmbSize, spBitrate, cmbEncoder, cmbAudioInput, cmbAudioOutput, chkKeepAspect, fileScene, screenScene, composeScene,
		         addRecent, applyPushUiRunningState]() {
			QString pushOutput = cmbOutput->currentText().trimmed();
			const bool viaService = cmbPushRouteMode->currentData().toString() == QStringLiteral("service");
			m_pushGateway = edtGateway->text().trimmed();
			m_pushServiceApp = edtServiceApp->text().trimmed();
			m_pushServiceStream = edtServiceStream->text().trimmed();
			m_pushRouteMode = cmbPushRouteMode->currentData().toString();
			m_pushServiceMode = cmbServiceMode->currentData().toString();
			m_pushProtocolTemplate = cmbOutput->currentText().trimmed();
			m_pushFps = spFps->value();
			m_pushSize = cmbSize->currentText().trimmed();
			m_pushBitrateKbps = spBitrate->value();
			m_pushEncoder = cmbEncoder->currentData().toString();
			m_pushAudioInput = cmbAudioInput->currentData().toString();
			m_pushAudioOutput = cmbAudioOutput->currentData().toString();
			m_pushKeepAspect = chkKeepAspect->isChecked();
			saveAndApplyTheme();
			if (viaService)
			{
				QString publishRtmp;
				QString playHttpFlv;
				QString streamId;
				QString requestError;
				const QString serviceMode = cmbServiceMode->currentData().toString().trimmed();
				const QString lanIp = selectLanHostForPublish();
				QJsonObject publisherMeta{
					{QStringLiteral("publisherIp"), lanIp},
					{QStringLiteral("publisherPort"), 1935},
					{QStringLiteral("scene"), composeScene ? QStringLiteral("compose")
					                                      : (screenScene ? QStringLiteral("screen")
					                                                     : (fileScene ? QStringLiteral("file") : QStringLiteral("camera")))}
				};
				QJsonObject sourceMeta{
					{QStringLiteral("fps"), spFps->value()},
					{QStringLiteral("bitrateKbps"), spBitrate->value()},
					{QStringLiteral("videoEncoder"), cmbEncoder->currentData().toString()},
					{QStringLiteral("audioInput"), cmbAudioInput->currentData().toString()},
					{QStringLiteral("audioOutput"), cmbAudioOutput->currentData().toString()}
				};
				if (!requestServiceStreamStart(edtGateway->text(), edtServiceApp->text(), edtServiceStream->text(), serviceMode, publisherMeta,
				                               sourceMeta,
				                               publishRtmp, playHttpFlv, streamId, requestError))
				{
					QMessageBox::warning(this, tr("推流失败"), tr("服务端流创建失败：%1").arg(requestError));
					return;
				}
				pushOutput = publishRtmp;
				cmbOutput->setCurrentText(pushOutput);
				lblServicePlayUrl->setText(playHttpFlv.isEmpty()
					                           ? tr("未返回（streamId=%1）").arg(streamId)
					                           : tr("%1\nstreamId=%2").arg(playHttpFlv, streamId));
			}
			if (pushOutput.isEmpty())
			{
				QMessageBox::warning(this, tr("推流失败"), tr("输出地址不能为空。"));
				return;
			}
			if (composeScene)
			{
				int outW = 0;
				int outH = 0;
				QString sizeText = cmbSize->currentText().trimmed();
				if (sizeText.isEmpty() || sizeText == tr("跟随当前"))
				{
					sizeText = cmbSize->currentData().toString().trimmed();
				}
				if (!sizeText.isEmpty())
				{
					const QRegularExpression sizeRe(R"((\d+)\s*x\s*(\d+))", QRegularExpression::CaseInsensitiveOption);
					const auto sizeM = sizeRe.match(sizeText);
					if (!sizeM.hasMatch())
					{
						QMessageBox::warning(this, tr("推流失败"), tr("尺寸格式无效，请使用 WxH，例如 1920x1080。"));
						return;
					}
					outW = sizeM.captured(1).toInt();
					outH = sizeM.captured(2).toInt();
				}
				QString inputSpec;
				if (!buildComposeScreenCaptureSpec(inputSpec,
				                                  spFps->value(),
				                                  outW,
				                                  outH,
				                                  spBitrate->value(),
				                                  cmbEncoder->currentData().toString(),
				                                  cmbAudioInput->currentData().toString(),
				                                  cmbAudioOutput->currentData().toString()))
				{
					QMessageBox::warning(this, tr("推流失败"), tr("组合预览窗口不可用，请先切换到组合模式并添加至少一个素材源。"));
					return;
				}
				if (!this->m_service->streamStartPush(inputSpec, pushOutput))
				{
					QMessageBox::warning(this, tr("推流失败"), this->m_service->streamLastError());
					return;
				}
				addRecent(m_recentPushOutputs, pushOutput);
				applyPushUiRunningState(true);
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
		applyPushUiRunningState(true);
		});
		connect(logTimer, &QTimer::timeout, &dlg,
		        [this, applyPushUiRunningState]() {
			const bool running = this->m_service->streamIsRunning();
			applyPushUiRunningState(running);
		});
		refreshPushParams();
		dlg.setMinimumSize(640, 400);
		if (auto* screen = QGuiApplication::primaryScreen()) {
			const QRect avail = screen->availableGeometry();
			dlg.setMaximumSize(dlg.maximumWidth(), qMax(400, avail.height() - 60));
		}
		dlg.exec();
	});
	connect(actionPullStream, &QAction::triggered, this, [this]() {
		if (m_pullMonitorDialog)
		{
			m_pullMonitorDialog->show();
			m_pullMonitorDialog->raise();
			m_pullMonitorDialog->activateWindow();
			return;
		}
		auto addRecent = [this](QStringList& list, const QString& value) {
			fplayer::Service::addRecentSetting(list, value, 8);
			saveAndApplyTheme();
		};
		auto* dlg = new QDialog(nullptr);
		auto* mainLayout = new QVBoxLayout(dlg);
		mainLayout->setContentsMargins(0, 0, 0, 0);
		auto* scrollArea = new QScrollArea(dlg);
		scrollArea->setWidgetResizable(true);
		scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scrollArea->setFrameShape(QFrame::NoFrame);
		auto* scrollContent = new QWidget(scrollArea);
		dlg->setAttribute(Qt::WA_DeleteOnClose, true);
		dlg->setModal(false);
		dlg->setWindowTitle(tr("拉流配置窗口"));
		m_pullMonitorDialog = dlg;
		connect(dlg, &QObject::destroyed, this, [this]() {
			m_pullMonitorDialog = nullptr;
			m_pullStartButton = nullptr;
			m_pullStopButton = nullptr;
			m_pullLogView = nullptr;
		});
		auto* layout = new QFormLayout(scrollContent);
		auto* cmbProtocol = new QComboBox(dlg);
		const int reservedPort = choosePullListenPort(m_pullReservedPort > 0 ? m_pullReservedPort : 1935);
		m_pullReservedPort = reservedPort;
		cmbProtocol->addItem(tr("RTMP"), QStringLiteral("rtmp"));
		cmbProtocol->addItem(tr("RTSP"), QStringLiteral("rtsp"));
		cmbProtocol->addItem(tr("SRT"), QStringLiteral("srt"));
		cmbProtocol->addItem(tr("UDP"), QStringLiteral("udp"));
		auto* edtStreamKey = new QLineEdit(QStringLiteral("stream001"), dlg);
		auto* urlRow = new QWidget(dlg);
		auto* urlLayout = new QHBoxLayout(urlRow);
		urlLayout->setContentsMargins(0, 0, 0, 0);
		auto* lblPullUrl = new QLabel(urlRow);
		lblPullUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
		lblPullUrl->setMinimumWidth(320);
		auto* btnCopyUrl = new QPushButton(tr("复制"), urlRow);
		urlLayout->addWidget(lblPullUrl, 1);
		urlLayout->addWidget(btnCopyUrl, 0);
		auto* cmbPullRouteMode = new QComboBox(dlg);
		cmbPullRouteMode->addItem(tr("P2P 拉流"), QStringLiteral("p2p"));
		cmbPullRouteMode->addItem(tr("服务端拉流"), QStringLiteral("service"));
		cmbPullRouteMode->setCurrentIndex(0);
		auto* edtPullGateway = new QLineEdit(m_pullGateway, dlg);
		edtPullGateway->setPlaceholderText(tr("服务地址，例如 http://127.0.0.1:9000"));
		auto* edtPullServiceApp = new QLineEdit(m_pullServiceApp, dlg);
		auto* edtPullServiceStream = new QLineEdit(m_pullServiceStream, dlg);
		edtPullServiceStream->setPlaceholderText(tr("输入 stream（如 stream001）"));
		auto* cmbPullServicePrefer = new QComboBox(dlg);
		cmbPullServicePrefer->addItem(tr("优先 HTTP-FLV"), QStringLiteral("httpflv"));
		cmbPullServicePrefer->addItem(tr("优先 RTMP"), QStringLiteral("rtmp"));
		auto* btnResolvePullUrl = new QPushButton(tr("解析地址"), dlg);
		auto* lblServiceResolvedUrl = new QLabel(tr("未解析"), dlg);
		lblServiceResolvedUrl->setWordWrap(true);
		lblServiceResolvedUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
		const QString lanHost = selectLanHostForPublish();
		auto makePublishUrl = [cmbProtocol, edtStreamKey, reservedPort, lanHost]() {
			const QString protocol = cmbProtocol->currentData().toString().trimmed().toLower();
			const QString key = edtStreamKey->text().trimmed().isEmpty() ? QStringLiteral("stream001") : edtStreamKey->text().trimmed();
			if (protocol == QStringLiteral("srt"))
			{
				return QStringLiteral("srt://%1:%2?mode=caller&streamid=%3").arg(lanHost).arg(reservedPort).arg(key);
			}
			if (protocol == QStringLiteral("rtmp"))
			{
				return QStringLiteral("rtmp://%1:%2/live/%3").arg(lanHost).arg(reservedPort).arg(key);
			}
			if (protocol == QStringLiteral("udp"))
			{
				return QStringLiteral("udp://%1:%2").arg(lanHost).arg(reservedPort);
			}
			return QStringLiteral("rtsp://%1:%2/live/%3").arg(lanHost).arg(reservedPort).arg(key);
		};
		auto makePullListenUrl = [cmbProtocol, edtStreamKey, reservedPort]() {
			const QString protocol = cmbProtocol->currentData().toString().trimmed().toLower();
			const QString key = edtStreamKey->text().trimmed().isEmpty() ? QStringLiteral("stream001") : edtStreamKey->text().trimmed();
			if (protocol == QStringLiteral("srt"))
			{
				return QStringLiteral("srt://0.0.0.0:%1?mode=listener&streamid=%2").arg(reservedPort).arg(key);
			}
			if (protocol == QStringLiteral("rtmp"))
			{
				return QStringLiteral("rtmp://0.0.0.0:%1/live/%2").arg(reservedPort).arg(key);
			}
			if (protocol == QStringLiteral("udp"))
			{
				return QStringLiteral("udp://0.0.0.0:%1").arg(reservedPort);
			}
			// FFmpeg 的 RTSP 监听常用 listen 参数方式，作为最小兼容方案。
			return QStringLiteral("rtsp://0.0.0.0:%1/live/%2").arg(reservedPort).arg(key);
		};
		auto refreshPullUrl = [lblPullUrl, makePublishUrl]() {
			const QString publishUrl = makePublishUrl();
			lblPullUrl->setText(publishUrl);
		};
		auto refreshStreamKeyVisibility = [cmbProtocol, edtStreamKey, layout]() {
			const bool isUdp = cmbProtocol->currentData().toString().trimmed().compare(QStringLiteral("udp"), Qt::CaseInsensitive) == 0;
			edtStreamKey->setVisible(!isUdp);
			if (QWidget* label = layout->labelForField(edtStreamKey))
			{
				label->setVisible(!isUdp);
			}
		};
		connect(cmbProtocol, &QComboBox::currentTextChanged, dlg, [refreshPullUrl]() {
			refreshPullUrl();
		});
		connect(cmbProtocol, &QComboBox::currentTextChanged, dlg, [refreshStreamKeyVisibility]() {
			refreshStreamKeyVisibility();
		});
		connect(edtStreamKey, &QLineEdit::textChanged, dlg, [refreshPullUrl]() {
			refreshPullUrl();
		});
		connect(btnCopyUrl, &QPushButton::clicked, dlg, [lblPullUrl]() {
			QApplication::clipboard()->setText(lblPullUrl->text().trimmed());
		});
		connect(btnResolvePullUrl, &QPushButton::clicked, dlg,
		        [this, edtPullGateway, edtPullServiceApp, edtPullServiceStream, cmbPullServicePrefer, lblServiceResolvedUrl]() {
			QString preferred;
			QString httpFlv;
			QString rtmp;
			QString err;
			if (!requestServiceStreamStatus(edtPullGateway->text(), edtPullServiceApp->text(), edtPullServiceStream->text(),
			                                preferred, httpFlv, rtmp, err))
			{
				QMessageBox::warning(this, tr("解析失败"), err);
				return;
			}
			const bool preferRtmp = cmbPullServicePrefer->currentData().toString() == QStringLiteral("rtmp");
			const QString picked = preferRtmp ? (!rtmp.isEmpty() ? rtmp : preferred) : preferred;
			lblServiceResolvedUrl->setText(tr("推荐地址：%1\nHTTP-FLV：%2\nRTMP：%3")
			                               .arg(picked.isEmpty() ? tr("无") : picked)
			                               .arg(httpFlv.isEmpty() ? tr("无") : httpFlv)
			                               .arg(rtmp.isEmpty() ? tr("无") : rtmp));
		});
		auto* lblStatus = new QLabel(tr("状态：未启动"), dlg);
		auto* txtLog = new QTextEdit(dlg);
		txtLog->setReadOnly(true);
		txtLog->setMinimumHeight(140);
		m_pullLogView = txtLog;
		dlg->setProperty("pullPreviewAutoOpened", false);
		dlg->setProperty("pullExtraLog", QString());
		dlg->setProperty("pullStopping", false);
		dlg->setProperty("pullCloseAfterStop", false);
		auto* buttons = new QDialogButtonBox(dlg);
		auto* btnStart = new QPushButton(tr("开始拉流"), dlg);
		auto* btnStop = new QPushButton(tr("结束拉流"), dlg);
		auto* btnPreview = new QPushButton(tr("预览窗口"), dlg);
		auto* btnDiag = new QPushButton(tr("网络自检"), dlg);
		btnStart->setProperty("role", QStringLiteral("primary"));
		buttons->addButton(btnStart, QDialogButtonBox::AcceptRole);
		buttons->addButton(btnStop, QDialogButtonBox::ActionRole);
		buttons->addButton(btnPreview, QDialogButtonBox::ActionRole);
		buttons->addButton(btnDiag, QDialogButtonBox::ActionRole);
		m_pullStartButton = btnStart;
		m_pullStopButton = btnStop;
		auto applyPullUiRunningState = [btnStart, btnStop, btnPreview, cmbPullRouteMode, cmbProtocol, edtStreamKey,
		                                edtPullGateway, edtPullServiceApp, edtPullServiceStream, cmbPullServicePrefer,
		                                btnResolvePullUrl](
			bool running) {
			btnStart->setEnabled(!running);
			btnStop->setEnabled(running);
			btnPreview->setEnabled(running);
			cmbPullRouteMode->setEnabled(!running);
			cmbProtocol->setEnabled(!running);
			edtStreamKey->setEnabled(!running);
			edtPullGateway->setEnabled(!running);
			edtPullServiceApp->setEnabled(!running);
			edtPullServiceStream->setEnabled(!running);
			cmbPullServicePrefer->setEnabled(!running);
			btnResolvePullUrl->setEnabled(!running);
		};
		auto refreshPullRouteUi = [layout, cmbPullRouteMode, cmbProtocol, edtStreamKey, urlRow, lblPullUrl, btnCopyUrl, edtPullGateway,
		                           edtPullServiceApp, edtPullServiceStream, cmbPullServicePrefer, btnResolvePullUrl,
		                           lblServiceResolvedUrl]() {
			const bool viaService = cmbPullRouteMode->currentData().toString() == QStringLiteral("service");
			cmbProtocol->setVisible(!viaService);
			edtStreamKey->setVisible(!viaService);
			lblPullUrl->setVisible(!viaService);
			btnCopyUrl->setVisible(!viaService);
			edtPullGateway->setVisible(viaService);
			edtPullServiceApp->setVisible(viaService);
			edtPullServiceStream->setVisible(viaService);
			cmbPullServicePrefer->setVisible(viaService);
			btnResolvePullUrl->setVisible(viaService);
			lblServiceResolvedUrl->setVisible(viaService);
			if (QWidget* label = layout->labelForField(cmbProtocol))
			{
				label->setVisible(!viaService);
			}
			if (QWidget* label = layout->labelForField(edtStreamKey))
			{
				label->setVisible(!viaService);
			}
			if (QWidget* label = layout->labelForField(urlRow))
			{
				label->setVisible(!viaService);
			}
			if (QWidget* label = layout->labelForField(edtPullGateway))
			{
				label->setVisible(viaService);
			}
			if (QWidget* label = layout->labelForField(edtPullServiceApp))
			{
				label->setVisible(viaService);
			}
			if (QWidget* label = layout->labelForField(edtPullServiceStream))
			{
				label->setVisible(viaService);
			}
			if (QWidget* label = layout->labelForField(cmbPullServicePrefer))
			{
				label->setVisible(viaService);
			}
			if (QWidget* label = layout->labelForField(lblServiceResolvedUrl))
			{
				label->setVisible(viaService);
			}
		};
		connect(cmbPullRouteMode, &QComboBox::currentTextChanged, dlg, [refreshPullRouteUi]() {
			refreshPullRouteUi();
		});
		refreshPullRouteUi();
		auto requestStopPullAsync = [this, dlg, applyPullUiRunningState]() {
			if (!this->m_service)
			{
				return;
			}
			if (dlg->property("pullStopping").toBool())
			{
				return;
			}
			dlg->setProperty("pullStopping", true);
			applyPullUiRunningState(false);
			QPointer<CaptureWindow> selfGuard(this);
			QPointer<QDialog> dlgGuard(dlg);
			std::thread([selfGuard, dlgGuard]() {
				if (!selfGuard || !dlgGuard || !selfGuard->m_service)
				{
					return;
				}
				selfGuard->m_service->streamStop();
				selfGuard->m_service->streamStopPullRecording();
				if (selfGuard->m_pullRecordService)
				{
					LOG_INFO("[pull-record]", "stop record service on pull stop");
					selfGuard->m_pullRecordService->streamStop();
				}
				QMetaObject::invokeMethod(selfGuard, [selfGuard, dlgGuard]() {
					if (!selfGuard || !dlgGuard)
					{
						return;
					}
					dlgGuard->setProperty("pullStopping", false);
					selfGuard->m_pullRecording = false;
					selfGuard->m_pullRecordingViaMainService = false;
					selfGuard->m_pullRecordOutputPath.clear();
					selfGuard->m_pullSessionStartMs = 0;
					if (selfGuard->m_pullRecordTimer)
					{
						selfGuard->m_pullRecordTimer->stop();
					}
					selfGuard->updatePullRecordButtonUi();
					if (selfGuard->m_pullPreviewDialog)
					{
						selfGuard->m_pullPreviewDialog->close();
					}
					if (dlgGuard->property("pullCloseAfterStop").toBool())
					{
						dlgGuard->setProperty("pullCloseAfterStop", false);
						dlgGuard->close();
					}
				}, Qt::QueuedConnection);
			}).detach();
		};
		applyPullUiRunningState(this->m_service->streamIsRunning());
		auto* logTimer = new QTimer(dlg);
		logTimer->setInterval(500);
		connect(logTimer, &QTimer::timeout, dlg, [this, dlg, makePullListenUrl, lblStatus, txtLog, applyPullUiRunningState]() {
			const bool running = this->m_service->streamIsRunning();
			applyPullUiRunningState(running);
			if (running)
			{
				lblStatus->setText(tr("状态：等待推流连接（监听中）"));
			}
			else if (this->m_service->streamHasCompletedSession())
			{
				lblStatus->setText(tr("状态：已停止，退出码=%1").arg(this->m_service->streamLastExitCode()));
			}
			else
			{
				lblStatus->setText(tr("状态：当前无拉流任务"));
			}
			const QString latestLog = this->m_service->streamRecentPullLog();
			const QString extraLog = dlg->property("pullExtraLog").toString();
			const QString mergedLog = extraLog.isEmpty() ? latestLog : (latestLog + extraLog);
			if (running && m_pullPreviewDialog && !dlg->property("pullPreviewAutoOpened").toBool() &&
				latestLog.contains(QStringLiteral("[拉流] 检测到上游推流连接")))
			{
				lblStatus->setText(tr("状态：已检测到推流连接，预览总线已接入"));
				const QString prev = dlg->property("pullExtraLog").toString();
				dlg->setProperty("pullExtraLog", prev + tr("[预览] 已切换为进程内预览总线（pull_preview）\n"));
				dlg->setProperty("pullPreviewAutoOpened", true);
			}
			syncStreamLogView(txtLog, mergedLog);
		});
		logTimer->start();
		connect(btnStart, &QPushButton::clicked, dlg,
		        [this, dlg, cmbPullRouteMode, makePullListenUrl, edtPullGateway, edtPullServiceApp, edtPullServiceStream,
		         cmbPullServicePrefer, lblServiceResolvedUrl, addRecent, applyPullUiRunningState, requestStopPullAsync]() {
			QString pullInput = makePullListenUrl();
			const bool viaService = cmbPullRouteMode->currentData().toString() == QStringLiteral("service");
			m_pullGateway = edtPullGateway->text().trimmed();
			m_pullServiceApp = edtPullServiceApp->text().trimmed();
			m_pullServiceStream = edtPullServiceStream->text().trimmed();
			saveAndApplyTheme();
			QString resolvedHttpFlv;
			QString resolvedRtmp;
			if (viaService)
			{
				const QString prev = dlg->property("pullExtraLog").toString();
				dlg->setProperty("pullExtraLog", prev + tr("[服务端拉流] 开始解析地址 app=%1 stream=%2\n")
				                .arg(edtPullServiceApp->text().trimmed(), edtPullServiceStream->text().trimmed()));
				QString preferred;
				QString err;
				if (!requestServiceStreamStatus(edtPullGateway->text(), edtPullServiceApp->text(), edtPullServiceStream->text(),
				                                preferred, resolvedHttpFlv, resolvedRtmp, err))
				{
					QMessageBox::warning(this, tr("拉流失败"), tr("服务端地址解析失败：%1").arg(err));
					return;
				}
				const bool preferRtmp = cmbPullServicePrefer->currentData().toString() == QStringLiteral("rtmp");
				pullInput = preferRtmp ? (!resolvedRtmp.isEmpty() ? resolvedRtmp : preferred) : preferred;
				const QString prev2 = dlg->property("pullExtraLog").toString();
				dlg->setProperty("pullExtraLog", prev2 + tr("[服务端拉流] 解析成功，最终地址=%1\n").arg(pullInput));
				lblServiceResolvedUrl->setText(tr("推荐地址：%1\nHTTP-FLV：%2\nRTMP：%3")
				                               .arg(pullInput.isEmpty() ? tr("无") : pullInput)
				                               .arg(resolvedHttpFlv.isEmpty() ? tr("无") : resolvedHttpFlv)
				                               .arg(resolvedRtmp.isEmpty() ? tr("无") : resolvedRtmp));
			}
			if (pullInput.isEmpty())
			{
				QMessageBox::warning(this, tr("拉流失败"), tr("输入地址不能为空。"));
				return;
			}
			bool started = false;
			QStringList candidates;
			candidates << pullInput;
			if (viaService)
			{
				if (!resolvedRtmp.isEmpty() && !candidates.contains(resolvedRtmp))
				{
					candidates << resolvedRtmp;
				}
				if (!resolvedHttpFlv.isEmpty() && !candidates.contains(resolvedHttpFlv))
				{
					candidates << resolvedHttpFlv;
				}
			}
			QString extra = dlg->property("pullExtraLog").toString();
			int attempt = 0;
			for (const QString& candidate : candidates)
			{
				++attempt;
				if (candidate.trimmed().isEmpty())
				{
					continue;
				}
				extra += tr("[服务端拉流] 尝试地址 %1/%2: %3\n").arg(attempt).arg(candidates.size()).arg(candidate);
				dlg->setProperty("pullExtraLog", extra);
				// 主链路负责预览；录制由独立录制链路承担，互不影响。
				started = this->m_service->streamStartPull(candidate, QString());
				if (!started)
				{
					extra += tr("[服务端拉流] 启动调用失败: %1\n").arg(this->m_service->streamLastError());
					dlg->setProperty("pullExtraLog", extra);
					continue;
				}
				// 等待短时间，若连接快速失败（典型 404），自动尝试下一个候选地址。
				QThread::msleep(viaService ? 900 : 300);
				if (this->m_service->streamIsRunning())
				{
					pullInput = candidate;
					started = true;
					break;
				}
				started = false;
				extra += tr("[服务端拉流] 地址失败，错误=%1\n").arg(this->m_service->streamLastError());
				dlg->setProperty("pullExtraLog", extra);
			}
			if (!started)
			{
				QMessageBox::warning(this, tr("拉流失败"), this->m_service->streamLastError());
				return;
			}
			this->m_service->streamSetPreviewPaused(false);
			m_pullCurrentInputUrl = pullInput;
			m_pullRecordInputUrl = pullInput;
			m_pullSessionStartMs = QDateTime::currentMSecsSinceEpoch();
			if (m_pullRecordTimer)
			{
				m_pullRecordTimer->start();
			}
			for (const QString& candidate : candidates)
			{
				const QString s = candidate.trimmed().toLower();
				if (s.isEmpty())
				{
					continue;
				}
				const bool looksListener = s.contains(QStringLiteral("://0.0.0.0:")) ||
				                           s.contains(QStringLiteral("://[::]:")) ||
				                           s.contains(QStringLiteral("listen=1")) ||
				                           s.contains(QStringLiteral("mode=listener"));
				if (!looksListener)
				{
					m_pullRecordInputUrl = candidate.trimmed();
					break;
				}
			}
			if (m_pullRecordService)
			{
				m_pullRecordService->streamStop();
			}
			m_pullRecording = false;
			addRecent(m_recentPullInputs, pullInput);
			applyPullUiRunningState(true);
			dlg->setProperty("pullPreviewAutoOpened", false);
			if (!m_pullPreviewDialog)
			{
				auto* preview = new PullPreviewDialog(nullptr);
				preview->setWindowTitle(tr("拉流预览"));
				preview->resize(900, 560);
				auto* vLayout = new QVBoxLayout(preview);
				auto* view = new fplayer::FVideoView(preview);
				view->setBackendType(fplayer::MediaBackendType::FFmpeg);
				vLayout->addWidget(view, 1);
				auto* ctrlRow = new QWidget(preview);
				auto* ctrlLayout = new QHBoxLayout(ctrlRow);
				ctrlLayout->setContentsMargins(0, 0, 0, 0);
				auto* btnPause = new QPushButton(ctrlRow);
				auto* btnRefresh = new QPushButton(ctrlRow);
				auto* btnShot = new QPushButton(ctrlRow);
				auto* btnRecord = new QPushButton(ctrlRow);
				auto* btnSettings = new QPushButton(ctrlRow);
				auto* btnFullscreen = new QPushButton(ctrlRow);
				auto* btnImagePool = new QPushButton(ctrlRow);
				auto* lblRecordDuration = new QLabel(tr("拉流时长：00:00"), ctrlRow);
				auto* sldVolume = new QSlider(Qt::Horizontal, ctrlRow);
				auto* lblVolume = new QLabel(tr("音量"), ctrlRow);
				auto* lblVolumeValue = new QLabel(ctrlRow);
				sldVolume->setRange(0, 200);
				sldVolume->setValue(qRound(this->m_service->streamPreviewVolume() * 100.0f));
				sldVolume->setFixedWidth(180);
				sldVolume->setTickPosition(QSlider::TicksBelow);
				sldVolume->setTickInterval(20);
				sldVolume->setToolTip(tr("音量：%1%").arg(sldVolume->value()));
				for (auto* b : {btnPause, btnRefresh, btnSettings, btnFullscreen, btnImagePool})
				{
					b->setMinimumSize(40, 40);
					b->setMaximumSize(40, 40);
					b->setText(QString());
					b->setFocusPolicy(Qt::NoFocus);
					b->setIconSize(QSize(24, 24));
				}
				btnPause->setToolTip(tr("暂停/继续"));
				btnRefresh->setToolTip(tr("刷新"));
				btnShot->setToolTip(tr("截图"));
				btnRecord->setToolTip(tr("录制"));
				btnSettings->setToolTip(tr("设置"));
				btnFullscreen->setToolTip(tr("全屏/退出全屏"));
				btnShot->setMinimumHeight(40);
				btnShot->setMaximumHeight(40);
				btnShot->setMinimumWidth(40);
				btnShot->setMaximumWidth(40);
				btnRecord->setMinimumHeight(40);
				btnRecord->setMaximumHeight(40);
				btnRecord->setMinimumWidth(40);
				btnRecord->setMaximumWidth(40);
			btnPause->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("pause"))));
			btnRefresh->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("refresh"))));
			btnShot->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("camera"))));
			btnRecord->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("video"))));
			btnSettings->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("settings"))));
			btnFullscreen->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("fullScreen"))));
			btnImagePool->setToolTip(tr("图池"));
			btnImagePool->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("pictures"))));
				lblRecordDuration->setMinimumWidth(140);
				lblVolume->setMinimumWidth(34);
				lblVolumeValue->setMinimumWidth(46);
				lblVolumeValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
				lblVolumeValue->setText(tr("%1%").arg(sldVolume->value()));
				ctrlLayout->addWidget(btnPause, 0);
				ctrlLayout->addWidget(btnRefresh, 0);
				ctrlLayout->addWidget(btnShot, 0);
				ctrlLayout->addWidget(btnRecord, 0);
				ctrlLayout->addWidget(btnImagePool, 0);
				ctrlLayout->addWidget(btnSettings, 0);
				ctrlLayout->addWidget(btnFullscreen, 0);
				ctrlLayout->addWidget(lblRecordDuration, 0);
				ctrlLayout->addStretch(1);
				ctrlLayout->addSpacing(8);
				ctrlLayout->addWidget(lblVolume, 0);
				ctrlLayout->addWidget(sldVolume, 0);
				ctrlLayout->addWidget(lblVolumeValue, 0);
				vLayout->addWidget(ctrlRow, 0);
				m_pullPreviewDialog = preview;
				m_pullPreviewView = view;
				m_pullVolumeSlider = sldVolume;
				m_pullPreviewShotButton = btnShot;
				m_pullPreviewRecordButton = btnRecord;
				m_pullPreviewSettingsButton = btnSettings;
				m_pullPreviewPauseButton = btnPause;
				m_pullPreviewRefreshButton = btnRefresh;
				m_pullPreviewFullscreenButton = btnFullscreen;
				m_pullPreviewImagePoolButton = btnImagePool;
				m_pullPreviewRecordDurationLabel = lblRecordDuration;
				updatePullRecordButtonUi();
				connect(btnImagePool, &QPushButton::clicked, [this]() {
					if (m_imagePoolSidebar)
					{
						if (m_imagePoolSidebar->isVisible())
						{
							if (m_imagePoolSidebar->isMinimized())
								m_imagePoolSidebar->showNormal();
							m_imagePoolSidebar->raise();
							m_imagePoolSidebar->activateWindow();
						}
						else
						{
							m_imagePoolSidebar->show();
						}
					}
				});
				connect(preview, &QObject::destroyed, this, [this]() {
					m_pullPreviewDialog = nullptr;
					m_pullPreviewView = nullptr;
					m_pullVolumeSlider = nullptr;
					m_pullPreviewShotButton = nullptr;
					m_pullPreviewRecordButton = nullptr;
					m_pullPreviewSettingsButton = nullptr;
					m_pullPreviewPauseButton = nullptr;
					m_pullPreviewRefreshButton = nullptr;
					m_pullPreviewFullscreenButton = nullptr;
					m_pullPreviewImagePoolButton = nullptr;
					m_pullPreviewRecordDurationLabel = nullptr;
					m_pullRecording = false;
					m_pullSessionStartMs = 0;
					if (m_pullRecordTimer)
					{
						m_pullRecordTimer->stop();
					}
					if (m_pullRecordService)
					{
						m_pullRecordService->streamStop();
					}
					// 关闭预览窗口不停止拉流，拉流在后台继续运行
				});
				preview->beforeClose = [preview, dlg]() -> bool {
					if (dlg->property("pullStopping").toBool())
					{
						return false;
					}
					// 关闭预览窗口不停止拉流，仅隐藏
					preview->hide();
					return false;
				};
				connect(btnPause, &QPushButton::clicked, preview, [this, btnPause]() {
					const bool paused = this->m_service->streamPreviewPaused();
					this->m_service->streamSetPreviewPaused(!paused);
					btnPause->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), paused ? QStringLiteral("pause") : QStringLiteral("play"))));
				});
				connect(btnRefresh, &QPushButton::clicked, preview, [this, dlg]() {
					const QString prev = dlg->property("pullExtraLog").toString();
					dlg->setProperty("pullExtraLog", prev + tr("[预览] 已刷新进程内预览总线（pull_preview）\n"));
				});
				connect(btnShot, &QPushButton::clicked, preview, [this, view, preview]() {
					QImage shotImage;
					fplayer::ScreenFrame frame;
					if (fplayer::ScreenFrameBus::instance().snapshotIfNew(0, frame, QStringLiteral("pull_preview")) && frame.valid)
					{
						shotImage = i420ToImage(frame.y, frame.u, frame.v, frame.width, frame.height, frame.yStride, frame.uStride, frame.vStride);
					}
					if (shotImage.isNull())
					{
						shotImage = view->grab().toImage();
					}
					if (shotImage.isNull())
					{
						QMessageBox::warning(this, tr("截图失败"), tr("当前拉流预览为空，无法截图。"));
						return;
					}
					QDir dir(m_screenshotSaveDir);
					if (!dir.exists())
					{
						dir.mkpath(QStringLiteral("."));
					}
					const QString savePath = makeScreenshotFilePath(QStringLiteral("pull_preview_shot"));
					if (!shotImage.save(savePath))
					{
						QMessageBox::warning(this, tr("截图失败"), tr("图片保存失败：%1").arg(savePath));
						return;
					}
					showNonBlockingHint(preview, tr("截图已保存：%1").arg(QDir::toNativeSeparators(savePath)));
						emit screenshotSaved(savePath);
				});
				connect(btnSettings, &QPushButton::clicked, preview, [this, preview]() {
					openCaptureSettingsDialog(preview);
				});
				connect(btnRecord, &QPushButton::clicked, preview, [this, dlg]() {
					const auto activePullInputFromLog = [this]() {
						if (!m_service)
						{
							return QString();
						}
						const QString log = m_service->streamRecentPullLog();
						const QRegularExpression re(QStringLiteral(R"(\[拉流\]\s*输入地址:\s*(.+))"));
						QRegularExpressionMatchIterator it = re.globalMatch(log);
						QString lastInput;
						while (it.hasNext())
						{
							const auto m = it.next();
							lastInput = m.captured(1).trimmed();
						}
						return lastInput;
					};
					const auto currentPullIsListenerFromLog = [this]() {
						if (!m_service)
						{
							return false;
						}
						const QString log = m_service->streamRecentPullLog();
						const QRegularExpression re(QStringLiteral(R"(\[拉流\]\s*监听模式:\s*(yes|no))"),
						                            QRegularExpression::CaseInsensitiveOption);
						QRegularExpressionMatchIterator it = re.globalMatch(log);
						QString lastFlag;
						while (it.hasNext())
						{
							const auto m = it.next();
							lastFlag = m.captured(1).trimmed().toLower();
						}
						return lastFlag == QStringLiteral("yes");
					};
					if (m_pullRecording)
					{
						if (m_service)
						{
							m_service->streamStopPullRecording();
						}
						QFileInfo recordedInfo(m_pullRecordOutputPath);
						const bool exists = recordedInfo.exists();
						const qint64 sizeBytes = exists ? recordedInfo.size() : 0;
						const QString stopLog = tr("[录制] 拉流录制结束: path=%1 exists=%2 size=%3\n")
						                            .arg(m_pullRecordOutputPath.isEmpty() ? tr("<空>") : QDir::toNativeSeparators(m_pullRecordOutputPath))
						                            .arg(exists ? tr("yes") : tr("no"))
						                            .arg(sizeBytes);
						const QString stoppedPath = m_pullRecordOutputPath;
						dlg->setProperty("pullExtraLog", dlg->property("pullExtraLog").toString() + stopLog);
						LOG_INFO("[pull-record]", "stop monitor recording path=", m_pullRecordOutputPath.toUtf8().constData(),
						         " exists=", exists ? 1 : 0, " size=", static_cast<long long>(sizeBytes));
						m_pullRecording = false;
						m_pullRecordOutputPath.clear();
						updatePullRecordButtonUi();
						showNonBlockingHint(this,
						                   tr("拉流预览录制已停止，文件已保存：%1")
						                           .arg(QDir::toNativeSeparators(stoppedPath)));
						return;
					}
					if (m_pullCurrentInputUrl.trimmed().isEmpty())
					{
						QMessageBox::warning(this, tr("录制失败"), tr("缺少拉流输入地址，无法开始录制。"));
						return;
					}
					if (!m_service || !m_service->streamIsRunning())
					{
						QMessageBox::warning(this, tr("录制失败"), tr("当前拉流未运行，无法开始录制。"));
						return;
					}
					QString recordInput = m_pullRecordInputUrl.trimmed();
					if (recordInput.isEmpty())
					{
						recordInput = activePullInputFromLog().trimmed();
					}
					if (recordInput.isEmpty())
					{
						recordInput = m_pullCurrentInputUrl.trimmed();
					}
					recordInput.replace(QStringLiteral("://0.0.0.0:"), QStringLiteral("://127.0.0.1:"));
					recordInput.replace(QStringLiteral("://[::]:"), QStringLiteral("://127.0.0.1:"));
					QString outputPath = makeRecordingFilePath(QStringLiteral("pull_preview_record"));
					QFileInfo outInfo(outputPath);
					if (outInfo.suffix().trimmed().isEmpty())
					{
						outputPath += QStringLiteral(".mkv");
						outInfo = QFileInfo(outputPath);
					}
					outputPath = outInfo.absoluteFilePath();
					QDir outDir(m_recordSaveDir);
					if (!outDir.exists())
					{
						outDir.mkpath(QStringLiteral("."));
					}
					const QString startLog = tr("[录制] 拉流录制开始: input=%1 output=%2\n")
					                             .arg(recordInput, QDir::toNativeSeparators(outputPath));
					dlg->setProperty("pullExtraLog", dlg->property("pullExtraLog").toString() + startLog);
					LOG_INFO("[pull-record]", "start monitor recording input=", recordInput.toUtf8().constData(),
					         " output=", outputPath.toUtf8().constData());
					if (!m_service->streamStartPullRecording(outputPath))
					{
						dlg->setProperty("pullExtraLog",
						                 dlg->property("pullExtraLog").toString() +
						                 tr("[录制] 启动失败: %1\n").arg(m_service->streamLastError()));
						QMessageBox::warning(this, tr("录制失败"),
						                     tr("%1\n\n输入：%2\n输出：%3")
						                             .arg(m_service->streamLastError(),
						                                  recordInput,
						                                  QDir::toNativeSeparators(outputPath)));
						return;
					}
					m_pullRecording = true;
					m_pullRecordOutputPath = outputPath;
					m_pullRecordStartMs = QDateTime::currentMSecsSinceEpoch();
					if (m_pullRecordTimer)
					{
						m_pullRecordTimer->start();
					}
					updatePullRecordButtonUi();
					showNonBlockingHint(this,
					                   tr("拉流预览录制已开始，保存到：%1").arg(QDir::toNativeSeparators(outputPath)));
					QPointer<CaptureWindow> selfGuard(this);
					QPointer<QDialog> pullDlgGuard(dlg);
					QTimer::singleShot(900, this, [selfGuard, pullDlgGuard, recordInput, outputPath]() {
						if (!selfGuard || !selfGuard->m_service)
						{
							return;
						}
						if (!selfGuard->m_pullRecording || selfGuard->m_service->streamIsPullRecording())
						{
							return;
						}
						const QString err = selfGuard->m_service->streamLastError();
						const QString log = selfGuard->m_service->streamRecentPullLog();
						if (pullDlgGuard)
						{
							pullDlgGuard->setProperty("pullExtraLog",
							                 pullDlgGuard->property("pullExtraLog").toString() +
							                 QObject::tr("[录制] 启动后快速退出: %1\n").arg(err));
						}
						QMessageBox::warning(selfGuard,
						                     QObject::tr("录制失败"),
						                     QObject::tr("录制链路启动后立即退出。\n错误：%1\n输入：%2\n输出：%3\n\n日志：\n%4")
						                             .arg(err.isEmpty() ? QObject::tr("未知错误") : err,
						                                  recordInput,
						                                  QDir::toNativeSeparators(outputPath),
						                                  log.isEmpty() ? QObject::tr("无") : log));
						selfGuard->m_pullRecording = false;
						if (selfGuard->m_pullRecordTimer)
						{
							selfGuard->m_pullRecordTimer->stop();
						}
						selfGuard->updatePullRecordButtonUi();
					});
				});
				connect(btnFullscreen, &QPushButton::clicked, preview, [preview]() {
					if (preview->isFullScreen())
					{
						preview->showNormal();
					}
					else
					{
						preview->showFullScreen();
					}
				});
				connect(sldVolume, &QSlider::valueChanged, preview, [this](int value) {
					this->m_service->streamSetPreviewVolume(static_cast<float>(value) / 100.0f);
				});
				connect(sldVolume, &QSlider::valueChanged, preview, [sldVolume](int value) {
					sldVolume->setToolTip(QObject::tr("音量：%1%").arg(value));
				});
				connect(sldVolume, &QSlider::valueChanged, preview, [lblVolumeValue](int value) {
					lblVolumeValue->setText(QObject::tr("%1%").arg(value));
				});
				const auto previewTarget = view->previewTarget();
				auto* gl = static_cast<fplayer::FGLWidget*>(previewTarget.backend_hint);
				auto* frameTimer = new QTimer(preview);
				frameTimer->setInterval(16);
				const auto lastSerial = std::make_shared<quint64>(0);
				connect(frameTimer, &QTimer::timeout, preview, [gl, lastSerial]() {
					if (!gl)
					{
						return;
					}
					fplayer::ScreenFrame frame;
					if (!fplayer::ScreenFrameBus::instance().snapshotIfNew(*lastSerial, frame, QStringLiteral("pull_preview")))
					{
						return;
					}
					*lastSerial = frame.serial;
					QMetaObject::invokeMethod(gl, "updateYUVFrame", Qt::QueuedConnection,
					                          Q_ARG(QByteArray, frame.y),
					                          Q_ARG(QByteArray, frame.u),
					                          Q_ARG(QByteArray, frame.v),
					                          Q_ARG(int, frame.width),
					                          Q_ARG(int, frame.height),
					                          Q_ARG(int, frame.yStride),
					                          Q_ARG(int, frame.uStride),
					                          Q_ARG(int, frame.vStride));
				});
				frameTimer->start();
				updatePullRecordButtonUi();
				preview->show();
			}
		});
		connect(btnStop, &QPushButton::clicked, dlg, [requestStopPullAsync]() {
			requestStopPullAsync();
		});
		connect(btnPreview, &QPushButton::clicked, dlg, [this]() {
			if (m_pullPreviewDialog)
			{
				m_pullPreviewDialog->show();
				m_pullPreviewDialog->raise();
				m_pullPreviewDialog->activateWindow();
			}
		});
		connect(btnDiag, &QPushButton::clicked, dlg, [this, dlg, makePullListenUrl, makePublishUrl, reservedPort]() {
			QStringList lines;
			lines << QStringLiteral("[自检] ===== 网络自检开始 =====");
			lines << QStringLiteral("[自检] 发布地址: %1").arg(makePublishUrl());
			lines << QStringLiteral("[自检] 监听地址: %1").arg(makePullListenUrl());
			lines << QStringLiteral("[自检] 本机IPv4: %1").arg(collectLanIpv4List().join(QStringLiteral(", ")));
			{
				QTcpServer probe;
				const bool canBind = probe.listen(QHostAddress::Any, static_cast<quint16>(reservedPort));
				lines << QStringLiteral("[自检] 端口%1可绑定: %2").arg(reservedPort).arg(canBind ? QStringLiteral("是") : QStringLiteral("否"));
			}
			const QString lastError = this->m_service->streamLastError().trimmed();
			if (!lastError.isEmpty())
			{
				lines << QStringLiteral("[自检] 最近错误: %1").arg(lastError);
			}
			const QString message = lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
			const QString prev = dlg->property("pullExtraLog").toString();
			dlg->setProperty("pullExtraLog", prev + message);
		});
		layout->addRow(tr("协议模板"), cmbProtocol);
		layout->addRow(tr("拉流模式"), cmbPullRouteMode);
		layout->addRow(tr("推流码"), edtStreamKey);
		layout->addRow(tr("拉流地址"), urlRow);
		layout->addRow(tr("服务地址"), edtPullGateway);
		layout->addRow(tr("服务 app"), edtPullServiceApp);
		layout->addRow(tr("服务 stream"), edtPullServiceStream);
		layout->addRow(tr("优先协议"), cmbPullServicePrefer);
		layout->addRow(QString(), btnResolvePullUrl);
		layout->addRow(tr("解析结果"), lblServiceResolvedUrl);
		layout->addRow(lblStatus);
		layout->addRow(txtLog);
		scrollArea->setWidget(scrollContent);
		mainLayout->addWidget(scrollArea, 1);
		mainLayout->addWidget(buttons, 0);
		refreshPullUrl();
		refreshStreamKeyVisibility();
		dlg->setMinimumSize(640, 400);
		if (auto* screen = QGuiApplication::primaryScreen()) {
			const QRect avail = screen->availableGeometry();
			dlg->setMaximumSize(dlg->maximumWidth(), qMax(400, avail.height() - 60));
		}
		dlg->show();
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
	updateRecordButtonUi();

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

void CaptureWindow::loadCapturePreferences()
{
	const QString defaultShotDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	const QString defaultVideoDir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	m_screenshotSaveDir = defaultShotDir.isEmpty() ? QDir::homePath() : defaultShotDir;
	m_recordSaveDir = defaultVideoDir.isEmpty() ? m_screenshotSaveDir : defaultVideoDir;
	m_pushGateway = QStringLiteral("http://127.0.0.1:9000");
	m_pushServiceApp = QStringLiteral("live");
	m_pushServiceStream = QStringLiteral("stream001");
	m_pullGateway = QStringLiteral("http://127.0.0.1:9000");
	m_pullServiceApp = QStringLiteral("live");
	m_pullServiceStream = QStringLiteral("stream001");
	m_pushRouteMode = QStringLiteral("p2p");
	m_pushServiceMode = QStringLiteral("httpflv");
	m_pushProtocolTemplate = QStringLiteral("rtmp://127.0.0.1:1935/live/stream");
	m_pushFps = 0;
	m_pushSize.clear();
	m_pushBitrateKbps = 0;
	m_pushEncoder = QStringLiteral("auto");
	m_pushAudioInput = QStringLiteral("off");
	m_pushAudioOutput = QStringLiteral("off");
	m_pushKeepAspect = true;
	m_closeToTrayOnClose = true;
	m_composeOutputSize.clear();
	m_screenBackendType = fplayer::MediaBackendType::Dxgi;
	fplayer::SystemSettings data;
	try
	{
		data = m_service ? m_service->loadSystemSettings() : fplayer::SystemSettings{};
	}
	catch (...)
	{
		data = fplayer::SystemSettings{};
	}
	if (!data.screenshotDir.trimmed().isEmpty())
	{
		m_screenshotSaveDir = data.screenshotDir;
	}
	if (!data.recordDir.trimmed().isEmpty())
	{
		m_recordSaveDir = data.recordDir;
	}
	m_recentPushInputs = data.recentPushInputs;
	m_recentPushOutputs = data.recentPushOutputs;
	m_recentPullInputs = data.recentPullInputs;
	m_recentPullOutputs = data.recentPullOutputs;
	if (!data.pushGateway.trimmed().isEmpty()) m_pushGateway = data.pushGateway;
	if (!data.pushServiceApp.trimmed().isEmpty()) m_pushServiceApp = data.pushServiceApp;
	if (!data.pushServiceStream.trimmed().isEmpty()) m_pushServiceStream = data.pushServiceStream;
	if (!data.pullGateway.trimmed().isEmpty()) m_pullGateway = data.pullGateway;
	if (!data.pullServiceApp.trimmed().isEmpty()) m_pullServiceApp = data.pullServiceApp;
	if (!data.pullServiceStream.trimmed().isEmpty()) m_pullServiceStream = data.pullServiceStream;
	if (!data.pushRouteMode.trimmed().isEmpty()) m_pushRouteMode = data.pushRouteMode;
	if (!data.pushServiceMode.trimmed().isEmpty()) m_pushServiceMode = data.pushServiceMode;
	if (!data.pushProtocolTemplate.trimmed().isEmpty()) m_pushProtocolTemplate = data.pushProtocolTemplate;
	m_pushFps = data.pushFps;
	m_pushSize = data.pushSize;
	m_pushBitrateKbps = data.pushBitrateKbps;
	if (!data.pushEncoder.trimmed().isEmpty()) m_pushEncoder = data.pushEncoder;
	if (!data.pushAudioInput.trimmed().isEmpty()) m_pushAudioInput = data.pushAudioInput;
	if (!data.pushAudioOutput.trimmed().isEmpty()) m_pushAudioOutput = data.pushAudioOutput;
	m_pushKeepAspect = data.pushKeepAspect;
	m_closeToTrayOnClose = data.closeToTrayOnClose;
	m_composeDragUseRubberBand = data.composeDragUseRubberBand;
	ComposeSourceWidget::setUseRubberBandDrag(m_composeDragUseRubberBand);
	if (!data.composeOutputSize.trimmed().isEmpty()) m_composeOutputSize = data.composeOutputSize;
	if (!data.aiEndpoint.trimmed().isEmpty()) m_aiEndpoint = data.aiEndpoint;
	if (!data.aiApiKey.trimmed().isEmpty()) m_aiApiKey = data.aiApiKey;
	if (!data.aiModel.trimmed().isEmpty()) m_aiModel = data.aiModel;
	m_aiUserBubbleColor = data.aiUserBubbleColor;
	m_aiAiBubbleColor = data.aiAiBubbleColor;
	m_aiChatBgColor = data.aiChatBgColor;
	m_aiTextColor = data.aiTextColor;
	m_userTextColor = data.userTextColor;
	m_aiSystemBubbleColor = data.aiSystemBubbleColor;
	m_aiSystemTextColor = data.aiSystemTextColor;
	m_aiSenderColor = data.aiSenderColor;
	m_aiSystemSenderColor = data.aiSystemSenderColor;
	m_aiSystemBubbleColor = data.aiSystemBubbleColor;
	m_aiSystemTextColor = data.aiSystemTextColor;
	m_aiSenderColor = data.aiSenderColor;
	m_aiSystemSenderColor = data.aiSystemSenderColor;
	if (!data.aiFontFamily.trimmed().isEmpty()) m_aiFontFamily = data.aiFontFamily;
	m_aiFontSize = (data.aiFontSize >= 8 && data.aiFontSize <= 32) ? data.aiFontSize : 13;
	m_theme = data.theme;
	if (!data.accentColor.trimmed().isEmpty()) m_accentColor = data.accentColor;
	{
		const QString backend = data.screenCaptureBackend.trimmed().toLower();
		if (backend == QStringLiteral("ffmpeg"))
		{
			m_screenBackendType = fplayer::MediaBackendType::FFmpeg;
		}
		else if (backend == QStringLiteral("dxgi"))
		{
			m_screenBackendType = fplayer::MediaBackendType::Dxgi;
		}
	}
	{
		const QString fileBackend = data.filePlaybackBackend.trimmed().toLower();
		if (fileBackend == QStringLiteral("ffmpeg"))
		{
			m_filePlaybackBackend = fplayer::MediaBackendType::FFmpeg;
		}
		else
		{
			m_filePlaybackBackend = fplayer::MediaBackendType::Qt6;
		}
	}
}


void CaptureWindow::saveCapturePreferences() const
{
	fplayer::SystemSettings data;
	data.screenshotDir = m_screenshotSaveDir;
	data.recordDir = m_recordSaveDir;
	data.recentPushInputs = m_recentPushInputs;
	data.recentPushOutputs = m_recentPushOutputs;
	data.recentPullInputs = m_recentPullInputs;
	data.recentPullOutputs = m_recentPullOutputs;
	data.pushGateway = m_pushGateway;
	data.pushServiceApp = m_pushServiceApp;
	data.pushServiceStream = m_pushServiceStream;
	data.pullGateway = m_pullGateway;
	data.pullServiceApp = m_pullServiceApp;
	data.pullServiceStream = m_pullServiceStream;
	data.pushRouteMode = m_pushRouteMode;
	data.pushServiceMode = m_pushServiceMode;
	data.pushProtocolTemplate = m_pushProtocolTemplate;
	data.pushFps = m_pushFps;
	data.pushSize = m_pushSize;
	data.pushBitrateKbps = m_pushBitrateKbps;
	data.pushEncoder = m_pushEncoder;
	data.pushAudioInput = m_pushAudioInput;
	data.pushAudioOutput = m_pushAudioOutput;
	data.pushKeepAspect = m_pushKeepAspect;
	data.closeToTrayOnClose = m_closeToTrayOnClose;
	data.composeDragUseRubberBand = m_composeDragUseRubberBand;
	data.composeOutputSize = m_composeOutputSize;
	data.aiEndpoint = m_aiEndpoint;
	data.aiApiKey = m_aiApiKey;
	data.aiModel = m_aiModel;
	data.aiUserBubbleColor = m_aiUserBubbleColor;
	data.aiAiBubbleColor = m_aiAiBubbleColor;
	data.aiChatBgColor = m_aiChatBgColor;
	data.userTextColor = m_userTextColor;
	data.aiTextColor = m_aiTextColor;
	data.aiSystemBubbleColor = m_aiSystemBubbleColor;
	data.aiSystemTextColor = m_aiSystemTextColor;
	data.aiSenderColor = m_aiSenderColor;
	data.aiSystemSenderColor = m_aiSystemSenderColor;
	data.aiFontFamily = m_aiFontFamily;
	data.aiFontSize = m_aiFontSize;
	data.theme = m_theme;
	data.accentColor = m_accentColor;
	data.screenCaptureBackend = (m_screenBackendType == fplayer::MediaBackendType::FFmpeg)
		                            ? QStringLiteral("ffmpeg")
		                            : QStringLiteral("dxgi");
	data.filePlaybackBackend = (m_filePlaybackBackend == fplayer::MediaBackendType::FFmpeg)
		                           ? QStringLiteral("ffmpeg")
		                           : QStringLiteral("qt6");
	if (m_service)
	{
		m_service->saveSystemSettings(data);
	}
}

void CaptureWindow::applyTheme()
{
	auto theme = static_cast<fplayer::tokens::Theme>(m_theme);
	const auto c = fplayer::tokens::colorsForTheme(theme);
	QString accentFocus;
	if (!m_accentColor.isEmpty())
	{
		QColor col(m_accentColor);
		if (col.isValid())
		{
			accentFocus = col.lighter(115).name();
		}
	}
	const auto qss = fplayer::tokens::globalStyleSheet(c, m_accentColor, accentFocus);
	setStyleSheet(qss);
	qApp->setStyleSheet(qss);
	const auto dialogs = findChildren<AiChatDialog*>();
	fplayer::AiConfig cfg;
	cfg.endpoint = m_aiEndpoint;
	cfg.apiKey = m_aiApiKey;
	cfg.model = m_aiModel;
	cfg.userBubbleColor = m_aiUserBubbleColor;
	cfg.aiBubbleColor = m_aiAiBubbleColor;
	cfg.chatBgColor = m_aiChatBgColor;
	cfg.aiTextColor = m_aiTextColor;
	cfg.userTextColor = m_userTextColor;
	cfg.systemBubbleColor = m_aiSystemBubbleColor;
	cfg.systemTextColor = m_aiSystemTextColor;
	cfg.senderColor = m_aiSenderColor;
	cfg.systemSenderColor = m_aiSystemSenderColor;
	cfg.fontFamily = m_aiFontFamily;
	cfg.fontSize = m_aiFontSize;
	for (auto* dlg : dialogs) {
		dlg->reconfigure(cfg);
	}
	refreshThemeIcons();
}

void CaptureWindow::refreshThemeIcons()
{
	const auto theme = static_cast<fplayer::tokens::Theme>(m_theme);
	// Static icons
	ui->btnCut->setIcon(QIcon(fplayer::tokens::themedIconPath(theme, QStringLiteral("camera"))));
	ui->btnCast->setIcon(QIcon(fplayer::tokens::themedIconPath(theme, QStringLiteral("video"))));
	ui->btnSettings->setIcon(QIcon(fplayer::tokens::themedIconPath(theme, QStringLiteral("settings"))));
	ui->btnImagePool->setIcon(QIcon(fplayer::tokens::themedIconPath(theme, QStringLiteral("pictures"))));
	ui->btnFullscreen->setIcon(QIcon(fplayer::tokens::themedIconPath(theme, QStringLiteral("fullScreen"))));
	// Play/pause: determine current playing state
	bool playing = false;
	if (m_isFileMode)
		playing = m_service && m_service->playerIsPlaying();
	else if (m_isComposeMode)
		playing = composeSourceIsPlaying(m_composeSelectedIndex);
	else if (m_captureMode == CaptureMode::Screen)
		playing = m_service && m_service->screenIsActive();
	else
		playing = m_service && m_service->cameraIsPlaying();
	ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(theme, playing ? QStringLiteral("pause") : QStringLiteral("play"))));
	// Pull preview dialog buttons
	if (m_pullPreviewPauseButton)
	{
		const bool paused = m_service && m_service->streamPreviewPaused();
		m_pullPreviewPauseButton->setIcon(QIcon(fplayer::tokens::themedIconPath(theme, paused ? QStringLiteral("pause") : QStringLiteral("play"))));
	}
	if (m_pullPreviewRefreshButton) m_pullPreviewRefreshButton->setIcon(QIcon(fplayer::tokens::themedIconPath(theme, QStringLiteral("refresh"))));
	if (m_pullPreviewShotButton) m_pullPreviewShotButton->setIcon(QIcon(fplayer::tokens::themedIconPath(theme, QStringLiteral("camera"))));
	if (m_pullPreviewRecordButton) m_pullPreviewRecordButton->setIcon(QIcon(fplayer::tokens::themedIconPath(theme, QStringLiteral("video"))));
	if (m_pullPreviewSettingsButton) m_pullPreviewSettingsButton->setIcon(QIcon(fplayer::tokens::themedIconPath(theme, QStringLiteral("settings"))));
	if (m_pullPreviewFullscreenButton) m_pullPreviewFullscreenButton->setIcon(QIcon(fplayer::tokens::themedIconPath(theme, QStringLiteral("fullScreen"))));
	if (m_pullPreviewImagePoolButton) m_pullPreviewImagePoolButton->setIcon(QIcon(fplayer::tokens::themedIconPath(theme, QStringLiteral("pictures"))));
}

void CaptureWindow::openCaptureSettingsDialog(QWidget* parent)
{
	QDialog dlg(nullptr);
	dlg.setWindowTitle(tr("系统设置"));
	auto* mainLayout = new QVBoxLayout(&dlg);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	auto* scrollArea = new QScrollArea(&dlg);
	scrollArea->setWidgetResizable(true);
	scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scrollArea->setFrameShape(QFrame::NoFrame);
	auto* scrollContent = new QWidget(scrollArea);
	auto* layout = new QFormLayout(scrollContent);
	auto* shotPath = new QLineEdit(m_screenshotSaveDir, &dlg);
	auto* recPath = new QLineEdit(m_recordSaveDir, &dlg);
	auto* shotBrowse = new QPushButton(tr("浏览..."), &dlg);
	auto* recBrowse = new QPushButton(tr("浏览..."), &dlg);
	auto* chkCloseToTray = new QCheckBox(tr("关闭主窗口时最小化到托盘（不退出程序）"), &dlg);
	chkCloseToTray->setChecked(m_closeToTrayOnClose);
	auto* chkComposeDragRubber = new QCheckBox(tr("组合模式下拖拽素材时显示虚框花纹"), &dlg);
	chkComposeDragRubber->setChecked(m_composeDragUseRubberBand);
	auto* cmbScreenBackend = new QComboBox(&dlg);
#if defined(_WIN32) && defined(FPLAYER_WITH_SCREEN_DXGI)
	cmbScreenBackend->addItem(tr("DXGI（默认）"), QStringLiteral("dxgi"));
#endif
	cmbScreenBackend->addItem(tr("FFmpeg（gdigrab）"), QStringLiteral("ffmpeg"));
	{
		const QString backend = (m_screenBackendType == fplayer::MediaBackendType::FFmpeg)
			                        ? QStringLiteral("ffmpeg")
			                        : QStringLiteral("dxgi");
		const int idx = cmbScreenBackend->findData(backend);
		cmbScreenBackend->setCurrentIndex(idx >= 0 ? idx : 0);
	}
	auto* lblHdrHint = new QLabel(
		tr("提示：检测到系统开启 HDR 时，若遇到屏幕采集问题，可尝试切换至 FFmpeg 后端，但该后端可能造成鼠标光标闪烁。"),
		&dlg);
	lblHdrHint->setWordWrap(true);
	auto* shotRow = new QWidget(&dlg);
	auto* recRow = new QWidget(&dlg);
	auto* shotLay = new QHBoxLayout(shotRow);
	auto* recLay = new QHBoxLayout(recRow);
	shotLay->setContentsMargins(0, 0, 0, 0);
	recLay->setContentsMargins(0, 0, 0, 0);
	shotLay->addWidget(shotPath, 1);
	shotLay->addWidget(shotBrowse, 0);
	recLay->addWidget(recPath, 1);
	recLay->addWidget(recBrowse, 0);
	layout->addRow(tr("截图保存目录"), shotRow);
	layout->addRow(tr("录制保存目录"), recRow);
	layout->addRow(tr("关闭行为"), chkCloseToTray);
	layout->addRow(tr("组合模式拖拽"), chkComposeDragRubber);
	layout->addRow(tr("屏幕采集后端"), cmbScreenBackend);
	layout->addRow(lblHdrHint);

	auto* cmbFileBackend = new QComboBox(&dlg);
	cmbFileBackend->addItem(tr("Qt6（默认）"), QStringLiteral("qt6"));
	cmbFileBackend->addItem(tr("FFmpeg"), QStringLiteral("ffmpeg"));
	{
		const QString currentFileBackend = (m_filePlaybackBackend == fplayer::MediaBackendType::FFmpeg)
			                                   ? QStringLiteral("ffmpeg")
			                                   : QStringLiteral("qt6");
		const int idx = cmbFileBackend->findData(currentFileBackend);
		cmbFileBackend->setCurrentIndex(idx >= 0 ? idx : 0);
	}
	auto* lblFileBackendHint = new QLabel(
		tr("文件播放后端切换后，若正在播放将自动以当前进度重新打开。"),
		&dlg);
	lblFileBackendHint->setWordWrap(true);
	layout->addRow(tr("文件播放后端"), cmbFileBackend);
	layout->addRow(lblFileBackendHint);

	auto* lblAiSection = new QLabel(tr("── AI 识别配置 ──"), &dlg);
	lblAiSection->setStyleSheet(QStringLiteral("font-weight: bold; color: #6b5ba0; margin-top: 8px;"));
	auto* aiEndpointEdit = new QLineEdit(m_aiEndpoint, &dlg);
	auto* aiApiKeyEdit = new QLineEdit(m_aiApiKey, &dlg);
	aiApiKeyEdit->setEchoMode(QLineEdit::Password);
	aiApiKeyEdit->setPlaceholderText(tr("sk-..."));
	auto* aiModelEdit = new QLineEdit(m_aiModel, &dlg);
	aiModelEdit->setPlaceholderText(tr("gpt-4o / gpt-4-vision-preview / claude-3-opus"));
	layout->addRow(lblAiSection);
	layout->addRow(tr("API 地址"), aiEndpointEdit);
	layout->addRow(tr("API Key"), aiApiKeyEdit);
	layout->addRow(tr("模型"), aiModelEdit);

	// AI chat color configuration
	auto makeColorSwatch = [&dlg, this](const QString& color) {
		const auto tc = fplayer::tokens::colorsForTheme(static_cast<fplayer::tokens::Theme>(m_theme));
		QPushButton* btn = new QPushButton(&dlg);
		btn->setFixedSize(32, 22);
		btn->setCursor(Qt::PointingHandCursor);
		btn->setToolTip(tr("点击选择颜色"));
		auto updateSwatch = [btn, tc](const QString& c) {
			btn->setStyleSheet(QStringLiteral("QPushButton{background-color:%1;border:1px solid %2;border-radius:4px;}QPushButton:hover{border-color:%3;}")
				.arg(c, tc.hairline, tc.primary));
		};
		updateSwatch(color);
		return std::make_pair(btn, updateSwatch);
	};

	auto [btnUserColor, updateUserSwatch] = makeColorSwatch(m_aiUserBubbleColor);
	auto userColorRow = new QWidget(&dlg);
	auto* userColorLay = new QHBoxLayout(userColorRow);
	userColorLay->setContentsMargins(0, 0, 0, 0);
	auto* userColorLabel = new QLabel(m_aiUserBubbleColor, &dlg);
	userColorLabel->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 12px;"));
	userColorLay->addWidget(btnUserColor);
	userColorLay->addWidget(userColorLabel);
	userColorLay->addStretch();
	layout->addRow(tr("用户气泡颜色"), userColorRow);

	auto [btnAiColor, updateAiSwatch] = makeColorSwatch(m_aiAiBubbleColor);
	auto aiColorRow = new QWidget(&dlg);
	auto* aiColorLay = new QHBoxLayout(aiColorRow);
	aiColorLay->setContentsMargins(0, 0, 0, 0);
	auto* aiColorLabel = new QLabel(m_aiAiBubbleColor, &dlg);
	aiColorLabel->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 12px;"));
	aiColorLay->addWidget(btnAiColor);
	aiColorLay->addWidget(aiColorLabel);
	aiColorLay->addStretch();
	layout->addRow(tr("AI 气泡颜色"), aiColorRow);

	auto [btnBgColor, updateBgSwatch] = makeColorSwatch(m_aiChatBgColor);
	auto bgColorRow = new QWidget(&dlg);
	auto* bgColorLay = new QHBoxLayout(bgColorRow);
	bgColorLay->setContentsMargins(0, 0, 0, 0);
	auto* bgColorLabel = new QLabel(m_aiChatBgColor, &dlg);
	bgColorLabel->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 12px;"));
	bgColorLay->addWidget(btnBgColor);
	bgColorLay->addWidget(bgColorLabel);
	bgColorLay->addStretch();
	layout->addRow(tr("聊天背景颜色"), bgColorRow);
		auto [btnTextColor, updateTextSwatch] = makeColorSwatch(m_aiTextColor);
		auto textColorRow = new QWidget(&dlg);
		auto* textColorLay = new QHBoxLayout(textColorRow);
		textColorLay->setContentsMargins(0, 0, 0, 0);
		auto* textColorLabel = new QLabel(m_aiTextColor, &dlg);
		textColorLabel->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 12px;"));
		textColorLay->addWidget(btnTextColor);
		textColorLay->addWidget(textColorLabel);
		textColorLay->addStretch();
		layout->addRow(tr("AI 文字颜色"), textColorRow);
		auto [btnUserTextColor, updateUserTextSwatch] = makeColorSwatch(m_userTextColor);
		auto userTextColorRow = new QWidget(&dlg);
		auto* userTextColorLay = new QHBoxLayout(userTextColorRow);
		userTextColorLay->setContentsMargins(0, 0, 0, 0);
		auto* userTextColorLabel = new QLabel(m_userTextColor, &dlg);
		userTextColorLabel->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 12px;"));
		userTextColorLay->addWidget(btnUserTextColor);
		userTextColorLay->addWidget(userTextColorLabel);

	userTextColorLay->addStretch();
		layout->addRow(tr("用户文字颜色"), userTextColorRow);

	auto [btnSysBubbleColor, updateSysBubbleSwatch] = makeColorSwatch(m_aiSystemBubbleColor);
	auto sysBubbleColorRow = new QWidget(&dlg);
	auto* sysBubbleColorLay = new QHBoxLayout(sysBubbleColorRow);
	sysBubbleColorLay->setContentsMargins(0, 0, 0, 0);
	auto* sysBubbleColorLabel = new QLabel(m_aiSystemBubbleColor, &dlg);
	sysBubbleColorLabel->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 12px;"));
	sysBubbleColorLay->addWidget(btnSysBubbleColor);
	sysBubbleColorLay->addWidget(sysBubbleColorLabel);
	sysBubbleColorLay->addStretch();
	layout->addRow(tr("系统气泡颜色"), sysBubbleColorRow);

	auto [btnSysTextColor, updateSysTextSwatch] = makeColorSwatch(m_aiSystemTextColor);
	auto sysTextColorRow = new QWidget(&dlg);
	auto* sysTextColorLay = new QHBoxLayout(sysTextColorRow);
	sysTextColorLay->setContentsMargins(0, 0, 0, 0);
	auto* sysTextColorLabel = new QLabel(m_aiSystemTextColor, &dlg);
	sysTextColorLabel->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 12px;"));
	sysTextColorLay->addWidget(btnSysTextColor);
	sysTextColorLay->addWidget(sysTextColorLabel);
	sysTextColorLay->addStretch();
	layout->addRow(tr("系统文字颜色"), sysTextColorRow);

	auto [btnSenderColor, updateSenderSwatch] = makeColorSwatch(m_aiSenderColor);
	auto senderColorRow = new QWidget(&dlg);
	auto* senderColorLay = new QHBoxLayout(senderColorRow);
	senderColorLay->setContentsMargins(0, 0, 0, 0);
	auto* senderColorLabel = new QLabel(m_aiSenderColor, &dlg);
	senderColorLabel->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 12px;"));
	senderColorLay->addWidget(btnSenderColor);
	senderColorLay->addWidget(senderColorLabel);
	senderColorLay->addStretch();
	layout->addRow(tr("发送者名称颜色"), senderColorRow);

	auto [btnSysSenderColor, updateSysSenderSwatch] = makeColorSwatch(m_aiSystemSenderColor);
	auto sysSenderColorRow = new QWidget(&dlg);
	auto* sysSenderColorLay = new QHBoxLayout(sysSenderColorRow);
	sysSenderColorLay->setContentsMargins(0, 0, 0, 0);
	auto* sysSenderColorLabel = new QLabel(m_aiSystemSenderColor, &dlg);
	sysSenderColorLabel->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 12px;"));
	sysSenderColorLay->addWidget(btnSysSenderColor);
	sysSenderColorLay->addWidget(sysSenderColorLabel);
	sysSenderColorLay->addStretch();
	layout->addRow(tr("系统发送者颜色"), sysSenderColorRow);

	auto* lblFontSection = new QLabel(tr("── AI 聊天字体 ──"), &dlg);
	lblFontSection->setStyleSheet(QStringLiteral("font-weight: bold; color: #6b5ba0; margin-top: 8px;"));
	layout->addRow(lblFontSection);

	auto* fontCombo = new QFontComboBox(&dlg);
	fontCombo->setCurrentFont(QFont(m_aiFontFamily.isEmpty() ? QFont().family() : m_aiFontFamily));
	fontCombo->setWritingSystem(QFontDatabase::SimplifiedChinese);
	layout->addRow(tr("字体"), fontCombo);

	auto* fontSizeSpin = new QSpinBox(&dlg);
	fontSizeSpin->setRange(8, 32);
	fontSizeSpin->setValue(m_aiFontSize);
	fontSizeSpin->setSuffix(tr(" px"));
	layout->addRow(tr("字号"), fontSizeSpin);

	auto* lblTheme = new QLabel(tr("── 主题 ──"), &dlg);
	lblTheme->setStyleSheet(QStringLiteral("font-weight: bold; color: #6e6e73; margin-top: 8px;"));
	layout->addRow(lblTheme);
	auto* themeCombo = new QComboBox(&dlg);
	themeCombo->addItem(tr("深色"), 0);
	themeCombo->addItem(tr("浅色"), 1);
	themeCombo->setCurrentIndex(m_theme);
	layout->addRow(tr("主题"), themeCombo);

	auto* lblAccent = new QLabel(tr("── 主题色 ──"), &dlg);
	lblAccent->setStyleSheet(QStringLiteral("font-weight: bold; color: #6b5ba0; margin-top: 8px;"));
	layout->addRow(lblAccent);
	auto accentColorRow = new QWidget(&dlg);
	auto* accentColorLay = new QHBoxLayout(accentColorRow);
	accentColorLay->setContentsMargins(0, 0, 0, 0);
	auto [btnAccentColor, updateAccentSwatch] = makeColorSwatch(m_accentColor);
	auto* accentColorLabel = new QLabel(m_accentColor, &dlg);
	accentColorLabel->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 12px;"));
	accentColorLay->addWidget(btnAccentColor);
	accentColorLay->addWidget(accentColorLabel);
	accentColorLay->addStretch();
	layout->addRow(tr("主题色"), accentColorRow);
	connect(btnAccentColor, &QPushButton::clicked, &dlg, [updateAccentSwatch, accentColorLabel, &dlg]() {
		QColor c = QColorDialog::getColor(QColor(accentColorLabel->text()), &dlg, tr("选择主题色"));
		if (c.isValid()) { updateAccentSwatch(c.name()); accentColorLabel->setText(c.name()); }
	});
	connect(btnUserColor, &QPushButton::clicked, &dlg, [updateUserSwatch, userColorLabel, &dlg]() {
		QColor c = QColorDialog::getColor(QColor(userColorLabel->text()), &dlg, tr("选择用户气泡颜色"));
		if (c.isValid()) { updateUserSwatch(c.name()); userColorLabel->setText(c.name()); }
	});
	connect(btnAiColor, &QPushButton::clicked, &dlg, [updateAiSwatch, aiColorLabel, &dlg]() {
		QColor c = QColorDialog::getColor(QColor(aiColorLabel->text()), &dlg, tr("选择AI气泡颜色"));
		if (c.isValid()) { updateAiSwatch(c.name()); aiColorLabel->setText(c.name()); }
	});
	connect(btnBgColor, &QPushButton::clicked, &dlg, [updateBgSwatch, bgColorLabel, &dlg]() {
		QColor c = QColorDialog::getColor(QColor(bgColorLabel->text()), &dlg, tr("选择聊天背景颜色"));
		if (c.isValid()) { updateBgSwatch(c.name()); bgColorLabel->setText(c.name()); }
	});
	connect(btnTextColor, &QPushButton::clicked, &dlg, [updateTextSwatch, textColorLabel, &dlg]() {
		QColor c = QColorDialog::getColor(QColor(textColorLabel->text()), &dlg, tr("选择AI文字颜色"));
		if (c.isValid()) { updateTextSwatch(c.name()); textColorLabel->setText(c.name()); }
	});
	connect(btnUserTextColor, &QPushButton::clicked, &dlg, [updateUserTextSwatch, userTextColorLabel, &dlg]() {
		QColor c = QColorDialog::getColor(QColor(userTextColorLabel->text()), &dlg, tr("选择用户文字颜色"));
		if (c.isValid()) { updateUserTextSwatch(c.name()); userTextColorLabel->setText(c.name()); }
	});
	connect(btnSysBubbleColor, &QPushButton::clicked, &dlg, [updateSysBubbleSwatch, sysBubbleColorLabel, &dlg]() {
		QColor c = QColorDialog::getColor(QColor(sysBubbleColorLabel->text()), &dlg, tr("选择系统气泡颜色"));
		if (c.isValid()) { updateSysBubbleSwatch(c.name()); sysBubbleColorLabel->setText(c.name()); }
	});
	connect(btnSysTextColor, &QPushButton::clicked, &dlg, [updateSysTextSwatch, sysTextColorLabel, &dlg]() {
		QColor c = QColorDialog::getColor(QColor(sysTextColorLabel->text()), &dlg, tr("选择系统文字颜色"));
		if (c.isValid()) { updateSysTextSwatch(c.name()); sysTextColorLabel->setText(c.name()); }
	});
	connect(btnSenderColor, &QPushButton::clicked, &dlg, [updateSenderSwatch, senderColorLabel, &dlg]() {
		QColor c = QColorDialog::getColor(QColor(senderColorLabel->text()), &dlg, tr("选择发送者名称颜色"));
		if (c.isValid()) { updateSenderSwatch(c.name()); senderColorLabel->setText(c.name()); }
	});
	connect(btnSysSenderColor, &QPushButton::clicked, &dlg, [updateSysSenderSwatch, sysSenderColorLabel, &dlg]() {
		QColor c = QColorDialog::getColor(QColor(sysSenderColorLabel->text()), &dlg, tr("选择系统发送者颜色"));
		if (c.isValid()) { updateSysSenderSwatch(c.name()); sysSenderColorLabel->setText(c.name()); }
	});

	auto* lblLinks = new QLabel(tr("── 相关链接 ──"), &dlg);
	lblLinks->setStyleSheet(QStringLiteral("font-weight: bold; color: #6e6e73; margin-top: 8px;"));
	layout->addRow(lblLinks);
	auto* linkLabel = new QLabel(&dlg);
	linkLabel->setTextFormat(Qt::RichText);
	linkLabel->setOpenExternalLinks(true);
	linkLabel->setText(QStringLiteral(
		"%1: <a href=\"http://codis.fun:5003\">http://codis.fun:5003</a>&nbsp;&nbsp;|&nbsp;&nbsp;"
		"%2: <a href=\"https://github.com/ff-283\">https://github.com/ff-283</a>"
	).arg(tr("官网地址"), tr("GitHub")));
	layout->addRow(linkLabel);

	auto* lblVersionSection = new QLabel(tr("── 版本信息 ──"), &dlg);
	lblVersionSection->setStyleSheet(QStringLiteral("font-weight: bold; color: #6e6e73; margin-top: 8px;"));
	layout->addRow(lblVersionSection);
	auto* versionLabel = new QLabel(QStringLiteral("FPlayer Desktop v" FPLAYER_VERSION), &dlg);
	versionLabel->setStyleSheet(QStringLiteral("color: #8e8e93;"));
	layout->addRow(versionLabel);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	scrollArea->setWidget(scrollContent);
	mainLayout->addWidget(scrollArea, 1);
	mainLayout->addWidget(buttons, 0);
	connect(shotBrowse, &QPushButton::clicked, &dlg, [shotPath, this]() {
		const QString dir = QFileDialog::getExistingDirectory(this, tr("选择截图保存目录"), shotPath->text().trimmed());
		if (!dir.isEmpty())
		{
			shotPath->setText(QDir::toNativeSeparators(dir));
		}
	});
	connect(recBrowse, &QPushButton::clicked, &dlg, [recPath, this]() {
		const QString dir = QFileDialog::getExistingDirectory(this, tr("选择录制保存目录"), recPath->text().trimmed());
		if (!dir.isEmpty())
		{
			recPath->setText(QDir::toNativeSeparators(dir));
		}
	});
	connect(buttons, &QDialogButtonBox::accepted, &dlg, [&dlg, shotPath, recPath, chkCloseToTray, chkComposeDragRubber, cmbScreenBackend, cmbFileBackend, aiEndpointEdit, aiApiKeyEdit, aiModelEdit, userColorLabel, aiColorLabel, bgColorLabel, textColorLabel, userTextColorLabel, sysBubbleColorLabel, sysTextColorLabel, senderColorLabel, 	sysSenderColorLabel, accentColorLabel, fontCombo, fontSizeSpin, themeCombo, this]() {
		const QString shot = shotPath->text().trimmed();
		const QString rec = recPath->text().trimmed();
		if (shot.isEmpty() || rec.isEmpty())
		{
			QMessageBox::warning(&dlg, tr("配置无效"), tr("截图与录制目录不能为空。"));
			return;
		}
		QDir shotDir(shot);
		QDir recDir(rec);
		if ((!shotDir.exists() && !shotDir.mkpath(QStringLiteral("."))) ||
			(!recDir.exists() && !recDir.mkpath(QStringLiteral("."))))
		{
			QMessageBox::warning(&dlg, tr("配置无效"), tr("目录不存在且无法自动创建，请检查权限。"));
			return;
		}
		m_screenshotSaveDir = shotDir.absolutePath();
		m_recordSaveDir = recDir.absolutePath();
		m_closeToTrayOnClose = chkCloseToTray->isChecked();
		m_composeDragUseRubberBand = chkComposeDragRubber->isChecked();
		const QString backend = cmbScreenBackend->currentData().toString().trimmed().toLower();
		fplayer::MediaBackendType selectedBackend = fplayer::MediaBackendType::FFmpeg;
#if defined(_WIN32) && defined(FPLAYER_WITH_SCREEN_DXGI)
		if (backend == QStringLiteral("dxgi"))
		{
			selectedBackend = fplayer::MediaBackendType::Dxgi;
		}
#endif
		const bool backendChanged = (selectedBackend != m_screenBackendType);
		struct ComposeScreenRuntimeState
		{
			int index = -1;
			int deviceIndex = 0;
			int fps = 30;
			bool captureCursor = false;
			bool wasActive = false;
		};
		const bool mainWasScreenActive = (m_captureMode == CaptureMode::Screen && m_service) ? m_service->screenIsActive() : false;
		const int mainScreenIndex = (m_captureMode == CaptureMode::Screen && ui->cmbDevices) ? ui->cmbDevices->currentIndex() : -1;
		QList<ComposeScreenRuntimeState> composeScreenStates;
		if (backendChanged && m_isComposeMode)
		{
			for (int i = 0; i < m_composeSources.size(); ++i)
			{
				const auto& src = m_composeSources[i];
				if (src.kind != ComposeSourceItem::SourceKind::Screen || !src.service)
				{
					continue;
				}
				ComposeScreenRuntimeState st;
				st.index = i;
				st.deviceIndex = src.deviceIndex;
				st.fps = src.screenFps;
				st.captureCursor = src.screenCaptureCursor;
				st.wasActive = src.service->screenIsActive();
				composeScreenStates.push_back(st);
			}
		}
		m_screenBackendType = selectedBackend;
		saveAndApplyTheme();
		if (backendChanged && m_service)
		{
			// 热切换：先停采集，再切后端，最后恢复原先状态。
			if (m_captureMode == CaptureMode::Screen)
			{
				m_service->screenSetActive(false);
			}
			for (const auto& st : composeScreenStates)
			{
				if (st.index >= 0 && st.index < m_composeSources.size() && m_composeSources[st.index].service)
				{
					m_composeSources[st.index].service->screenSetActive(false);
				}
			}
			m_service->initScreenCapture(m_screenBackendType);
			if (m_captureMode == CaptureMode::Screen)
			{
				ui->wgtView->setBackendType(m_screenBackendType);
				m_service->bindScreenPreview(ui->wgtView);
				refreshScreenDeviceUi();
				if (mainScreenIndex >= 0 && ui->cmbDevices && ui->cmbDevices->count() > 0)
				{
					const int idx = qBound(0, mainScreenIndex, ui->cmbDevices->count() - 1);
					selectScreen(idx);
				}
				if (!mainWasScreenActive)
				{
					m_service->screenSetActive(false);
					ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("play"))));
				}
			}
			for (const auto& st : composeScreenStates)
			{
				if (st.index < 0 || st.index >= m_composeSources.size())
				{
					continue;
				}
				auto& src = m_composeSources[st.index];
				if (src.kind != ComposeSourceItem::SourceKind::Screen || !src.service)
				{
					continue;
				}
				src.service->initScreenCapture(m_screenBackendType);
				if (src.view)
				{
					src.view->setBackendType(m_screenBackendType);
				}
				src.service->bindScreenPreview(src.view);
				src.service->selectScreen(st.deviceIndex);
				src.service->screenSetFrameRate(qMax(1, st.fps));
				src.service->screenSetCursorCaptureEnabled(st.captureCursor);
				src.service->screenSetActive(st.wasActive);
			}
			if (m_isComposeMode)
			{
				refreshComposeScreenCaptureState(m_composeSelectedIndex);
				updateComposePlaybackIcons();
				forceRefreshComposePreview();
			}
		}
		// 文件播放后端热切换
		{
			const QString fileBackendStr = cmbFileBackend->currentData().toString().trimmed().toLower();
			const fplayer::MediaBackendType selectedFileBackend = (fileBackendStr == QStringLiteral("ffmpeg"))
				                                                      ? fplayer::MediaBackendType::FFmpeg
				                                                      : fplayer::MediaBackendType::Qt6;
			const bool fileBackendChanged = (selectedFileBackend != m_filePlaybackBackend);
			if (fileBackendChanged && m_service)
			{
				// 保存当前播放状态
				const bool wasFileMode = m_isFileMode;
				const QString currentFile = m_currentFilePath;
				const qint64 savedPosition = wasFileMode ? m_service->playerPositionMs() : 0;
				const bool wasPlaying = wasFileMode && m_service->playerIsPlaying();

				// 停止并切换后端
				m_service->playerStop();
				m_filePlaybackBackend = selectedFileBackend;
				m_service->initPlayer(m_filePlaybackBackend);
				ui->wgtView->setBackendType(m_filePlaybackBackend);
				m_service->bindPlayerPreview(ui->wgtView);

				// 如果正在文件模式，重新打开并恢复进度
				if (wasFileMode && !currentFile.isEmpty())
				{
					if (m_service->openMediaFile(currentFile))
					{
						if (savedPosition > 0)
						{
							m_service->playerSeekMs(savedPosition);
						}
						if (!wasPlaying)
						{
							m_service->playerPause();
						}
						// 更新组合推流通道
						for (auto& src : m_composeSources)
						{
							if (src.kind == ComposeSourceItem::SourceKind::File && src.service == m_service)
							{
								m_service->setPlayerComposeStreamBusId(src.sourceId);
								break;
							}
						}
					}
				}

				// 更新组合模式中所有文件素材的服务
				for (auto& src : m_composeSources)
				{
					if (src.kind == ComposeSourceItem::SourceKind::File && src.service && src.service != m_service)
					{
						src.service->playerDebugStats();
						if (src.view)
						{
							src.view->setBackendType(m_filePlaybackBackend);
						}
					}
				}
			}
		}

		m_aiEndpoint = aiEndpointEdit->text().trimmed();
		m_aiApiKey = aiApiKeyEdit->text().trimmed();
		m_aiModel = aiModelEdit->text().trimmed();
		m_aiUserBubbleColor = userColorLabel->text();
		m_aiAiBubbleColor = aiColorLabel->text();
			m_userTextColor = userTextColorLabel->text();
		m_aiSystemBubbleColor = sysBubbleColorLabel->text();
		m_aiSystemTextColor = sysTextColorLabel->text();
		m_aiSenderColor = senderColorLabel->text();
		m_aiSystemSenderColor = sysSenderColorLabel->text();
		m_aiChatBgColor = bgColorLabel->text();
			m_aiTextColor = textColorLabel->text();
		m_aiFontFamily = fontCombo->currentFont().family();
		m_aiFontSize = fontSizeSpin->value();
		m_theme = themeCombo->currentData().toInt();
		m_accentColor = accentColorLabel->text();
		saveAndApplyTheme();
		m_imagePoolSidebar->setScreenshotDir(m_screenshotSaveDir);
		const auto dialogs = findChildren<AiChatDialog*>();
		for (auto* dlg : dialogs)
		{
			fplayer::AiConfig cfg;
			cfg.endpoint = m_aiEndpoint;
			cfg.apiKey = m_aiApiKey;
			cfg.model = m_aiModel;
			cfg.userBubbleColor = m_aiUserBubbleColor;
			cfg.aiBubbleColor = m_aiAiBubbleColor;
			cfg.chatBgColor = m_aiChatBgColor;
				cfg.aiTextColor = m_aiTextColor;
				cfg.userTextColor = m_userTextColor;
			cfg.systemBubbleColor = m_aiSystemBubbleColor;
			cfg.systemTextColor = m_aiSystemTextColor;
			cfg.senderColor = m_aiSenderColor;
			cfg.systemSenderColor = m_aiSystemSenderColor;
			cfg.fontFamily = m_aiFontFamily;
			cfg.fontSize = m_aiFontSize;
			dlg->reconfigure(cfg);
		}
		dlg.accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	if (auto* screen = QGuiApplication::primaryScreen()) {
		const QRect avail = screen->availableGeometry();
		dlg.resize(qMin(760, avail.width() - 80), qMin(600, avail.height() - 80));
		dlg.setMinimumSize(640, 400);
		dlg.move(avail.center() - dlg.rect().center());
	}
	dlg.exec();
}

QString CaptureWindow::makeScreenshotFilePath(const QString& prefix) const
{
	const QString fileName = QStringLiteral("%1_%2.png")
	                         .arg(prefix)
	                         .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
	return QDir(m_screenshotSaveDir).filePath(fileName);
}

QString CaptureWindow::makeRecordingFilePath(const QString& prefix) const
{
	const QString fileName = QStringLiteral("%1_%2.mkv")
	                         .arg(prefix)
	                         .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
	return QDir(m_recordSaveDir).filePath(fileName);
}

QSize CaptureWindow::preferredComposeExportSize() const
{
	if (m_composeSizeCombo && m_composeSizeCombo->count() > 0)
	{
		QString sizeText = m_composeSizeCombo->currentData().toString().trimmed();
		if (sizeText.isEmpty())
		{
			sizeText = m_composeSizeCombo->currentText().trimmed();
		}
		const QRegularExpression re(R"((\d+)\s*x\s*(\d+))", QRegularExpression::CaseInsensitiveOption);
		const auto m = re.match(sizeText);
		if (m.hasMatch())
		{
			const int w = m.captured(1).toInt();
			const int h = m.captured(2).toInt();
			if (w > 0 && h > 0)
			{
				return {w, h};
			}
		}
	}
	const int aw = qMax(1, m_composeAspectW);
	const int ah = qMax(1, m_composeAspectH);
	// 组合导出默认以 1080p 基准，保证小窗口下也有可用清晰度。
	if (aw >= ah)
	{
		const int w = 1920;
		const int h = qMax(2, ((w * ah) / aw) & ~1);
		return {w, h};
	}
	const int h = 1920;
	const int w = qMax(2, ((h * aw) / ah) & ~1);
	return {w, h};
}

void CaptureWindow::refreshComposeOutputSizeOptions()
{
	if (!m_composeSizeCombo)
	{
		return;
	}
	const QString prev = m_composeSizeCombo->currentData().toString().trimmed();
	m_composeSizeCombo->clear();
	const int aw = qMax(1, m_composeAspectW);
	const int ah = qMax(1, m_composeAspectH);
	const QList<int> widths{3840, 2560, 1920, 1600, 1280, 960, 854, 640};
	for (const int w : widths)
	{
		const int h = qMax(2, ((w * ah) / aw) & ~1);
		const QString text = QStringLiteral("%1x%2").arg(w).arg(h);
		m_composeSizeCombo->addItem(text, text);
	}
	int idx = -1;
	if (!prev.isEmpty())
	{
		idx = m_composeSizeCombo->findData(prev);
	}
	if (idx < 0 && !m_composeOutputSize.trimmed().isEmpty())
	{
		idx = m_composeSizeCombo->findData(m_composeOutputSize.trimmed());
	}
	if (idx < 0)
	{
		idx = m_composeSizeCombo->findData(QStringLiteral("1920x%1").arg(qMax(2, ((1920 * ah) / aw) & ~1)));
	}
	m_composeSizeCombo->setCurrentIndex(idx >= 0 ? idx : 0);
	m_composeOutputSize = m_composeSizeCombo->currentData().toString();
}

QImage CaptureWindow::buildComposeSnapshotImage(const QSize& outSize) const
{
	if (!m_composeMdiArea || !m_composePreviewHost || outSize.width() <= 0 || outSize.height() <= 0)
	{
		return {};
	}
	QImage canvas(outSize, QImage::Format_RGB888);
	canvas.fill(Qt::black);
	const QRect srcBounds = m_composeMdiArea->viewport()->rect();
	if (srcBounds.width() <= 0 || srcBounds.height() <= 0)
	{
		return canvas;
	}
	QPainter painter(&canvas);
	painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
	for (const auto& src : m_composeSources)
	{
		if (!src.subWindow)
		{
			continue;
		}
		QImage srcImage;
		if (src.kind == ComposeSourceItem::SourceKind::Camera)
		{
			const auto frame = fplayer::CameraFrameBus::instance().snapshot(src.sourceId);
			if (frame.valid)
			{
				srcImage = i420ToImage(frame.y, frame.u, frame.v, frame.width, frame.height, frame.yStride, frame.uStride, frame.vStride);
			}
		}
		else
		{
			const auto frame = fplayer::ScreenFrameBus::instance().snapshot(src.sourceId);
			if (frame.valid)
			{
				srcImage = i420ToImage(frame.y, frame.u, frame.v, frame.width, frame.height, frame.yStride, frame.uStride, frame.vStride);
			}
		}
		if (srcImage.isNull())
		{
			continue;
		}
		const QRect g = src.subWindow->geometry();
		const QRect target(
			qRound((static_cast<double>(g.x()) / srcBounds.width()) * outSize.width()),
			qRound((static_cast<double>(g.y()) / srcBounds.height()) * outSize.height()),
			qRound((static_cast<double>(g.width()) / srcBounds.width()) * outSize.width()),
			qRound((static_cast<double>(g.height()) / srcBounds.height()) * outSize.height()));
		if (target.width() <= 1 || target.height() <= 1)
		{
			continue;
		}
		{
			// 保持源帧的宽高比，在目标区域内居中绘制，与预览窗口中的实际显示保持一致。
			const double srcAspect = static_cast<double>(srcImage.width()) / qMax(1, srcImage.height());
			const double targetAspect = static_cast<double>(target.width()) / qMax(1, target.height());
			QRect fitted = target;
			if (srcAspect > targetAspect)
			{
				const int fittedH = qMax(1, qRound(target.width() / srcAspect));
				fitted.setY(target.y() + (target.height() - fittedH) / 2);
				fitted.setHeight(fittedH);
			}
			else
			{
				const int fittedW = qMax(1, qRound(target.height() * srcAspect));
				fitted.setX(target.x() + (target.width() - fittedW) / 2);
				fitted.setWidth(fittedW);
			}
			painter.drawImage(fitted, srcImage);
		}
	}
	painter.end();
	return canvas;
}

void CaptureWindow::updateRecordButtonUi()
{
	const qint64 elapsed = m_mainRecording ? (QDateTime::currentMSecsSinceEpoch() - m_mainRecordStartMs) : 0;
	this->ui->btnCast->setText(m_mainRecording ? formatTimeMs(elapsed) : QString());
	this->ui->btnCast->setToolTip(m_mainRecording ? tr("结束录制") : tr("录制"));
}

void CaptureWindow::updatePullRecordButtonUi()
{
	if (!m_pullPreviewRecordDurationLabel)
	{
		return;
	}
	const bool pullRunning = m_service && m_service->streamIsRunning();
	const qint64 recordElapsed = m_pullRecording ? (QDateTime::currentMSecsSinceEpoch() - m_pullRecordStartMs) : 0;
	const qint64 elapsed = (pullRunning && m_pullSessionStartMs > 0) ? (QDateTime::currentMSecsSinceEpoch() - m_pullSessionStartMs) : 0;
	QString text = tr("拉流时长：%1").arg(formatTimeMs(elapsed));
	if (m_pullRecording)
	{
		text += tr("  录制：%1").arg(formatTimeMs(recordElapsed));
	}
	m_pullPreviewRecordDurationLabel->setText(text);
	if (m_pullPreviewRecordButton)
	{
		m_pullPreviewRecordButton->setToolTip(m_pullRecording ? tr("结束录制") : tr("开始录制"));
	}
}

void CaptureWindow::handleMainCaptureScreenshot()
{
	QImage sourceImage;
	// 优先走原始帧截图（不受预览窗口缩放影响），无可用原帧时再回退窗口抓图。
	if (m_isComposeMode)
	{
		sourceImage = buildComposeSnapshotImage(preferredComposeExportSize());
	}
	else if (m_captureMode == CaptureMode::Camera && m_service)
	{
		const auto frame = fplayer::CameraFrameBus::instance().snapshot(m_service->cameraFrameBusSourceId());
		if (frame.valid)
		{
			sourceImage = i420ToImage(frame.y, frame.u, frame.v, frame.width, frame.height, frame.yStride, frame.uStride, frame.vStride);
		}
	}
	else if (!m_isComposeMode && m_captureMode == CaptureMode::Screen && m_service)
	{
		const auto frame = fplayer::ScreenFrameBus::instance().snapshot(m_service->screenFrameBusSourceId());
		if (frame.valid)
		{
			sourceImage = i420ToImage(frame.y, frame.u, frame.v, frame.width, frame.height, frame.yStride, frame.uStride, frame.vStride);
		}
	}
	else if (m_captureMode == CaptureMode::File && m_service)
	{
		sourceImage = m_service->playerCurrentFrameImage();
	}
	if (sourceImage.isNull())
	{
		const QPixmap fallbackShot = (m_isComposeMode && m_composePreviewHost)
			                             ? m_composePreviewHost->grab()
			                             : this->ui->wgtView->grab();
		sourceImage = fallbackShot.toImage();
	}
	const QPixmap shot = QPixmap::fromImage(sourceImage);
	if (shot.isNull())
	{
		QMessageBox::warning(this, tr("截图失败"), tr("当前画面为空，无法截图。"));
		return;
	}
	QDir dir(m_screenshotSaveDir);
	if (!dir.exists())
	{
		dir.mkpath(QStringLiteral("."));
	}
	const QString savePath = makeScreenshotFilePath(QStringLiteral("preview_shot"));
	if (!shot.save(savePath))
	{
		QMessageBox::warning(this, tr("截图失败"), tr("图片保存失败：%1").arg(savePath));
		return;
	}
	showNonBlockingHint(this, tr("截图已保存：%1").arg(QDir::toNativeSeparators(savePath)));
		emit screenshotSaved(savePath);
}

void CaptureWindow::handleMainCaptureRecordToggle()
{
	if (!m_service)
	{
		return;
	}
	if (m_mainRecording)
	{
		m_service->streamStop();
		if (m_pullRecordService)
		{
			m_pullRecordService->streamStop();
		}
		m_mainRecording = false;
		if (m_mainRecordTimer)
		{
			m_mainRecordTimer->stop();
		}
		updateRecordButtonUi();
		// 录制停止后，恢复当前场景的本地预览状态，避免出现“停止录制即暂停预览”。
		if (m_captureMode == CaptureMode::Camera)
		{
			if (!m_service->cameraIsPlaying())
			{
				m_service->cameraResume();
			}
			this->ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("pause"))));
		}
		else if (m_captureMode == CaptureMode::Screen)
		{
			if (!m_service->screenIsActive())
			{
				m_service->screenSetActive(true);
			}
			this->ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("pause"))));
		}
		else if (m_captureMode == CaptureMode::File)
		{
			if (!m_service->playerIsPlaying())
			{
				m_service->playerResume();
			}
			this->ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("pause"))));
		}
		showNonBlockingHint(this, tr("录制已停止，文件已保存：%1").arg(QDir::toNativeSeparators(m_mainRecordOutputPath)));
		return;
	}
	if (m_service->streamIsRunning())
	{
		QMessageBox::warning(this, tr("录制失败"), tr("当前已有推拉流任务在运行，请先停止后再开始主预览录制。"));
		return;
	}
	QString outputPath = makeRecordingFilePath(QStringLiteral("preview_record"));
	QDir outDir(m_recordSaveDir);
	if (!outDir.exists())
	{
		outDir.mkpath(QStringLiteral("."));
	}
	bool started = false;
	if (m_isComposeMode)
	{
		QString inputSpec;
		const int fps = 30;
		const QSize exportSize = preferredComposeExportSize();
		const int outW = exportSize.width();
		const int outH = exportSize.height();
		if (!buildComposeScreenCaptureSpec(inputSpec, fps, outW, outH, 2500, QStringLiteral("auto"), QStringLiteral("off"),
		                                   QStringLiteral("off")))
		{
			QMessageBox::warning(this, tr("录制失败"), tr("组合预览窗口不可用，请先进入组合模式并添加素材。"));
			return;
		}
		started = m_service->streamStartPush(inputSpec, outputPath);
	}
	else if (m_captureMode == CaptureMode::Camera)
	{
		started = m_service->streamStartPushByScene(fplayer::Service::PushScene::Camera, outputPath);
	}
	else if (m_captureMode == CaptureMode::Screen)
	{
		started = m_service->streamStartPushByScene(fplayer::Service::PushScene::Screen, outputPath);
	}
	else if (m_captureMode == CaptureMode::File)
	{
		started = m_service->streamStartPushByScene(fplayer::Service::PushScene::File, outputPath, m_currentFilePath);
	}
	else
	{
		QMessageBox::warning(this, tr("录制失败"), tr("当前模式暂不支持一键录制。"));
		return;
	}
	if (!started)
	{
		QMessageBox::warning(this, tr("录制失败"), m_service->streamLastError());
		return;
	}
	m_mainRecording = true;
	m_mainRecordOutputPath = outputPath;
	m_mainRecordStartMs = QDateTime::currentMSecsSinceEpoch();
	if (m_mainRecordTimer)
	{
		m_mainRecordTimer->start();
	}
	updateRecordButtonUi();
}

void CaptureWindow::handleMainCaptureSettings()
{
	openCaptureSettingsDialog(this);
}

void CaptureWindow::togglePlayPause()
{
	if (m_isComposeMode)
	{
		if (m_composeSelectedIndex < 0 || m_composeSelectedIndex >= m_composeSources.size())
		{
			return;
		}
		toggleComposeSourcePlayPauseAt(m_composeSelectedIndex);
		return;
	}
	if (m_captureMode == CaptureMode::File)
	{
		if (this->m_service->playerIsPlaying())
		{
			this->m_service->playerPause();
			this->ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("play"))));
		}
		else
		{
			this->m_service->playerResume();
			this->ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("pause"))));
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
		this->ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme),
			!active ? QStringLiteral("pause") : QStringLiteral("play"))));
		return;
	}

	if (this->m_service->cameraIsPlaying())
	{
		this->m_service->cameraPause();
		this->ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("play"))));
	}
	else
	{
		this->m_service->cameraResume();
		this->ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("pause"))));
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
	clearComposeSources();
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
	if (m_pullRecordService)
	{
		m_pullRecordService->streamStop();
		delete m_pullRecordService;
		m_pullRecordService = nullptr;
	}
	delete ui;
}

void CaptureWindow::ensureComposeWorkspace()
{
	if (m_composeSplitter)
	{
		return;
	}
	m_composeSplitter = new QSplitter(Qt::Horizontal, this);
	auto* leftPanel = new QWidget(m_composeSplitter);
	leftPanel->setObjectName(QStringLiteral("composeLeftPanel"));
	leftPanel->setMinimumWidth(220);
	auto* leftLayout = new QVBoxLayout(leftPanel);
	leftLayout->setContentsMargins(8, 8, 8, 8);
	leftLayout->setSpacing(8);
	auto* titleLabel = new QLabel(tr("视频流来源"), leftPanel);
	titleLabel->setStyleSheet(QStringLiteral("font-weight:600;"));
	auto* aspectLabel = new QLabel(tr("画布比例"), leftPanel);
	auto* sizeLabel = new QLabel(tr("尺寸"), leftPanel);
	m_composeAspectCombo = new QComboBox(leftPanel);
	m_composeAspectCombo->addItem(QStringLiteral("16:9 (横屏直播)"), QStringLiteral("16:9"));
	m_composeAspectCombo->addItem(QStringLiteral("9:16 (手机竖屏)"), QStringLiteral("9:16"));
	m_composeAspectCombo->addItem(QStringLiteral("4:3"), QStringLiteral("4:3"));
	m_composeAspectCombo->addItem(QStringLiteral("3:4"), QStringLiteral("3:4"));
	m_composeAspectCombo->addItem(QStringLiteral("1:1"), QStringLiteral("1:1"));
	m_composeAspectCombo->setCurrentIndex(0);
	m_composeSizeCombo = new QComboBox(leftPanel);
	m_btnComposeAddFile = new QPushButton(tr("追加文件播放"), leftPanel);
	m_btnComposeAddCamera = new QPushButton(tr("追加摄像头"), leftPanel);
	m_btnComposeAddScreen = new QPushButton(tr("追加屏幕"), leftPanel);
	m_composeSourceList = new QListWidget(leftPanel);
	m_composeSourceList->setSelectionMode(QAbstractItemView::SingleSelection);
	leftLayout->addWidget(titleLabel);
	leftLayout->addWidget(aspectLabel);
	leftLayout->addWidget(m_composeAspectCombo);
	leftLayout->addWidget(sizeLabel);
	leftLayout->addWidget(m_composeSizeCombo);
	leftLayout->addWidget(m_btnComposeAddFile);
	leftLayout->addWidget(m_btnComposeAddCamera);
	leftLayout->addWidget(m_btnComposeAddScreen);
	leftLayout->addWidget(m_composeSourceList, 1);

	m_composePreviewHost = new AspectRatioHostWidget(m_composeSplitter);
	m_composeMdiArea = new QMdiArea(m_composePreviewHost);
	m_composeMdiArea->setObjectName(QStringLiteral("composeMdiArea"));
	m_composeMdiArea->setViewMode(QMdiArea::SubWindowView);
	m_composeMdiArea->setOption(QMdiArea::DontMaximizeSubWindowOnActivation, true);
	m_composeMdiArea->setOption(QMdiArea::DontMaximizeSubWindowOnActivation, true);
	m_composeMdiArea->setActivationOrder(QMdiArea::StackingOrder);
	m_composeMdiArea->setUpdatesEnabled(true);
	if (m_composeMdiArea->viewport())
	{
		m_composeMdiArea->viewport()->setAutoFillBackground(true);
		m_composeMdiArea->viewport()->setAttribute(Qt::WA_OpaquePaintEvent, true);
	}
	static_cast<AspectRatioHostWidget*>(m_composePreviewHost)->attachContent(m_composeMdiArea);
	auto* hostWidget = static_cast<AspectRatioHostWidget*>(m_composePreviewHost);
	hostWidget->onContentGeometryChanged = [this](const QRect& oldRect, const QRect& newRect) {
		if (!m_isComposeMode)
		{
			return;
		}
		remapComposeSourcesToViewport(oldRect, newRect);
		applyComposeZOrder();
		forceRefreshComposePreview();
	};
	hostWidget->setAspectRatio(m_composeAspectW, m_composeAspectH);
	m_composeSplitter->setStretchFactor(0, 0);
	m_composeSplitter->setStretchFactor(1, 1);
	m_composeSplitter->hide();
	ui->verticalLayout->insertWidget(0, m_composeSplitter, 1);

	connect(m_btnComposeAddFile, &QPushButton::clicked, this, &CaptureWindow::addComposeFileSource);
	connect(m_btnComposeAddCamera, &QPushButton::clicked, this, &CaptureWindow::addComposeCameraSource);
	connect(m_btnComposeAddScreen, &QPushButton::clicked, this, &CaptureWindow::addComposeScreenSource);
	connect(m_composeAspectCombo, &QComboBox::currentIndexChanged, this, [this](const int index) {
		if (!m_composeAspectCombo || index < 0)
		{
			return;
		}
		const QString text = m_composeAspectCombo->itemData(index).toString();
		const auto parts = text.split(':');
		if (parts.size() == 2)
		{
			bool wOk = false;
			bool hOk = false;
			const int w = parts.at(0).toInt(&wOk);
			const int h = parts.at(1).toInt(&hOk);
			if (wOk && hOk && w > 0 && h > 0)
			{
				m_composeAspectW = w;
				m_composeAspectH = h;
				refreshComposeOutputSizeOptions();
				applyComposeAspectRatio();
			}
		}
	});
	connect(m_composeSizeCombo, &QComboBox::currentIndexChanged, this, [this](const int index) {
		if (!m_composeSizeCombo || index < 0)
		{
			return;
		}
		m_composeOutputSize = m_composeSizeCombo->itemData(index).toString();
		saveAndApplyTheme();
	});
	refreshComposeOutputSizeOptions();
	connect(m_composeMdiArea, &QMdiArea::subWindowActivated, this, [this](QMdiSubWindow*) {
		if (!m_isComposeMode)
		{
			return;
		}
		QTimer::singleShot(0, this, [this]() {
			applyComposeZOrder();
		});
	});
	connect(qApp, &QApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
		if (!m_isComposeMode || state != Qt::ApplicationActive)
		{
			return;
		}
		QTimer::singleShot(0, this, [this]() {
			applyComposeZOrder();
		});
	});
	m_composeZOrderGuardTimer = new QTimer(this);
	m_composeZOrderGuardTimer->setInterval(400);
	connect(m_composeZOrderGuardTimer, &QTimer::timeout, this, [this]() {
		if (!m_isComposeMode)
		{
			return;
		}
		for (const auto& src : m_composeSources)
		{
			auto* container = static_cast<ComposeSourceWidget*>(src.container);
			if (container && container->isDragging())
			{
				return;
			}
		}
		applyComposeZOrder();
	});
	m_composeSourceList->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(m_composeSourceList, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
		if (!m_composeSourceList)
		{
			return;
		}
		const int row = m_composeSourceList->indexAt(pos).row();
		if (row < 0 || row >= m_composeSources.size())
		{
			return;
		}
		QMenu menu(this);
		auto* actionDelete = menu.addAction(tr("删除"));
		if (menu.exec(m_composeSourceList->viewport()->mapToGlobal(pos)) == actionDelete)
		{
			removeComposeSourceAt(row);
		}
	});
	connect(m_composeSourceList, &QListWidget::currentRowChanged, this, [this](const int row) {
		if (row < 0 || row >= m_composeSources.size())
		{
			return;
		}
		m_composeSelectedIndex = row;
		updateComposeSelectionHighlight();
		syncComposeControlPanel();
	});
	connect(m_composeSourceList, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
		if (!item || !m_composeSourceList) return;
		const int row = m_composeSourceList->row(item);
		if (row < 0 || row >= m_composeSources.size()) return;
		auto& src = m_composeSources[row];
		const bool checked = (item->checkState() == Qt::Checked);
		if (checked == src.visible) return;
		src.visible = checked;
		if (!checked) {
			src.lastGeometry = src.subWindow ? src.subWindow->geometry() : QRect();
			if (src.subWindow) src.subWindow->hide();
			if (src.kind == ComposeSourceItem::SourceKind::File && src.service) src.service->playerPause();
			else if (src.kind == ComposeSourceItem::SourceKind::Camera && src.service) src.service->cameraPause();
			else if (src.kind == ComposeSourceItem::SourceKind::Screen && src.service) src.service->screenSetActive(false);
		} else {
			if (src.subWindow) {
				if (src.lastGeometry.isValid()) src.subWindow->setGeometry(src.lastGeometry);
				src.subWindow->show();
			}
			if (src.kind == ComposeSourceItem::SourceKind::File && src.service) src.service->playerResume();
			else if (src.kind == ComposeSourceItem::SourceKind::Camera && src.service) src.service->cameraResume();
			else if (src.kind == ComposeSourceItem::SourceKind::Screen && src.service) src.service->screenSetActive(true);
		}
		forceRefreshComposePreview();
	});
	connect(this, &CaptureWindow::composeSourceRemoved, this, [this]() {
		for (auto& src : m_composeSources) {
			if (!src.service) continue;
			if (src.kind == ComposeSourceItem::SourceKind::Camera && !src.service->cameraIsPlaying()) {
				bool occ = false;
				for (const auto& o : m_composeSources)
					if (&o != &src && o.kind == ComposeSourceItem::SourceKind::Camera && o.deviceIndex == src.deviceIndex)
						{ occ = true; break; }
				if (!occ) {
					src.service->selectCamera(src.deviceIndex);
					src.service->selectCameraFormat(src.formatIndex);
					src.service->cameraResume();
					const auto cl = src.service->getCameraList();
					src.subWindow->setWindowTitle(tr("摄像头：%1").arg(cl.value(src.deviceIndex)));
					src.subWindow->setStyleSheet(QString());
					src.title = src.subWindow->windowTitle();
					refreshComposeSourceListItems();
				}
			} else if (src.kind == ComposeSourceItem::SourceKind::Screen && !src.service->screenIsActive()) {
				bool occ = false;
				for (const auto& o : m_composeSources)
					if (&o != &src && o.kind == ComposeSourceItem::SourceKind::Screen && o.deviceIndex == src.deviceIndex)
						{ occ = true; break; }
				if (!occ) {
					src.service->selectScreen(src.deviceIndex);
					src.service->screenSetActive(true);
					const auto sl = src.service->getScreenList();
					src.subWindow->setWindowTitle(tr("屏幕：%1").arg(sl.value(src.deviceIndex)));
					src.subWindow->setStyleSheet(QString());
					src.title = src.subWindow->windowTitle();
					refreshComposeSourceListItems();
				}
			}
		}
	});
}

void CaptureWindow::suspendComposeSourcesForBackground()
{
	for (auto& src : m_composeSources)
	{
		if (!src.service)
		{
			continue;
		}
		switch (src.kind)
		{
		case ComposeSourceItem::SourceKind::File:
			src.service->playerPause();
			break;
		case ComposeSourceItem::SourceKind::Camera:
			src.service->cameraPause();
			break;
		case ComposeSourceItem::SourceKind::Screen:
			src.service->screenSetActive(false);
			break;
		}
	}
}

void CaptureWindow::setComposeMode(const bool enabled)
{
	ensureComposeWorkspace();
	if (enabled)
	{
		m_isComposeMode = true;
		m_captureMode = CaptureMode::Screen;
		m_isFileMode = false;
		stopScreenCapture();
		m_service->playerStop();
		m_service->cameraPause();
		// 从摄像头模式切到组合模式前，重建主相机实例以释放设备占用，
		// 避免组合内新建相机素材时因句柄冲突拿不到画面。
		m_service->initCamera(m_cameraBackendType);
		// 原生/GL 预览控件与透明度特效叠加时在窗口拉伸阶段容易残影，这里禁用透明叠加切换。
		if (ui && ui->wgtView)
		{
			ui->wgtView->setGraphicsEffect(nullptr);
			ui->wgtView->hide();
		}
		if (m_composeSplitter)
		{
			m_composeSplitter->setGraphicsEffect(nullptr);
		}
		m_composeSplitter->show();
		if (m_composeMdiArea)
		{
			m_composeMdiArea->setUpdatesEnabled(false);
		}
		for (auto& src : m_composeSources)
		{
			if (src.kind == ComposeSourceItem::SourceKind::Camera && src.service && src.view)
			{
				// 组合模式重新进入时，恢复摄像头源的预览绑定与设备选择。
				src.service->initCamera(m_cameraBackendType);
				src.service->bindCameraPreview(src.view);
				src.service->cameraSetFrameBusSourceId(src.sourceId);
				const QStringList cameras = src.service->getCameraList();
				if (!cameras.isEmpty())
				{
					const int dev = qBound(0, src.deviceIndex, cameras.size() - 1);
					src.deviceIndex = dev;
					src.service->selectCamera(dev);
					const QStringList formats = src.service->getCameraFormats(dev);
					if (!formats.isEmpty())
					{
						src.formatIndex = qBound(0, src.formatIndex, formats.size() - 1);
						src.service->selectCameraFormat(src.formatIndex);
					}
					src.service->cameraResume();
				}
			}
			if (src.subWindow)
			{
				src.subWindow->show();
			}
		}
		applyComposeAspectRatio();
		ui->wgtDevices->setVisible(false);
		ui->cmbFormats->setVisible(false);
		ui->chkCaptureCursor->setVisible(false);
		ui->cmbScreenFps->setVisible(false);
		m_fileProgress->setVisible(false);
		m_fileProgressLabel->setVisible(false);
		m_speedCombo->setVisible(false);
		m_debugStatsLabel->setVisible(false);
		m_fileProgressTimer->stop();
		m_debugStatsTimer->stop();
		if (m_composeSources.isEmpty())
		{
			addComposeCameraSource();
		}
		refreshComposeScreenCaptureState(m_composeSelectedIndex);
		if (m_composeZOrderGuardTimer && !m_composeZOrderGuardTimer->isActive())
		{
			m_composeZOrderGuardTimer->start();
		}
		syncComposeControlPanel();
		// 在几何动画/布局稳定后重新启用更新并做分帧刷新，清理残留纹路与白线。
		QTimer::singleShot(180, this, [this]() {
			if (m_composeMdiArea)
			{
				m_composeMdiArea->setUpdatesEnabled(true);
			}
			forceRefreshComposePreview();
			QTimer::singleShot(16, this, [this]() { forceRefreshComposePreview(); });
		});
		return;
	}
	if (!m_isComposeMode)
	{
		return;
	}
	suspendComposeSourcesForBackground();
	for (auto& src : m_composeSources)
	{
		if (src.kind == ComposeSourceItem::SourceKind::Camera && src.service && src.view)
		{
			// 离开组合模式时释放组合内摄像头设备占用，避免与主摄像头模式互抢资源。
			src.service->cameraPause();
			src.service->initCamera(m_cameraBackendType);
			src.service->bindCameraPreview(src.view);
			src.service->cameraSetFrameBusSourceId(src.sourceId);
		}
	}
	for (auto& src : m_composeSources)
	{
		if (src.subWindow)
		{
			src.subWindow->hide();
		}
	}
	m_isComposeMode = false;
	if (m_composeZOrderGuardTimer && m_composeZOrderGuardTimer->isActive())
	{
		m_composeZOrderGuardTimer->stop();
	}
	if (m_composeSplitter)
	{
		m_composeSplitter->hide();
		ui->wgtView->setGraphicsEffect(nullptr);
		ui->wgtView->show();
	}
	else
	{
		ui->wgtView->show();
	}
}

void CaptureWindow::clearComposeSources()
{
	while (!m_composeSources.isEmpty())
	{
		removeComposeSourceAt(m_composeSources.size() - 1);
	}
	m_composeSelectedIndex = -1;
}

void CaptureWindow::addComposeFileSource()
{
	ensureComposeWorkspace();
	const QString filePath = QFileDialog::getOpenFileName(
		this,
		tr("选择媒体文件"),
		QString(),
		tr("Media Files (*.mp4 *.mkv *.mov *.avi *.flv *.wmv *.mp3 *.aac *.wav *.flac);;All Files (*.*)")
	);
	if (filePath.isEmpty())
	{
		return;
	}
	auto* svc = new fplayer::Service();
	svc->initPlayer(m_filePlaybackBackend);
	auto* container = new ComposeSourceWidget();
	auto* view = new fplayer::FVideoView(container);
	view->setBackendType(m_filePlaybackBackend);
	view->setAttribute(Qt::WA_TransparentForMouseEvents, true);
	container->setInnerView(view);
	const QString fileStreamBusId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	// 须先放入 MDI 并 show，再绑定预览：否则 FVideoView/FGLWidget 尺寸为 0，OpenGL 无法正常绘制。
	auto* sub = m_composeMdiArea->addSubWindow(container, Qt::FramelessWindowHint);
	sub->setFocusPolicy(Qt::NoFocus);
	sub->setAttribute(Qt::WA_ShowWithoutActivating, true);
	sub->setWindowTitle(QFileInfo(filePath).fileName());
	sub->resize(480, 270);
	sub->show();
	sub->move(0, 0);
	QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
	svc->bindPlayerPreview(view);
	svc->setPlayerComposeStreamBusId(fileStreamBusId);
	if (!svc->openMediaFile(filePath))
	{
		svc->setPlayerComposeStreamBusId(QString());
		delete sub;
		delete svc;
		QMessageBox::warning(this, tr("追加失败"), tr("无法打开该媒体文件。"));
		return;
	}
	ComposeSourceItem item;
	item.kind = ComposeSourceItem::SourceKind::File;
	item.sourceId = fileStreamBusId;
	item.service = svc;
	item.container = container;
	item.view = view;
	item.subWindow = sub;
	item.title = sub->windowTitle();
	container->onSelected = [this, sub]() {
		if (!sub)
		{
			return;
		}
		const int idx = std::distance(m_composeSources.begin(), std::find_if(m_composeSources.begin(), m_composeSources.end(),
		                                                                      [sub](const ComposeSourceItem& i) {
			                                                                      return i.subWindow == sub;
		                                                                      }));
		if (idx >= 0 && idx < m_composeSources.size())
		{
			m_composeSelectedIndex = idx;
			refreshComposeSourceListSelection();
			updateComposeSelectionHighlight();
			if (auto* c = static_cast<ComposeSourceWidget*>(m_composeSources[idx].container))
			{
				c->setAspectResizeEnabled(m_composeSources[idx].keepAspectResize);
			}
			syncComposeControlPanel();
		}
	};
	container->onContextMenu = [this, sub](const QPoint& globalPos) {
		const int idx = std::distance(m_composeSources.begin(), std::find_if(m_composeSources.begin(), m_composeSources.end(),
		                                                                      [sub](const ComposeSourceItem& i) {
			                                                                      return i.subWindow == sub;
		                                                                      }));
		if (idx >= 0 && idx < m_composeSources.size())
		{
			requestComposeSourceContextMenu(globalPos, idx);
		}
	};
	container->onCropFinished = [this, sub]() {
		const int idx = std::distance(m_composeSources.begin(), std::find_if(m_composeSources.begin(), m_composeSources.end(),
		                                                                      [sub](const ComposeSourceItem& i) {
			                                                                      return i.subWindow == sub;
		                                                                      }));
		if (idx >= 0 && idx < m_composeSources.size())
		{
			setComposeCropMode(idx, false);
		}
	};
	container->onDragFinished = [this, sub]() {
		const int idx = std::distance(m_composeSources.begin(), std::find_if(m_composeSources.begin(), m_composeSources.end(),
		                                                                      [sub](const ComposeSourceItem& i) {
			                                                                      return i.subWindow == sub;
		                                                                      }));
		if (idx >= 0 && idx < m_composeSources.size())
		{
			auto& src = m_composeSources[idx];
			if (src.kind == ComposeSourceItem::SourceKind::Screen && src.service && src.view)
			{
				// 拉伸后不做 stop/start，仅重绑预览目标，避免黑屏。
				src.service->bindScreenPreview(src.view);
			}
		}
		forceRefreshComposePreview();
	};
	m_composeSources.push_back(item);
	refreshComposeSourceListItems();
	m_composeSelectedIndex = m_composeSources.size() - 1;
	refreshComposeSourceListSelection();
	updateComposeSelectionHighlight();
	syncComposeControlPanel();
	refreshComposeScreenCaptureState(m_composeSelectedIndex);
	updateComposePlaybackIcons();
}

void CaptureWindow::addComposeCameraSource()
{
	ensureComposeWorkspace();
	auto* svc = new fplayer::Service();
	svc->initCamera(m_cameraBackendType);
	auto* container = new ComposeSourceWidget();
	auto* view = new fplayer::FVideoView(container);
	view->setBackendType(m_cameraBackendType);
	view->setAttribute(Qt::WA_TransparentForMouseEvents, true);
	container->setInnerView(view);
	svc->bindCameraPreview(view);
	const auto cameras = svc->getCameraList();
	if (cameras.isEmpty())
	{
		delete svc;
		delete container;
		QMessageBox::warning(this, tr("追加失败"), tr("未检测到可用摄像头。"));
		return;
	}
	// 检测同设备冲突：若摄像头已被占用，新素材跳过打开设备，直接标记为暂停
	bool occupied = false;
	for (const auto& existing : m_composeSources)
	{
		if (existing.kind == ComposeSourceItem::SourceKind::Camera && existing.deviceIndex == 0)
		{
			occupied = true;
			break;
		}
	}
	if (!occupied)
	{
		svc->selectCamera(0);
		const auto fmts = svc->getCameraFormats(0);
		if (!fmts.isEmpty())
		{
			svc->selectCameraFormat(0);
		}
	}
	auto* sub = m_composeMdiArea->addSubWindow(container, Qt::FramelessWindowHint);
	sub->setFocusPolicy(Qt::NoFocus);
	sub->setAttribute(Qt::WA_ShowWithoutActivating, true);
	const QString camName = cameras.first();
	if (occupied)
	{
		sub->setWindowTitle(tr("[被占用] 摄像头：%1").arg(camName));
		sub->setStyleSheet(QStringLiteral("background:black;"));
	}
	else
	{
		sub->setWindowTitle(tr("摄像头：%1").arg(camName));
	}
	sub->resize(480, 270);
	sub->show();
	sub->move(0, 0);
	ComposeSourceItem item;
	item.kind = ComposeSourceItem::SourceKind::Camera;
	item.sourceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	item.service = svc;
	item.container = container;
	item.view = view;
	item.subWindow = sub;
	item.title = sub->windowTitle();
	item.deviceIndex = 0;
	item.formatIndex = 0;
	svc->cameraSetFrameBusSourceId(item.sourceId);
	container->onSelected = [this, sub]() {
		const int idx = std::distance(m_composeSources.begin(), std::find_if(m_composeSources.begin(), m_composeSources.end(),
		                                                                      [sub](const ComposeSourceItem& i) {
			                                                                      return i.subWindow == sub;
		                                                                      }));
		if (idx >= 0 && idx < m_composeSources.size())
		{
			m_composeSelectedIndex = idx;
			refreshComposeSourceListSelection();
			updateComposeSelectionHighlight();
			if (auto* c = static_cast<ComposeSourceWidget*>(m_composeSources[idx].container))
			{
				c->setAspectResizeEnabled(m_composeSources[idx].keepAspectResize);
			}
			syncComposeControlPanel();
		}
	};
	container->onContextMenu = [this, sub](const QPoint& globalPos) {
		const int idx = std::distance(m_composeSources.begin(), std::find_if(m_composeSources.begin(), m_composeSources.end(),
		                                                                      [sub](const ComposeSourceItem& i) {
			                                                                      return i.subWindow == sub;
		                                                                      }));
		if (idx >= 0 && idx < m_composeSources.size())
		{
			requestComposeSourceContextMenu(globalPos, idx);
		}
	};
	container->onCropFinished = [this, sub]() {
		const int idx = std::distance(m_composeSources.begin(), std::find_if(m_composeSources.begin(), m_composeSources.end(),
		                                                                      [sub](const ComposeSourceItem& i) {
			                                                                      return i.subWindow == sub;
		                                                                      }));
		if (idx >= 0 && idx < m_composeSources.size())
		{
			setComposeCropMode(idx, false);
		}
	};
	container->onDragFinished = [this]() {
		syncComposeControlPanel();
		forceRefreshComposePreview();
	};
	m_composeSources.push_back(item);
	refreshComposeSourceListItems();
	m_composeSelectedIndex = m_composeSources.size() - 1;
	refreshComposeSourceListSelection();
	updateComposeSelectionHighlight();
	syncComposeControlPanel();
	refreshComposeScreenCaptureState(m_composeSelectedIndex);
	updateComposePlaybackIcons();
}

void CaptureWindow::addComposeScreenSource()
{
	ensureComposeWorkspace();
#if defined(_WIN32)
	if (m_screenBackendType == fplayer::MediaBackendType::Dxgi && !m_hdrPromptedScreenIndexes.contains(0))
	{
		if (isHdrEnabledForScreenIndex(0))
		{
			m_hdrPromptedScreenIndexes.insert(0);
			showNonBlockingHint(this,
			                    tr("检测到当前系统开启了HDR，若遇到屏幕采集问题，可尝试切换至ffmepg后端，但该后端可能造成鼠标光标闪烁"),
			                    4200);
		}
	}
#endif
	auto* svc = new fplayer::Service();
	svc->initScreenCapture(m_screenBackendType);
	const QString screenSourceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	svc->screenSetFrameBusSourceId(screenSourceId);
	auto* container = new ComposeSourceWidget();
	auto* view = new fplayer::FVideoView(container);
	view->setBackendType(m_screenBackendType);
	view->setAttribute(Qt::WA_TransparentForMouseEvents, true);
	container->setInnerView(view);
	svc->bindScreenPreview(view);
	auto screens = svc->getScreenList();
	if (screens.isEmpty())
	{
		delete svc;
		delete container;
		QMessageBox::warning(this, tr("追加失败"), tr("未检测到可用屏幕。"));
		return;
	}
	// 检测同设备冲突：若屏幕已被占用，新素材跳过初始化采集
	bool occupied = false;
	for (const auto& existing : m_composeSources)
	{
		if (existing.kind == ComposeSourceItem::SourceKind::Screen && existing.deviceIndex == 0)
		{
			occupied = true;
			break;
		}
	}
	if (!occupied)
	{
		if (!svc->selectScreen(0))
		{
			if (m_screenBackendType == fplayer::MediaBackendType::Dxgi)
			{
				svc->initScreenCapture(fplayer::MediaBackendType::FFmpeg);
				m_screenBackendType = fplayer::MediaBackendType::FFmpeg;
				view->setBackendType(m_screenBackendType);
				svc->bindScreenPreview(view);
				screens = svc->getScreenList();
			}
			if (screens.isEmpty() || !svc->selectScreen(0))
			{
				delete svc;
				delete container;
				QMessageBox::warning(this, tr("追加失败"), tr("屏幕采集初始化失败。"));
				return;
			}
		}
		svc->screenSetActive(true);
	}
	auto* sub = m_composeMdiArea->addSubWindow(container, Qt::FramelessWindowHint);
	sub->setFocusPolicy(Qt::NoFocus);
	sub->setAttribute(Qt::WA_ShowWithoutActivating, true);
	const QString scrName = screens.first();
	if (occupied)
	{
		sub->setWindowTitle(tr("[被占用] 屏幕：%1").arg(scrName));
		sub->setStyleSheet(QStringLiteral("background:black;"));
	}
	else
	{
		sub->setWindowTitle(tr("屏幕：%1").arg(scrName));
	}
	sub->resize(640, 360);
	sub->show();
	sub->move(0, 0);
	ComposeSourceItem item;
	item.kind = ComposeSourceItem::SourceKind::Screen;
	item.service = svc;
	item.container = container;
	item.view = view;
	item.subWindow = sub;
	item.title = sub->windowTitle();
	item.sourceId = screenSourceId;
	item.deviceIndex = 0;
	item.screenFps = qMax(1, svc->screenFrameRate());
	item.screenCaptureCursor = false;
	container->onSelected = [this, sub]() {
		const int idx = std::distance(m_composeSources.begin(), std::find_if(m_composeSources.begin(), m_composeSources.end(),
		                                                                      [sub](const ComposeSourceItem& i) {
			                                                                      return i.subWindow == sub;
		                                                                      }));
		if (idx >= 0 && idx < m_composeSources.size())
		{
			m_composeSelectedIndex = idx;
			refreshComposeSourceListSelection();
			updateComposeSelectionHighlight();
			if (auto* c = static_cast<ComposeSourceWidget*>(m_composeSources[idx].container))
			{
				c->setAspectResizeEnabled(m_composeSources[idx].keepAspectResize);
			}
			syncComposeControlPanel();
		}
	};
	container->onContextMenu = [this, sub](const QPoint& globalPos) {
		const int idx = std::distance(m_composeSources.begin(), std::find_if(m_composeSources.begin(), m_composeSources.end(),
		                                                                      [sub](const ComposeSourceItem& i) {
			                                                                      return i.subWindow == sub;
		                                                                      }));
		if (idx >= 0 && idx < m_composeSources.size())
		{
			requestComposeSourceContextMenu(globalPos, idx);
		}
	};
	container->onCropFinished = [this, sub]() {
		const int idx = std::distance(m_composeSources.begin(), std::find_if(m_composeSources.begin(), m_composeSources.end(),
		                                                                      [sub](const ComposeSourceItem& i) {
			                                                                      return i.subWindow == sub;
		                                                                      }));
		if (idx >= 0 && idx < m_composeSources.size())
		{
			setComposeCropMode(idx, false);
		}
	};
	container->onDragFinished = [this]() {
		syncComposeControlPanel();
		forceRefreshComposePreview();
	};
	m_composeSources.push_back(item);
	refreshComposeSourceListItems();
	m_composeSelectedIndex = m_composeSources.size() - 1;
	refreshComposeSourceListSelection();
	updateComposeSelectionHighlight();
	syncComposeControlPanel();
	refreshComposeScreenCaptureState(m_composeSelectedIndex);
	updateComposePlaybackIcons();
}


void CaptureWindow::removeComposeSourceAt(const int index)
{
	if (index < 0 || index >= m_composeSources.size())
	{
		return;
	}
	ComposeSourceItem item = m_composeSources.takeAt(index);
	if (item.service)
	{
		item.service->setPlayerComposeStreamBusId(QString());
		item.service->cameraPause();
		item.service->playerStop();
		item.service->screenSetActive(false);
		delete item.service;
		item.service = nullptr;
	}
	if (item.subWindow)
	{
		item.subWindow->close();
		item.subWindow->deleteLater();
		item.subWindow = nullptr;
	}
	if (m_composeSourceList)
	{
		delete m_composeSourceList->takeItem(index);
	}
	if (m_composeSelectedIndex >= m_composeSources.size())
	{
		m_composeSelectedIndex = m_composeSources.size() - 1;
	}
	refreshComposeSourceListSelection();
	updateComposeSelectionHighlight();
	syncComposeControlPanel();
	refreshComposeScreenCaptureState(m_composeSelectedIndex);
	// 通知其他素材检查是否可恢复
	emit composeSourceRemoved();
}

void CaptureWindow::refreshComposeSourceListSelection()
{
	if (!m_composeSourceList)
	{
		return;
	}
	m_composeSourceList->blockSignals(true);
	m_composeSourceList->setCurrentRow(m_composeSelectedIndex);
	m_composeSourceList->blockSignals(false);
}

void CaptureWindow::refreshComposeSourceListItems()
{
	if (!m_composeSourceList)
	{
		return;
	}
	m_composeSourceList->blockSignals(true);
	m_composeSourceList->clear();
	for (const auto& src : m_composeSources)
	{
		QString displayTitle = src.title;
		const QString sid = src.sourceId.trimmed();
		if (!sid.isEmpty())
		{
			displayTitle += QStringLiteral(" [%1]").arg(sid.left(8));
		}
		auto* item = new QListWidgetItem(displayTitle);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(src.visible ? Qt::Checked : Qt::Unchecked);
		m_composeSourceList->addItem(item);
	}
	m_composeSourceList->setCurrentRow(m_composeSelectedIndex);
	m_composeSourceList->blockSignals(false);
}

void CaptureWindow::updateComposeSelectionHighlight()
{
	for (int i = 0; i < m_composeSources.size(); ++i)
	{
		auto* container = static_cast<ComposeSourceWidget*>(m_composeSources[i].container);
		if (!container)
		{
			continue;
		}
		container->setSelected(i == m_composeSelectedIndex);
		container->setCropMode(i == m_composeSelectedIndex && m_composeSources[i].cropMode);
	}
	applyComposeZOrder();
	if (m_isComposeMode)
	{
		updateComposePlaybackIcons();
	}
}

void CaptureWindow::applyComposeZOrder()
{
	if (!m_composeMdiArea)
	{
		return;
	}
	// m_composeSources 顺序即 z-order：前面的在底部，后面的在顶部。
	for (int i = 0; i < m_composeSources.size(); ++i)
	{
		auto& src = m_composeSources[i];
		QMdiSubWindow* sub = src.subWindow;
		if (!sub || sub->mdiArea() != m_composeMdiArea)
		{
			src.subWindow = nullptr;
			continue;
		}
		sub->lower();
	}
	for (int i = 0; i < m_composeSources.size(); ++i)
	{
		auto& src = m_composeSources[i];
		QMdiSubWindow* sub = src.subWindow;
		if (!sub || sub->mdiArea() != m_composeMdiArea)
		{
			src.subWindow = nullptr;
			continue;
		}
		sub->raise();
	}
}

void CaptureWindow::bringComposeSourceToFront(const int index)
{
	if (index < 0 || index >= m_composeSources.size() || !m_composeSources[index].subWindow)
	{
		return;
	}
	const ComposeSourceItem item = m_composeSources.takeAt(index);
	m_composeSources.push_back(item);
	m_composeSelectedIndex = m_composeSources.size() - 1;
	refreshComposeSourceListItems();
	applyComposeZOrder();
}

void CaptureWindow::sendComposeSourceToBack(const int index)
{
	if (index < 0 || index >= m_composeSources.size() || !m_composeSources[index].subWindow)
	{
		return;
	}
	const ComposeSourceItem item = m_composeSources.takeAt(index);
	m_composeSources.push_front(item);
	m_composeSelectedIndex = 0;
	refreshComposeSourceListItems();
	applyComposeZOrder();
}

void CaptureWindow::moveComposeSourceUp(const int index)
{
	if (index < 0 || index >= m_composeSources.size() - 1)
	{
		return;
	}
	m_composeSources.swapItemsAt(index, index + 1);
	m_composeSelectedIndex = index + 1;
	refreshComposeSourceListItems();
	applyComposeZOrder();
}

void CaptureWindow::moveComposeSourceDown(const int index)
{
	if (index <= 0 || index >= m_composeSources.size())
	{
		return;
	}
	m_composeSources.swapItemsAt(index, index - 1);
	m_composeSelectedIndex = index - 1;
	refreshComposeSourceListItems();
	applyComposeZOrder();
}

void CaptureWindow::setComposeCropMode(const int index, const bool enabled)
{
	if (index < 0 || index >= m_composeSources.size())
	{
		return;
	}
	for (int i = 0; i < m_composeSources.size(); ++i)
	{
		m_composeSources[i].cropMode = false;
	}
	m_composeSources[index].cropMode = enabled;
	m_composeSelectedIndex = index;
	refreshComposeSourceListSelection();
	updateComposeSelectionHighlight();
	syncComposeControlPanel();
}

void CaptureWindow::syncComposeControlPanel()
{
	if (!m_isComposeMode || m_composeSelectedIndex < 0 || m_composeSelectedIndex >= m_composeSources.size())
	{
		ui->wgtDevices->setVisible(false);
		ui->cmbFormats->setVisible(false);
		ui->chkCaptureCursor->setVisible(false);
		ui->cmbScreenFps->setVisible(false);
		updateComposePlaybackIcons();
		return;
	}
	auto& src = m_composeSources[m_composeSelectedIndex];
	if (!src.service)
	{
		updateComposePlaybackIcons();
		return;
	}
	if (src.kind == ComposeSourceItem::SourceKind::Camera)
	{
		ui->wgtDevices->setVisible(true);
		ui->cmbFormats->setVisible(true);
		ui->chkCaptureCursor->setVisible(false);
		ui->cmbScreenFps->setVisible(false);
		ui->cmbDevices->blockSignals(true);
		ui->cmbFormats->blockSignals(true);
		ui->cmbDevices->clear();
		ui->cmbDevices->addItems(src.service->getCameraList());
		const int dev = qBound(0, src.deviceIndex, qMax(0, ui->cmbDevices->count() - 1));
		ui->cmbDevices->setCurrentIndex(ui->cmbDevices->count() > 0 ? dev : -1);
		src.deviceIndex = ui->cmbDevices->currentIndex();
		ui->cmbFormats->clear();
		if (ui->cmbDevices->currentIndex() >= 0)
		{
			ui->cmbFormats->addItems(src.service->getCameraFormats(ui->cmbDevices->currentIndex()));
			src.formatIndex = ui->cmbFormats->count() > 0 ? qBound(0, src.formatIndex, ui->cmbFormats->count() - 1) : -1;
			ui->cmbFormats->setCurrentIndex(src.formatIndex);
		}
		ui->cmbFormats->blockSignals(false);
		ui->cmbDevices->blockSignals(false);
		updateComposePlaybackIcons();
		return;
	}
	if (src.kind == ComposeSourceItem::SourceKind::Screen)
	{
		ui->wgtDevices->setVisible(true);
		ui->cmbFormats->setVisible(false);
		ui->chkCaptureCursor->setVisible(true);
		ui->cmbScreenFps->setVisible(true);
		ui->cmbDevices->blockSignals(true);
		ui->cmbDevices->clear();
		ui->cmbDevices->addItems(src.service->getScreenList());
		src.deviceIndex = ui->cmbDevices->count() > 0 ? qBound(0, src.deviceIndex, ui->cmbDevices->count() - 1) : -1;
		ui->cmbDevices->setCurrentIndex(src.deviceIndex);
		ui->cmbDevices->blockSignals(false);
		ui->cmbScreenFps->blockSignals(true);
		ui->cmbScreenFps->clear();
		const auto screens = QGuiApplication::screens();
		qreal refreshRate = 60.0;
		if (src.deviceIndex >= 0 && src.deviceIndex < screens.size() && screens.at(src.deviceIndex))
		{
			refreshRate = screens.at(src.deviceIndex)->refreshRate();
		}
		const auto& fpsCandidates = kFpsCandidates;
		for (const int fps : fpsCandidates)
		{
			if (fps <= static_cast<int>(refreshRate + 0.5))
			{
				ui->cmbScreenFps->addItem(tr("%1 FPS").arg(fps), fps);
			}
		}
		if (ui->cmbScreenFps->count() <= 0)
		{
			const int fallback = qMax(15, static_cast<int>(refreshRate + 0.5));
			ui->cmbScreenFps->addItem(tr("%1 FPS").arg(fallback), fallback);
		}
		src.screenFps = src.screenFps > 0 ? src.screenFps : qMax(1, src.service->screenFrameRate());
		const int fpsIndex = ui->cmbScreenFps->findData(src.screenFps);
		ui->cmbScreenFps->setCurrentIndex(fpsIndex >= 0 ? fpsIndex : 0);
		ui->chkCaptureCursor->setChecked(src.screenCaptureCursor);
		ui->cmbScreenFps->setToolTip(tr("当前帧率：%1 FPS").arg(src.service->screenFrameRate()));
		ui->cmbScreenFps->blockSignals(false);
		// 仅在该屏幕素材当前为播放态时 refresh，暂停态保持不变，避免单击/拖拽/缩放等 UI 交互把它「拉起来播放」。
		if (composeSourceIsPlaying(m_composeSelectedIndex))
		{
			refreshComposeScreenCaptureState(m_composeSelectedIndex);
		}
		updateComposePlaybackIcons();
		return;
	}
	ui->wgtDevices->setVisible(false);
	ui->cmbFormats->setVisible(false);
	ui->chkCaptureCursor->setVisible(false);
	ui->cmbScreenFps->setVisible(false);
	updateComposePlaybackIcons();
}

void CaptureWindow::applyComposeAspectRatio()
{
	if (!m_composePreviewHost || !m_composeMdiArea || m_composeAspectW <= 0 || m_composeAspectH <= 0)
	{
		return;
	}
	const QRect oldBounds = m_composeMdiArea->viewport() ? m_composeMdiArea->viewport()->rect() : QRect();
	resizeWindowForComposeAspect();
	auto* host = static_cast<AspectRatioHostWidget*>(m_composePreviewHost);
	host->setAspectRatio(m_composeAspectW, m_composeAspectH);
	const QRect newBounds = m_composeMdiArea->viewport() ? m_composeMdiArea->viewport()->rect() : QRect();
	remapComposeSourcesToViewport(oldBounds, newBounds);
	const QRect bounds = m_composeMdiArea->viewport() ? m_composeMdiArea->viewport()->rect() : m_composeMdiArea->rect();
	const auto windows = m_composeMdiArea->subWindowList(QMdiArea::StackingOrder);
	for (QMdiSubWindow* sub : windows)
	{
		if (!sub)
		{
			continue;
		}
		QRect g = sub->geometry();
		if (g.width() > bounds.width())
		{
			g.setWidth(bounds.width());
		}
		if (g.height() > bounds.height())
		{
			g.setHeight(bounds.height());
		}
		if (g.right() > bounds.right())
		{
			g.moveRight(bounds.right());
		}
		if (g.bottom() > bounds.bottom())
		{
			g.moveBottom(bounds.bottom());
		}
		if (g.left() < bounds.left())
		{
			g.moveLeft(bounds.left());
		}
		if (g.top() < bounds.top())
		{
			g.moveTop(bounds.top());
		}
		sub->setGeometry(g);
	}
	forceRefreshComposePreview();
}

void CaptureWindow::refreshComposeScreenCaptureState(const int selectedComposeIndex, const int preferScreenRow,
                                                      const int excludeScreenRow)
{
	Q_UNUSED(selectedComposeIndex);
	if (!m_isComposeMode)
	{
		return;
	}
	for (int i = 0; i < m_composeSources.size(); ++i)
	{
		auto& src = m_composeSources[i];
		if (src.kind != ComposeSourceItem::SourceKind::Screen || !src.service)
		{
			continue;
		}
		const bool forcedPlay = (preferScreenRow >= 0 && i == preferScreenRow);
		const bool forcedPause = (excludeScreenRow >= 0 && i == excludeScreenRow);
		const bool shouldPlay = forcedPlay ? true : (forcedPause ? false : src.service->screenIsActive());
		src.service->screenSetActive(false);
		src.service->selectScreen(qMax(0, src.deviceIndex));
		src.service->screenSetFrameRate(qMax(1, src.screenFps));
		src.service->screenSetCursorCaptureEnabled(src.screenCaptureCursor);
		if (shouldPlay)
		{
			src.service->screenSetActive(true);
		}
	}
}

bool CaptureWindow::composeSourceIsPlaying(const int index) const
{
	if (index < 0 || index >= m_composeSources.size())
	{
		return false;
	}
	const auto& src = m_composeSources.at(index);
	if (!src.service)
	{
		return false;
	}
	switch (src.kind)
	{
	case ComposeSourceItem::SourceKind::File:
		return src.service->playerIsPlaying();
	case ComposeSourceItem::SourceKind::Screen:
		return src.service->screenIsActive();
	case ComposeSourceItem::SourceKind::Camera:
		return src.service->cameraIsPlaying();
	}
	return false;
}

void CaptureWindow::updateComposePlaybackIcons()
{
	if (!m_isComposeMode || !ui || !ui->btnPlay)
	{
		return;
	}
	if (m_composeSelectedIndex < 0 || m_composeSelectedIndex >= m_composeSources.size())
	{
		ui->btnPlay->setVisible(false);
		return;
	}
	const auto& src = m_composeSources.at(m_composeSelectedIndex);
	if (src.kind != ComposeSourceItem::SourceKind::File)
	{
		ui->btnPlay->setVisible(false);
		return;
	}
	ui->btnPlay->setVisible(true);
	const bool playing = src.service ? src.service->playerIsPlaying() : false;
	ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme),
		playing ? QStringLiteral("pause") : QStringLiteral("play"))));
}

void CaptureWindow::toggleComposeSourcePlayPauseAt(const int index)
{
	if (index < 0 || index >= m_composeSources.size())
	{
		return;
	}
	auto& src = m_composeSources[index];
	if (!src.service)
	{
		return;
	}
	if (src.kind != ComposeSourceItem::SourceKind::File)
	{
		return;
	}
	if (src.service->playerIsPlaying())
	{
		src.service->playerPause();
	}
	else
	{
		src.service->playerResume();
	}
	updateComposePlaybackIcons();
}

void CaptureWindow::resizeWindowForComposeAspect()
{
	if (!m_isComposeMode || m_adjustingComposeWindowSize || !m_composePreviewHost || !ui || !ui->wgtDown)
	{
		return;
	}
	m_adjustingComposeWindowSize = true;
	const int leftWidth = m_composeSplitter ? qMax(220, m_composeSplitter->sizes().isEmpty() ? 260 : m_composeSplitter->sizes().at(0)) :
	                                      260;
	const int targetPreviewH = qMax(360, m_composePreviewHost->height());
	const int targetPreviewW = qMax(320, targetPreviewH * m_composeAspectW / qMax(1, m_composeAspectH));
	const int targetW = leftWidth + targetPreviewW + 16;
	const int menuH = m_modeMenuBar ? m_modeMenuBar->height() : 24;
	const int bottomH = ui->wgtDown ? ui->wgtDown->height() : 40;
	const int targetH = targetPreviewH + bottomH + menuH + 12;
	const QRect targetGeom(x(), y(), targetW, targetH);
	auto* anim = new QPropertyAnimation(this, "geometry");
	anim->setDuration(160);
	anim->setEasingCurve(QEasingCurve::OutCubic);
	anim->setStartValue(geometry());
	anim->setEndValue(targetGeom);
	connect(anim, &QPropertyAnimation::finished, this, [this, anim]() {
		anim->deleteLater();
		m_adjustingComposeWindowSize = false;
		// 窗体几何动画结束后，组合画布里的原生预览控件可能仍保留旧帧残影；
		// 这里补一次“当前帧+下一帧”刷新，确保交换链与子窗口内容同步到最新几何。
		forceRefreshComposePreview();
		QTimer::singleShot(16, this, [this]() { forceRefreshComposePreview(); });
	});
	anim->start();
	return;
}

void CaptureWindow::remapComposeSourcesToViewport(const QRect& oldBounds, const QRect& newBounds)
{
	if (!m_composeMdiArea || oldBounds.width() <= 0 || oldBounds.height() <= 0 || newBounds.width() <= 0 || newBounds.height() <= 0)
	{
		return;
	}
	const double sx = static_cast<double>(newBounds.width()) / static_cast<double>(oldBounds.width());
	const double sy = static_cast<double>(newBounds.height()) / static_cast<double>(oldBounds.height());
	const auto windows = m_composeMdiArea->subWindowList(QMdiArea::StackingOrder);
	for (QMdiSubWindow* sub : windows)
	{
		if (!sub)
		{
			continue;
		}
		const QRect g = sub->geometry();
		const int nx = static_cast<int>(g.x() * sx);
		const int ny = static_cast<int>(g.y() * sy);
		const int nw = qMax(80, static_cast<int>(g.width() * sx));
		const int nh = qMax(60, static_cast<int>(g.height() * sy));
		sub->setGeometry(nx, ny, nw, nh);
	}
}

void CaptureWindow::forceRefreshComposePreview()
{
	if (!m_composeMdiArea)
	{
		return;
	}
	m_composeMdiArea->update();
	if (QWidget* vp = m_composeMdiArea->viewport())
	{
		vp->update();
		vp->repaint();
	}
	const auto windows = m_composeMdiArea->subWindowList(QMdiArea::StackingOrder);
	for (QMdiSubWindow* sub : windows)
	{
		if (!sub) continue;
		sub->update();
		sub->repaint();
	}
	// 先处理待处理的 resize/layout 事件，确保控件几何已稳定
	QApplication::sendPostedEvents(nullptr, QEvent::Resize);
	QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
	// 遍历 composeSources，显式触发 GL 控件 paintGL，防止窗口拉伸/最大化恢复后冻结
	for (const auto& src : m_composeSources)
	{
		if (auto* container = static_cast<ComposeSourceWidget*>(src.container))
		{
			container->update();
			container->repaint();
			if (auto* view = src.view)
			{
				view->update();
				view->repaint();
				if (auto* glw = view->findChild<fplayer::FGLWidget*>())
				{
					glw->repaint();
				}
			}
		}
	}
	// 延迟二次刷新：几何变化和 GL 上下文完全稳定后再渲染一次
	QTimer::singleShot(100, this, [this]() {
		if (!m_composeMdiArea || !m_isComposeMode) return;
		for (const auto& src : m_composeSources)
		{
			if (auto* view = src.view)
			{
				view->repaint();
				if (auto* glw = view->findChild<fplayer::FGLWidget*>())
				{
					glw->repaint();
				}
			}
		}
	});
}

void CaptureWindow::requestComposeSourceContextMenu(const QPoint& globalPos, const int index)
{
	if (index < 0 || index >= m_composeSources.size())
	{
		return;
	}
	m_composeSelectedIndex = index;
	refreshComposeSourceListSelection();
	updateComposeSelectionHighlight();
	QMenu menu(this);
	auto* actionTop = menu.addAction(tr("置于顶部"));
	auto* actionBottom = menu.addAction(tr("置于底部"));
	auto* actionUp = menu.addAction(tr("向上调整"));
	auto* actionDown = menu.addAction(tr("向下调整"));
	auto* actionFillCanvas = menu.addAction(tr("铺满画布"));
	auto* actionKeepAspect = menu.addAction(m_composeSources[index].keepAspectResize ? tr("取消锁定缩放比例") : tr("锁定缩放比例"));
	auto* actionCrop = menu.addAction(m_composeSources[index].cropMode ? tr("退出裁剪") : tr("裁剪"));
	menu.addSeparator();
	auto* actionDelete = menu.addAction(tr("删除"));
	QAction* chosen = menu.exec(globalPos);
	if (!chosen)
	{
		return;
	}
	if (chosen == actionTop)
	{
		bringComposeSourceToFront(index);
		return;
	}
	if (chosen == actionBottom)
	{
		sendComposeSourceToBack(index);
		return;
	}
	if (chosen == actionUp)
	{
		moveComposeSourceUp(index);
		return;
	}
	if (chosen == actionDown)
	{
		moveComposeSourceDown(index);
		return;
	}
	if (chosen == actionFillCanvas)
	{
		if (m_composeSources[index].subWindow && m_composeMdiArea && m_composeMdiArea->viewport())
		{
			m_composeSources[index].subWindow->setGeometry(m_composeMdiArea->viewport()->rect());
			forceRefreshComposePreview();
		}
		return;
	}
	if (chosen == actionCrop)
	{
		setComposeCropMode(index, !m_composeSources[index].cropMode);
		return;
	}
	if (chosen == actionKeepAspect)
	{
		m_composeSources[index].keepAspectResize = !m_composeSources[index].keepAspectResize;
		if (auto* c = static_cast<ComposeSourceWidget*>(m_composeSources[index].container))
		{
			c->setAspectResizeEnabled(m_composeSources[index].keepAspectResize);
		}
		return;
	}
	if (chosen == actionDelete)
	{
		removeComposeSourceAt(index);
	}
}

bool CaptureWindow::buildComposeScreenCaptureSpec(QString& spec,
                                                  const int fps,
                                                  const int outW,
                                                  const int outH,
                                                  const int bitrateKbps,
                                                  const QString& encoder,
                                                  const QString& audioIn,
                                                  const QString& audioOut) const
{
	spec.clear();
	if (!m_isComposeMode || !m_composeMdiArea || m_composeSources.isEmpty())
	{
		return false;
	}
	const QWidget* target = m_composeMdiArea->viewport();
	if (!target || target->width() <= 0 || target->height() <= 0)
	{
		return false;
	}
	QPoint global = target->mapToGlobal(QPoint(0, 0));
	int capW = target->width();
	int capH = target->height();
#if defined(_WIN32)
	// gdigrab 采集参数使用设备像素；Qt 坐标是逻辑像素，需按屏幕 DPR 转换。
	qreal dpr = 1.0;
	if (const QWindow* wnd = this->windowHandle())
	{
		dpr = wnd->devicePixelRatio();
	}
	else if (const QScreen* sc = this->screen())
	{
		dpr = sc->devicePixelRatio();
	}
	global.setX(qRound(static_cast<qreal>(global.x()) * dpr));
	global.setY(qRound(static_cast<qreal>(global.y()) * dpr));
	capW = qRound(static_cast<qreal>(capW) * dpr);
	capH = qRound(static_cast<qreal>(capH) * dpr);
#endif
	QStringList parts;
	parts << QStringLiteral("fps=%1").arg(fps > 0 ? fps : 30);
	parts << QStringLiteral("x=%1").arg(global.x());
	parts << QStringLiteral("y=%1").arg(global.y());
	parts << QStringLiteral("size=%1x%2").arg(capW).arg(capH);
	parts << QStringLiteral("scene_w=%1").arg(target->width());
	parts << QStringLiteral("scene_h=%1").arg(target->height());
	for (int i = 0; i < m_composeSources.size(); ++i)
	{
		const auto& src = m_composeSources.at(i);
		if (!src.subWindow)
		{
			continue;
		}
		const QRect g = src.subWindow->geometry();
		const QString kind = (src.kind == ComposeSourceItem::SourceKind::Camera)
			                     ? QStringLiteral("camera")
			                     : (src.kind == ComposeSourceItem::SourceKind::Screen ? QStringLiteral("screen") : QStringLiteral("file"));
		parts << QStringLiteral("src%1=%2,%3,%4,%5,%6,%7")
			         .arg(i)
			         .arg(kind)
			         .arg(src.sourceId.trimmed().isEmpty() ? QStringLiteral("default") : src.sourceId.trimmed())
			         .arg(g.x())
			         .arg(g.y())
			         .arg(g.width())
			         .arg(g.height());
	}
	if (outW > 0 && outH > 0)
	{
		parts << QStringLiteral("outsize=%1x%2").arg(outW).arg(outH);
	}
	if (bitrateKbps > 0)
	{
		parts << QStringLiteral("bitrate=%1").arg(bitrateKbps);
	}
	if (!encoder.trimmed().isEmpty())
	{
		parts << QStringLiteral("encoder=%1").arg(encoder.trimmed().toLower());
	}
	if (!audioIn.trimmed().isEmpty())
	{
		parts << QStringLiteral("audio_in=%1").arg(audioIn.trimmed());
	}
	if (!audioOut.trimmed().isEmpty())
	{
		parts << QStringLiteral("audio_out=%1").arg(audioOut.trimmed());
	}
	spec = QStringLiteral("__compose_scene__:") + parts.join(';');
	return true;
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
	const QRect oldBounds = (m_isComposeMode && m_composeMdiArea && m_composeMdiArea->viewport())
		                        ? m_composeMdiArea->viewport()->rect()
		                        : QRect();
	QWidget::resizeEvent(event);
	relocateTitleWidget();
	if (m_isComposeMode && m_composePreviewHost && m_composeAspectW > 0 && m_composeAspectH > 0)
	{
		static_cast<AspectRatioHostWidget*>(m_composePreviewHost)->setAspectRatio(m_composeAspectW, m_composeAspectH);
		const QRect newBounds = (m_composeMdiArea && m_composeMdiArea->viewport()) ? m_composeMdiArea->viewport()->rect() : QRect();
		remapComposeSourcesToViewport(oldBounds, newBounds);
		applyComposeZOrder();
		forceRefreshComposePreview();
	}
}

void CaptureWindow::closeEvent(QCloseEvent* event)
{
	if (!event)
	{
		return;
	}
	if (m_closeToTrayOnClose && !m_quitFromTray && m_trayIcon && m_trayIcon->isVisible())
	{
		event->ignore();
		hide();
		m_trayIcon->showMessage(tr("FPlayer"),
		                        tr("程序已最小化到系统托盘，双击托盘图标可恢复窗口。"),
		                        QSystemTrayIcon::Information,
		                        2000);
		return;
	}
	if (m_service && m_service->streamIsRunning())
	{
		const auto ret = QMessageBox::question(this,
		                                       tr("确认退出"),
		                                       tr("当前正在执行推拉流任务，是否确定停止并退出"),
		                                       QMessageBox::Yes | QMessageBox::No,
		                                       QMessageBox::No);
		if (ret != QMessageBox::Yes)
		{
			m_quitFromTray = false;
			event->ignore();
			return;
		}
		m_service->streamStop();
		m_mainRecording = false;
		m_pullRecording = false;
		if (m_mainRecordTimer)
		{
			m_mainRecordTimer->stop();
		}
		if (m_pullRecordTimer)
		{
			m_pullRecordTimer->stop();
		}
		updateRecordButtonUi();
		updatePullRecordButtonUi();
		if (m_pullPreviewDialog)
		{
			m_pullPreviewDialog->close();
		}
		if (m_pullMonitorDialog)
		{
			m_pullMonitorDialog->close();
		}
	}
	QWidget::closeEvent(event);
}

void CaptureWindow::setupTrayIcon()
{
	if (!QSystemTrayIcon::isSystemTrayAvailable() || m_trayIcon)
	{
		return;
	}
	m_trayIcon = new QSystemTrayIcon(this);
	m_trayIcon->setIcon(windowIcon());
	m_trayIcon->setToolTip(tr("FPlayer Desktop"));
	m_trayMenu = new QMenu(this);
	m_trayShowAction = m_trayMenu->addAction(tr("显示主窗口"));
	m_trayQuitAction = m_trayMenu->addAction(tr("退出程序"));
	connect(m_trayShowAction, &QAction::triggered, this, &CaptureWindow::showFromTray);
	connect(m_trayQuitAction, &QAction::triggered, this, &CaptureWindow::quitFromTray);
	connect(m_trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
		if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger)
		{
			showFromTray();
		}
	});
	m_trayIcon->setContextMenu(m_trayMenu);
	m_trayIcon->show();
}

void CaptureWindow::showFromTray()
{
	if (!isVisible())
	{
		show();
	}
	setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
	raise();
	activateWindow();
}

void CaptureWindow::quitFromTray()
{
	m_quitFromTray = true;
	// 直接停掉所有推拉流和录制，避免 closeEvent 里弹确认框
	if (m_service)
	{
		if (m_mainRecordTimer) m_mainRecordTimer->stop();
		if (m_pullRecordTimer) m_pullRecordTimer->stop();
		if (m_pullRecordService) m_pullRecordService->streamStop();
		m_service->streamStop();
	}
	m_mainRecording = false;
	m_pullRecording = false;
	if (m_pullPreviewDialog) m_pullPreviewDialog->close();
	if (m_pullMonitorDialog) m_pullMonitorDialog->close();
	if (m_imagePoolSidebar) m_imagePoolSidebar->close();
	QApplication::quit();
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
	const auto& baseFps = kFpsCandidates;
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
		this->ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("pause"))));
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
			showNonBlockingHint(this,
			                    tr("检测到当前系统开启了HDR，若遇到屏幕采集问题，可尝试切换至ffmepg后端，但该后端可能造成鼠标光标闪烁"),
			                    4200);
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
	this->ui->btnPlay->setIcon(QIcon(fplayer::tokens::themedIconPath(static_cast<fplayer::tokens::Theme>(m_theme), QStringLiteral("pause"))));
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