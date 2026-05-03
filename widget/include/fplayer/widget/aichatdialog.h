#ifndef FPLAYER_WIDGET_AICHATDIALOG_H
#define FPLAYER_WIDGET_AICHATDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QTimer>
#include <fplayer/widget/export.h>
#include <fplayer/widget/chatbubblewidget.h>
#include <fplayer/service/aiservice.h>

class FPLAYER_WIDGET_EXPORT AiChatDialog : public QDialog
{
	Q_OBJECT

public:
	explicit AiChatDialog(QWidget* parent = nullptr);

	void startChat(const QString& imagePath, const fplayer::AiConfig& config);
	void reconfigure(const fplayer::AiConfig& config);

private slots:
	void onSendClicked();
	void onResponseChunk(const QString& chunk);
	void onResponseFinished();
	void onRequestFailed(const QString& error);
	void onTypewriterTick();

private:
	void autoSend();
	void beginAiMessage();
	void appendToAiMessage(const QString& text);
	void finishAiMessage();
	void appendBubble(ChatBubbleWidget::SenderType type, const QString& sender, const QString& text);
	void setInputEnabled(bool enabled);
	void clearChat();
	void scrollToBottom();
	void applyContentSize();
	void updateAllMaxWidths();
	bool eventFilter(QObject* obj, QEvent* event) override;
	void closeEvent(QCloseEvent* event) override;

	fplayer::AiService* m_aiService = nullptr;
	QScrollArea* m_chatScroll = nullptr;
	QWidget* m_chatContent = nullptr;
	QVBoxLayout* m_chatLayout = nullptr;
	QLineEdit* m_input = nullptr;
	QPushButton* m_btnSend = nullptr;
	QLabel* m_imageLabel = nullptr;
	QString m_imagePath;
	bool m_aiStreaming = false;
	fplayer::AiConfig m_colors;
	QVector<ChatBubbleWidget*> m_bubbles;
	ChatBubbleWidget* m_streamingBubble = nullptr;
	QTimer* m_typewriterTimer = nullptr;
	QString m_pendingChars;
	QString m_aiCurrentContent;
	bool m_responseComplete = false;
};

#endif
