#ifndef FPLAYER_BACKEND_STREAM_FFMPEG_HELPERS_H
#define FPLAYER_BACKEND_STREAM_FFMPEG_HELPERS_H

#include <QList>
#include <QString>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

namespace fplayer::streamffmpeg_helpers
{
	enum class PushInputKind
	{
		ComposeScene,
		ScreenCapture,
		ScreenPreview,
		CameraCapture,
		CameraPreview,
		FileTranscode,
		Remux
	};

	struct PushInputRoute
	{
		PushInputKind kind = PushInputKind::Remux;
		QString spec;
	};

	struct CaptureParams
	{
		QString device;
		QString source;
		QString audioInputSource = QStringLiteral("off");
		QString audioOutputSource = QStringLiteral("off");
		QString videoEncoder = QStringLiteral("auto");
		int fps = 30;
		int width = 0;
		int height = 0;
		int outWidth = 0;
		int outHeight = 0;
		int x = 0;
		int y = 0;
		int bitrateKbps = 0;
	};

	struct VideoEncoderChoice
	{
		const AVCodec* codec = nullptr;
		QString name;
		bool isHardware = false;
	};

	void appendLimited(QString& buf, const QString& line);
	PushInputRoute parsePushInputRoute(const QString& inputUrl);
	QString pushStartedLog(PushInputKind kind);
	CaptureParams parseCaptureParams(const QString& spec);
	QList<VideoEncoderChoice> pickVideoEncoderCandidates(const QString& prefer);
	bool canCreateD3D11HwDevice();
	QString encoderHwFramesHint(const AVCodec* enc);
	AVPixelFormat pickEncoderPixelFormat(const AVCodec* enc, bool preferHardware);
	int estimateBitrateKbps(int width, int height, int fps);
}

#endif // FPLAYER_BACKEND_STREAM_FFMPEG_HELPERS_H
