/*************************************************
  * 描述：
  *
  * File：ifvideoview.h
  * Date：2026/3/2
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_IFVIDEOVIEW_H
#define FPLAYER_DESKETOP_IFVIDEOVIEW_H
#include <fplayer/api/media/mediabackendtype.h>
#include <fplayer/api/media/previewtarget.h>
#include <fplayer/api/export.h>

namespace fplayer
{
	class FPLAYER_API_EXPORT IFVideoView
	{
	public:
		IFVideoView(MediaBackendType backendType = MediaBackendType::Qt6);
		virtual ~IFVideoView();

		virtual PreviewTarget previewTarget() = 0;

		void setBackendType(MediaBackendType backendType);

	protected:
		MediaBackendType m_backendType;
	};
}

#endif //FPLAYER_DESKETOP_IFVIDEOVIEW_H