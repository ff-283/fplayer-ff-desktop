#include <fplayer/widget/aichatdialog.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QFileInfo>
#include <QScrollBar>

AiChatDialog::AiChatDialog(QWidget* parent)
	: QDialog(parent)
	, m_aiService(new fplayer::AiService(this))
{
	setWindowTitle(tr("AI 图片分析"));
	resize(800, 600);
	setMinimumSize(600, 400);

	auto* root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);

	auto* splitter = new QSplitter(Qt::Horizontal, this);

	auto* imgPanel = new QWidget(splitter);
	auto* imgLayout = new QVBoxLayout(imgPanel);
	imgLayout->setContentsMargins(8, 8, 8, 8);
	m_imageLabel = new QLabel(imgPanel);
	m_imageLabel->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
	m_imageLabel->setStyleSheet(QStringLiteral("QLabel { background-color: #1a1a1a; border-radius: 4px; }"));
	imgLayout->addWidget(m_imageLabel);
	imgLayout->addStretch();

	auto* chatPanel = new QWidget(splitter);
	auto* chatLayout = new QVBoxLayout(chatPanel);
	chatLayout->setContentsMargins(8, 8, 8, 8);

	m_chatView = new QTextEdit(chatPanel);
	m_chatView->setReadOnly(true);
	m_chatView->setStyleSheet(QStringLiteral(
		"QTextEdit { background-color: #000000; border: 1px solid #404040; border-radius: 4px; font-size: 13px; }"));
	chatLayout->addWidget(m_chatView, 1);

	auto* inputRow = new QWidget(chatPanel);
	auto* inputLayout = new QHBoxLayout(inputRow);
	inputLayout->setContentsMargins(0, 4, 0, 0);
	inputLayout->setSpacing(6);

	m_input = new QLineEdit(inputRow);
	m_input->setPlaceholderText(tr("输入你的问题..."));
	m_input->setStyleSheet(QStringLiteral(
		"QLineEdit { border: 1px solid #ccc; border-radius: 4px; padding: 6px; font-size: 13px; }"));
	m_btnSend = new QPushButton(tr("发送"), inputRow);
	m_btnSend->setFixedSize(60, 32);

	inputLayout->addWidget(m_input, 1);
	inputLayout->addWidget(m_btnSend, 0);
	chatLayout->addWidget(inputRow, 0);

	splitter->addWidget(imgPanel);
	splitter->addWidget(chatPanel);
	splitter->setStretchFactor(0, 1);
	splitter->setStretchFactor(1, 2);

	root->addWidget(splitter, 1);

	connect(m_btnSend, &QPushButton::clicked, this, &AiChatDialog::onSendClicked);
	connect(m_input, &QLineEdit::returnPressed, this, &AiChatDialog::onSendClicked);
	connect(m_aiService, &fplayer::AiService::responseChunk,
	        this, &AiChatDialog::onResponseChunk);
	connect(m_aiService, &fplayer::AiService::responseFinished,
	        this, &AiChatDialog::onResponseFinished);
	connect(m_aiService, &fplayer::AiService::requestFailed,
	        this, &AiChatDialog::onRequestFailed);
}

