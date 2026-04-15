#ifndef FPLAYER_DESKTOP_SCREENCAPTUREDXGI_H
#define FPLAYER_DESKTOP_SCREENCAPTUREDXGI_H

#include <QObject>
#include <QByteArray>
#include <QThread>
#include <atomic>

#include <fplayer/api/media/iscreencapture.h>
#include <fplayer/backend/desktopcapture_dxgi/export.h>

namespace fplayer
{
	class FGLWidget;

	class FPLAYER_BACKEND_DESKTOP_CAPTURE_DXGI_EXPORT ScreenCaptureDxgi : public QObject, public IScreenCapture
	{
	public:
		ScreenCaptureDxgi();
		~ScreenCaptureDxgi() override;

		void refreshScreens() override;
		QList<ScreenDescription> getDescriptions() override;
		bool selectScreen(int index) override;
		int getIndex() const override;
		void setPreviewTarget(const PreviewTarget& target) override;
		void setActive(bool active) override;
		bool isActive() const override;
		bool setCursorCaptureEnabled(bool enabled) override;
		bool canControlCursorCapture() const override;
		bool setFrameRate(int fps) override;
		int frameRate() const override;
		bool canControlFrameRate() const override;

	private:
		void dispatchFrameToView(const QByteArray& yData, const QByteArray& uData, const QByteArray& vData, int width,
		                         int height, int yStride, int uStride, int vStride);
		void captureLoop();
		void stopCaptureThread();
		void releaseDxgi();

#ifdef _WIN32
		bool openDuplicationForScreenIndex(int screenIndex);
		bool captureOneFrame();
		void drawCursorOnBgra(uint8_t* data, int w, int h, int pitch);
#endif

	private:
		QList<ScreenDescription> m_descriptions;
		QThread* m_captureThread = nullptr;
		std::atomic<bool> m_capturing{false};
		std::atomic<bool> m_active{false};
		bool m_captureCursor = true;
		FGLWidget* m_glWidget = nullptr;

#ifdef _WIN32
		void* m_swsPreview = nullptr;
		void* m_swsPush = nullptr;
		void* m_d3dDevice = nullptr;
		void* m_d3dContext = nullptr;
		void* m_duplication = nullptr;
		void* m_stagingTex = nullptr;
		int m_frameW = 0;
		int m_frameH = 0;
		int m_previewW = 0;
		int m_previewH = 0;
		int m_pushW = 0;
		int m_pushH = 0;
		long m_outputLeft = 0;
		long m_outputTop = 0;
#endif
	};
}

#endif
