#include <fplayer/widget/imagepoolsidebar.h>
#include <fplayer/widget/imageviewerdialog.h>
#include <fplayer/service/imagepoolservice.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QInputDialog>
#include <QClipboard>
#include <QApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QProcess>
#include <QTimer>
#include <QDir>
#include <QCloseEvent>
#include <QPainter>
#include <QImageReader>

ImagePoolSidebar::ImagePoolSidebar(QWidget* parent)
	: QWidget(parent, Qt::Window)
	, m_service(new fplayer::ImagePoolService(this))
{
	setupUi();
	resize(420, 560);
	setMinimumSize(300, 360);
	connect(m_service, &fplayer::ImagePoolService::directoryChanged,
	        this, &ImagePoolSidebar::onDirectoryChanged);
	connect(m_service, &fplayer::ImagePoolService::imageAdded,
	        this, &ImagePoolSidebar::onImageAdded);
	connect(m_service, &fplayer::ImagePoolService::imageRemoved,
	        this, &ImagePoolSidebar::onImageRemoved);
	connect(m_service, &fplayer::ImagePoolService::scanFinished,
	        this, [this]() { loadImages(); });
}

ImagePoolSidebar::~ImagePoolSidebar() = default;

void ImagePoolSidebar::closeEvent(QCloseEvent* event)
{
	hide();
	event->ignore();
	emit visibilityChanged(false);
}

void ImagePoolSidebar::setupUi()
{
	auto* root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);

	// --- Toolbar ---
	auto* toolbar = new QWidget(this);
	toolbar->setFixedHeight(36);
	toolbar->setStyleSheet(QStringLiteral("QWidget { background-color: #f0f0f0; }"));
	auto* tbLayout = new QHBoxLayout(toolbar);
	tbLayout->setContentsMargins(8, 2, 8, 2);
	tbLayout->setSpacing(6);

	m_btnRefresh = new QPushButton(tr("刷新"), toolbar);
	m_btnRefresh->setFixedSize(50, 28);

	m_sortCombo = new QComboBox(toolbar);
	m_sortCombo->setFixedHeight(28);
	m_sortCombo->addItem(tr("日期 ↓"), static_cast<int>(fplayer::ImagePoolService::SortByDateDesc));
	m_sortCombo->addItem(tr("日期 ↑"), static_cast<int>(fplayer::ImagePoolService::SortByDateAsc));
	m_sortCombo->addItem(tr("名称 A-Z"), static_cast<int>(fplayer::ImagePoolService::SortByNameAsc));
	m_sortCombo->addItem(tr("名称 Z-A"), static_cast<int>(fplayer::ImagePoolService::SortByNameDesc));
	m_sortCombo->addItem(tr("大小 ↓"), static_cast<int>(fplayer::ImagePoolService::SortBySizeDesc));
	m_sortCombo->addItem(tr("大小 ↑"), static_cast<int>(fplayer::ImagePoolService::SortBySizeAsc));

	m_countLabel = new QLabel(toolbar);
	m_countLabel->setStyleSheet(QStringLiteral("color: #666; font-size: 12px;"));

	tbLayout->addWidget(m_btnRefresh);
	tbLayout->addWidget(m_sortCombo, 1);
	tbLayout->addWidget(m_countLabel);
	tbLayout->addStretch();

	// --- Image grid ---
	m_listWidget = new QListWidget(this);
	m_listWidget->setViewMode(QListView::IconMode);
	m_listWidget->setIconSize(QSize(kThumbnailMaxW + 8, kThumbnailMaxH + 8));
	m_listWidget->setGridSize(QSize(kThumbnailMaxW + 24, kThumbnailMaxH + 44));
	m_listWidget->setResizeMode(QListView::Adjust);
	m_listWidget->setMovement(QListView::Static);
	m_listWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_listWidget->setContextMenuPolicy(Qt::CustomContextMenu);
	m_listWidget->setWordWrap(true);
	m_listWidget->setSpacing(4);

	root->addWidget(toolbar, 0);
	root->addWidget(m_listWidget, 1);

	// --- Connections ---
	connect(m_btnRefresh, &QPushButton::clicked, this, &ImagePoolSidebar::onRefreshClicked);
	connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, &ImagePoolSidebar::onSortChanged);
	connect(m_listWidget, &QListWidget::itemDoubleClicked,
	        this, &ImagePoolSidebar::onItemDoubleClicked);
	connect(m_listWidget, &QListWidget::customContextMenuRequested,
	        this, &ImagePoolSidebar::onContextMenuRequested);
}

