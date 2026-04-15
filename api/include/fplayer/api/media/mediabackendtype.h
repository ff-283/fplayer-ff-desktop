/*************************************************
  * 描述：
  *
  * File：mediabackendtype.h
  * Date：2026/3/1
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_MEDIABACKENDTYPE_H
#define FPLAYER_DESKETOP_MEDIABACKENDTYPE_H

namespace fplayer
{
	enum class MediaBackendType
	{
		Qt6,
		FFmpeg,
		/// Windows：屏幕采集使用 DXGI Desktop Duplication（非 FFmpeg gdigrab）
		Dxgi
	};
}

#endif //FPLAYER_DESKETOP_MEDIABACKENDTYPE_H