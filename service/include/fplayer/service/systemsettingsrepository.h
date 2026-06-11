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
		QString filePlaybackBackend;  // "qt6" (default) or "ffmpeg"
		bool closeToTrayOnClose = true;
		bool composeDragUseRubberBand = false;
		// AI configuration
		QString aiEndpoint = QStringLiteral("https://api.openai.com/v1/chat/completions");
		QString aiApiKey;
		QString aiModel = QStringLiteral("gpt-4o");
		// AI chat appearance
		QString aiUserBubbleColor = QStringLiteral("#2997ff");
		QString aiAiBubbleColor = QStringLiteral("#1c1c1e");
		QString aiChatBgColor = QStringLiteral("#0d0d0f");
		QString aiFontFamily;
		int aiFontSize = 14;
		QString aiTextColor = QStringLiteral("#f5f5f7");
		QString userTextColor = QStringLiteral("#ffffff");
		QString aiSystemBubbleColor = QStringLiteral("#1a1a1c");
		QString aiSystemTextColor = QStringLiteral("#a1a1a6");
		QString aiSenderColor = QStringLiteral("#6e6e73");
		QString aiSystemSenderColor = QStringLiteral("#ff9f0a");
		int theme = 0;   // 0=dark, 1=light
		QString accentColor = QStringLiteral("#2997ff");   // 主题色（蓝色默认）
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
