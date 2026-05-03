#include <fplayer/widget/aichatdialog.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QFileInfo>
#include <QScrollBar>
#include <QEvent>
#include <QCloseEvent>
#include <QDebug>

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
	m_imageLabel->setStyleSheet(QStringLiteral("QLabel { background-color: #e5e7eb; border-radius: 4px; }"));
	imgLayout->addWidget(m_imageLabel);
	imgLayout->addStretch();

	auto* chatPanel = new QWidget(splitter);
	auto* chatLayout = new QVBoxLayout(chatPanel);
	chatLayout->setContentsMargins(8, 8, 8, 8);

	m_chatScroll = new QScrollArea(chatPanel);
	m_chatScroll->setWidgetResizable(false);
	m_chatScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_chatScroll->setStyleSheet(QStringLiteral(
		"QScrollArea { border: 1px solid #d1d5db; border-radius: 4px; background-color: #f3f4f6; }"));
	m_chatContent = new QWidget();
	m_chatLayout = new QVBoxLayout(m_chatContent);
	m_chatLayout->setContentsMargins(8, 8, 8, 8);
	m_chatLayout->setSpacing(0);
	m_chatLayout->addStretch();
	m_chatScroll->setWidget(m_chatContent);
	chatLayout->addWidget(m_chatScroll, 1);

	m_chatScroll->viewport()->installEventFilter(this);

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

	m_typewriterTimer = new QTimer(this);
	m_typewriterTimer->setInterval(30);
	connect(m_typewriterTimer, &QTimer::timeout, this, &AiChatDialog::onTypewriterTick);
}

void AiChatDialog::startChat(const QString& imagePath, const fplayer::AiConfig& config)
{
	m_imagePath = imagePath;
	m_aiService->setConfig(config);
	m_colors = config;

	clearChat();

	const QString fontFamilyCss = m_colors.fontFamily.isEmpty()
		? QString() : QStringLiteral("font-family: %1;").arg(m_colors.fontFamily);

	m_chatScroll->setStyleSheet(QStringLiteral(
		"QScrollArea { border: 1px solid #d1d5db; border-radius: 4px; background-color: %1; }")
		.arg(m_colors.chatBgColor));
	m_chatContent->setAutoFillBackground(true);
	{
		QPalette pal = m_chatContent->palette();
		pal.setColor(QPalette::Window, QColor(m_colors.chatBgColor));
		m_chatContent->setPalette(pal);
	}
	m_input->setStyleSheet(QStringLiteral(
		"QLineEdit { border: 1px solid #ccc; border-radius: 4px; padding: 6px; font-size: %1px; %2 }")
		.arg(m_colors.fontSize).arg(fontFamilyCss));

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

	appendBubble(ChatBubbleWidget::AI, tr("AI 助手"), tr("关于这张图片，我能为你做些什么？"));

	show();
	raise();
	activateWindow();

	QTimer::singleShot(200, this, &AiChatDialog::autoSend);
}

void AiChatDialog::reconfigure(const fplayer::AiConfig& config)
{
	m_aiService->setConfig(config);
	m_colors = config;

	const QString fontFamilyCss = m_colors.fontFamily.isEmpty()
		? QString() : QStringLiteral("font-family: %1;").arg(m_colors.fontFamily);

	m_chatScroll->setStyleSheet(QStringLiteral(
		"QScrollArea { border: 1px solid #d1d5db; border-radius: 4px; background-color: %1; }")
		.arg(m_colors.chatBgColor));
	m_chatContent->setAutoFillBackground(true);
	{
		QPalette pal = m_chatContent->palette();
		pal.setColor(QPalette::Window, QColor(m_colors.chatBgColor));
		m_chatContent->setPalette(pal);
	}
	m_input->setStyleSheet(QStringLiteral(
		"QLineEdit { border: 1px solid #ccc; border-radius: 4px; padding: 6px; font-size: %1px; %2 }")
		.arg(m_colors.fontSize).arg(fontFamilyCss));

	for (auto* b : m_bubbles)
		b->setColors(m_colors);
	if (m_streamingBubble)
		m_streamingBubble->setColors(m_colors);

	updateAllMaxWidths();
}