void ImagePoolSidebar::setScreenshotDir(const QString& dir)
{
	m_screenshotDir = dir;
	m_service->setWatchDir(dir);
}

QString ImagePoolSidebar::screenshotDir() const
{
	return m_screenshotDir;
}

void ImagePoolSidebar::setToolbarColor(const QString& color)
{
	m_toolbarColor = color;
	// Find and update toolbar widget (first child of root layout)
	if (auto* root = qobject_cast<QWidget*>(layout()->itemAt(0)->widget()))
		root->setStyleSheet(QStringLiteral("QWidget { background-color: %1; }").arg(color));
}

void ImagePoolSidebar::onScreenshotSaved(const QString& filePath)
{
	QFileInfo info(filePath);
	if (info.absolutePath() == QDir(m_screenshotDir).absolutePath())
	{
		addImageToList(filePath);
	}
}

void ImagePoolSidebar::onRefreshClicked()
{
	m_service->refresh();
}

void ImagePoolSidebar::onSortChanged(int /*index*/)
{
	loadImages();
}

void ImagePoolSidebar::onItemDoubleClicked(QListWidgetItem* item)
{
	if (!item)
	{
		return;
	}
	const QString path = item->data(Qt::UserRole).toString();
	if (path.isEmpty())
	{
		return;
	}
	if (!m_viewerDialog)
	{
		m_viewerDialog = new ImageViewerDialog(this);
	}
	const QStringList allPaths = m_service->imagePaths();
	const int idx = allPaths.indexOf(path);
	m_viewerDialog->setImageList(allPaths, idx >= 0 ? idx : 0);
	m_viewerDialog->show();
	m_viewerDialog->raise();
	m_viewerDialog->activateWindow();
}

void ImagePoolSidebar::onContextMenuRequested(const QPoint& pos)
{
	QListWidgetItem* item = m_listWidget->itemAt(pos);
	if (!item)
	{
		return;
	}
	const QString path = item->data(Qt::UserRole).toString();
	if (path.isEmpty())
	{
		return;
	}
	QMenu menu(this);
	QAction* actOpen = menu.addAction(tr("打开"));
	QAction* actOpenFolder = menu.addAction(tr("打开所在文件夹"));
	menu.addSeparator();
	QAction* actCopy = menu.addAction(tr("复制图片"));
	QAction* actCopyPath = menu.addAction(tr("复制图片路径"));
	menu.addSeparator();
	QAction* actRename = menu.addAction(tr("重命名"));
	QAction* actDelete = menu.addAction(tr("删除"));
	menu.addSeparator();
	QAction* actAnalyze = menu.addAction(tr("AI 识别"));

	QAction* chosen = menu.exec(m_listWidget->viewport()->mapToGlobal(pos));
	if (!chosen)
	{
		return;
	}
	if (chosen == actOpen)
	{
		openInSystemViewer(path);
	}
	else if (chosen == actOpenFolder)
	{
		openFileLocation(path);
	}
	else if (chosen == actCopy)
	{
		copyImage(path);
	}
	else if (chosen == actCopyPath)
	{
		copyPath(path);
	}
	else if (chosen == actRename)
	{
		renameImage(path);
	}
	else if (chosen == actDelete)
	{
		deleteImage(path);
	}
	else if (chosen == actAnalyze)
	{
		emit analyzeRequested(path);
	}
}

