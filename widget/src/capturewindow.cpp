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
#include <QFormLayout>
#include <QPushButton>
#include <QMessageBox>
#include <QTextEdit>
#include <QSpinBox>
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
#include <QTcpServer>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QPointer>
#include <QCloseEvent>
#include <QApplication>
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
		setAttribute(Qt::WA_StyledBackground, true);
		setStyleSheet(QStringLiteral("background:#0b1018;border:1px solid #243145;border-radius:8px;"));
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
		m_content->setGeometry(x, y, qMax(1, cw), qMax(1, ch));
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
		setMouseTracking(true);
		setAttribute(Qt::WA_StyledBackground, true);
		setStyleSheet(QStringLiteral("background:#0a0f16;border:1px solid #31435b;border-radius:6px;"));
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
				if (!m_dragRubberBand)
				{
					m_dragRubberBand = new QRubberBand(QRubberBand::Rectangle, sub->parentWidget());
				}
				if (!m_vGuideBand)
				{
					m_vGuideBand = new QRubberBand(QRubberBand::Rectangle, sub->parentWidget());
					m_vGuideBand->setStyleSheet(QStringLiteral("background:rgba(171,120,255,0.40);border:none;"));
				}
				if (!m_hGuideBand)
				{
					m_hGuideBand = new QRubberBand(QRubberBand::Rectangle, sub->parentWidget());
					m_hGuideBand->setStyleSheet(QStringLiteral("background:rgba(171,120,255,0.40);border:none;"));
				}
				m_vGuideBand->hide();
				m_hGuideBand->hide();
				m_dragRubberBand->setGeometry(sub->geometry());
				m_dragRubberBand->show();
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
		if (m_dragRubberBand && m_dragRubberBand->isVisible())
		{
			m_dragRubberBand->setGeometry(g);
			m_dragChanged = true;
		}
		else if (auto* sub = subWindow())
		{
			sub->setGeometry(g);
			m_dragChanged = true;
		}
		QToolTip::showText(event->globalPosition().toPoint(),
		                   QStringLiteral("%1 x %2").arg(g.width()).arg(g.height()),
		                   this);
		event->accept();
	}

	void mouseReleaseEvent(QMouseEvent* event) override
	{
		const bool shouldFinishCrop = m_cropMode && m_dragChanged;
		const bool shouldNotifyDragFinished = m_dragChanged;
		if (m_dragRubberBand && m_dragRubberBand->isVisible())
		{
			if (auto* sub = subWindow())
			{
				sub->setGeometry(m_dragRubberBand->geometry());
			}
			m_dragRubberBand->hide();
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
			return m_cropMode ? DragMode::ResizeLeft : DragMode::ResizeLeft;
		}
		if (right)
		{
			return m_cropMode ? DragMode::ResizeRight : DragMode::ResizeRight;
		}
		if (top)
		{
			return m_cropMode ? DragMode::ResizeTop : DragMode::ResizeTop;
		}
		if (bottom)
		{
			return m_cropMode ? DragMode::ResizeBottom : DragMode::ResizeBottom;
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
		QString border = QStringLiteral("#3a506a");
		if (m_cropMode)
		{
			border = QStringLiteral("#ffd166");
		}
		else if (m_selected)
		{
			border = QStringLiteral("#b388ff");
		}
		setStyleSheet(QStringLiteral("background:#0a0f16;border:2px solid %1;border-radius:6px;").arg(border));
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
	bool m_dragInProgress = false;
};

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
	setStyleSheet(QStringLiteral(
		"QWidget#CaptureWindow{background:#090d14;color:#d6e2f1;}"
		"QMenuBar{background:#0d1320;color:#e7defc;border:none;padding:4px 6px;}"
		"QMenuBar::item{background:transparent;padding:6px 10px;border-radius:5px;}"
		"QMenuBar::item:selected{background:#241c3c;}"
		"QMenu{background:#121127;color:#e8e1ff;border:none;}"
		"QMenu::item:selected{background:#2d1f4d;}"
		"QPushButton{background:#201938;border:1px solid #6e4ea5;color:#f3ebff;border-radius:6px;padding:4px 8px;}"
		"QPushButton:hover{background:#2a2148;border-color:#8c69c8;}"
		"QPushButton:pressed{background:#18142c;border-color:#5d448d;padding-top:5px;padding-bottom:3px;}"
		"QPushButton[role=\"primary\"]{background:#6f49b5;border:1px solid #a786e4;color:#fff7ff;font-weight:600;}"
		"QPushButton[role=\"primary\"]:hover{background:#7f56cc;border-color:#c1a5f1;}"
		"QPushButton[role=\"primary\"]:pressed{background:#603fa0;border-color:#9f84d6;}"
		"QPushButton[role=\"primary\"]:disabled{background:#31274c;border-color:#4a3a70;color:#bbaed7;}"
		"QToolButton{color:#eadfff;border-radius:5px;padding:2px 6px;}"
		"QToolButton:hover{background:#241c3d;}"
		"QComboBox,QLineEdit,QSpinBox,QAbstractSpinBox,QListWidget,QTextEdit{"
		"background:#0d0b1d;border:1px solid #4f3b79;border-radius:6px;color:#e2d9f7;padding:3px 4px;}"
		"QComboBox:focus,QLineEdit:focus,QSpinBox:focus,QAbstractSpinBox:focus,QTextEdit:focus{background:#15122a;border-color:#8b67c7;}"
		"QComboBox::drop-down{border-left:1px solid #5f478f;width:22px;}"
		"QComboBox::down-arrow{image:none;width:0;height:0;border-left:5px solid transparent;border-right:5px solid transparent;border-top:6px solid #ccb8f2;}"
		"QComboBox QAbstractItemView{background:#131128;border:none;selection-background-color:#3b2964;}"
		"QCheckBox{spacing:6px;}"
		"QSlider::groove:horizontal{background:#2a2340;height:6px;border-radius:3px;}"
		"QSlider::handle:horizontal{background:#bc8cff;border:1px solid #dfc4ff;width:14px;margin:-5px 0;border-radius:7px;}"
		"QListWidget::item{padding:6px;border-radius:5px;}"
		"QListWidget::item:hover{background:#201938;}"
		"QListWidget::item:selected{background:#322453;color:#f4eeff;}"
		"#wgtDown{background:#111025;border:none;}"
		"#wgtOperate{background:transparent;}"
		"#wgtDevices{background:transparent;}"
	));
	m_service = new fplayer::Service();
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
	m_fileTitleButton->setStyleSheet(QStringLiteral(
		"QToolButton{font-weight:600;color:#efe3ff;background:transparent;border:none;border-radius:6px;}"
		"QToolButton:hover{background:#241c3d;}"
	));

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

	connect(this->ui->btnPlay, &QPushButton::clicked, [this]() {
		this->togglePlayPause();
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
		setComposeMode(false);
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
		setComposeMode(false);
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
	connect(actionComposeMode, &QAction::triggered, this, [this]() {
		setComposeMode(true);
	});
	connect(actionPushStream, &QAction::triggered, this, [this]() {
		QDialog dlg(this);
		dlg.setWindowTitle(tr("推流配置"));
		dlg.setWindowFlag(Qt::WindowCloseButtonHint, false);
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
		cmbProtocol->addItem(tr("UDP"), QStringLiteral("udp://127.0.0.1:23000"));
		const bool fileScene = (m_captureMode == CaptureMode::File);
		const bool screenScene = (m_captureMode == CaptureMode::Screen);
		const bool composeScene = m_isComposeMode;
		auto* lblInputMode = new QLabel(screenScene
			                                ? tr("来源：屏幕采集后端（由 Service 统一编排）")
			                                : (fileScene
				                                   ? tr("来源：当前文件模式媒体源")
				                                   : (composeScene ? tr("来源：组合预览窗口（所见即所得）") : tr("来源：当前摄像头模式"))),
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
		auto refreshPushParams = [this, lblPushParams, lblInputValue, fileScene, screenScene, composeScene]() {
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
		auto* buttons = new QDialogButtonBox(&dlg);
		auto* btnStart = new QPushButton(tr("开始推流"), &dlg);
		auto* btnStop = new QPushButton(tr("停止推流"), &dlg);
		auto* btnClose = new QPushButton(tr("关闭窗口"), &dlg);
		btnStart->setProperty("role", QStringLiteral("primary"));
		buttons->addButton(btnStart, QDialogButtonBox::AcceptRole);
		buttons->addButton(btnStop, QDialogButtonBox::ActionRole);
		buttons->addButton(btnClose, QDialogButtonBox::RejectRole);
		layout->addRow(buttons);
		auto applyPushUiRunningState = [btnStart, btnStop, cmbProtocol, cmbOutput, spFps, cmbSize, spBitrate, cmbEncoder, cmbAudioInput,
		                                cmbAudioOutput, fileScene](const bool running) {
			btnStart->setEnabled(!running);
			btnStop->setEnabled(running);
			cmbProtocol->setEnabled(!running);
			cmbOutput->setEnabled(!running);
			spFps->setEnabled(!running && !fileScene);
			cmbSize->setEnabled(!running && !fileScene);
			spBitrate->setEnabled(!running);
			cmbEncoder->setEnabled(!running);
			cmbAudioInput->setEnabled(!running && !fileScene);
			cmbAudioOutput->setEnabled(!running && !fileScene);
		};
		applyPushUiRunningState(this->m_service->streamIsRunning());
		connect(btnClose, &QPushButton::clicked, &dlg, [this, &dlg]() {
			if (this->m_service->streamIsRunning())
			{
				const auto answer = QMessageBox::question(&dlg,
				                                          tr("确认关闭"),
				                                          tr("当前正在推流，是否先停止推流再关闭窗口？"),
				                                          QMessageBox::Yes | QMessageBox::No,
				                                          QMessageBox::No);
				if (answer != QMessageBox::Yes)
				{
					return;
				}
				this->m_service->streamStop();
			}
			dlg.accept();
		});
		connect(btnStop, &QPushButton::clicked, &dlg,
		        [this, applyPushUiRunningState]() {
			this->m_service->streamStop();
			applyPushUiRunningState(false);
		});
		connect(btnStart, &QPushButton::clicked, &dlg,
		        [this, btnStart, cmbProtocol, cmbOutput, spFps, cmbSize, spBitrate, cmbEncoder, cmbAudioInput, cmbAudioOutput,
		         chkKeepAspect, fileScene, screenScene, composeScene, addRecent, applyPushUiRunningState]() {
			const QString pushOutput = cmbOutput->currentText().trimmed();
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
		auto* dlg = new QDialog(this);
		dlg->setAttribute(Qt::WA_DeleteOnClose, true);
		dlg->setModal(false);
		dlg->setWindowTitle(tr("拉流监视窗口"));
		m_pullMonitorDialog = dlg;
		connect(dlg, &QObject::destroyed, this, [this]() {
			m_pullMonitorDialog = nullptr;
			m_pullStartButton = nullptr;
			m_pullStopButton = nullptr;
			m_pullLogView = nullptr;
		});
		auto* layout = new QFormLayout(dlg);
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
		auto* cmbOutput = new QComboBox(dlg);
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
		auto* btnBrowseOutput = new QPushButton(tr("选择输出文件"), dlg);
		connect(btnBrowseOutput, &QPushButton::clicked, dlg, [cmbOutput, this]() {
			const QString outPath = QFileDialog::getSaveFileName(this, tr("选择拉流输出文件"), QString(),
			                                                     tr("Media Files (*.mp4 *.mkv *.flv);;All Files (*.*)"));
			if (!outPath.isEmpty())
			{
				cmbOutput->setCurrentText(outPath);
			}
		});
		auto* cmbPullMode = new QComboBox(dlg);
		cmbPullMode->addItem(tr("监看模式"), QStringLiteral("preview"));
		cmbPullMode->addItem(tr("录制模式"), QStringLiteral("record"));
		cmbPullMode->setCurrentIndex(0);
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
		auto makePullPreviewUrl = [dlg, cmbProtocol, edtStreamKey, reservedPort]() {
			const QString relayUrl = dlg->property("pullPreviewRelayUrl").toString().trimmed();
			if (!relayUrl.isEmpty())
			{
				return relayUrl;
			}
			const QString protocol = cmbProtocol->currentData().toString().trimmed().toLower();
			const QString key = edtStreamKey->text().trimmed().isEmpty() ? QStringLiteral("stream001") : edtStreamKey->text().trimmed();
			if (protocol == QStringLiteral("srt"))
			{
				return QStringLiteral("srt://127.0.0.1:%1?mode=caller&streamid=%2").arg(reservedPort).arg(key);
			}
			if (protocol == QStringLiteral("rtmp"))
			{
				return QStringLiteral("rtmp://127.0.0.1:%1/live/%2").arg(reservedPort).arg(key);
			}
			if (protocol == QStringLiteral("udp"))
			{
				return QStringLiteral("udp://127.0.0.1:%1").arg(reservedPort);
			}
			return QStringLiteral("rtsp://127.0.0.1:%1/live/%2").arg(reservedPort).arg(key);
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
		auto* lblStatus = new QLabel(tr("状态：未启动"), dlg);
		auto* txtLog = new QTextEdit(dlg);
		txtLog->setReadOnly(true);
		txtLog->setMinimumHeight(140);
		m_pullLogView = txtLog;
		dlg->setProperty("pullPreviewAutoOpened", false);
		dlg->setProperty("pullExtraLog", QString());
		dlg->setProperty("pullPreviewRelayUrl", QString());
		dlg->setProperty("pullStopping", false);
		dlg->setProperty("pullCloseAfterStop", false);
		auto* buttons = new QDialogButtonBox(dlg);
		auto* btnStart = new QPushButton(tr("开始拉流"), dlg);
		auto* btnStop = new QPushButton(tr("结束拉流"), dlg);
		auto* btnDiag = new QPushButton(tr("网络自检"), dlg);
		auto* btnClose = new QPushButton(tr("关闭窗口"), dlg);
		btnStart->setProperty("role", QStringLiteral("primary"));
		buttons->addButton(btnStart, QDialogButtonBox::AcceptRole);
		buttons->addButton(btnStop, QDialogButtonBox::ActionRole);
		buttons->addButton(btnDiag, QDialogButtonBox::ActionRole);
		buttons->addButton(btnClose, QDialogButtonBox::RejectRole);
		m_pullStartButton = btnStart;
		m_pullStopButton = btnStop;
		auto applyPullUiRunningState = [btnStart, btnStop, cmbPullMode, cmbProtocol, edtStreamKey, cmbOutput, btnBrowseOutput](bool running) {
			btnStart->setEnabled(!running);
			btnStop->setEnabled(running);
			cmbPullMode->setEnabled(!running);
			cmbProtocol->setEnabled(!running);
			edtStreamKey->setEnabled(!running);
			cmbOutput->setEnabled(!running);
			btnBrowseOutput->setEnabled(!running);
		};
		auto refreshPullModeUi = [cmbPullMode, cmbOutput, btnBrowseOutput, layout]() {
			const bool recordMode = cmbPullMode->currentData().toString() == QStringLiteral("record");
			cmbOutput->setVisible(recordMode);
			btnBrowseOutput->setVisible(recordMode);
			if (QWidget* label = layout->labelForField(cmbOutput))
			{
				label->setVisible(recordMode);
			}
		};
		connect(cmbPullMode, &QComboBox::currentTextChanged, dlg, [refreshPullModeUi]() {
			refreshPullModeUi();
		});
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
			this->m_service->playerStop();
			QPointer<CaptureWindow> selfGuard(this);
			QPointer<QDialog> dlgGuard(dlg);
			std::thread([selfGuard, dlgGuard]() {
				if (!selfGuard || !dlgGuard || !selfGuard->m_service)
				{
					return;
				}
				selfGuard->m_service->streamStop();
				QMetaObject::invokeMethod(selfGuard, [selfGuard, dlgGuard]() {
					if (!selfGuard || !dlgGuard)
					{
						return;
					}
					dlgGuard->setProperty("pullStopping", false);
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
		connect(logTimer, &QTimer::timeout, dlg, [this, dlg, makePullListenUrl, makePullPreviewUrl, lblStatus, txtLog, applyPullUiRunningState]() {
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
			const QString latestLog = this->m_service->streamRecentLog();
			const QString extraLog = dlg->property("pullExtraLog").toString();
			const QString mergedLog = extraLog.isEmpty() ? latestLog : (latestLog + extraLog);
			if (running && m_pullPreviewDialog && !dlg->property("pullPreviewAutoOpened").toBool() &&
				latestLog.contains(QStringLiteral("[拉流] 检测到上游推流连接")))
			{
				const QString source = makePullPreviewUrl();
				if (!source.isEmpty())
				{
					if (this->m_service->openMediaFile(source))
					{
						lblStatus->setText(tr("状态：已检测到推流连接，已自动打开预览"));
						const QString prev = dlg->property("pullExtraLog").toString();
						dlg->setProperty("pullExtraLog", prev + tr("[预览] 自动打开成功：%1\n").arg(source));
					}
					else
					{
						lblStatus->setText(tr("状态：已检测到推流连接，但预览打开失败，请点击“刷新”重试"));
						const QString prev = dlg->property("pullExtraLog").toString();
						dlg->setProperty("pullExtraLog", prev + tr("[预览] 自动打开失败：%1\n").arg(source));
					}
					dlg->setProperty("pullPreviewAutoOpened", true);
				}
			}
			syncStreamLogView(txtLog, mergedLog);
		});
		logTimer->start();
		connect(btnStart, &QPushButton::clicked, dlg, [this, dlg, cmbPullMode, cmbOutput, makePullListenUrl, makePullPreviewUrl, addRecent, applyPullUiRunningState]() {
			const QString pullInput = makePullListenUrl();
			const bool recordMode = cmbPullMode->currentData().toString() == QStringLiteral("record");
			QString pullOutput = recordMode ? cmbOutput->currentText().trimmed() : QString();
			if (!recordMode)
			{
				const int relayPort = choosePullListenPort(20000);
				pullOutput = QStringLiteral("udp://127.0.0.1:%1").arg(relayPort);
				dlg->setProperty("pullPreviewRelayUrl", pullOutput);
				const QString prev = dlg->property("pullExtraLog").toString();
				dlg->setProperty("pullExtraLog", prev + tr("[预览] 使用内部中继：%1\n").arg(pullOutput));
			}
			else
			{
				dlg->setProperty("pullPreviewRelayUrl", QString());
			}
			if (pullInput.isEmpty() || (recordMode && pullOutput.isEmpty()))
			{
				QMessageBox::warning(this, tr("拉流失败"), recordMode ? tr("输入地址和保存路径不能为空。") : tr("输入地址不能为空。"));
				return;
			}
			if (!this->m_service->streamStartPull(pullInput, pullOutput))
			{
				QMessageBox::warning(this, tr("拉流失败"), this->m_service->streamLastError());
				return;
			}
			addRecent(m_recentPullInputs, pullInput);
			if (recordMode)
			{
				addRecent(m_recentPullOutputs, pullOutput);
			}
			applyPullUiRunningState(true);
			dlg->setProperty("pullPreviewAutoOpened", false);
			if (!m_pullPreviewDialog)
			{
				auto* preview = new QDialog(this);
				preview->setAttribute(Qt::WA_DeleteOnClose, true);
				preview->setWindowTitle(tr("拉流预览"));
				preview->resize(900, 560);
				auto* vLayout = new QVBoxLayout(preview);
				auto* view = new fplayer::FVideoView(preview);
				view->setBackendType(fplayer::MediaBackendType::FFmpeg);
				vLayout->addWidget(view, 1);
				auto* ctrlRow = new QWidget(preview);
				auto* ctrlLayout = new QHBoxLayout(ctrlRow);
				ctrlLayout->setContentsMargins(0, 0, 0, 0);
				auto* btnPause = new QPushButton(tr("暂停"), ctrlRow);
				auto* btnRefresh = new QPushButton(tr("刷新"), ctrlRow);
				auto* sldVolume = new QSlider(Qt::Horizontal, ctrlRow);
				sldVolume->setRange(0, 100);
				sldVolume->setValue(qRound(this->m_service->playerVolume() * 100.0f));
				ctrlLayout->addWidget(btnPause, 0);
				ctrlLayout->addWidget(btnRefresh, 0);
				ctrlLayout->addWidget(new QLabel(tr("音量"), ctrlRow), 0);
				ctrlLayout->addWidget(sldVolume, 1);
				vLayout->addWidget(ctrlRow, 0);
				m_pullPreviewDialog = preview;
				m_pullPreviewView = view;
				m_pullVolumeSlider = sldVolume;
				connect(preview, &QObject::destroyed, this, [this]() {
					m_pullPreviewDialog = nullptr;
					m_pullPreviewView = nullptr;
					m_pullVolumeSlider = nullptr;
					if (m_service) { m_service->playerStop(); }
				});
				connect(btnPause, &QPushButton::clicked, preview, [this, btnPause]() {
					if (m_service->playerIsPlaying())
					{
						m_service->playerPause();
						btnPause->setText(tr("继续"));
					}
					else
					{
						m_service->playerResume();
						btnPause->setText(tr("暂停"));
					}
				});
				connect(btnRefresh, &QPushButton::clicked, preview, [this, dlg, makePullPreviewUrl]() {
					const QString source = makePullPreviewUrl();
					if (!source.isEmpty())
					{
						const bool ok = m_service->openMediaFile(source);
						const QString prev = dlg->property("pullExtraLog").toString();
						dlg->setProperty("pullExtraLog",
						                 prev + (ok ? tr("[预览] 手动刷新成功：%1\n").arg(source)
						                            : tr("[预览] 手动刷新失败：%1\n").arg(source)));
					}
				});
				connect(sldVolume, &QSlider::valueChanged, preview, [this](int value) {
					this->m_service->playerSetVolume(static_cast<float>(value) / 100.0f);
				});
				this->m_service->bindPlayerPreview(view);
				preview->show();
			}
		});
		connect(btnStop, &QPushButton::clicked, dlg, [requestStopPullAsync]() {
			requestStopPullAsync();
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
		connect(btnClose, &QPushButton::clicked, dlg, [this, dlg, requestStopPullAsync]() {
			if (this->m_service->streamIsRunning())
			{
				const auto answer = QMessageBox::question(dlg,
				                                          tr("确认关闭"),
				                                          tr("当前正在拉流，确定先停止拉流再关闭监视窗口吗？"),
				                                          QMessageBox::Yes | QMessageBox::No,
				                                          QMessageBox::No);
				if (answer != QMessageBox::Yes)
				{
					return;
				}
				dlg->setProperty("pullCloseAfterStop", true);
				requestStopPullAsync();
				return;
			}
			dlg->close();
		});
		layout->addRow(tr("协议模板"), cmbProtocol);
		layout->addRow(tr("模式"), cmbPullMode);
		layout->addRow(tr("推流码"), edtStreamKey);
		layout->addRow(tr("拉流地址"), urlRow);
		layout->addRow(tr("保存路径"), cmbOutput);
		layout->addRow(QString(), btnBrowseOutput);
		layout->addRow(lblStatus);
		layout->addRow(txtLog);
		layout->addRow(buttons);
		refreshPullUrl();
		refreshStreamKeyVisibility();
		refreshPullModeUi();
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
	m_composeAspectCombo = new QComboBox(leftPanel);
	m_composeAspectCombo->addItem(QStringLiteral("16:9 (横屏直播)"), QStringLiteral("16:9"));
	m_composeAspectCombo->addItem(QStringLiteral("9:16 (手机竖屏)"), QStringLiteral("9:16"));
	m_composeAspectCombo->addItem(QStringLiteral("4:3"), QStringLiteral("4:3"));
	m_composeAspectCombo->addItem(QStringLiteral("3:4"), QStringLiteral("3:4"));
	m_composeAspectCombo->addItem(QStringLiteral("1:1"), QStringLiteral("1:1"));
	m_composeAspectCombo->setCurrentIndex(0);
	m_btnComposeAddFile = new QPushButton(tr("追加文件播放"), leftPanel);
	m_btnComposeAddCamera = new QPushButton(tr("追加摄像头"), leftPanel);
	m_btnComposeAddScreen = new QPushButton(tr("追加屏幕"), leftPanel);
	m_composeSourceList = new QListWidget(leftPanel);
	m_composeSourceList->setSelectionMode(QAbstractItemView::SingleSelection);
	leftLayout->addWidget(titleLabel);
	leftLayout->addWidget(aspectLabel);
	leftLayout->addWidget(m_composeAspectCombo);
	leftLayout->addWidget(m_btnComposeAddFile);
	leftLayout->addWidget(m_btnComposeAddCamera);
	leftLayout->addWidget(m_btnComposeAddScreen);
	leftLayout->addWidget(m_composeSourceList, 1);
	leftPanel->setStyleSheet(QStringLiteral(
		"#composeLeftPanel{background:#100f23;border:none;border-radius:8px;}"
		"QLabel{color:#ebdeff;}"
	));

	m_composePreviewHost = new AspectRatioHostWidget(m_composeSplitter);
	m_composeMdiArea = new QMdiArea(m_composePreviewHost);
	m_composeMdiArea->setBackground(QBrush(QColor(0, 0, 0)));
	m_composeMdiArea->setStyleSheet(QStringLiteral("QMdiArea{border:none;background:#04030a;}"));
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
	static_cast<AspectRatioHostWidget*>(m_composePreviewHost)->setAspectRatio(m_composeAspectW, m_composeAspectH);
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
				applyComposeAspectRatio();
			}
		}
	});
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
		m_service->playerPause();
		m_service->cameraPause();
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
	svc->initPlayer(fplayer::MediaBackendType::FFmpeg);
	auto* container = new ComposeSourceWidget();
	auto* view = new fplayer::FVideoView(container);
	view->setBackendType(fplayer::MediaBackendType::FFmpeg);
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
				// 拖动/缩放后仅在该路原本处于播放态时才重绑并激活，避免暂停素材被误恢复。
				if (composeSourceIsPlaying(idx))
				{
					src.service->bindScreenPreview(src.view);
					src.service->selectScreen(qMax(0, src.deviceIndex));
					src.service->screenSetFrameRate(qMax(1, src.screenFps));
					src.service->screenSetCursorCaptureEnabled(src.screenCaptureCursor);
					src.service->screenSetActive(true);
				}
			}
		}
		forceRefreshComposePreview();
	};
	m_composeSources.push_back(item);
	m_composeSourceList->addItem(item.title);
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
	svc->selectCamera(0);
	const auto fmts = svc->getCameraFormats(0);
	if (!fmts.isEmpty())
	{
		svc->selectCameraFormat(0);
	}
	auto* sub = m_composeMdiArea->addSubWindow(container, Qt::FramelessWindowHint);
	sub->setFocusPolicy(Qt::NoFocus);
	sub->setAttribute(Qt::WA_ShowWithoutActivating, true);
	sub->setWindowTitle(tr("摄像头：%1").arg(cameras.first()));
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
	m_composeSourceList->addItem(item.title);
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
			const auto choice = QMessageBox::question(
				this,
				tr("屏幕捕获后端切换"),
				tr("已检测到系统HDR打开，为确保稳定，将屏幕获取后端改为ffmepg。\n是否切换？"),
				QMessageBox::Yes | QMessageBox::No,
				QMessageBox::Yes);
			if (choice == QMessageBox::Yes)
			{
				m_screenBackendType = fplayer::MediaBackendType::FFmpeg;
				if (m_service)
				{
					m_service->initScreenCapture(m_screenBackendType);
					if (m_captureMode == CaptureMode::Screen)
					{
						ui->wgtView->setBackendType(m_screenBackendType);
						m_service->bindScreenPreview(ui->wgtView);
						refreshScreenDeviceUi();
					}
				}
			}
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
	auto* sub = m_composeMdiArea->addSubWindow(container, Qt::FramelessWindowHint);
	sub->setFocusPolicy(Qt::NoFocus);
	sub->setAttribute(Qt::WA_ShowWithoutActivating, true);
	sub->setWindowTitle(tr("屏幕：%1").arg(screens.first()));
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
				// 拖动/缩放后仅在该路原本处于播放态时才重绑并激活，避免暂停素材被误恢复。
				if (composeSourceIsPlaying(idx))
				{
					src.service->bindScreenPreview(src.view);
					src.service->selectScreen(qMax(0, src.deviceIndex));
					src.service->screenSetFrameRate(qMax(1, src.screenFps));
					src.service->screenSetCursorCaptureEnabled(src.screenCaptureCursor);
					src.service->screenSetActive(true);
				}
			}
		}
		forceRefreshComposePreview();
	};
	m_composeSources.push_back(item);
	m_composeSourceList->addItem(item.title);
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
		m_composeSourceList->addItem(displayTitle);
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
		const QList<int> fpsCandidates{15, 24, 25, 30, 45, 50, 60, 75, 90, 100, 120, 144, 165, 180, 200, 240};
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
		return;
	}
	const bool playing = composeSourceIsPlaying(m_composeSelectedIndex);
	ui->btnPlay->setIcon(QIcon::fromTheme(
		playing ? QIcon::ThemeIcon::MediaPlaybackPause : QIcon::ThemeIcon::MediaPlaybackStart));
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
	switch (src.kind)
	{
	case ComposeSourceItem::SourceKind::File:
		if (src.service->playerIsPlaying())
		{
			src.service->playerPause();
		}
		else
		{
			src.service->playerResume();
		}
		break;
	case ComposeSourceItem::SourceKind::Camera:
		if (src.service->cameraIsPlaying())
		{
			src.service->cameraPause();
		}
		else
		{
			src.service->cameraResume();
		}
		break;
	case ComposeSourceItem::SourceKind::Screen:
		if (src.service->screenIsActive())
		{
			src.service->screenSetActive(false);
			// 必须排除本行，否则 refresh 仍会把「当前选中屏幕」重新激活。
			refreshComposeScreenCaptureState(m_composeSelectedIndex, -1, index);
		}
		else
		{
			refreshComposeScreenCaptureState(m_composeSelectedIndex, index);
		}
		break;
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
	// 原生视频渲染控件在 MDI 内频繁几何变化后，偶发残影；拖拽结束时强制刷新一次画布和子项。
	m_composeMdiArea->update();
	if (QWidget* vp = m_composeMdiArea->viewport())
	{
		vp->update();
		vp->repaint();
	}
	const auto windows = m_composeMdiArea->subWindowList(QMdiArea::StackingOrder);
	for (QMdiSubWindow* sub : windows)
	{
		if (!sub)
		{
			continue;
		}
		sub->update();
		sub->repaint();
	}
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
	QWidget::resizeEvent(event);
	relocateTitleWidget();
	if (m_isComposeMode && m_composePreviewHost && m_composeAspectW > 0 && m_composeAspectH > 0)
	{
		static_cast<AspectRatioHostWidget*>(m_composePreviewHost)->setAspectRatio(m_composeAspectW, m_composeAspectH);
		applyComposeZOrder();
	}
}

void CaptureWindow::closeEvent(QCloseEvent* event)
{
	if (!event)
	{
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
			event->ignore();
			return;
		}
		m_service->streamStop();
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