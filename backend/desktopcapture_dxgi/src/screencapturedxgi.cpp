#include <fplayer/backend/desktopcapture_dxgi/screencapturedxgi.h>

#include <QDateTime>
#include <QGuiApplication>
#include <QMetaObject>
#include <QScreen>
#include <QThread>

#include <fplayer/common/fglwidget/fglwidget.h>

#ifdef _WIN32
#	define NOMINMAX
#	include <windows.h>
#	include <d3d11.h>
#	include <dxgi1_2.h>
#	include <cstring>

extern "C"
{
#	include <libavutil/imgutils.h>
#	include <libswscale/swscale.h>
}

namespace
{
	template<typename T>
	void safeRelease(T*& p)
	{
		if (p)
		{
			p->Release();
			p = nullptr;
		}
	}
}

#endif

fplayer::ScreenCaptureDxgi::ScreenCaptureDxgi()
{
	m_backendType = MediaBackendType::Dxgi;
}

fplayer::ScreenCaptureDxgi::~ScreenCaptureDxgi()
{
	stopCaptureThread();
	releaseDxgi();
}

void fplayer::ScreenCaptureDxgi::refreshScreens()
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
		d.isPrimary = screen == QGuiApplication::primaryScreen();
		const QSize logical = screen->geometry().size();
		const qreal dpr = screen->devicePixelRatio();
		d.width = qRound(logical.width() * dpr);
		d.height = qRound(logical.height() * dpr);
		d.x = qRound(screen->geometry().x() * dpr);
		d.y = qRound(screen->geometry().y() * dpr);
		m_descriptions.push_back(d);
	}
}

QList<fplayer::ScreenDescription> fplayer::ScreenCaptureDxgi::getDescriptions()
{
	if (m_descriptions.isEmpty())
	{
		refreshScreens();
	}
	return m_descriptions;
}

bool fplayer::ScreenCaptureDxgi::selectScreen(int index)
{
	if (m_descriptions.isEmpty())
	{
		refreshScreens();
	}
	if (index < 0 || index >= m_descriptions.size())
	{
		return false;
	}
	const bool wasActive = m_active.load();
	stopCaptureThread();
	releaseDxgi();
	m_screenIndex = index;
	if (wasActive)
	{
		setActive(true);
	}
	return true;
}

int fplayer::ScreenCaptureDxgi::getIndex() const
{
	return m_screenIndex;
}

void fplayer::ScreenCaptureDxgi::setPreviewTarget(const PreviewTarget& target)
{
	if (!target.backend_hint)
	{
		return;
	}
	m_glWidget = static_cast<FGLWidget*>(target.backend_hint);
}

void fplayer::ScreenCaptureDxgi::setActive(bool active)
{
	if (active == m_active.load())
	{
		return;
	}
	m_active.store(active);
	if (!active)
	{
		stopCaptureThread();
		return;
	}
	m_capturing.store(true);
	m_captureThread = QThread::create([this]() {
		captureLoop();
	});
	m_captureThread->start();
}

bool fplayer::ScreenCaptureDxgi::isActive() const
{
	return m_active.load();
}

bool fplayer::ScreenCaptureDxgi::setCursorCaptureEnabled(bool enabled)
{
	m_captureCursor = enabled;
	return true;
}

bool fplayer::ScreenCaptureDxgi::canControlCursorCapture() const
{
	return true;
}

bool fplayer::ScreenCaptureDxgi::setFrameRate(int fps)
{
	if (fps <= 0)
	{
		return false;
	}
	m_fps = fps;
	return true;
}

int fplayer::ScreenCaptureDxgi::frameRate() const
{
	return m_fps;
}

bool fplayer::ScreenCaptureDxgi::canControlFrameRate() const
{
	return true;
}

void fplayer::ScreenCaptureDxgi::dispatchFrameToView(const QByteArray& yData, const QByteArray& uData, const QByteArray& vData,
                                                     int width, int height, int yStride, int uStride, int vStride)
{
	if (!m_glWidget)
	{
		return;
	}
	QMetaObject::invokeMethod(m_glWidget, [this, yData, uData, vData, width, height, yStride, uStride, vStride]() {
		if (!m_glWidget)
		{
			return;
		}
		m_glWidget->updateYUVFrame(yData, uData, vData, width, height, yStride, uStride, vStride);
	}, Qt::QueuedConnection);
}

void fplayer::ScreenCaptureDxgi::stopCaptureThread()
{
	m_capturing.store(false);
	m_active.store(false);
	if (m_captureThread && m_captureThread->isRunning())
	{
		m_captureThread->wait(3000);
	}
	delete m_captureThread;
	m_captureThread = nullptr;
}

void fplayer::ScreenCaptureDxgi::releaseDxgi()
{
#ifdef _WIN32
	sws_freeContext(static_cast<SwsContext*>(m_sws));
	m_sws = nullptr;
	{
		auto* st = static_cast<ID3D11Texture2D*>(m_stagingTex);
		safeRelease(st);
		m_stagingTex = nullptr;
	}
	{
		auto* d = static_cast<IDXGIOutputDuplication*>(m_duplication);
		safeRelease(d);
		m_duplication = nullptr;
	}
	{
		auto* c = static_cast<ID3D11DeviceContext*>(m_d3dContext);
		safeRelease(c);
		m_d3dContext = nullptr;
	}
	{
		auto* dev = static_cast<ID3D11Device*>(m_d3dDevice);
		safeRelease(dev);
		m_d3dDevice = nullptr;
	}
	m_frameW = m_frameH = 0;
#endif
}

