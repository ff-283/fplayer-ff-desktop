#ifndef FPLAYER_SERVICE_IMAGEPOOLSERVICE_H
#define FPLAYER_SERVICE_IMAGEPOOLSERVICE_H

#include <QObject>
#include <QStringList>
#include <QHash>
#include <QPixmap>
#include <QSize>
#include <QFileSystemWatcher>
#include <fplayer/service/export.h>
#include <fplayer/common/imagepool/imagemetadata.h>

namespace fplayer
{
	class FPLAYER_SERVICE_EXPORT ImagePoolService : public QObject
	{
		Q_OBJECT

	public:
		enum SortMode
		{
			SortByNameAsc,
			SortByNameDesc,
			SortByDateAsc,
			SortByDateDesc,
			SortBySizeAsc,
			SortBySizeDesc,
		};
		Q_ENUM(SortMode)

		explicit ImagePoolService(QObject* parent = nullptr);
		~ImagePoolService() override;

		void setWatchDir(const QString& dir);
		QString watchDir() const;

		QStringList imagePaths() const;
		QList<ImageMeta> imageMetaList() const;
		ImageMeta imageMeta(const QString& filePath) const;

		QPixmap thumbnail(const QString& filePath, const QSize& size);

		void refresh();

		static QStringList supportedExtensions();

	signals:
		void directoryChanged(const QString& dir);
		void imageAdded(const QString& filePath);
		void imageRemoved(const QString& filePath);
		void scanFinished();

	private slots:
		void onDirectoryChanged(const QString& path);

	private:
		void scanDirectory();
		QString cacheKey(const QString& filePath, const QSize& size) const;
		ImageMeta buildMeta(const QString& filePath, const QSize& imageSize = QSize()) const;

		QString m_watchDir;
		QStringList m_imagePaths;
		QHash<QString, ImageMeta> m_metaCache;
		QFileSystemWatcher* m_watcher = nullptr;
		QHash<QString, QPixmap> m_thumbnailCache;
		QStringList m_lruList;
		static constexpr int kMaxThumbnailCache = 200;
	};
}

#endif // FPLAYER_SERVICE_IMAGEPOOLSERVICE_H