void AiChatDialog::startChat(const QString& imagePath, const fplayer::AiConfig& config)
{
	m_imagePath = imagePath;
	m_aiService->setConfig(config);
	m_colors = config;
	m_chatHistory.clear();
	m_chatView->setStyleSheet(QStringLiteral("QTextEdit { background-color: %1; border: 1px solid #d0d0d0; border-radius: 4px; font-size: 13px; }").arg(m_colors.chatBgColor));

	QPixmap pix(imagePath);
	if (!pix.isNull())
	{
		QPixmap scaled = pix.scaled(260, 400, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		m_imageLabel->setPixmap(scaled);
	}
	else
	{
		m_imageLabel->setText(tr("无法加载图片"));
	}
	const QString name = QFileInfo(imagePath).fileName();
	setWindowTitle(tr("AI 图片分析 - %1").arg(name));

	appendMessage(tr("AI 助手"), tr("关于这张图片，我能为你做些什么？"));

	show();
	raise();
	activateWindow();

	// Auto-send initial prompt once
	QTimer::singleShot(200, this, &AiChatDialog::autoSend);
}

void AiChatDialog::autoSend()
{
	if (m_imagePath.isEmpty() || m_aiStreaming)
		return;
	m_input->setText(QString());
	setInputEnabled(false);
	appendMessage(tr("我"), tr("请分析一下图片内容"));
	beginAiMessage();
	m_aiService->sendMessage(m_imagePath, tr("请分析一下图片内容"),
	                         tr("你是一个有帮助的AI助手。请用中文回答用户的问题。"));
}

void AiChatDialog::onSendClicked()
{
	const QString text = m_input->text().trimmed();
	if (text.isEmpty() || m_imagePath.isEmpty() || m_aiStreaming)
		return;

	m_input->clear();
	setInputEnabled(false);
	appendMessage(tr("我"), text);
	beginAiMessage();
	m_aiService->sendMessage(m_imagePath, text,
	                         tr("你是一个有帮助的AI助手。请用中文回答用户的问题。"));
}

void AiChatDialog::onResponseChunk(const QString& chunk)
{
	if (!m_aiStreaming)
		beginAiMessage();
	appendToAiMessage(chunk);
}

void AiChatDialog::onResponseFinished()
{
	finishAiMessage();
	setInputEnabled(true);
	m_input->setFocus();
}

void AiChatDialog::onRequestFailed(const QString& error)
{
	if (m_aiStreaming)
		finishAiMessage();
	setInputEnabled(true);
	appendMessage(tr("系统"), tr("[错误] %1").arg(error));
	m_input->setFocus();
}

// ── Streaming bubble management ──

void AiChatDialog::beginAiMessage()
{
	m_aiStreaming = true;
	m_aiCurrentContent.clear();
	// Append placeholder bubble to history
	m_chatHistory += bubbleHtml(tr("AI 助手"), tr("● ● ●"), true);
	renderChat();
}

void AiChatDialog::appendToAiMessage(const QString& text)
{
	m_aiCurrentContent += text;
	// Strip the last bubble from history (the streaming AI message)
	int lastBubble = m_chatHistory.lastIndexOf(QStringLiteral("<table "));
	if (lastBubble >= 0)
		m_chatHistory = m_chatHistory.left(lastBubble);
	// Re-append with accumulated content
	m_chatHistory += bubbleHtml(tr("AI 助手"), m_aiCurrentContent, true);
	renderChat();
}

void AiChatDialog::finishAiMessage()
{
	if (!m_aiStreaming)
		return;
	m_aiStreaming = false;
	// Strip the last bubble and re-append as finalized
	int lastBubble = m_chatHistory.lastIndexOf(QStringLiteral("<table "));
	if (lastBubble >= 0)
		m_chatHistory = m_chatHistory.left(lastBubble);
	m_chatHistory += bubbleHtml(tr("AI 助手"), m_aiCurrentContent, false);
	renderChat();
	m_aiCurrentContent.clear();
}

// ── Static message bubble ──

void AiChatDialog::appendMessage(const QString& sender, const QString& text)
{
	m_chatHistory += bubbleHtml(sender, text, false);
	renderChat();
}

// ── HTML rendering ──

QString AiChatDialog::bubbleHtml(const QString& sender, const QString& text, bool isStreaming) const
{
	const bool isUser = (sender == tr("我"));
	const bool isSystem = (sender == tr("系统"));

	QString align, bubbleStyle, senderColor, displayText;

	if (isUser)
	{
		align = QStringLiteral("right");
		bubbleStyle = QStringLiteral("background:%1;color:#ffffff;border-radius:12px 4px 12px 12px;").arg(m_colors.userBubbleColor);
		senderColor = QStringLiteral("#0078d4");
		displayText = text.toHtmlEscaped().replace(QStringLiteral("\n"), QStringLiteral("<br>"));
	}
	else if (isSystem)
	{
		align = QStringLiteral("left");
		bubbleStyle = QStringLiteral(
			"background:#fff3cd;color:#856404;border:1px solid #ffc107;border-radius:4px 12px 12px 12px;");
		senderColor = QStringLiteral("#856404");
		displayText = text.toHtmlEscaped().replace(QStringLiteral("\n"), QStringLiteral("<br>"));
	}
	else
	{
		align = QStringLiteral("left");
		bubbleStyle = QStringLiteral(
			"background:%1;color:%2;border:1px solid #dcdcdc;border-radius:4px 12px 12px 12px;").arg(m_colors.aiBubbleColor, QStringLiteral("#1a1a1a"));
		senderColor = QStringLiteral("#555555");
		if (isStreaming)
		{
			if (text == tr("● ● ●"))
				displayText = QStringLiteral("<span style=\"color:#999;\">● ● ●</span>");
			else
				displayText = text.toHtmlEscaped().replace(QStringLiteral("\n"), QStringLiteral("<br>"))
				              + QStringLiteral("<span style=\"color:#999;\">&nbsp;▌</span>");
		}
		else
		{
			displayText = text.toHtmlEscaped().replace(QStringLiteral("\n"), QStringLiteral("<br>"));
		}
	}

	return QStringLiteral(
		"<table width=\"100%%\" cellpadding=\"0\" cellspacing=\"0\" style=\"margin:6px 0;\">"
		"<tr><td align=\"%1\" style=\"padding:0 8px;\">"
		"<span style=\"font-size:11px;color:%2;\">%3</span>"
		"</td></tr>"
		"<tr><td align=\"%1\" style=\"padding:2px 8px 0 8px;\">"
		"<div style=\"display:inline-block;max-width:78%%;%4padding:8px 12px;font-size:13px;text-align:left;word-wrap:break-word;\">"
		"%5"
		"</div>"
		"</td></tr>"
		"</table>")
		.arg(align, senderColor, sender.toHtmlEscaped(), bubbleStyle, displayText);
}

void AiChatDialog::renderChat()
{
	// Preserve scroll position
	QScrollBar* sb = m_chatView->verticalScrollBar();
	const bool atBottom = sb && (sb->value() >= sb->maximum() - 10);

	m_chatView->setHtml(m_chatHistory);

	if (atBottom && sb)
		sb->setValue(sb->maximum());
}

void AiChatDialog::setInputEnabled(bool enabled)
{
	m_input->setEnabled(enabled);
	m_btnSend->setEnabled(enabled);
	m_btnSend->setText(enabled ? tr("发送") : tr("..."));
}
