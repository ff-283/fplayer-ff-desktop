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
		// AI configuration
		QString aiEndpoint = QStringLiteral("https://api.openai.com/v1/chat/completions");
		QString aiApiKey;
		QString aiModel = QStringLiteral("gpt-4o");
		// AI chat appearance
		QString aiUserBubbleColor = QStringLiteral("#2563eb");
		QString aiAiBubbleColor = QStringLiteral("#ffffff");
		QString aiChatBgColor = QStringLiteral("#f3f4f6");
		QString imagePoolToolbarColor = QStringLiteral("#000000");
		QString aiFontFamily;
		int aiFontSize = 13;
		QString aiTextColor = QStringLiteral("#374151");
		QString userTextColor = QStringLiteral("#ffffff");
		QString aiSystemBubbleColor = QStringLiteral("#fef3c7");
		QString aiSystemTextColor = QStringLiteral("#92400e");
		QString aiSenderColor = QStringLiteral("#9ca3af");
		QString aiSystemSenderColor = QStringLiteral("#d97706");
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
