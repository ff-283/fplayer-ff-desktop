#include <fplayer/backend/desktopcapture_dxgi/screencapturedxgi.h>

#include <QDateTime>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <logger/logger.h>

#include <fplayer/common/fglwidget/fglwidget.h>
#include <fplayer/common/screenframebus/screenframebus.h>

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

	const char* dxgiFormatName(const DXGI_FORMAT fmt)
	{
		switch (fmt)
		{
		case DXGI_FORMAT_B8G8R8A8_UNORM:
			return "B8G8R8A8_UNORM";
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return "B8G8R8A8_UNORM_SRGB";
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			return "R8G8B8A8_UNORM";
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			return "R8G8B8A8_UNORM_SRGB";
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			return "R10G10B10A2_UNORM";
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			return "R16G16B16A16_FLOAT";
		default:
			return "OTHER";
		}
	}

	bool resolveInputPixelFormat(const DXGI_FORMAT fmt, AVPixelFormat& srcFmt, int& bytesPerPixel)
	{
		switch (fmt)
		{
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			srcFmt = AV_PIX_FMT_BGRA;
			bytesPerPixel = 4;
			return true;
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			srcFmt = AV_PIX_FMT_RGBA;
			bytesPerPixel = 4;
			return true;
		default:
			srcFmt = AV_PIX_FMT_NONE;
			bytesPerPixel = 0;
			return false;
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
#ifdef _WIN32
	// DXGI 后端优先使用 OutputDesc 原生坐标与尺寸，避免 Qt 几何+DPR 推导引入的 1px 偏差。
	IDXGIFactory1* factory = nullptr;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory)
	{
		const QScreen* primary = QGuiApplication::primaryScreen();
		const QRect primaryRect = primary ? primary->geometry() : QRect();
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
				if (SUCCEEDED(output->GetDesc(&od)) && od.AttachedToDesktop)
				{
					ScreenDescription d;
					const QString devName = QString::fromWCharArray(od.DeviceName).trimmed();
					d.name = devName.isEmpty() ? QStringLiteral("屏幕") : devName;
					d.x = od.DesktopCoordinates.left;
					d.y = od.DesktopCoordinates.top;
					d.width = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
					d.height = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
					const QRect outRect(d.x, d.y, d.width, d.height);
					d.isPrimary = !primaryRect.isNull() && outRect.intersects(primaryRect);
					m_descriptions.push_back(d);
				}
				safeRelease(output);
			}
			safeRelease(adapter);
		}
		safeRelease(factory);
	}
	if (!m_descriptions.isEmpty())
	{
		return;
	}