#ifdef _WIN32

bool fplayer::ScreenCaptureDxgi::openDuplicationForScreenIndex(int screenIndex)
{
	if (screenIndex < 0 || screenIndex >= m_descriptions.size())
	{
		return false;
	}
	const auto expected = m_descriptions.at(screenIndex);
	IDXGIFactory1* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
	{
		return false;
	}
	bool ok = false;
	for (UINT ai = 0;; ++ai)
	{
		IDXGIAdapter1* adapter = nullptr;
		if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND)
		{
			break;
		}
		for (UINT oi = 0;; ++oi)
		{
			IDXGIOutput* output = nullptr;
			if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND)
			{
				break;
			}
			DXGI_OUTPUT_DESC od{};
			output->GetDesc(&od);
			const long outW = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
			const long outH = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
			if (od.DesktopCoordinates.left != expected.x || od.DesktopCoordinates.top != expected.y || outW != expected.width || outH != expected.height)
			{
				output->Release();
				output = nullptr;
				continue;
			}
			IDXGIOutput1* out1 = nullptr;
			if (FAILED(output->QueryInterface(IID_PPV_ARGS(&out1))))
			{
				safeRelease(output);
				continue;
			}
			ID3D11Device* device = nullptr;
			ID3D11DeviceContext* context = nullptr;
			D3D_FEATURE_LEVEL fl{};
			const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
			const UINT nlevels = static_cast<UINT>(sizeof(levels) / sizeof(levels[0]));
			if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
			                             nlevels, D3D11_SDK_VERSION, &device, &fl, &context)))
			{
				safeRelease(out1);
				safeRelease(output);
				continue;
			}
			IDXGIOutputDuplication* dupl = nullptr;
			const HRESULT hrDup = out1->DuplicateOutput(device, &dupl);
			if (FAILED(hrDup))
			{
				safeRelease(device);
				safeRelease(context);
				safeRelease(out1);
				safeRelease(output);
				continue;
			}
			m_d3dDevice = device;
			m_d3dContext = context;
			m_duplication = dupl;
			m_outputLeft = od.DesktopCoordinates.left;
			m_outputTop = od.DesktopCoordinates.top;
			ok = true;
			safeRelease(out1);
			safeRelease(output);
			safeRelease(adapter);
			break;
		}
		if (ok)
		{
			break;
		}
		safeRelease(adapter);
	}
	safeRelease(factory);
	return ok;
}

void fplayer::ScreenCaptureDxgi::drawCursorOnBgra(uint8_t* data, int w, int h, int pitch)
{
	CURSORINFO ci{};
	ci.cbSize = sizeof(ci);
	if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING))
	{
		return;
	}
	HICON icon = CopyIcon(ci.hCursor);
	if (!icon)
	{
		return;
	}
	ICONINFO inf{};
	if (!GetIconInfo(icon, &inf))
	{
		DestroyIcon(icon);
		return;
	}
	const int lx = ci.ptScreenPos.x - static_cast<int>(m_outputLeft) - static_cast<int>(inf.xHotspot);
	const int ly = ci.ptScreenPos.y - static_cast<int>(m_outputTop) - static_cast<int>(inf.yHotspot);

	HDC hdc = GetDC(nullptr);
	HDC mem = CreateCompatibleDC(hdc);
	BITMAPINFO bi{};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	HBITMAP hbmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	HGDIOBJ old = SelectObject(mem, hbmp);
	for (int row = 0; row < h; ++row)
	{
		memcpy(static_cast<uint8_t*>(bits) + row * w * 4, data + row * pitch, static_cast<size_t>(w) * 4);
	}
	DrawIconEx(mem, lx, ly, icon, 0, 0, 0, nullptr, DI_NORMAL);
	for (int row = 0; row < h; ++row)
	{
		memcpy(data + row * pitch, static_cast<uint8_t*>(bits) + row * w * 4, static_cast<size_t>(w) * 4);
	}
	SelectObject(mem, old);
	DeleteObject(hbmp);
	DeleteDC(mem);
	ReleaseDC(nullptr, hdc);
	if (inf.hbmMask)
	{
		DeleteObject(inf.hbmMask);
	}
	if (inf.hbmColor)
	{
		DeleteObject(inf.hbmColor);
	}
	DestroyIcon(icon);
}

