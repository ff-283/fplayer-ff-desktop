#include <fplayer/service/imagepoolservice.h>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>

namespace fplayer
{
	ImagePoolService::ImagePoolService(QObject* parent)
		: QObject(parent)
		, m_watcher(new QFileSystemWatcher(this))
	{
		connect(m_watcher, &QFileSystemWatcher::directoryChanged,
		        this, &ImagePoolService::onDirectoryChanged);
	}

	ImagePoolService::~ImagePoolService() = default;

	void ImagePoolService::setWatchDir(const QString& dir)
	{
		if (m_watchDir == dir)
		{
			return;
		}
		if (!m_watchDir.isEmpty())
		{
			m_watcher->removePath(m_watchDir);
		}
		m_watchDir = dir;
		m_imagePaths.clear();
		m_metaCache.clear();
		if (!dir.isEmpty())
		{
			m_watcher->addPath(dir);
			scanDirectory();
		}
	}

	QString ImagePoolService::watchDir() const
	{
		return m_watchDir;
	}

	QStringList ImagePoolService::imagePaths() const
	{
		return m_imagePaths;
	}

	QList<ImageMeta> ImagePoolService::imageMetaList() const
	{
		QList<ImageMeta> list;
		list.reserve(m_imagePaths.size());
		for (const auto& p : m_imagePaths)
		{
			if (m_metaCache.contains(p))
			{
				list.append(m_metaCache[p]);
			}
		}
		return list;
	}

	ImageMeta ImagePoolService::imageMeta(const QString& filePath) const
	{
		return m_metaCache.value(filePath);
	}

	QPixmap ImagePoolService::thumbnail(const QString& filePath, const QSize& size)
	{
		const QString key = cacheKey(filePath, size);
		if (m_thumbnailCache.contains(key))
		{
			m_lruList.removeOne(key);
			m_lruList.prepend(key);
			return m_thumbnailCache[key];
		}
		const QSize targetSize = size.isValid() ? size : QSize(256, 256);
		QImageReader reader(filePath);
		reader.setAutoTransform(true);
		const QSize origSize = reader.size();
		reader.setScaledSize(targetSize);
		const QImage img = reader.read();
		if (img.isNull())
		{
			return {};
		}
		QPixmap pix = QPixmap::fromImage(img);

		// Update meta with actual image dimensions if not yet loaded
		if (m_metaCache.contains(filePath) && !m_metaCache[filePath].isLoaded && origSize.isValid())
		{
			m_metaCache[filePath].imageSize = origSize;
			m_metaCache[filePath].isLoaded = true;
		}

		m_thumbnailCache[key] = pix;
		m_lruList.prepend(key);
		while (m_lruList.size() > kMaxThumbnailCache)
		{
			m_thumbnailCache.remove(m_lruList.takeLast());
		}
		return pix;
	}

	void ImagePoolService::refresh()
	{
		scanDirectory();
	}

	QStringList ImagePoolService::supportedExtensions()
	{
		return {QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
		        QStringLiteral("*.bmp"), QStringLiteral("*.gif"), QStringLiteral("*.webp")};
	}

	void ImagePoolService::onDirectoryChanged(const QString& path)
	{
		Q_UNUSED(path)
		const QStringList oldPaths = m_imagePaths;
		scanDirectory();
		const QSet<QString> oldSet(oldPaths.begin(), oldPaths.end());
		const QSet<QString> newSet(m_imagePaths.begin(), m_imagePaths.end());

		for (const auto& p : m_imagePaths)
		{
			if (!oldSet.contains(p))
			{
				emit imageAdded(p);
			}
		}
		for (const auto& p : oldPaths)
		{
			if (!newSet.contains(p))
			{
				emit imageRemoved(p);
			}
		}
	}

	void ImagePoolService::scanDirectory()
	{
		m_imagePaths.clear();
		if (m_watchDir.isEmpty())
		{
			return;
		}
		QDir dir(m_watchDir);
		if (!dir.exists())
		{
			emit scanFinished();
			return;
		}
		const QStringList nameFilters = supportedExtensions();
		const QFileInfoList entries = dir.entryInfoList(nameFilters, QDir::Files, QDir::Time);
		m_imagePaths.reserve(entries.size());
		for (const auto& info : entries)
		{
			const QString path = info.absoluteFilePath();
			m_imagePaths.append(path);
			if (!m_metaCache.contains(path) || m_metaCache[path].lastModified != info.lastModified())
			{
				m_metaCache[path] = buildMeta(path);
			}
		}
		// Remove stale meta entries
		const QSet<QString> currentSet(m_imagePaths.begin(), m_imagePaths.end());
		QStringList staleKeys;
		for (auto it = m_metaCache.begin(); it != m_metaCache.end(); ++it)
		{
			if (!currentSet.contains(it.key()))
			{
				staleKeys.append(it.key());
			}
		}
		for (const auto& k : staleKeys)
		{
			m_metaCache.remove(k);
		}
		emit scanFinished();
	}

	QString ImagePoolService::cacheKey(const QString& filePath, const QSize& size) const
	{
		return QStringLiteral("%1_%2x%3").arg(filePath).arg(size.width()).arg(size.height());
	}

	ImageMeta ImagePoolService::buildMeta(const QString& filePath, const QSize& imageSize) const
	{
		QFileInfo info(filePath);
		ImageMeta meta;
		meta.filePath = filePath;
		meta.fileName = info.fileName();
		meta.fileSize = info.size();
		meta.birthTime = info.birthTime();
		meta.lastModified = info.lastModified();
		meta.imageSize = imageSize;
		meta.isLoaded = imageSize.isValid();
		return meta;
	}
}

