#ifndef FPLAYER_COMMON_IMAGEMETADATA_H
#define FPLAYER_COMMON_IMAGEMETADATA_H

#include <QString>
#include <QDateTime>
#include <QSize>

namespace fplayer
{
	struct ImageMeta
	{
		QString filePath;
		QString fileName;
		qint64 fileSize = 0;
		QDateTime birthTime;
		QDateTime lastModified;
		QSize imageSize;
		bool isLoaded = false;

		bool isValid() const { return !filePath.isEmpty(); }
	};
}

#endif // FPLAYER_COMMON_IMAGEMETADATA_H