bool fplayer::ScreenCaptureDxgi::captureOneFrame()
{
	auto* dupl = static_cast<IDXGIOutputDuplication*>(m_duplication);
	auto* ctx = static_cast<ID3D11DeviceContext*>(m_d3dContext);
	auto* dev = static_cast<ID3D11Device*>(m_d3dDevice);
	if (!dupl || !ctx || !dev)
	{
		return false;
	}
	DXGI_OUTDUPL_FRAME_INFO fi{};
	IDXGIResource* desktopRes = nullptr;
	const HRESULT hr = dupl->AcquireNextFrame(100, &fi, &desktopRes);
	if (hr == DXGI_ERROR_WAIT_TIMEOUT)
	{
		return false;
	}
	if (hr == DXGI_ERROR_ACCESS_LOST)
	{
		return false;
	}
	if (FAILED(hr) || !desktopRes)
	{
		return false;
	}
	ID3D11Texture2D* acquired = nullptr;
	if (FAILED(desktopRes->QueryInterface(IID_PPV_ARGS(&acquired))))
	{
		safeRelease(desktopRes);
		dupl->ReleaseFrame();
		return false;
	}
	safeRelease(desktopRes);

	D3D11_TEXTURE2D_DESC td{};
	acquired->GetDesc(&td);

	ID3D11Texture2D* staging = static_cast<ID3D11Texture2D*>(m_stagingTex);
	if (!staging || m_frameW != static_cast<int>(td.Width) || m_frameH != static_cast<int>(td.Height))
	{
		auto* oldStaging = static_cast<ID3D11Texture2D*>(m_stagingTex);
		safeRelease(oldStaging);
		m_stagingTex = nullptr;
		D3D11_TEXTURE2D_DESC sd = td;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;
		ID3D11Texture2D* st = nullptr;
		if (FAILED(dev->CreateTexture2D(&sd, nullptr, &st)))
		{
			safeRelease(acquired);
			dupl->ReleaseFrame();
			return false;
		}
		m_stagingTex = st;
		staging = st;
		m_frameW = static_cast<int>(td.Width);
		m_frameH = static_cast<int>(td.Height);
		sws_freeContext(static_cast<SwsContext*>(m_sws));
		m_sws = nullptr;
	}

	ctx->CopyResource(staging, acquired);
	safeRelease(acquired);
	dupl->ReleaseFrame();

	D3D11_MAPPED_SUBRESOURCE map{};
	if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
	{
		return false;
	}
	const int tw = m_frameW;
	const int th = m_frameH;
	QByteArray bgra;
	bgra.resize(tw * th * 4);
	auto* dst = reinterpret_cast<uint8_t*>(bgra.data());
	auto* src = static_cast<const uint8_t*>(map.pData);
	for (int y = 0; y < th; ++y)
	{
		memcpy(dst + y * tw * 4, src + y * map.RowPitch, static_cast<size_t>(tw) * 4);
	}
	ctx->Unmap(staging, 0);

	if (m_captureCursor)
	{
		drawCursorOnBgra(reinterpret_cast<uint8_t*>(bgra.data()), tw, th, tw * 4);
	}

	SwsContext* sws = static_cast<SwsContext*>(m_sws);
	if (!sws)
	{
		sws = sws_getContext(tw, th, AV_PIX_FMT_BGRA, tw, th, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
		m_sws = sws;
	}
	uint8_t* dstYuv[4] = {};
	int dstStride[4] = {};
	if (av_image_alloc(dstYuv, dstStride, tw, th, AV_PIX_FMT_YUV420P, 32) < 0)
	{
		return false;
	}
	const uint8_t* srcSlice[4] = {reinterpret_cast<const uint8_t*>(bgra.constData()), nullptr, nullptr, nullptr};
	int srcStrideArr[4] = {tw * 4, 0, 0, 0};
	sws_scale(sws, srcSlice, srcStrideArr, 0, th, dstYuv, dstStride);

	const int yStride = dstStride[0];
	const int uStride = dstStride[1];
	const int vStride = dstStride[2];
	QByteArray yb(reinterpret_cast<const char*>(dstYuv[0]), yStride * th);
	QByteArray ub(reinterpret_cast<const char*>(dstYuv[1]), uStride * (th / 2));
	QByteArray vb(reinterpret_cast<const char*>(dstYuv[2]), vStride * (th / 2));
	dispatchFrameToView(yb, ub, vb, tw, th, yStride, uStride, vStride);
	av_freep(&dstYuv[0]);
	return true;
}

void fplayer::ScreenCaptureDxgi::captureLoop()
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#ifdef _WIN32
	if (!openDuplicationForScreenIndex(m_screenIndex))
	{
		m_active.store(false);
		CoUninitialize();
		return;
	}
#endif
	const int frameMs = qMax(1, 1000 / qMax(1, m_fps));
	while (m_capturing.load())
	{
		const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
#ifdef _WIN32
		if (!m_duplication || !m_d3dDevice)
		{
			QThread::msleep(50);
			continue;
		}
		if (!captureOneFrame())
		{
			if (m_capturing.load())
			{
				releaseDxgi();
				QThread::msleep(80);
				openDuplicationForScreenIndex(m_screenIndex);
			}
		}
#endif
		const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - t0;
		const int sleepMs = static_cast<int>(frameMs - elapsed);
		if (sleepMs > 0)
		{
			QThread::msleep(sleepMs);
		}
	}
	releaseDxgi();
	CoUninitialize();
}

#else

void fplayer::ScreenCaptureDxgi::captureLoop()
{
}

#endif
