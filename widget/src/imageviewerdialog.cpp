#include <fplayer/widget/imageviewerdialog.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QScrollBar>
#include <QPixmap>
#include <QApplication>
#include <QFileInfo>

ImageViewerDialog::ImageViewerDialog(QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("图片预览"));
	setMinimumSize(400, 300);
	resize(900, 650);
	setAttribute(Qt::WA_DeleteOnClose, false);

	auto* root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);

	// Scroll area for image
	m_scrollArea = new QScrollArea(this);
	m_scrollArea->setWidgetResizable(false);
	m_scrollArea->setAlignment(Qt::AlignCenter);
	m_scrollArea->setStyleSheet(QStringLiteral("QScrollArea { background-color: #1a1a1a; border: none; }"));

	m_imageLabel = new QLabel;
	m_imageLabel->setAlignment(Qt::AlignCenter);
	m_imageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
	m_imageLabel->setStyleSheet(QStringLiteral("QLabel { background-color: #1a1a1a; }"));
	m_scrollArea->setWidget(m_imageLabel);

	// Navigation bar
	auto* navBar = new QWidget(this);
	navBar->setFixedHeight(48);
	navBar->setStyleSheet(QStringLiteral("QWidget { background-color: #2d2d2d; }"));
	auto* navLayout = new QHBoxLayout(navBar);
	navLayout->setContentsMargins(12, 4, 12, 4);

	m_btnPrev = new QPushButton(tr("< 上一张"), navBar);
	m_btnPrev->setFixedSize(90, 32);
	m_btnNext = new QPushButton(tr("下一张 >"), navBar);
	m_btnNext->setFixedSize(90, 32);
	m_indexLabel = new QLabel(navBar);
	m_indexLabel->setAlignment(Qt::AlignCenter);
	m_indexLabel->setStyleSheet(QStringLiteral("color: #cccccc; font-size: 13px;"));

	navLayout->addWidget(m_btnPrev);
	navLayout->addStretch(1);
	navLayout->addWidget(m_indexLabel);
	navLayout->addStretch(1);
	navLayout->addWidget(m_btnNext);

	root->addWidget(m_scrollArea, 1);
	root->addWidget(navBar, 0);

	connect(m_btnPrev, &QPushButton::clicked, this, &ImageViewerDialog::navigatePrev);
	connect(m_btnNext, &QPushButton::clicked, this, &ImageViewerDialog::navigateNext);
}

void ImageViewerDialog::setImageList(const QStringList& paths, int currentIndex)
{
	m_paths = paths;
	m_currentIndex = qBound(0, currentIndex, m_paths.size() - 1);
	updateImage();
}

int ImageViewerDialog::currentIndex() const
{
	return m_currentIndex;
}

void ImageViewerDialog::keyPressEvent(QKeyEvent* event)
{
	switch (event->key())
	{
	case Qt::Key_Left:
	case Qt::Key_Up:
		navigatePrev();
		break;
	case Qt::Key_Right:
	case Qt::Key_Down:
		navigateNext();
		break;
	case Qt::Key_Escape:
		close();
		break;
	case Qt::Key_Plus:
	case Qt::Key_Equal:
		m_zoomLevel = qMin(m_zoomLevel * 1.25, 5.0);
		updateImage();
		break;
	case Qt::Key_Minus:
		m_zoomLevel = qMax(m_zoomLevel / 1.25, 0.1);
		updateImage();
		break;
	case Qt::Key_0:
		resetZoom();
		break;
	default:
		QDialog::keyPressEvent(event);
		break;
	}
}

void ImageViewerDialog::resizeEvent(QResizeEvent* event)
{
	QDialog::resizeEvent(event);
	if (!m_currentPixmap.isNull() && m_zoomLevel == 1.0)
		fitToWindow();
}

void ImageViewerDialog::navigateTo(int index)
{
	if (index < 0 || index >= m_paths.size())
	{
		return;
	}
	m_currentIndex = index;
	resetZoom();
	updateImage();
}

void ImageViewerDialog::navigatePrev()
{
	if (m_currentIndex > 0)
	{
		navigateTo(m_currentIndex - 1);
	}
}

void ImageViewerDialog::navigateNext()
{
	if (m_currentIndex < m_paths.size() - 1)
	{
		navigateTo(m_currentIndex + 1);
	}
}

void ImageViewerDialog::updateImage()
{
	if (m_currentIndex < 0 || m_currentIndex >= m_paths.size())
	{
		m_imageLabel->clear();
		return;
	}
	m_currentPixmap = QPixmap(m_paths[m_currentIndex]);
	if (m_currentPixmap.isNull())
	{
		m_imageLabel->setText(tr("无法加载图片"));
		m_imageLabel->setStyleSheet(QStringLiteral("color: #ff4444; font-size: 16px; background-color: #1a1a1a;"));
		return;
	}
	m_imageLabel->setStyleSheet(QStringLiteral("QLabel { background-color: #1a1a1a; }"));
	m_imageLabel->setPixmap(m_currentPixmap);
	m_imageLabel->resize(m_currentPixmap.size());
	updateTitle();
}

void ImageViewerDialog::updateTitle()
{
	if (m_currentIndex >= 0 && m_currentIndex < m_paths.size())
	{
		const QString name = QFileInfo(m_paths[m_currentIndex]).fileName();
		setWindowTitle(tr("图片预览 - %1").arg(name));
		m_indexLabel->setText(tr("%1 / %2").arg(m_currentIndex + 1).arg(m_paths.size()));
	}
	m_btnPrev->setEnabled(m_currentIndex > 0);
	m_btnNext->setEnabled(m_currentIndex < m_paths.size() - 1);
}

void ImageViewerDialog::fitToWindow()
{
	if (m_currentPixmap.isNull())
		return;
	const QSize viewSize = m_scrollArea->viewport()->size();
	QSize targetSize = viewSize;
	if (targetSize.width() < 100)
		targetSize.setWidth(800);
	if (targetSize.height() < 100)
		targetSize.setHeight(600);
	QPixmap scaled = m_currentPixmap.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	m_imageLabel->setPixmap(scaled);
	m_imageLabel->resize(scaled.size());
}

void ImageViewerDialog::resetZoom()
{
	m_zoomLevel = 1.0;
	fitToWindow();
}

void ImageViewerDialog::showEvent(QShowEvent* event)
{
	QDialog::showEvent(event);
	if (m_zoomLevel == 1.0)
		fitToWindow();
}