void ImagePoolSidebar::onDirectoryChanged(const QString& /*dir*/)
{
	// Full reload
	loadImages();
}

void ImagePoolSidebar::onImageAdded(const QString& filePath)
{
	addImageToList(filePath);
	updateCountLabel();
}

void ImagePoolSidebar::onImageRemoved(const QString& filePath)
{
	removeImageFromList(filePath);
	updateCountLabel();
}

// ─── Image loading ───────────────────────────────────

void ImagePoolSidebar::loadImages()
{
	m_listWidget->clear();
	const auto sortMode = static_cast<fplayer::ImagePoolService::SortMode>(
		m_sortCombo->currentData().toInt());
	QStringList paths = m_service->imagePaths();

	// Sort
	auto sortPred = [sortMode, this](const QString& a, const QString& b) -> bool {
		const auto ma = m_service->imageMeta(a);
		const auto mb = m_service->imageMeta(b);
		switch (sortMode)
		{
		case fplayer::ImagePoolService::SortByNameAsc:
			return ma.fileName.compare(mb.fileName, Qt::CaseInsensitive) < 0;
		case fplayer::ImagePoolService::SortByNameDesc:
			return ma.fileName.compare(mb.fileName, Qt::CaseInsensitive) > 0;
		case fplayer::ImagePoolService::SortByDateAsc:
			return ma.birthTime < mb.birthTime;
		case fplayer::ImagePoolService::SortByDateDesc:
			return ma.birthTime > mb.birthTime;
		case fplayer::ImagePoolService::SortBySizeAsc:
			return ma.fileSize < mb.fileSize;
		case fplayer::ImagePoolService::SortBySizeDesc:
			return ma.fileSize > mb.fileSize;
		}
		return false;
	};
	std::stable_sort(paths.begin(), paths.end(), sortPred);

	// Batch load to avoid blocking UI
	m_pendingPaths = paths;
	m_loadIndex = 0;
	const int batchSize = 20;
	const int end = qMin(batchSize, static_cast<int>(m_pendingPaths.size()));
	for (int i = 0; i < end; ++i)
	{
		addImageToList(m_pendingPaths[i]);
	}
	m_loadIndex = end;
	if (m_loadIndex < static_cast<int>(m_pendingPaths.size()))
	{
		QTimer::singleShot(10, this, [this]() {
			const int nextBatch = qMin(m_loadIndex + 20, static_cast<int>(m_pendingPaths.size()));
			for (int i = m_loadIndex; i < nextBatch; ++i)
			{
				addImageToList(m_pendingPaths[i]);
			}
			m_loadIndex = nextBatch;
			if (m_loadIndex < static_cast<int>(m_pendingPaths.size()))
			{
				// Continue via timer
				QTimer::singleShot(10, this, [this]() {
					while (m_loadIndex < static_cast<int>(m_pendingPaths.size()))
					{
						addImageToList(m_pendingPaths[m_loadIndex++]);
						if (m_loadIndex % 20 == 0)
						{
							QApplication::processEvents();
						}
					}
					updateCountLabel();
				});
			}
			else
			{
				updateCountLabel();
			}
		});
	}
	else
	{
		updateCountLabel();
	}
}

void ImagePoolSidebar::addImageToList(const QString& path)
{
	for (int i = 0; i < m_listWidget->count(); ++i)
	{
		if (m_listWidget->item(i)->data(Qt::UserRole).toString() == path)
		{
			return;
		}
	}
	QFileInfo info(path);
	if (!info.exists())
	{
		return;
	}
	const QPixmap thumb = makeThumbnail(path);
	auto* item = new QListWidgetItem();
	item->setIcon(thumb.isNull() ? QIcon() : QIcon(thumb));
	// Show filename without extension as primary text
	const QString baseName = info.completeBaseName();
	item->setText(baseName);
	item->setToolTip(path);
	item->setData(Qt::UserRole, path);
	item->setData(Qt::UserRole + 1, info.size());
	item->setData(Qt::UserRole + 2, info.birthTime());
	m_listWidget->addItem(item);
}

