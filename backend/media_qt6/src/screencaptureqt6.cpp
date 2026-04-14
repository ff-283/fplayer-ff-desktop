#include <fplayer/backend/media_qt6/screencaptureqt6.h>

#include <QGuiApplication>
#include <QMetaObject>
#include <QScreen>
#include <QVideoWidget>
#include <QtMultimedia/QMediaCaptureSession>
#include <QtMultimedia/QScreenCapture>

fplayer::ScreenCaptureQt6::ScreenCaptureQt6()
{
	m_backendType = MediaBackendType::Qt6;
	m_session = new QMediaCaptureSession(this);
	m_screenCapture = new QScreenCapture(this);
	m_session->setScreenCapture(m_screenCapture);
}

void fplayer::ScreenCaptureQt6::refreshScreens()
{
	m_descriptions.clear();
	const auto screens = QGuiApplication::screens();
	for (auto* screen : screens)
	{
		if (!screen)
		{
			continue;
		}
		ScreenDescription d;
		d.name = screen->name().isEmpty() ? QStringLiteral("屏幕") : screen->name();
		d.isPrimary = (screen == QGuiApplication::primaryScreen());
		const QSize logical = screen->geometry().size();
		const qreal dpr = screen->devicePixelRatio();
		d.width = qRound(logical.width() * dpr);
		d.height = qRound(logical.height() * dpr);
		d.x = qRound(screen->geometry().x() * dpr);
		d.y = qRound(screen->geometry().y() * dpr);
		m_descriptions.push_back(d);
	}
}

QList<fplayer::ScreenDescription> fplayer::ScreenCaptureQt6::getDescriptions()
{
	if (m_descriptions.isEmpty())
	{
		refreshScreens();
	}
	return m_descriptions;
}

bool fplayer::ScreenCaptureQt6::selectScreen(int index)
{
	const auto screens = QGuiApplication::screens();
	if (index < 0 || index >= screens.size())
	{
		return false;
	}
	auto* screen = screens.at(index);
	if (!screen)
	{
		return false;
	}
	m_screenCapture->setScreen(screen);
	m_screenIndex = index;
	return true;
}

int fplayer::ScreenCaptureQt6::getIndex() const
{
	return m_screenIndex;
}

void fplayer::ScreenCaptureQt6::setPreviewTarget(const PreviewTarget& target)
{
	auto* qtVideoWidget = static_cast<QVideoWidget*>(target.backend_hint);
	if (m_session)
	{
		m_session->setVideoOutput(qtVideoWidget);
	}
}

void fplayer::ScreenCaptureQt6::setActive(bool active)
{
	m_screenCapture->setActive(active);
}

bool fplayer::ScreenCaptureQt6::isActive() const
{
	return m_screenCapture->isActive();
}

bool fplayer::ScreenCaptureQt6::setCursorCaptureEnabled(bool enabled)
{
	const QMetaObject* mo = m_screenCapture->metaObject();
	const int methodIdx = mo ? mo->indexOfMethod("setCursorCaptureEnabled(bool)") : -1;
	if (methodIdx < 0)
	{
		return false;
	}
	QMetaObject::invokeMethod(m_screenCapture, "setCursorCaptureEnabled", Q_ARG(bool, enabled));
	return true;
}

bool fplayer::ScreenCaptureQt6::canControlCursorCapture() const
{
	const QMetaObject* mo = m_screenCapture->metaObject();
	return mo && mo->indexOfMethod("setCursorCaptureEnabled(bool)") >= 0;
}
