/*************************************************
  * 描述：
  *
  * File：runtime.h
  * Date：2026/2/28
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_RUNTIME_H
#define FPLAYER_DESKETOP_RUNTIME_H


#include <memory>
#include <fplayer/api/media/icamera.h>
#include <fplayer/api/media/iplayer.h>
#include <fplayer/api/media/previewtarget.h>
#include <fplayer/runtime/export.h>

namespace fplayer
{
	class FPLAYER_RUNTIME_EXPORT RunTime
	{
	public:
		std::shared_ptr<ICamera> createCamera(MediaBackendType backend);
		std::shared_ptr<IPlayer> createPlayer(MediaBackendType backend);
		void bindCameraPreview(const fplayer::PreviewTarget& target);
		void bindPlayerPreview(const fplayer::PreviewTarget& target);

	private:
		std::shared_ptr<ICamera> m_camera;
		std::shared_ptr<IPlayer> m_player;
	};


}

#endif //FPLAYER_DESKETOP_RUNTIME_H