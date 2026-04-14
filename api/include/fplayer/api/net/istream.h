/*************************************************
  * 描述：推拉流抽象
  *
  * File：istream.h
  * ************************************************/
#ifndef FPLAYER_DESKETOP_ISTREAM_H
#define FPLAYER_DESKETOP_ISTREAM_H

#include <QString>
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
		virtual int lastExitCode() const = 0;
	};
}

#endif //FPLAYER_DESKETOP_ISTREAM_H
