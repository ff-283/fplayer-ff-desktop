#include <fplayer/backend/media_qt6/cameraqt6.h>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QCameraDevice>
#include <QDebug>
#include <QCamera>
#include <QVideoWidget>

namespace fplayer
{
	CameraQt6::CameraQt6() :
		m_camera(nullptr)
	{
		m_backend = MediaBackendType::Qt6;
	}

	CameraQt6::~CameraQt6() = default;

	bool CameraQt6::selectCamera(int index)
	{
		if (index < 0 || index >= m_devices.size())
		{
			qWarning() << "selectCamera invalid index:" << index;
			return false;
		}

		if (m_camera)
		{
			m_camera->stop();
			m_camera->deleteLater();
			m_camera = nullptr;
		}

		m_camera = new QCamera(m_devices[index], this);

		connect(m_camera, &QCamera::errorOccurred, this,
		        [](QCamera::Error e, const QString& s) {
			        qWarning() << "Camera error:" << e << s;
		        });

		m_session.setCamera(m_camera);
		m_camera->start();

		qDebug() << "Selected camera:" << index << m_devices[index].description();
		m_cameraIndex = index;

		return true;
	}

	bool CameraQt6::selectCameraFormat(int index)
	{
		if (index < 0 || index >= m_qtCameraFormats[m_cameraIndex].size() ||
		    m_cameraIndex < 0 || m_cameraIndex >= m_devices.size())
		{
			qWarning() << "selectCameraFormat invalid index:" << index;
			return false;
		}

		if (!m_camera)
		{
			qWarning() << "selectCameraFormat camera is null";
			return false;
		}

		auto fmt = m_qtCameraFormats.at(m_cameraIndex).at(index);
		this->m_camera->stop();
		this->m_camera->setCameraFormat(fmt);
		if (m_isPlaying)
		{
			this->m_camera->start();
		}

		qDebug() << "Applied camera format:"
				<< "cam=" << m_cameraIndex
				<< "fmtIndex=" << index
				<< "res=" << fmt.resolution()
				<< "fps=" << fmt.minFrameRate() << "-" << fmt.maxFrameRate();
		return true;
	}

	void CameraQt6::refreshCameras()
	{
		m_descriptions.clear();
		m_qtCameraFormats.clear();
		const auto def = QMediaDevices::defaultVideoInput();
		qDebug() << "defaultVideoInput isNull =" << def.isNull()
				<< "desc =" << def.description()
				<< "id =" << def.id();

		// 获取所有摄像头
		m_devices = QMediaDevices::videoInputs();
		qDebug() << "Cameras found:" << m_devices.size();

		// 遍历摄像头
		for (int i = 0; i < m_devices.size(); ++i)
		{
			const auto& dev = m_devices[i];
			qDebug() << "[" << i << "]" << dev.description() << dev.id();

			// 获取摄像头格式信息
			const auto formats = dev.videoFormats();
			qDebug() << "  formats count =" << formats.size();
			auto cameraDescriptions = fplayer::CameraDescription();
			cameraDescriptions.description = dev.description();
			cameraDescriptions.id = dev.id();

			// 遍历摄像头格式信息
			for (const QCameraFormat& fmt: formats)
			{
				QSize res = fmt.resolution();
				float minFps = fmt.minFrameRate();
				float maxFps = fmt.maxFrameRate();

				QString format = QString("%1x%2 fps: %3-%4")
				                 .arg(res.width())
				                 .arg(res.height())
				                 .arg(minFps)
				                 .arg(maxFps);

				if (!cameraDescriptions.formats.contains(format))
				{
					qDebug() << "    "
							<< format;
					cameraDescriptions.formats.push_back(format);

				}
			}
			m_descriptions.push_back({dev.description(), dev.id(), 0, cameraDescriptions.formats});
			m_qtCameraFormats.push_back(formats);
		}
	}

	QList<CameraDescription> CameraQt6::getDescriptions()
	{
		if (m_descriptions.empty())
		{
			this->refreshCameras();
		}
		return m_descriptions;
	}

	int CameraQt6::getIndex()
	{
		return m_cameraIndex;
	}

	void CameraQt6::pause()
	{
		m_camera->stop();
		m_isPlaying = false;
	}

	void CameraQt6::resume()
	{
		m_camera->start();
		m_isPlaying = true;
	}

	void CameraQt6::setPreviewTarget(const PreviewTarget& target)
	{
		auto* qtVideoWidget = static_cast<QVideoWidget*>(target.backend_hint);
		m_session.setVideoOutput(qtVideoWidget);
	}
}// fplayer