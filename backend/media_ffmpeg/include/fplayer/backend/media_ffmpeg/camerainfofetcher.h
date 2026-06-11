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

	public:
		static QList<CameraDescription> getDescriptions();
		static void forceRefresh();

	private:
		CameraDescriptionFetcher(const CameraDescriptionFetcher&) = delete;
		CameraDescriptionFetcher& operator=(const CameraDescriptionFetcher&) = delete;

		CameraDescriptionFetcher();
		~CameraDescriptionFetcher() = default;

		static CameraDescriptionFetcher& instance();
		QList<CameraDescription> enumerateDescriptions();

	private:
		static QVector<QList<FCameraFormat>> m_cameraFormats;
		qint64 m_lastEnumerateMs = 0;
		QList<CameraDescription> m_cachedDescriptions;
		static constexpr qint64 kCacheTtlMs = 30000;
	};
}

#endif //FPLAYER_DESKETOP_CAMERAINFOFETCHER_H