#ifndef FPLAYER_DESKETOP_SCREENCAPTUREFFMPEG_H
#define FPLAYER_DESKETOP_SCREENCAPTUREFFMPEG_H

#include <QObject>
#include <QByteArray>
#include <QThread>
#include <atomic>

#include <fplayer/backend/media_ffmpeg/export.h>
#include <fplayer/api/media/iscreencapture.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace fplayer
{
	class FGLWidget;

	class FPLAYER_BACKEND_MEDIA_FFMPEG_EXPORT ScreenCaptureFFmpeg : public QObject, public IScreenCapture
	{
	public:
		ScreenCaptureFFmpeg();
		~ScreenCaptureFFmpeg() override;

		void refreshScreens() override;
		QList<ScreenDescription> getDescriptions() override;
		bool selectScreen(int index) override;
		int getIndex() const override;
		void setPreviewTarget(const PreviewTarget& target) override;
		void setActive(bool active) override;
		bool isActive() const override;
		bool setCursorCaptureEnabled(bool enabled) override;
		bool canControlCursorCapture() const override;

	private:
		void dispatchFrameToView(const QByteArray& yData, const QByteArray& uData, const QByteArray& vData,
		                         int width, int height, int yStride, int uStride, int vStride);
		void captureLoop();
		bool openInputForSelectedScreen();
		void stopCapture();
		void cleanup();

	private:
		QList<ScreenDescription> m_descriptions;
		AVFormatContext* m_formatContext = nullptr;
		AVCodecContext* m_codecContext = nullptr;
		AVStream* m_stream = nullptr;
		QThread* m_captureThread = nullptr;
		std::atomic<bool> m_capturing{false};
		std::atomic<bool> m_active{false};
		bool m_captureCursor = true;
		FGLWidget* m_glWidget = nullptr;
	};
}

#endif //FPLAYER_DESKETOP_SCREENCAPTUREFFMPEG_H
