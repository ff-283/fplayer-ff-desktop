#ifndef FPLAYER_DESKTOP_SYSTEMSETTINGSREPOSITORY_H
#define FPLAYER_DESKTOP_SYSTEMSETTINGSREPOSITORY_H

#include <QString>
#include <QStringList>

namespace fplayer
{
	struct SystemSettings
	{
		QString screenshotDir;
		QString recordDir;
		QStringList recentPushInputs;
		QStringList recentPushOutputs;
		QStringList recentPullInputs;
		QStringList recentPullOutputs;
		QString pushGateway;
		QString pushServiceApp;
		QString pushServiceStream;
		QString pullGateway;
		QString pullServiceApp;
		QString pullServiceStream;
		QString pushRouteMode;
		QString pushServiceMode;
		QString pushProtocolTemplate;
		int pushFps = 0;
		QString pushSize;
		int pushBitrateKbps = 0;
		QString pushEncoder;
		QString pushAudioInput;
		QString pushAudioOutput;
		bool pushKeepAspect = true;
		QString composeOutputSize;
		QString screenCaptureBackend;
		bool closeToTrayOnClose = true;
	};

	class SystemSettingsRepository
	{
	public:
		explicit SystemSettingsRepository(QString filePath);
		bool load(SystemSettings& data) const;
		bool save(const SystemSettings& data) const;
		static void addRecent(QStringList& list, const QString& value, int maxItems = 8);
		QString filePath() const;

	private:
		QString m_filePath;
	};
}

#endif
