/*************************************************
  * 描述：
  *
  * File：cameraffmpeg.h
  * Date：2026/3/3
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_CAMERAFFMPEG_H
#define FPLAYER_DESKETOP_CAMERAFFMPEG_H

#include <fplayer/backend/media_ffmpeg/export.h>
#include <fplayer/api/media/icamera.h>
#include <QObject>
#include <QList>
#include <QString>
#include <QByteArray>

// FFmpeg 头文件
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

namespace fplayer
{
	class FPLAYER_BACKEND_MEDIA_FFMPEG_EXPORT CameraFFmpeg : public QObject, public ICamera
	{
		Q_OBJECT

	public:
		CameraFFmpeg();
		~CameraFFmpeg() override;

		bool selectCamera(int index) override;

		bool selectCameraFormat(int index) override;

		void refreshCameras() override;

		QList<CameraDescription> getDescriptions() override;

		int getIndex() override;

		void pause() override;

		void resume() override;

		bool isPlaying() const override;
		void setPreviewTarget(const PreviewTarget& target) override;

	signals:
		void yuvFrameReady(const QByteArray& yData, const QByteArray& uData, const QByteArray& vData,
		                   int width, int height, int yStride, int uStride, int vStride);

	private:
		void captureLoop();

		private:
			// 内部实现类
			struct Impl;
			Impl* m_impl;
		};
	}

#endif //FPLAYER_DESKETOP_CAMERAFFMPEG_H