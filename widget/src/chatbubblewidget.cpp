#include <fplayer/widget/chatbubblewidget.h>
#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QAbstractTextDocumentLayout>

ChatBubbleWidget::ChatBubbleWidget(SenderType type, const QString& senderName,
                                   const QString& text, const fplayer::AiConfig& colors,
                                   QWidget* parent)
	: QWidget(parent)
	, m_type(type)
	, m_senderName(senderName)
	, m_text(text)
	, m_colors(colors)
	, m_doc(new QTextDocument(this))
{
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	m_doc->setUndoRedoEnabled(false);
	recalculateLayout();
}

void ChatBubbleWidget::setText(const QString& text)
{
	if (m_text == text)
		return;
	m_text = text;
	recalculateLayout();
	updateGeometry();
	update();
}

void ChatBubbleWidget::setStreaming(bool streaming)
{
	if (m_streaming == streaming)
		return;
	m_streaming = streaming;
	update();
}

void ChatBubbleWidget::setColors(const fplayer::AiConfig& colors)
{
	m_colors = colors;
	recalculateLayout();
	updateGeometry();
	update();
}

void ChatBubbleWidget::setMaxBubbleWidth(int width)
{
	if (width < 100)
		width = 100;
	if (m_maxBubbleWidth == width)
		return;
	m_maxBubbleWidth = width;
	recalculateLayout();
	updateGeometry();
	update();
}

QSize ChatBubbleWidget::sizeHint() const
{
	return QSize(m_widgetWidth, m_widgetHeight);
}

// ── Helpers ──

static QColor parseColor(const QString& hex)
{
	QColor c(hex);
	return c.isValid() ? c : QColor(Qt::white);
}

static QPainterPath roundedRectPath(const QRectF& r, qreal tl, qreal tr, qreal br, qreal bl)
{
	QPainterPath path;
	qreal x = r.x(), y = r.y(), w = r.width(), h = r.height();

	path.moveTo(x + tl, y);
	path.lineTo(x + w - tr, y);
	path.arcTo(x + w - 2 * tr, y, 2 * tr, 2 * tr, 90, -90);
	path.lineTo(x + w, y + h - br);
	path.arcTo(x + w - 2 * br, y + h - 2 * br, 2 * br, 2 * br, 0, -90);
	path.lineTo(x + bl, y + h);
	path.arcTo(x, y + h - 2 * bl, 2 * bl, 2 * bl, 270, -90);
	path.lineTo(x, y + tl);
	path.arcTo(x, y, 2 * tl, 2 * tl, 180, -90);
	path.closeSubpath();

	return path;
}

// ── Layout calculation ──

