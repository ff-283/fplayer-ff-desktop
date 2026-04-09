/*************************************************
  * 描述：
  *
  * File：service.h
  * Date：2026/2/28
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_SERVICE_H
#define FPLAYER_DESKETOP_SERVICE_H
#include <fplayer/api/media/ifvideoview.h>
#include <fplayer/service/export.h>
#include <fplayer/runtime/runtime.h>

class QWidget;

namespace fplayer
{
	class FPLAYER_SERVICE_EXPORT Service
	{
	public:
		Service();

		~Service();

		void initCamera(MediaBackendType backend);

		/**
		 * 初始化摄像头视频播放窗口
		 * @param videoView
		 */
		void bindCameraPreview(fplayer::IFVideoView* videoView);

		void selectCamera(int index);

		void selectCameraFormat(int index);

		QList<QString> getCameraList() const;

		QList<QString> getCameraFormats(int index) const;

		/**
		 * 暂停播放
		 */
		void cameraPause();

		/**
		 * 恢复播放
		 */
		void cameraResume();

		bool cameraIsPlaying();

	private:
		// void bindCameraPreviewQt6(QWidget* widget);
		// void bindCameraPreviewFFmpeg(QWidget* widget);

	private:
		fplayer::RunTime* m_runtime;
		std::shared_ptr<fplayer::ICamera> m_camera;
		int m_cameraIndex;// 摄像头索引
	};
}

#endif //FPLAYER_DESKETOP_SERVICE_H