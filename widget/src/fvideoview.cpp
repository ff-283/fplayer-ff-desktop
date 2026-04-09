#include <fplayer/widget/fvideoview.h>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QtMultimedia/QVideoSink>

fplayer::FVideoView::FVideoView(QWidget* parent) : QWidget(parent)
{
	// 让这个控件有原生窗口句柄（FFmpeg/SDL/D3D 渲染需要）
	setAttribute(Qt::WA_NativeWindow, true);

	m_lay = new QVBoxLayout(this);
	m_lay->setContentsMargins(0, 0, 0, 0);
	m_lay->setSpacing(0);
}

fplayer::FVideoView::~FVideoView() = default;

void fplayer::FVideoView::setBackendType(MediaBackendType backendType)
{
	if (m_backendType == backendType)
	{
		return;
	}

	// 清理旧的 widget
	if (m_qtVideoWidget)
	{
		delete m_qtVideoWidget;
		m_qtVideoWidget = nullptr;
	}
	if (m_glWidget)
	{
		delete m_glWidget;
		m_glWidget = nullptr;
	}

	m_backendType = backendType;
}

// QVideoSink* fplayer::FVideoView::videoSink() const
// {
// 	return m_sink;
// }

fplayer::PreviewTarget fplayer::FVideoView::previewTarget()
{
	PreviewTarget t{};

	// 下面的没什么大用
#ifdef _WIN32
	t.window.hwnd = reinterpret_cast<void*>(winId());
#else
	t.window.handle = reinterpret_cast<void*>(winId());
#endif
	t.window.width = width();
	t.window.height = height();
	t.window.device_pixel_ratio = devicePixelRatioF();

	// 只在需要时创建 widget
	if (m_backendType == MediaBackendType::Qt6 && !m_qtVideoWidget)
	{
		m_qtVideoWidget = new QVideoWidget(this);
		m_lay->addWidget(m_qtVideoWidget);
	}
	else if (m_backendType == MediaBackendType::FFmpeg && !m_glWidget)
	{
		m_glWidget = new FGLWidget(this);
		m_lay->addWidget(m_glWidget);
	}

	// 真正有用的部分
	switch (m_backendType)
	{
	case MediaBackendType::Qt6:
		// Qt6 backend 用
		t.backend_hint = static_cast<void*>(m_qtVideoWidget);
		break;
	case MediaBackendType::FFmpeg:
		// FFmpeg backend 用
		t.backend_hint = static_cast<void*>(m_glWidget);
		break;
	default:
		break;
	}
	return t;
}

void fplayer::FVideoView::showEvent(QShowEvent* e)
{
	QWidget::showEvent(e);
	// 触发/确保句柄创建
	(void)winId();
	emit nativeWindowReady();
}

void fplayer::FVideoView::resizeEvent(QResizeEvent* e)
{
	QWidget::resizeEvent(e);
	emit viewResized(width(), height(), devicePixelRatioF());
}