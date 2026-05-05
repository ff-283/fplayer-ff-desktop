#ifndef FPLAYER_SERVICE_AISERVICE_H
#define FPLAYER_SERVICE_AISERVICE_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <fplayer/service/export.h>

namespace fplayer
{
	struct AiConfig
	{
		QString endpoint = QStringLiteral("https://api.openai.com/v1/chat/completions");
		QString apiKey;
		QString model = QStringLiteral("gpt-4o");
		QString userBubbleColor = QStringLiteral("#2997ff");
		QString aiBubbleColor = QStringLiteral("#1c1c1e");
		QString chatBgColor = QStringLiteral("#0d0d0f");
		QString fontFamily;
		int fontSize = 14;
		QString aiTextColor = QStringLiteral("#f5f5f7");
		QString userTextColor = QStringLiteral("#ffffff");
	QString systemBubbleColor = QStringLiteral("#1a1a1c");
	QString systemTextColor = QStringLiteral("#a1a1a6");
	QString senderColor = QStringLiteral("#6e6e73");
	QString systemSenderColor = QStringLiteral("#ff9f0a");
	};

	class FPLAYER_SERVICE_EXPORT AiService : public QObject
	{
		Q_OBJECT

	public:
		explicit AiService(QObject* parent = nullptr);
	~AiService() override;

		void setConfig(const AiConfig& config);
		AiConfig config() const;

		void sendMessage(const QString& imagePath, const QString& userMessage,
		                 const QString& systemPrompt = QString());

	signals:
		void responseChunk(const QString& chunk);
		void responseFinished();
		void requestFailed(const QString& error);

	private slots:
		void onReadyRead();
		void onReplyFinished(QNetworkReply* reply);

	private:
		QByteArray encodeImageBase64(const QString& imagePath) const;
		QNetworkAccessManager* m_net = nullptr;
		QNetworkReply* m_activeReply = nullptr;
		QByteArray m_streamBuffer;
		AiConfig m_config;
	};
}

#endif