void AiChatDialog::autoSend()
{
	if (m_imagePath.isEmpty() || m_aiStreaming)
		return;
	m_input->setText(QString());
	setInputEnabled(false);
	appendBubble(ChatBubbleWidget::User, tr("我"), tr("请分析一下图片内容"));
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
	appendBubble(ChatBubbleWidget::User, tr("我"), text);
	beginAiMessage();
	m_aiService->sendMessage(m_imagePath, text,
	                         tr("你是一个有帮助的AI助手。请用中文回答用户的问题。"));
}

void AiChatDialog::onResponseChunk(const QString& chunk)
{
	qDebug() << "[AiChat] onResponseChunk:" << chunk << "pending size:" << m_pendingChars.size();
	if (!m_aiStreaming)
		beginAiMessage();
	m_pendingChars += chunk;
	if (!m_typewriterTimer->isActive())
	{
		qDebug() << "[AiChat] starting typewriter timer (30ms)";
		m_typewriterTimer->start();
	}
}

void AiChatDialog::onResponseFinished()
{
	qDebug() << "[AiChat] onResponseFinished, pending chars:" << m_pendingChars.size()
	         << "timer active:" << m_typewriterTimer->isActive();
	m_responseComplete = true;
	if (m_pendingChars.isEmpty() && !m_typewriterTimer->isActive())
	{
		finishAiMessage();
		setInputEnabled(true);
		m_input->setFocus();
	}
}

void AiChatDialog::onRequestFailed(const QString& error)
{
	qWarning() << "[AiChat] onRequestFailed:" << error;
	m_typewriterTimer->stop();
	m_pendingChars.clear();
	m_responseComplete = false;
	if (m_aiStreaming)
		finishAiMessage();
	setInputEnabled(true);
	appendBubble(ChatBubbleWidget::System, tr("系统"), tr("[错误] %1").arg(error));
	m_input->setFocus();
}

// ── Streaming bubble management ──

void AiChatDialog::beginAiMessage()
{
	qDebug() << "[AiChat] beginAiMessage";
	m_aiStreaming = true;
	m_aiCurrentContent.clear();
	m_pendingChars.clear();
	m_responseComplete = false;

	m_streamingBubble = new ChatBubbleWidget(
		ChatBubbleWidget::AI, tr("AI 助手"), QString(), m_colors, m_chatContent);
	m_streamingBubble->setStreaming(true);
	m_streamingBubble->setMaxBubbleWidth(int(m_chatScroll->viewport()->width() * 0.7));

	auto* row = new QHBoxLayout();
	row->setContentsMargins(0, 3, 0, 3);
	row->addWidget(m_streamingBubble);
	row->addStretch();

	m_chatLayout->insertLayout(m_chatLayout->count() - 1, row);
	scrollToBottom();
}

void AiChatDialog::appendToAiMessage(const QString& text)
{
	m_aiCurrentContent += text;
	if (m_streamingBubble)
		m_streamingBubble->setText(m_aiCurrentContent);
	scrollToBottom();
}

void AiChatDialog::finishAiMessage()
{
	if (!m_aiStreaming)
		return;
	m_aiStreaming = false;
	if (m_streamingBubble)
	{
		m_streamingBubble->setStreaming(false);
		m_bubbles.append(m_streamingBubble);
		m_streamingBubble = nullptr;
	}
	m_aiCurrentContent.clear();
}

