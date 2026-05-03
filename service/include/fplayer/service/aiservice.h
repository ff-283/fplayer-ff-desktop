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
		QString userBubbleColor = QStringLiteral("#2563eb");
		QString aiBubbleColor = QStringLiteral("#ffffff");
		QString chatBgColor = QStringLiteral("#f3f4f6");
		QString fontFamily;
		int fontSize = 13;
		QString aiTextColor = QStringLiteral("#374151");
		QString userTextColor = QStringLiteral("#ffffff");
	QString systemBubbleColor = QStringLiteral("#fef3c7");
	QString systemTextColor = QStringLiteral("#92400e");
	QString senderColor = QStringLiteral("#9ca3af");
	QString systemSenderColor = QStringLiteral("#d97706");
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
