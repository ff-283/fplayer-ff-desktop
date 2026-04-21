/*************************************************
  * 描述：推拉流抽象
  *
  * File：istream.h
  * ************************************************/
#ifndef FPLAYER_DESKETOP_ISTREAM_H
#define FPLAYER_DESKETOP_ISTREAM_H

#include <QString>
#include <QStringList>
#include <fplayer/api/export.h>

namespace fplayer
{
	class FPLAYER_API_EXPORT IStream
	{
	public:
		virtual ~IStream();
		virtual bool startPush(const QString& inputUrl, const QString& outputUrl) = 0;
		virtual bool startPull(const QString& inputUrl, const QString& outputUrl) = 0;
		virtual void stop() = 0;
		virtual bool isRunning() const = 0;
		virtual QString lastError() const = 0;
		virtual QString recentLog() const = 0;
		virtual QString recentPushLog() const { return recentLog(); }
		virtual QString recentPullLog() const { return recentLog(); }
		virtual int lastExitCode() const = 0;
		virtual QStringList availableVideoEncoders() const { return {QStringLiteral("cpu")}; }
		/// 是否存在“上一轮推拉流任务已结束”的结果（用于区分从未启动与已结束；未启动时不要展示 lastExitCode）。
		virtual bool hasCompletedStreamSession() const { return false; }
		virtual void setPreviewPaused(bool paused) { (void)paused; }
		virtual bool previewPaused() const { return false; }
		virtual void setPreviewVolume(float volume) { (void)volume; }
		virtual float previewVolume() const { return 1.0f; } // 1.0=100%, 2.0=200%
	};
}

#endif //FPLAYER_DESKETOP_ISTREAM_H
