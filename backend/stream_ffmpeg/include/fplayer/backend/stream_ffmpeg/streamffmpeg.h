#ifndef FPLAYER_DESKETOP_STREAMFFMPEG_H
#define FPLAYER_DESKETOP_STREAMFFMPEG_H

#include <QMutex>
#include <QObject>
#include <atomic>
#include <memory>
#include <thread>
#include <fplayer/backend/stream_ffmpeg/export.h>
#include <fplayer/api/net/istream.h>

namespace fplayer::streamffmpeg_helpers
{
	struct PushInputRoute;
}

namespace fplayer
{
	class FPLAYER_BACKEND_STREAM_FFMPEG_EXPORT StreamFFmpeg : public QObject, public IStream
	{
	public:
		explicit StreamFFmpeg(QObject* parent = nullptr);
		~StreamFFmpeg() override;

		bool startPush(const QString& inputUrl, const QString& outputUrl) override;
		bool startPull(const QString& inputUrl, const QString& outputUrl) override;
		void stop() override;
		bool isRunning() const override;
		QString lastError() const override;
		QString recentLog() const override;
		int lastExitCode() const override;
		QStringList availableVideoEncoders() const override;
		bool hasCompletedStreamSession() const override;
		void setPreviewPaused(bool paused) override;
		bool previewPaused() const override;
		void setPreviewVolume(float volume) override;
		float previewVolume() const override;

	private:
		/// 使用 libav* 转封装（流拷贝），\p outputShortName 非空时传给 avformat_alloc_output_context2。
		void remuxLoop(const QString& inputUrl, const QString& outputUrl, const char* outputShortName);
		/// 文件转码后推流（用于可调码率/尺寸/帧率）。
		void transcodeFileLoop(const QString& outputUrl, const QString& transcodeSpec);
		/// 采集桌面并编码后推流（不依赖播放器输入源）。
		void pushScreenLoop(const QString& outputUrl, const QString& captureSpec);
		/// 使用当前屏幕预览帧编码推流（DXGI 预览链路）。
		void pushScreenPreviewLoop(const QString& outputUrl, const QString& captureSpec);
		/// 采集摄像头并编码后推流（不依赖前端渲染链）。
		void pushCameraLoop(const QString& outputUrl, const QString& captureSpec);
		/// 使用当前摄像头预览帧编码推流，避免设备二次占用。
		void pushCameraPreviewLoop(const QString& outputUrl, const QString& captureSpec);
		/// 组合场景：按布局合成预览帧后推流（最小可用版，先支持摄像头/屏幕）。
		void pushComposeSceneLoop(const QString& outputUrl, const QString& sceneSpec);

		bool startPushWorkerByRoute(const streamffmpeg_helpers::PushInputRoute& route, const QString& inputUrl, const QString& outputUrl);
		bool startPullWorker(const QString& inputUrl, const QString& outputUrl);
		void appendLogLine(const QString& line);
		void setLastError(const QString& error);
		static int interruptCallback(void* opaque);

	private:
		mutable QMutex m_mutex;
		std::unique_ptr<std::thread> m_worker;
		std::atomic<bool> m_stopRequest{false};
		std::atomic<bool> m_running{false};
		QString m_lastError;
		QString m_recentLog;
		int m_lastExitCode = 0;
		std::atomic<bool> m_completedSession{false};
		std::atomic<bool> m_previewPaused{false};
		std::atomic<float> m_previewVolume{1.0f};
	};
}

#endif //FPLAYER_DESKETOP_STREAMFFMPEG_H
