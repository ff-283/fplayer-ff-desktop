#ifndef FPLAYER_WIDGET_AICHATDIALOG_H
#define FPLAYER_WIDGET_AICHATDIALOG_H

#include <QDialog>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <fplayer/widget/export.h>
#include <fplayer/service/aiservice.h>

class FPLAYER_WIDGET_EXPORT AiChatDialog : public QDialog
{
	Q_OBJECT

public:
	explicit AiChatDialog(QWidget* parent = nullptr);

	void startChat(const QString& imagePath, const fplayer::AiConfig& config);

private slots:
	void onSendClicked();
	void onResponseChunk(const QString& chunk);
	void onResponseFinished();
	void onRequestFailed(const QString& error);

private:
	void autoSend();
	void beginAiMessage();
	void appendToAiMessage(const QString& text);
	void finishAiMessage();
	void appendMessage(const QString& sender, const QString& text);
	void setInputEnabled(bool enabled);
	void renderChat();
	QString bubbleHtml(const QString& sender, const QString& text, bool isStreaming) const;

	fplayer::AiService* m_aiService = nullptr;
	QTextEdit* m_chatView = nullptr;
	QLineEdit* m_input = nullptr;
	QPushButton* m_btnSend = nullptr;
	QLabel* m_imageLabel = nullptr;
	QString m_imagePath;
	bool m_aiStreaming = false;
	fplayer::AiConfig m_colors;
	QString m_chatHistory;
	QString m_aiCurrentContent;
};

#endif
