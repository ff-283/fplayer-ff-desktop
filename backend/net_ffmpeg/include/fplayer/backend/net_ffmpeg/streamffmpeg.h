#ifndef FPLAYER_DESKETOP_STREAMFFMPEG_H
#define FPLAYER_DESKETOP_STREAMFFMPEG_H

#include <QObject>
#include <QProcess>
#include <fplayer/backend/net_ffmpeg/export.h>
#include <fplayer/api/net/istream.h>

namespace fplayer
{
	class FPLAYER_BACKEND_NET_FFMPEG_EXPORT StreamFFmpeg : public QObject, public IStream
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

	private:
		bool startWithArgs(const QStringList& args);
		QString resolveFfmpegProgram() const;

	private:
		QProcess* m_ffmpegProcess = nullptr;
		QString m_lastError;
		QString m_recentLog;
		int m_lastExitCode = 0;
	};
}

#endif //FPLAYER_DESKETOP_STREAMFFMPEG_H
