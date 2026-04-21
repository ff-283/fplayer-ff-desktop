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
#include <QString>
#include <QtTypes>
#include <fplayer/service/export.h>
#include <fplayer/runtime/runtime.h>

class QWidget;

namespace fplayer
{
	class FPLAYER_SERVICE_EXPORT Service
	{
	public:
		struct PushOptions
		{
			int fps = 0;
			int width = 0;
			int height = 0;
			int bitrateKbps = 0;
			bool keepAspectRatio = false;
			QString audioInputSource = QStringLiteral("off");
			QString audioOutputSource = QStringLiteral("off");
			QString videoEncoder = QStringLiteral("auto");
		};

		enum class PushScene
		{
			Camera,
			File,
			Screen,
		};
		Service();

		~Service();

		void initCamera(MediaBackendType backend);
		void initPlayer(MediaBackendType backend);
		void initScreenCapture(MediaBackendType backend);
		void initStream(MediaBackendType backend);

		/**
		 * 初始化摄像头视频播放窗口
		 * @param videoView
		 */
		void bindCameraPreview(fplayer::IFVideoView* videoView);
		void bindPlayerPreview(fplayer::IFVideoView* videoView);
		void bindScreenPreview(fplayer::IFVideoView* videoView);

		bool openMediaFile(const QString& filePath);

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
		void cameraSetFrameBusSourceId(const QString& sourceId);
		QString cameraFrameBusSourceId() const;
		void playerPause();
		void playerResume();
		void playerStop();
		bool playerIsPlaying();
		qint64 playerDurationMs() const;
		qint64 playerPositionMs() const;
		bool playerSeekMs(qint64 positionMs);
		void playerSetPlaybackRate(double rate);
		double playerPlaybackRate() const;
		void playerSetVolume(float volume);
		float playerVolume() const;
		QString playerDebugStats() const;
		/** 文件播放 YUV 发布到 ScreenFrameBus 的通道（组合推流用）；传空停止发布。 */
		void setPlayerComposeStreamBusId(const QString& sourceId);
		QList<QString> getScreenList() const;
		bool selectScreen(int index);
		void screenSetActive(bool active);
		bool screenIsActive() const;
		bool screenSetCursorCaptureEnabled(bool enabled);
		bool screenCanControlCursorCapture() const;
		bool screenSetFrameRate(int fps);
		int screenFrameRate() const;
		bool screenCanControlFrameRate() const;
		void screenSetFrameBusSourceId(const QString& sourceId);
		QString screenFrameBusSourceId() const;
		MediaBackendType screenBackendType() const;
		bool streamStartPush(const QString& inputUrl, const QString& outputUrl);
		bool streamStartPushByScene(PushScene scene, const QString& outputUrl, const QString& sceneInput = QString());
		bool streamStartPushByScene(PushScene scene, const QString& outputUrl, const QString& sceneInput,
		                            const PushOptions& options);
		bool streamStartPull(const QString& inputUrl, const QString& outputUrl);
		void streamStop();
		bool streamIsRunning() const;
		QString streamLastError() const;
		QString streamRecentLog() const;
		int streamLastExitCode() const;
		QStringList streamAvailableVideoEncoders() const;
		bool streamHasCompletedSession() const;

	private:
		// void bindCameraPreviewQt6(QWidget* widget);
		// void bindCameraPreviewFFmpeg(QWidget* widget);

	private:
		fplayer::RunTime* m_runtime;
		std::shared_ptr<fplayer::ICamera> m_camera;
		std::shared_ptr<fplayer::IPlayer> m_player;
		std::shared_ptr<fplayer::IScreenCapture> m_screenCapture;
		std::shared_ptr<fplayer::IStream> m_stream;
		int m_cameraIndex;// 摄像头索引
		QString m_streamInitErrorHint;
	};
}

#endif //FPLAYER_DESKETOP_SERVICE_H