#include <fplayer/backend/media_qt6/playerqt6.h>

#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoWidget>
#include <QVideoSink>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QUrl>
#include <QFileInfo>
#include <QImage>

#include <fplayer/common/screenframebus/screenframebus.h>
#include <logger/logger.h>

namespace fplayer
{

PlayerQt6::PlayerQt6()
{
	m_backend = MediaBackendType::Qt6;

	m_mediaPlayer = new QMediaPlayer(this);
	m_audioOutput = new QAudioOutput(this);

#if defined(_WIN32)
	// Windows: QMediaPlayer uses Windows Media Foundation by default.
	// No extra initialization needed.
#else
	// TODO: Linux may need GStreamer pipeline configuration or additional setup.
#endif

	m_mediaPlayer->setAudioOutput(m_audioOutput);
	m_audioOutput->setVolume(1.0f);

	// 默认循环播放，与 FFmpeg 后端行为一致。
	m_mediaPlayer->setLoops(QMediaPlayer::Infinite);

	connect(m_mediaPlayer, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error error, const QString& errorString) {
		Q_UNUSED(error);
		LOG_ERROR("[PlayerQt6] error: ", errorString.toStdString());
		m_isPlaying.store(false);
	});
}

PlayerQt6::~PlayerQt6()
{
	stop();
}

bool PlayerQt6::openFile(const QString& filePath)
{
	if (filePath.isEmpty())
	{
		return false;
	}

	// 停止当前播放（如有）
	stop();

	const QUrl url = QUrl::fromLocalFile(QFileInfo(filePath).absoluteFilePath());
	m_mediaPlayer->setSource(url);

	LOG_INFO("[PlayerQt6] openFile: ", filePath.toStdString());

	play();
	return true;
}

void PlayerQt6::play()
{
#if defined(_WIN32)
	m_mediaPlayer->play();
#else
	// TODO: Linux — ensure pipeline is ready before play.
	m_mediaPlayer->play();
#endif
	m_isPlaying.store(true);
}

void PlayerQt6::pause()
{
	m_mediaPlayer->pause();
	m_isPlaying.store(false);
}

void PlayerQt6::stop()
{
	m_mediaPlayer->stop();
	// 清空 source 以释放文件句柄
	m_mediaPlayer->setSource(QUrl());
	m_isPlaying.store(false);
}

bool PlayerQt6::isPlaying() const
{
	return m_isPlaying.load();
}

qint64 PlayerQt6::durationMs() const
{
	return m_mediaPlayer->duration();
}

qint64 PlayerQt6::positionMs() const
{
	return m_mediaPlayer->position();
}

bool PlayerQt6::seekMs(qint64 positionMs)
{
	if (positionMs < 0)
	{
		return false;
	}
	m_mediaPlayer->setPosition(positionMs);
	return true;
}

void PlayerQt6::setPlaybackRate(double rate)
{
	const double clamped = qBound(1.0, rate, 2.0);
	m_mediaPlayer->setPlaybackRate(clamped);
}

double PlayerQt6::playbackRate() const
{
	return m_mediaPlayer->playbackRate();
}

void PlayerQt6::setVolume(const float volume)
{
	if (m_audioOutput)
	{
		m_audioOutput->setVolume(qBound(0.0f, volume, 1.0f));
	}
}

float PlayerQt6::volume() const
{
	return m_audioOutput ? m_audioOutput->volume() : 1.0f;
}

QString PlayerQt6::debugStats() const
{
	const auto state = m_mediaPlayer->playbackState();
	const char* stateStr = "Stopped";
	switch (state)
	{
	case QMediaPlayer::PlayingState:
		stateStr = "Playing";
		break;
	case QMediaPlayer::PausedState:
		stateStr = "Paused";
		break;
	case QMediaPlayer::StoppedState:
		stateStr = "Stopped";
		break;
	}
	return QString("Qt6 %1 pos:%2ms dur:%3ms rate:%4x")
		.arg(QString::fromLatin1(stateStr))
		.arg(positionMs())
		.arg(durationMs())
		.arg(playbackRate());
}

void PlayerQt6::setPreviewTarget(const PreviewTarget& target)
{
	// 断开旧的 video sink
	if (m_videoSink && !m_composeStreamBusId.isEmpty())
	{
		QObject::disconnect(m_videoSink, &QVideoSink::videoFrameChanged,
		                    this, &PlayerQt6::onVideoFrameChanged);
	}
	m_videoWidget = nullptr;
	m_videoSink = nullptr;

	if (!target.backend_hint)
	{
		return;
	}
	// 仅接受 QVideoWidget 类型的 backend_hint（FFmpeg 后端传入的是 FGLWidget，忽略）
	auto* w = qobject_cast<QVideoWidget*>(static_cast<QObject*>(target.backend_hint));
	if (!w)
	{
		return;
	}
	m_videoWidget = w;
	m_mediaPlayer->setVideoOutput(m_videoWidget);
	m_videoSink = m_videoWidget->videoSink();
}

void PlayerQt6::setComposeStreamBusId(const QString& sourceId)
{
	// 断开旧连接
	if (m_videoSink && !m_composeStreamBusId.isEmpty())
	{
		QObject::disconnect(m_videoSink, &QVideoSink::videoFrameChanged,
		                    this, &PlayerQt6::onVideoFrameChanged);
	}

	m_composeStreamBusId = sourceId;

	// 建立新连接
	if (m_videoSink && !sourceId.isEmpty())
	{
		QObject::connect(m_videoSink, &QVideoSink::videoFrameChanged,
		                 this, &PlayerQt6::onVideoFrameChanged);
	}
}