void ChatBubbleWidget::recalculateLayout()
{
	const int paddingH = 12;
	const int paddingV = 8;
	const int senderGap = 2;

	const QFont senderFont(m_colors.fontFamily, qMax(8, m_colors.fontSize - 2));
	const QFontMetrics senderFm(senderFont);

	const int senderNameWidth = senderFm.horizontalAdvance(m_senderName);
	const int senderNameHeight = senderFm.height();

	// Text color (needed for CSS before measurement)
	QColor textColor;
	if (m_type == User)
		textColor = parseColor(m_colors.userTextColor);
	else if (m_type == System)
		textColor = parseColor(m_colors.systemTextColor);
	else
		textColor = parseColor(m_colors.aiTextColor);

	// Build display text (streaming cursor)
	QString displayText = m_text;
	if (m_streaming && m_text.isEmpty())
		displayText = QStringLiteral("● ● ●");
	else if (m_streaming)
		displayText += QStringLiteral(" ▌");

	// Apply CSS for font, then Markdown, then explicit text color via QTextCursor
	const QString fontFamilyCss = m_colors.fontFamily.isEmpty()
		? QStringLiteral("sans-serif")
		: m_colors.fontFamily;
	m_doc->setDefaultStyleSheet(
		QStringLiteral("body { font-family: '%1'; font-size: %2px; }")
			.arg(fontFamilyCss).arg(m_colors.fontSize));
	m_doc->setMarkdown(displayText);

	{
		QTextCursor cursor(m_doc);
		cursor.select(QTextCursor::Document);
		QTextCharFormat fmt;
		fmt.setForeground(textColor);
		cursor.mergeCharFormat(fmt);
	}

	const int maxTextWidth = m_maxBubbleWidth - 2 * paddingH;
	m_doc->setTextWidth(maxTextWidth);

	const QSizeF docSize = m_doc->size();
	const qreal idealW = m_doc->idealWidth();

	const int textWidth = (idealW <= maxTextWidth)
		? qMax(20, (int)idealW)
		: maxTextWidth;
	const int textHeight = qMax((int)docSize.height(), senderFm.height());

	// Bubble size
	const int bubbleWidth = textWidth + 2 * paddingH;
	const int bubbleHeight = textHeight + 2 * paddingV;

	m_widgetWidth = qMax(20, qMax(bubbleWidth, senderNameWidth + 2 * 8));
	m_widgetHeight = qMax(20, senderNameHeight + senderGap + bubbleHeight);

	// Sender name rect
	{
		int sx, sy = 0;
		if (m_type == User)
			sx = m_widgetWidth - senderNameWidth - 8;
		else if (m_type == AI)
			sx = 8;
		else
			sx = (m_widgetWidth - senderNameWidth) / 2;
		m_senderRect = QRect(sx, sy, senderNameWidth, senderNameHeight);
	}

	// Bubble rect
	{
		int bx, by = senderNameHeight + senderGap;
		if (m_type == User)
			bx = m_widgetWidth - bubbleWidth;
		else if (m_type == AI)
			bx = 0;
		else
			bx = (m_widgetWidth - bubbleWidth) / 2;
		m_bubbleRect = QRect(bx, by, bubbleWidth, bubbleHeight);
	}

	m_textRect = QRect(m_bubbleRect.x() + paddingH,
	                   m_bubbleRect.y() + paddingV,
	                   bubbleWidth - 2 * paddingH,
	                   textHeight);
}

// ── Paint ──

void ChatBubbleWidget::paintEvent(QPaintEvent* /*event*/)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);

	// Sender name
	const QFont senderFont(m_colors.fontFamily, qMax(8, m_colors.fontSize - 2));
	p.setFont(senderFont);

	const QColor senderColor = (m_type == System)
		? parseColor(m_colors.systemSenderColor)
		: parseColor(m_colors.senderColor);

	p.setPen(senderColor);
	p.drawText(m_senderRect, Qt::AlignLeft | Qt::AlignVCenter, m_senderName);

	// Bubble colors
	QColor bubbleBg;
	if (m_type == User)
		bubbleBg = parseColor(m_colors.userBubbleColor);
	else if (m_type == System)
		bubbleBg = parseColor(m_colors.systemBubbleColor);
	else
		bubbleBg = parseColor(m_colors.aiBubbleColor);

	// Corner radii
	qreal tl, tr, br, bl;
	if (m_type == User)      { tl = 12; tr = 4;  br = 12; bl = 12; }
	else if (m_type == AI)   { tl = 4;  tr = 12; br = 12; bl = 12; }
	else                     { tl = tr = br = bl = 8; }

	QPainterPath bubblePath = roundedRectPath(m_bubbleRect, tl, tr, br, bl);
	p.setPen(Qt::NoPen);
	p.setBrush(bubbleBg);
	p.drawPath(bubblePath);

	// Text color (must match recalculateLayout)
	QColor textColor;
	if (m_type == User)
		textColor = parseColor(m_colors.userTextColor);
	else if (m_type == System)
		textColor = parseColor(m_colors.systemTextColor);
	else
		textColor = parseColor(m_colors.aiTextColor);

	// Markdown content via QTextDocument
	m_doc->setTextWidth(m_textRect.width());

	QAbstractTextDocumentLayout::PaintContext ctx;
	ctx.palette.setColor(QPalette::Text, textColor);

	p.save();
	p.setClipRect(m_textRect);
	p.translate(m_textRect.topLeft());
	m_doc->documentLayout()->draw(&p, ctx);
	p.restore();
}
