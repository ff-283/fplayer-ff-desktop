/*************************************************
  * 描述：ffmpeg下摄像头信息获取类 单例
  *
  * File：camerainfofetcher.h
  * Date：2026/3/6
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_CAMERAINFOFETCHER_H
#define FPLAYER_DESKETOP_CAMERAINFOFETCHER_H
#include <QVector>
#include <QList>
#include <fplayer/api/media/icamera.h>
#include <fplayer/backend/media_ffmpeg/export.h>

namespace fplayer
{
	class FPLAYER_BACKEND_MEDIA_FFMPEG_EXPORT CameraDescriptionFetcher
	{
	public:
		struct FCameraFormat
		{
			int height;
			int width;
			int fps;
		};

		// TODO：后面有可能/建议在线程中调用这个单例类及相关方法
	public:
		static QList<CameraDescription> getDescriptions();

	private:
		CameraDescriptionFetcher(const CameraDescriptionFetcher&) = delete;
		CameraDescriptionFetcher& operator=(const CameraDescriptionFetcher&) = delete;

		CameraDescriptionFetcher();
		~CameraDescriptionFetcher() = default;

		static CameraDescriptionFetcher& instance();

	private:
		static QVector<QList<FCameraFormat>> m_cameraFormats;// 用于存储各个摄像头的相对原始格式信息
	};
}

#endif //FPLAYER_DESKETOP_CAMERAINFOFETCHER_H