void PlayerQt6::onVideoFrameChanged(const QVideoFrame& frame)
{
	if (!frame.isValid())
	{
		return;
	}
	if (m_composeStreamBusId.isEmpty())
	{
		return;
	}
	static int frameCount = 0;
	if (++frameCount <= 3)
	{
		LOG_INFO("[PlayerQt6] videoFrameChanged #", frameCount,
		         " size=", frame.width(), "x", frame.height(),
		         " format=", static_cast<int>(frame.surfaceFormat().pixelFormat()));
	}
	publishYuvFromQVideoFrame(frame);
}

static void rgbToYuv420p(const QImage& img, QByteArray& y, QByteArray& u, QByteArray& v,
                         int& width, int& height, int& yStride, int& uStride, int& vStride)
{
	width = img.width();
	height = img.height();
	yStride = width;
	uStride = (width + 1) / 2;
	vStride = (width + 1) / 2;

	const int ySize = yStride * height;
	const int uvSize = uStride * ((height + 1) / 2);
	y.resize(ySize);
	u.resize(uvSize);
	v.resize(uvSize);

	auto* yPtr = reinterpret_cast<uint8_t*>(y.data());
	auto* uPtr = reinterpret_cast<uint8_t*>(u.data());
	auto* vPtr = reinterpret_cast<uint8_t*>(v.data());

	for (int row = 0; row < height; ++row)
	{
		const auto* rgb = reinterpret_cast<const uint8_t*>(img.constScanLine(row));
		auto* yRow = yPtr + row * yStride;

		for (int col = 0; col < width; ++col)
		{
			const int r = rgb[col * 4 + 2];
			const int g = rgb[col * 4 + 1];
			const int b = rgb[col * 4 + 0];

			// ITU-R BT.601
			yRow[col] = static_cast<uint8_t>(qBound(0, ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16, 255));

			if ((row & 1) == 0 && (col & 1) == 0)
			{
				const int uvIdx = (row / 2) * uStride + (col / 2);
				uPtr[uvIdx] = static_cast<uint8_t>(qBound(0, ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128, 255));
				vPtr[uvIdx] = static_cast<uint8_t>(qBound(0, ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128, 255));
			}
		}
	}
}

void PlayerQt6::publishYuvFromQVideoFrame(const QVideoFrame& frame)
{
	QByteArray y, u, v;
	int width = 0, height = 0;
	int yStride = 0, uStride = 0, vStride = 0;

	const auto format = frame.surfaceFormat().pixelFormat();

	if (format == QVideoFrameFormat::Format_YUV420P)
	{
		// 直接拷贝 YUV420P
		auto mapped = frame;
		if (!mapped.map(QVideoFrame::ReadOnly)) return;

		width = mapped.width();
		height = mapped.height();
		yStride = mapped.bytesPerLine(0);
		uStride = mapped.bytesPerLine(1);
		vStride = mapped.bytesPerLine(2);

		const int uvHeight = (height + 1) / 2;
		y = QByteArray(reinterpret_cast<const char*>(mapped.bits(0)), yStride * height);
		u = QByteArray(reinterpret_cast<const char*>(mapped.bits(1)), uStride * uvHeight);
		v = QByteArray(reinterpret_cast<const char*>(mapped.bits(2)), vStride * uvHeight);

		mapped.unmap();
	}
	else if (format == QVideoFrameFormat::Format_NV12 || format == QVideoFrameFormat::Format_NV21)
	{
		// NV12(UV交错) / NV21(VU交错) → YUV420P：分离交错的 UV 平面
		const bool nv21 = (format == QVideoFrameFormat::Format_NV21);
		auto mapped = frame;
		if (!mapped.map(QVideoFrame::ReadOnly)) return;

		width = mapped.width();
		height = mapped.height();
		yStride = mapped.bytesPerLine(0);
		const int uvPackedStride = mapped.bytesPerLine(1);
		uStride = (width + 1) / 2;
		vStride = uStride;

		const int uvHeight = (height + 1) / 2;
		y = QByteArray(reinterpret_cast<const char*>(mapped.bits(0)), yStride * height);

		const auto* uvSrc = reinterpret_cast<const uint8_t*>(mapped.bits(1));
		u.resize(uStride * uvHeight);
		v.resize(vStride * uvHeight);
		auto* uDst = reinterpret_cast<uint8_t*>(u.data());
		auto* vDst = reinterpret_cast<uint8_t*>(v.data());

		for (int row = 0; row < uvHeight; ++row)
		{
			const auto* srcRow = uvSrc + row * uvPackedStride;
			auto* uRow = uDst + row * uStride;
			auto* vRow = vDst + row * vStride;
			for (int col = 0; col < uStride; ++col)
			{
				const uint8_t a = srcRow[col * 2];
				const uint8_t b = srcRow[col * 2 + 1];
				uRow[col] = nv21 ? b : a;
				vRow[col] = nv21 ? a : b;
			}
		}

		mapped.unmap();
	}
	else
	{
		// 其他格式：通过 QImage 中转
		QImage img = frame.toImage();
		if (img.isNull())
		{
			static int qimgFailCount = 0;
			if (++qimgFailCount <= 3)
				LOG_ERROR("[PlayerQt6] QVideoFrame::toImage() failed, format=", static_cast<int>(format));
			return;
		}
		if (img.format() != QImage::Format_ARGB32 && img.format() != QImage::Format_RGB32)
		{
			img.convertTo(QImage::Format_ARGB32);
		}
		rgbToYuv420p(img, y, u, v, width, height, yStride, uStride, vStride);
	}

	if (!y.isEmpty() && width > 0 && height > 0)
	{
		ScreenFrameBus::instance().publish(y, u, v, width, height,
		                                   yStride, uStride, vStride,
		                                   m_composeStreamBusId);
	}
}

} // namespace fplayer
