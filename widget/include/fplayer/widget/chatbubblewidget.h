#ifndef FPLAYER_WIDGET_CHATBUBBLEWIDGET_H
#define FPLAYER_WIDGET_CHATBUBBLEWIDGET_H

#include <QWidget>
#include <QTextDocument>
#include <fplayer/service/aiservice.h>

class ChatBubbleWidget : public QWidget
{
	Q_OBJECT

public:
	enum SenderType { User, AI, System };

	explicit ChatBubbleWidget(SenderType type, const QString& senderName,
	                          const QString& text, const fplayer::AiConfig& colors,
	                          QWidget* parent = nullptr);

	void setText(const QString& text);
	void setStreaming(bool streaming);
	void setColors(const fplayer::AiConfig& colors);
	void setMaxBubbleWidth(int width);

	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent* event) override;

private:
	void recalculateLayout();

	SenderType m_type;
	QString m_senderName;
	QString m_text;
	fplayer::AiConfig m_colors;
	bool m_streaming = false;
	int m_maxBubbleWidth = 400;

	// Cached layout
	QRect m_senderRect;
	QRect m_bubbleRect;
	QRect m_textRect;
	int m_widgetWidth = 0;
	int m_widgetHeight = 0;

	QTextDocument* m_doc = nullptr;
};

#endif
