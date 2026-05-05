#include <fplayer/service/aiservice.h>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QImageReader>
#include <QBuffer>
#include <QNetworkRequest>
#include <QDebug>

namespace fplayer
{
	AiService::AiService(QObject* parent)
		: QObject(parent)
		, m_net(new QNetworkAccessManager(this))
	{
		connect(m_net, &QNetworkAccessManager::finished,
		        this, &AiService::onReplyFinished);
	}

	AiService::~AiService()
	{
		if (m_activeReply)
		{
			disconnect(m_activeReply, nullptr, this, nullptr);
			m_activeReply->abort();
			m_activeReply = nullptr;
		}
	}

	void AiService::setConfig(const AiConfig& config)
	{
		m_config = config;
	}

	AiConfig AiService::config() const
	{
		return m_config;
	}

	void AiService::sendMessage(const QString& imagePath, const QString& userMessage, const QString& systemPrompt)
	{
		if (m_config.apiKey.isEmpty())
		{
			emit requestFailed(tr("未配置 API Key，请在系统设置中填写。"));
			return;
		}
		const QByteArray imageB64 = encodeImageBase64(imagePath);
		if (imageB64.isEmpty())
		{
			emit requestFailed(tr("无法读取图片文件。"));
			return;
		}

		QJsonArray messages;
		if (!systemPrompt.isEmpty())
		{
			QJsonObject sysMsg;
			sysMsg["role"] = QStringLiteral("system");
			sysMsg["content"] = systemPrompt;
			messages.append(sysMsg);
		}

		QJsonObject userMsg;
		userMsg["role"] = QStringLiteral("user");
		QJsonArray content;

		QJsonObject imgPart;
		imgPart["type"] = QStringLiteral("image_url");
		QJsonObject imgUrl;
		imgUrl["url"] = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(imageB64);
		imgPart["image_url"] = imgUrl;
		content.append(imgPart);

		QJsonObject textPart;
		textPart["type"] = QStringLiteral("text");
		textPart["text"] = userMessage;
		content.append(textPart);

		userMsg["content"] = content;
		messages.append(userMsg);

		QJsonObject body;
		body["model"] = m_config.model;
		body["messages"] = messages;
		body["max_tokens"] = 1000;
		body["stream"] = true;

		QNetworkRequest req(QUrl(m_config.endpoint));
		req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
		req.setRawHeader("Authorization", ("Bearer " + m_config.apiKey).toUtf8());
		req.setRawHeader("Accept", "text/event-stream");

		m_streamBuffer.clear();
		qDebug() << "[AiService] sendMessage endpoint:" << m_config.endpoint
		         << "model:" << m_config.model << "image:" << imagePath;
		m_activeReply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
		if (m_activeReply)
		{
			qDebug() << "[AiService] request posted, waiting for stream...";
			connect(m_activeReply, &QNetworkReply::readyRead,
			        this, &AiService::onReadyRead);
		}
		else
		{
			qWarning() << "[AiService] post() returned nullptr!";
		}
	}

	void AiService::onReadyRead()
	{
		auto* reply = qobject_cast<QNetworkReply*>(sender());
		if (!reply)
			return;

		QByteArray raw = reply->readAll();
		qDebug() << "[AiService] onReadyRead received" << raw.size() << "bytes";
		m_streamBuffer.append(raw);

		// Parse complete SSE lines from buffer
		while (true)
		{
			const int lineEnd = m_streamBuffer.indexOf('\n');
			if (lineEnd < 0)
				break;

			QByteArray line = m_streamBuffer.left(lineEnd).trimmed();
			m_streamBuffer.remove(0, lineEnd + 1);

			if (line.isEmpty() || !line.startsWith("data: "))
				continue;

			QByteArray data = line.mid(6); // skip "data: "

			if (data == "[DONE]")
				continue;

			QJsonParseError err;
			const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
			if (err.error != QJsonParseError::NoError)
				continue;

			const QJsonObject obj = doc.object();
			const QJsonArray choices = obj["choices"].toArray();
			if (choices.isEmpty())
				continue;

			const QString chunk = choices[0].toObject()["delta"].toObject()["content"].toString();
			if (!chunk.isEmpty())
			{
				qDebug() << "[AiService] emit responseChunk:" << chunk;
				emit responseChunk(chunk);
			}
		}
	}

	void AiService::onReplyFinished(QNetworkReply* reply)
	{
		if (reply == m_activeReply)
			m_activeReply = nullptr;

		// Read any remaining data not yet consumed by onReadyRead
		const QByteArray remaining = reply->readAll();
		if (!remaining.isEmpty())
		{
			m_streamBuffer.append(remaining);
			qDebug() << "[AiService] late body:" << remaining;
		}
		// Log full buffer contents when error (it's the error response JSON)
		if (reply->error() != QNetworkReply::NoError && !m_streamBuffer.isEmpty())
		{
			qDebug() << "[AiService] error buffer:" << m_streamBuffer;
		}

		qDebug() << "[AiService] onReplyFinished error:" << reply->error()
		         << "status:" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
		         << "streamBuffer size:" << m_streamBuffer.size();
		if (reply->error() != QNetworkReply::NoError)
		{
			const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

			// Try to extract error message from response body
			QString errorMsg;
			if (!m_streamBuffer.isEmpty())
			{
				const QJsonDocument doc = QJsonDocument::fromJson(m_streamBuffer);
				if (!doc.isNull() && doc.isObject())
				{
					const QJsonObject obj = doc.object();
					const QJsonObject errObj = obj["error"].toObject();
					if (!errObj.isEmpty())
					{
						errorMsg = errObj["message"].toString();
						if (errorMsg.isEmpty())
							errorMsg = QString::fromUtf8(m_streamBuffer);
					}
				}
			}

			if (status == 401)
				emit requestFailed(tr("API Key 无效 (401)，请检查设置。"));
			else if (status == 429)
				emit requestFailed(tr("请求过于频繁 (429)，请稍后重试。"));
			else if (!errorMsg.isEmpty())
				emit requestFailed(tr("API 错误 (HTTP %1)：%2").arg(status).arg(errorMsg));
			else
				emit requestFailed(tr("请求失败 (HTTP %1)：%2").arg(status).arg(reply->errorString()));
		}
		else
		{
			// May have residual data not yet processed by onReadyRead
			onReadyRead();
			if (!m_streamBuffer.isEmpty())
			{
				emit responseChunk(QString::fromUtf8(m_streamBuffer));
				m_streamBuffer.clear();
			}
			emit responseFinished();
		}
		reply->deleteLater();
	}

	QByteArray AiService::encodeImageBase64(const QString& imagePath) const
	{
		QImageReader reader(imagePath);
		reader.setAutoTransform(true);
		QImage img = reader.read();
		if (img.isNull())
			return {};
		if (img.width() > 2048 || img.height() > 2048)
			img = img.scaled(2048, 2048, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		QByteArray ba;
		QBuffer buf(&ba);
		buf.open(QIODevice::WriteOnly);
		img.save(&buf, "PNG");
		return ba.toBase64();
	}
}
