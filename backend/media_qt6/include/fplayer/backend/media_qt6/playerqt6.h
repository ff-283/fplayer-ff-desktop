/*************************************************
  * 描述：Qt6 多媒体文件播放器（基于 QMediaPlayer）
  *
  * File：playerqt6.h
  * Date：2026/5/7
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_PLAYERQT6_H
#define FPLAYER_DESKETOP_PLAYERQT6_H

#include <QObject>

#include <fplayer/api/media/iplayer.h>
#include <fplayer/backend/media_qt6/export.h>

class QMediaPlayer;
class QAudioOutput;
class QVideoWidget;
class QVideoSink;
class QVideoFrame;

namespace fplayer
{
	class FPLAYER_BACKEND_MEDIA_QT6_EXPORT PlayerQt6 : public QObject, public IPlayer
	{
		Q_OBJECT

	public:
		PlayerQt6();
		~PlayerQt6() override;

		bool openFile(const QString& filePath) override;
		void play() override;
		void pause() override;
		void stop() override;
		qint64 durationMs() const override;
		qint64 positionMs() const override;
		bool seekMs(qint64 positionMs) override;
		void setPlaybackRate(double rate) override;
		double playbackRate() const override;
		void setVolume(float volume) override;
		float volume() const override;
		QString debugStats() const override;
		bool isPlaying() const override;
		void setPreviewTarget(const PreviewTarget& target) override;
		void setComposeStreamBusId(const QString& sourceId) override;

	private slots:
		void onVideoFrameChanged(const QVideoFrame& frame);

	private:
		void publishYuvFromQVideoFrame(const QVideoFrame& frame);

		QMediaPlayer* m_mediaPlayer = nullptr;
		QAudioOutput* m_audioOutput = nullptr;
		QVideoWidget* m_videoWidget = nullptr;
		QVideoSink* m_videoSink = nullptr;
		QString m_composeStreamBusId;
	};
}

#endif //FPLAYER_DESKETOP_PLAYERQT6_H
