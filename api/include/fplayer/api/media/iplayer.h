/*************************************************
  * 描述：媒体文件播放抽象类
  *
  * File：player.h
  * Date：2026/2/28
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_PLAYER_H
#define FPLAYER_DESKETOP_PLAYER_H

#include <fplayer/api/export.h>
#include <fplayer/api/media/mediabackendtype.h>
#include <fplayer/api/media/previewtarget.h>
#include <QString>
#include <QtTypes>
#include <atomic>

namespace fplayer
{
    class FPLAYER_API_EXPORT IPlayer
    {
        public:
            virtual ~IPlayer();

            virtual bool openFile(const QString& filePath) = 0;
            virtual void play() = 0;
            virtual void pause() = 0;
            virtual void stop() = 0;
            virtual qint64 durationMs() const = 0;
            virtual qint64 positionMs() const = 0;
            virtual bool seekMs(qint64 positionMs) = 0;
            virtual void setPlaybackRate(double rate) = 0;
            virtual double playbackRate() const = 0;
            virtual QString debugStats() const = 0;
            virtual bool isPlaying() const;
            virtual void setPreviewTarget(const PreviewTarget& target) = 0;

            MediaBackendType getBackendType() const;

        protected:
            MediaBackendType m_backend = MediaBackendType::Qt6;
            std::atomic<bool> m_isPlaying{false};
    };
}


#endif //FPLAYER_DESKETOP_PLAYER_H