#endif
	// DXGI 枚举失败时，回退到 Qt 层几何信息。
	auto alignEvenFloor = [](const int v) -> int {
		return (v >= 0) ? (v & ~1) : -(((-v) + 1) & ~1);
	};
	auto alignEvenSize = [](const int v) -> int {
		return qMax(2, v) & ~1;
	};
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
		d.width = alignEvenSize(qRound(logical.width() * dpr));
		d.height = alignEvenSize(qRound(logical.height() * dpr));
		d.x = alignEvenFloor(qRound(screen->geometry().x() * dpr));
		d.y = alignEvenFloor(qRound(screen->geometry().y() * dpr));
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
	// 采集线程反复写入 m_previewY/U/V；lambda 排队时若只浅拷贝 QByteArray，会与采集线程共享同一块缓冲区导致数据竞争。
	const QByteArray yCopy(yData.constData(), yData.size());
	const QByteArray uCopy(uData.constData(), uData.size());
	const QByteArray vCopy(vData.constData(), vData.size());
	QPointer<FGLWidget> target = m_glWidget;
	QMetaObject::invokeMethod(m_glWidget, [target, yCopy, uCopy, vCopy, width, height, yStride, uStride, vStride]() {
		if (!target)
		{
			return;
		}
		target->updateYUVFrame(yCopy, uCopy, vCopy, width, height, yStride, uStride, vStride);
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
	sws_freeContext(static_cast<SwsContext*>(m_swsPreview));
	m_swsPreview = nullptr;
	sws_freeContext(static_cast<SwsContext*>(m_swsPush));
	m_swsPush = nullptr;
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
	m_previewW = m_previewH = 0;
	m_pushW = m_pushH = 0;
	m_bgraFrame.clear();
	m_previewY.clear();
	m_previewU.clear();
	m_previewV.clear();
	m_pushY.clear();
	m_pushU.clear();
	m_pushV.clear();
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
	constexpr int kRectTolerancePx = 2;
	IDXGIFactory1* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
	{
		return false;
	}
	bool ok = false;
	IDXGIAdapter1* fallbackAdapter = nullptr;
	IDXGIOutput* fallbackOutput = nullptr;
	UINT globalOutputIndex = 0;
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
			const bool isApproxMatch =
				(qAbs(od.DesktopCoordinates.left - expected.x) <= kRectTolerancePx) &&
				(qAbs(od.DesktopCoordinates.top - expected.y) <= kRectTolerancePx) &&
				(qAbs(outW - expected.width) <= kRectTolerancePx) &&
				(qAbs(outH - expected.height) <= kRectTolerancePx);
			const bool isIndexFallback = (globalOutputIndex == static_cast<UINT>(screenIndex));
			if (!isApproxMatch)
			{
				if (isIndexFallback)
				{
					safeRelease(fallbackAdapter);
					safeRelease(fallbackOutput);
					adapter->AddRef();
					output->AddRef();
					fallbackAdapter = adapter;
					fallbackOutput = output;
				}
				++globalOutputIndex;
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
			safeRelease(fallbackAdapter);
			safeRelease(fallbackOutput);
			break;
		}
		if (ok)
		{
			break;
		}
		safeRelease(adapter);
	}
	if (!ok && fallbackAdapter && fallbackOutput)
	{
		IDXGIOutput1* out1 = nullptr;
		if (SUCCEEDED(fallbackOutput->QueryInterface(IID_PPV_ARGS(&out1))))
		{
			ID3D11Device* device = nullptr;
			ID3D11DeviceContext* context = nullptr;
			D3D_FEATURE_LEVEL fl{};
			const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
			const UINT nlevels = static_cast<UINT>(sizeof(levels) / sizeof(levels[0]));
			if (SUCCEEDED(D3D11CreateDevice(fallbackAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
			                                nlevels, D3D11_SDK_VERSION, &device, &fl, &context)))
			{
				IDXGIOutputDuplication* dupl = nullptr;
				if (SUCCEEDED(out1->DuplicateOutput(device, &dupl)))
				{
					DXGI_OUTPUT_DESC od{};
					fallbackOutput->GetDesc(&od);
					m_d3dDevice = device;
					m_d3dContext = context;
					m_duplication = dupl;
					m_outputLeft = od.DesktopCoordinates.left;
					m_outputTop = od.DesktopCoordinates.top;
					ok = true;
				}
				else
				{
					safeRelease(device);
					safeRelease(context);
				}
			}
		}
		safeRelease(out1);
	}
	safeRelease(fallbackAdapter);
	safeRelease(fallbackOutput);
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
		// 桌面复制在画面静止时会超时；这不是故障，不应触发重建设备。
		return true;
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
	AVPixelFormat srcPixFmt = AV_PIX_FMT_NONE;
	int bytesPerPixel = 0;
	if (!resolveInputPixelFormat(td.Format, srcPixFmt, bytesPerPixel))
	{
		const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
		++m_unsupportedSkipCount;
		// HDR/高级色彩场景会高频命中该分支，按时间窗口合并日志，避免刷屏。
		if ((nowMs - m_lastUnsupportedLogMs) >= 3000)
		{
			m_lastUnsupportedLogMs = nowMs;
			m_lastDxgiFormat = static_cast<int>(td.Format);
			LOG_WARN("[screen][dxgi]", "skip unsupported duplication format=", dxgiFormatName(td.Format), "(",
			         static_cast<int>(td.Format), "), usually caused by HDR/advanced-color",
			         ", skipped=", m_unsupportedSkipCount);
			m_unsupportedSkipCount = 0;
		}
		safeRelease(acquired);
		dupl->ReleaseFrame();
		// HDR/高级色彩场景可能间歇出现浮点格式帧；跳过该帧，等待下一帧，
		// 避免把它当作故障触发 duplication 重建（会造成画面扭曲/抖动）。
		return true;
	}
	const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
	if (!m_loggedFrameMeta || m_lastDxgiFormat != static_cast<int>(td.Format) || (nowMs - m_lastFrameMetaLogMs) >= 5000)
	{
		m_loggedFrameMeta = true;
		m_lastFrameMetaLogMs = nowMs;
		m_lastDxgiFormat = static_cast<int>(td.Format);
#if 0
		LOG_INFO("[screen][dxgi]", "frame meta w=", static_cast<int>(td.Width), " h=", static_cast<int>(td.Height), " fmt=",
		         dxgiFormatName(td.Format), " bpp=", bytesPerPixel);
#endif
	}

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
		sws_freeContext(static_cast<SwsContext*>(m_swsPreview));
		m_swsPreview = nullptr;
		sws_freeContext(static_cast<SwsContext*>(m_swsPush));
		m_swsPush = nullptr;
		m_previewW = m_previewH = 0;
		m_pushW = m_pushH = 0;
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
	const int packedBytes = tw * th * bytesPerPixel;
	if (m_bgraFrame.size() != packedBytes)
	{
		m_bgraFrame.resize(packedBytes);
	}
	auto* dst = reinterpret_cast<uint8_t*>(m_bgraFrame.data());
	auto* src = static_cast<const uint8_t*>(map.pData);
	if (map.RowPitch < static_cast<UINT>(tw * bytesPerPixel))
	{
		LOG_ERROR("[screen][dxgi]", "invalid row pitch=", static_cast<int>(map.RowPitch), " expectedAtLeast=", tw * bytesPerPixel);
		ctx->Unmap(staging, 0);
		return false;
	}
	for (int y = 0; y < th; ++y)
	{
		memcpy(dst + static_cast<size_t>(y) * tw * bytesPerPixel, src + y * map.RowPitch, static_cast<size_t>(tw) * bytesPerPixel);
	}
	ctx->Unmap(staging, 0);

	if (m_captureCursor)
	{
		if (srcPixFmt == AV_PIX_FMT_BGRA)
		{
			drawCursorOnBgra(reinterpret_cast<uint8_t*>(m_bgraFrame.data()), tw, th, tw * bytesPerPixel);
		}
		else
		{
			// 仅 BGRA 路径做 GDI 指针叠加；RGBA 叠加会通道错乱，这里保持原帧。
		}
	}

	const uint8_t* srcSlice[4] = {reinterpret_cast<const uint8_t*>(m_bgraFrame.constData()), nullptr, nullptr, nullptr};
	int srcStrideArr[4] = {tw * bytesPerPixel, 0, 0, 0};

	// 预览链路固定使用屏幕尺寸；YUV420P 目的尺寸需为偶数，奇数时做 1px 裁剪避免 U/V 错位。
	const int previewW = qMax(2, tw) & ~1;
	const int previewH = qMax(2, th) & ~1;
	SwsContext* swsPreview = static_cast<SwsContext*>(m_swsPreview);
	if (!swsPreview || m_previewW != previewW || m_previewH != previewH)
	{
		sws_freeContext(swsPreview);
		swsPreview = sws_getContext(tw, th, srcPixFmt, previewW, previewH, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr,
		                            nullptr);
		m_swsPreview = swsPreview;
		m_previewW = previewW;
		m_previewH = previewH;
	}
	if (!swsPreview)
	{
		return false;
	}
	const int previewYStride = previewW;
	const int previewUStride = previewW / 2;
	const int previewVStride = previewW / 2;
	const int previewYBytes = previewYStride * previewH;
	const int previewUvHeight = previewH / 2;
	const int previewUBytes = previewUStride * previewUvHeight;
	const int previewVBytes = previewVStride * previewUvHeight;
	if (m_previewY.size() != previewYBytes)
	{
		m_previewY.resize(previewYBytes);
	}
	if (m_previewU.size() != previewUBytes)
	{
		m_previewU.resize(previewUBytes);
	}
	if (m_previewV.size() != previewVBytes)
	{
		m_previewV.resize(previewVBytes);
	}
	uint8_t* previewYuv[4] = {
		reinterpret_cast<uint8_t*>(m_previewY.data()),
		reinterpret_cast<uint8_t*>(m_previewU.data()),
		reinterpret_cast<uint8_t*>(m_previewV.data()),
		nullptr
	};
	int previewStride[4] = {previewYStride, previewUStride, previewVStride, 0};
	sws_scale(swsPreview, srcSlice, srcStrideArr, 0, th, previewYuv, previewStride);
	dispatchFrameToView(m_previewY, m_previewU, m_previewV, previewW, previewH, previewStride[0], previewStride[1], previewStride[2]);

	// 推流链路按目标尺寸独立缩放并发布到总线。
	int targetW = 0;
	int targetH = 0;
	fplayer::ScreenFrameBus::instance().publishTargetSize(targetW, targetH, m_frameBusSourceId);
	const bool pushRequested = (targetW > 0 && targetH > 0);
	if (targetW <= 0 || targetH <= 0)
	{
		targetW = tw;
		targetH = th;
	}
	targetW = qMax(2, targetW) & ~1;
	targetH = qMax(2, targetH) & ~1;

	// 未下发推流尺寸时，先发布预览同尺寸帧，保证推流启动阶段能拿到首帧。
	if (!pushRequested)
	{
		fplayer::ScreenFrameBus::instance().publish(m_previewY, m_previewU, m_previewV, previewW, previewH, previewStride[0], previewStride[1],
		                                            previewStride[2], m_frameBusSourceId);
		return true;
	}

	// 推流尺寸与预览尺寸一致时，只做一次 BGRA->YUV 转换并复用结果。
	if (targetW == previewW && targetH == previewH)
	{
		fplayer::ScreenFrameBus::instance().publish(m_previewY, m_previewU, m_previewV, previewW, previewH, previewStride[0], previewStride[1],
		                                            previewStride[2], m_frameBusSourceId);
		return true;
	}

	SwsContext* swsPush = static_cast<SwsContext*>(m_swsPush);
	if (!swsPush || m_pushW != targetW || m_pushH != targetH)
	{
		sws_freeContext(swsPush);
		swsPush = sws_getContext(tw, th, srcPixFmt, targetW, targetH, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
		m_swsPush = swsPush;
		m_pushW = targetW;
		m_pushH = targetH;
	}
	if (!swsPush)
	{
		return false;
	}
	const int pushYStride = targetW;
	const int pushUStride = targetW / 2;
	const int pushVStride = targetW / 2;
	const int pushYBytes = pushYStride * targetH;
	const int pushUBytes = pushUStride * (targetH / 2);
	const int pushVBytes = pushVStride * (targetH / 2);
	if (m_pushY.size() != pushYBytes)
	{
		m_pushY.resize(pushYBytes);
	}
	if (m_pushU.size() != pushUBytes)
	{
		m_pushU.resize(pushUBytes);
	}
	if (m_pushV.size() != pushVBytes)
	{
		m_pushV.resize(pushVBytes);
	}
	uint8_t* pushYuv[4] = {
		reinterpret_cast<uint8_t*>(m_pushY.data()),
		reinterpret_cast<uint8_t*>(m_pushU.data()),
		reinterpret_cast<uint8_t*>(m_pushV.data()),
		nullptr
	};
	int pushStride[4] = {pushYStride, pushUStride, pushVStride, 0};
	sws_scale(swsPush, srcSlice, srcStrideArr, 0, th, pushYuv, pushStride);
	fplayer::ScreenFrameBus::instance().publish(m_pushY, m_pushU, m_pushV, targetW, targetH, pushStride[0], pushStride[1], pushStride[2],
	                                            m_frameBusSourceId);
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