void AiChatDialog::onTypewriterTick()
{
	if (m_pendingChars.isEmpty())
	{
		qDebug() << "[AiChat] typewriter tick: buffer empty, stopping timer";
		m_typewriterTimer->stop();
		if (m_responseComplete)
		{
			finishAiMessage();
			setInputEnabled(true);
			m_input->setFocus();
		}
		return;
	}

	const QString ch = m_pendingChars.left(1);
	m_pendingChars = m_pendingChars.mid(1);
	// Log first few chars only to avoid flooding
	if (m_aiCurrentContent.size() < 10)
		qDebug() << "[AiChat] typewriter output char:" << ch
		         << "total:" << m_aiCurrentContent.size() + 1;
	appendToAiMessage(ch);
}

// ── Static message bubble ──

void AiChatDialog::appendBubble(ChatBubbleWidget::SenderType type,
                                const QString& sender, const QString& text)
{
	auto* bubble = new ChatBubbleWidget(type, sender, text, m_colors, m_chatContent);
	bubble->setMaxBubbleWidth(int(m_chatScroll->viewport()->width() * 0.7));

	auto* row = new QHBoxLayout();
	row->setContentsMargins(0, 3, 0, 3);

	if (type == ChatBubbleWidget::User)
	{
		row->addStretch();
		row->addWidget(bubble);
	}
	else if (type == ChatBubbleWidget::AI)
	{
		row->addWidget(bubble);
		row->addStretch();
	}
	else
	{
		row->addStretch();
		row->addWidget(bubble);
		row->addStretch();
	}

	m_chatLayout->insertLayout(m_chatLayout->count() - 1, row);
	m_bubbles.append(bubble);

	scrollToBottom();
}

// ── Helpers ──

void AiChatDialog::setInputEnabled(bool enabled)
{
	m_input->setEnabled(true);           // always allow typing
	m_btnSend->setEnabled(enabled);
	m_btnSend->setText(enabled ? tr("发送") : tr("..."));
}

void AiChatDialog::clearChat()
{
	m_typewriterTimer->stop();
	m_pendingChars.clear();
	m_responseComplete = false;
	m_bubbles.clear();
	m_streamingBubble = nullptr;
	m_aiCurrentContent.clear();

	m_chatScroll->takeWidget();
	delete m_chatContent;

	m_chatContent = new QWidget();
	m_chatLayout = new QVBoxLayout(m_chatContent);
	m_chatLayout->setContentsMargins(8, 8, 8, 8);
	m_chatLayout->setSpacing(0);
	m_chatLayout->addStretch();
	m_chatScroll->setWidget(m_chatContent);
}

void AiChatDialog::scrollToBottom()
{
	m_chatLayout->activate();
	applyContentSize();
	m_chatScroll->verticalScrollBar()->setValue(
		m_chatScroll->verticalScrollBar()->maximum());
}

void AiChatDialog::applyContentSize()
{
	const int vpW = m_chatScroll->viewport()->width();
	const int vpH = m_chatScroll->viewport()->height();
	const QSize needed = m_chatLayout->sizeHint();
	m_chatContent->resize(qMax(vpW, needed.width()),
	                      qMax(vpH, needed.height()));
}

void AiChatDialog::updateAllMaxWidths()
{
	if (!m_chatScroll->viewport())
		return;
	const int vpW = m_chatScroll->viewport()->width();
	int maxW = int(vpW * 0.7);
	for (auto* b : m_bubbles)
		b->setMaxBubbleWidth(maxW);
	if (m_streamingBubble)
		m_streamingBubble->setMaxBubbleWidth(maxW);
	// Keep content widget width in sync with viewport
	applyContentSize();
}

bool AiChatDialog::eventFilter(QObject* obj, QEvent* event)
{
	if (obj == m_chatScroll->viewport() && event->type() == QEvent::Resize)
		updateAllMaxWidths();
	return QDialog::eventFilter(obj, event);
}

void AiChatDialog::closeEvent(QCloseEvent* event)
{
	m_typewriterTimer->stop();
	m_pendingChars.clear();
	m_responseComplete = false;
	m_aiStreaming = false;

	disconnect(m_aiService, nullptr, this, nullptr);

	QDialog::closeEvent(event);
}
