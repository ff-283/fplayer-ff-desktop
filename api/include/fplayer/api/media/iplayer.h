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

namespace fplayer
{
    class FPLAYER_API_EXPORT IPlayer
    {
        public:
            virtual ~IPlayer();
    };
}


#endif //FPLAYER_DESKETOP_PLAYER_H
