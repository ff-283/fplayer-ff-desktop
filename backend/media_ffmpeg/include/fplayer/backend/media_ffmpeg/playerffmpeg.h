/*************************************************
  * 描述：FFmpeg 文件播放器
  *
  * File：playerffmpeg.h
  * Date：2026/4/9
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_PLAYERFFMPEG_H
#define FPLAYER_DESKETOP_PLAYERFFMPEG_H

#include <QObject>
#include <QByteArray>

#include <fplayer/api/media/iplayer.h>
#include <fplayer/backend/media_ffmpeg/export.h>

namespace fplayer
{
	class FPLAYER_BACKEND_MEDIA_FFMPEG_EXPORT PlayerFFmpeg : public QObject, public IPlayer
	{
		Q_OBJECT

	public:
		PlayerFFmpeg();
		~PlayerFFmpeg() override;

		bool openFile(const QString& filePath) override;
		void play() override;
		void pause() override;
		void stop() override;
		qint64 durationMs() const override;
		qint64 positionMs() const override;
		bool seekMs(qint64 positionMs) override;
		void setPlaybackRate(double rate) override;
		double playbackRate() const override;
		QString debugStats() const override;
		bool isPlaying() const override;
		void setPreviewTarget(const PreviewTarget& target) override;

	private slots:
		void deliverPreviewFrame();

	private:
		void queuePreviewYuv(QByteArray y, QByteArray u, QByteArray v, int width, int height, int yStride, int uStride, int vStride);

		struct Impl;
		Impl* m_impl;
	};
}

#endif //FPLAYER_DESKETOP_PLAYERFFMPEG_H