void ImagePoolSidebar::removeImageFromList(const QString& path)
{
	for (int i = 0; i < m_listWidget->count(); ++i)
	{
		if (m_listWidget->item(i)->data(Qt::UserRole).toString() == path)
		{
			delete m_listWidget->takeItem(i);
			return;
		}
	}
}

QPixmap ImagePoolSidebar::makeThumbnail(const QString& path) const
{
	QImageReader reader(path);
	reader.setAutoTransform(true);
	const QSize origSize = reader.size();
	if (!origSize.isValid())
		return {};
	// Scale to fit within kThumbnailMaxW x kThumbnailMaxH, keep aspect ratio
	QSize scaledSize = origSize.scaled(kThumbnailMaxW, kThumbnailMaxH, Qt::KeepAspectRatio);
	reader.setScaledSize(scaledSize);
	QImage img = reader.read();
	if (img.isNull())
		return {};
	// Draw centered on a transparent canvas of the max icon size
	const int canvasW = kThumbnailMaxW + 8;
	const int canvasH = kThumbnailMaxH + 8;
	QPixmap canvas(canvasW, canvasH);
	canvas.fill(Qt::transparent);
	QPainter painter(&canvas);
	int x = (canvasW - img.width()) / 2;
	int y = (canvasH - img.height()) / 2;
	painter.drawImage(x, y, img);
	painter.end();
	return canvas;
}

void ImagePoolSidebar::updateCountLabel()
{
	const int count = m_listWidget->count();
	m_countLabel->setText(tr("共 %1 张").arg(count));
}

// ─── Actions ─────────────────────────────────────────

void ImagePoolSidebar::openInSystemViewer(const QString& path)
{
	QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void ImagePoolSidebar::openFileLocation(const QString& path)
{
#ifdef Q_OS_WIN
	QProcess::startDetached(QStringLiteral("explorer"), {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#else
	QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

void ImagePoolSidebar::copyImage(const QString& path)
{
	QPixmap pix(path);
	if (pix.isNull())
	{
		return;
	}
	QApplication::clipboard()->setPixmap(pix);
}

void ImagePoolSidebar::copyPath(const QString& path)
{
	QApplication::clipboard()->setText(QDir::toNativeSeparators(path));
}

void ImagePoolSidebar::deleteImage(const QString& path)
{
	const QString name = QFileInfo(path).fileName();
	const auto answer = QMessageBox::question(
		this, tr("确认删除"),
		tr("确定要删除 \"%1\" 吗？\n此操作不可恢复。").arg(name),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (answer == QMessageBox::Yes)
	{
		if (QFile::remove(path))
		{
			removeImageFromList(path);
			updateCountLabel();
		}
		else
		{
			QMessageBox::warning(this, tr("删除失败"), tr("无法删除文件，请检查权限。"));
		}
	}
}

void ImagePoolSidebar::renameImage(const QString& path)
{
	QFileInfo info(path);
	const QString oldName = info.completeBaseName();
	bool ok = false;
	const QString newName = QInputDialog::getText(
		this, tr("重命名"), tr("新名称（不含扩展名）："),
		QLineEdit::Normal, oldName, &ok);
	if (!ok || newName.isEmpty() || newName == oldName)
	{
		return;
	}
	const QString newPath = info.absoluteDir().filePath(newName + QStringLiteral(".") + info.suffix());
	if (QFile::exists(newPath))
	{
		QMessageBox::warning(this, tr("重命名失败"), tr("目标文件已存在。"));
		return;
	}
	if (QFile::rename(path, newPath))
	{
		removeImageFromList(path);
		addImageToList(newPath);
		updateCountLabel();
	}
	else
	{
		QMessageBox::warning(this, tr("重命名失败"), tr("无法重命名文件，请检查权限。"));
	}
}

