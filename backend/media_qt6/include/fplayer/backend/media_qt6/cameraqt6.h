/*************************************************
  * 描述：
  *
  * File：cameraqt6.h
  * Date：2026/2/28
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_CAPTUREQT6_H
#define FPLAYER_DESKETOP_CAPTUREQT6_H
#include <fplayer/backend/media_qt6/export.h>
#include <fplayer/api/media/icamera.h>
#include <QList>
#include <QtMultimedia/QMediaCaptureSession>
#include <QObject>
#include <QString>

class QCameraFormat;

namespace fplayer
{
	class FPLAYER_BACKEND_MEDIA_QT6_EXPORT CameraQt6 : public QObject, public ICamera// QObject的继承必须放在第一位
	{
		Q_OBJECT

	public:
		CameraQt6();

		~CameraQt6() override;

		bool selectCamera(int index) override;

		bool selectCameraFormat(int index) override;

		void refreshCameras() override;

		QList<CameraDescription> getDescriptions() override;

		int getIndex() override;

		void pause() override;

		void resume() override;

		void setPreviewTarget(const PreviewTarget& target) override;

	private:
		struct Impl;
		QList<QCameraDevice> m_devices;
		QCamera* m_camera;
		QMediaCaptureSession m_session;
		QVector<QList<QCameraFormat>> m_qtCameraFormats;// 存储各个摄像头的Qt原始格式信息
	};
}// fplayer

#endif //FPLAYER_DESKETOP_CAPTUREQT6_H