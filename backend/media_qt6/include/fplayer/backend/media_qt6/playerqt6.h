/*************************************************
  * 描述：
  *
  * File：playerqt6.h
  * Date：2026/2/28
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_PLAYERQT6_H
#define FPLAYER_DESKETOP_PLAYERQT6_H
#include <fplayer/backend/media_qt6/export.h>
#include <QObject>

namespace fplayer
{
	class FPLAYER_BACKEND_MEDIA_QT6_EXPORT PlayerQt6 : public QObject
	{
		Q_OBJECT

	public:
		PlayerQt6();
		~PlayerQt6() override;
	};
}// fplayer


#endif //FPLAYER_DESKETOP_PLAYERQT6_H