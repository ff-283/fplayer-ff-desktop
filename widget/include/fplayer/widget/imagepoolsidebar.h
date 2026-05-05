#ifndef FPLAYER_WIDGET_IMAGEPOOLSIDEBAR_H
#define FPLAYER_WIDGET_IMAGEPOOLSIDEBAR_H

#include <QWidget>
#include <QListWidget>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QStringList>
#include <QFileInfo>
#include <fplayer/widget/export.h>
#include <fplayer/common/imagepool/imagemetadata.h>

namespace fplayer
{
	class ImagePoolService;
}

class ImageViewerDialog;

class FPLAYER_WIDGET_EXPORT ImagePoolSidebar : public QWidget
{
	Q_OBJECT

public:
	explicit ImagePoolSidebar(QWidget* parent = nullptr);
	~ImagePoolSidebar() override;

	void closeEvent(QCloseEvent* event) override;
	void setScreenshotDir(const QString& dir);
	QString screenshotDir() const;

signals:
	void visibilityChanged(bool visible);
	void analyzeRequested(const QString& filePath);

public slots:
	void onScreenshotSaved(const QString& filePath);

private slots:
	void onRefreshClicked();
	void onSortChanged(int index);
	void onItemDoubleClicked(QListWidgetItem* item);
	void onContextMenuRequested(const QPoint& pos);
	void onDirectoryChanged(const QString& dir);
	void onImageAdded(const QString& filePath);
	void onImageRemoved(const QString& filePath);

private:
	void setupUi();
	void loadImages();
	void addImageToList(const QString& path);
	void removeImageFromList(const QString& path);
	void openInSystemViewer(const QString& path);
	void openFileLocation(const QString& path);
	void copyImage(const QString& path);
	void copyPath(const QString& path);
	void deleteImage(const QString& path);
	void renameImage(const QString& path);
	void updateCountLabel();
	QPixmap makeThumbnail(const QString& path) const;

	fplayer::ImagePoolService* m_service = nullptr;
	QListWidget* m_listWidget = nullptr;
	QComboBox* m_sortCombo = nullptr;
	QPushButton* m_btnRefresh = nullptr;
	QLabel* m_countLabel = nullptr;
	ImageViewerDialog* m_viewerDialog = nullptr;
	QString m_screenshotDir;
	QStringList m_pendingPaths;
	int m_loadIndex = 0;
	static constexpr int kThumbnailMaxW = 200;
	static constexpr int kThumbnailMaxH = 150;
};

#endif // FPLAYER_WIDGET_IMAGEPOOLSIDEBAR_H
