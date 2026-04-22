#include "audioinputprobe_linux.h"

#include <QList>
#include <QStringList>

extern "C"
{
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

namespace fplayer::linux_api
{
	bool openLinuxAudioInputWithFallback(const QString& requestedSource,
	                                     std::atomic<bool>& stopRequest,
	                                     AVFormatContext*& outIfmt,
	                                     QString& openedDeviceLabel,
	                                     QString& detailLog)
	{
		(void)stopRequest;
		outIfmt = nullptr;
		openedDeviceLabel.clear();
		struct Candidate
		{
			QString fmtName;
			QString deviceSpec;
		};
		QList<Candidate> candidates;
		const QString req = requestedSource.trimmed().toLower();
		if (req.isEmpty() || req == QStringLiteral("system") || req == QStringLiteral("default"))
		{
			candidates.push_back({QStringLiteral("pulse"), QStringLiteral("default")});
			candidates.push_back({QStringLiteral("alsa"), QStringLiteral("default")});
		}
		else if (req.startsWith(QStringLiteral("alsa:")))
		{
			candidates.push_back({QStringLiteral("alsa"), requestedSource.mid(QStringLiteral("alsa:").size()).trimmed()});
		}
		else if (req.startsWith(QStringLiteral("pulse:")))
		{
			candidates.push_back({QStringLiteral("pulse"), requestedSource.mid(QStringLiteral("pulse:").size()).trimmed()});
		}
		else
		{
			candidates.push_back({QStringLiteral("pulse"), requestedSource});
			candidates.push_back({QStringLiteral("alsa"), requestedSource});
			candidates.push_back({QStringLiteral("alsa"), QStringLiteral("default")});
		}

		QStringList errors;
		for (const Candidate& c : candidates)
		{
			const AVInputFormat* inFmt = av_find_input_format(c.fmtName.toUtf8().constData());
			if (!inFmt)
			{
				errors << QStringLiteral("%1:%2 -> input-format-not-found").arg(c.fmtName, c.deviceSpec);
				continue;
			}
			AVFormatContext* ifmt = avformat_alloc_context();
			if (!ifmt)
			{
				errors << QStringLiteral("%1:%2 -> OOM").arg(c.fmtName, c.deviceSpec);
				continue;
			}
			AVDictionary* inOpts = nullptr;
			av_dict_set(&inOpts, "thread_queue_size", "64", 0);
			av_dict_set(&inOpts, "audio_buffer_size", "20", 0);
			const QByteArray devUtf8 = c.deviceSpec.toUtf8();
			const int ret = avformat_open_input(&ifmt, devUtf8.constData(), inFmt, &inOpts);
			av_dict_free(&inOpts);
			if (ret >= 0)
			{
				outIfmt = ifmt;
				openedDeviceLabel = QStringLiteral("%1:%2").arg(c.fmtName, c.deviceSpec);
				detailLog = QStringLiteral("ok=%1").arg(openedDeviceLabel);
				return true;
			}
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			errors << QStringLiteral("%1:%2 -> %3").arg(c.fmtName, c.deviceSpec, QString::fromUtf8(errbuf));
			avformat_free_context(ifmt);
		}
		detailLog = errors.isEmpty() ? QStringLiteral("no-candidate-device") : errors.join(QStringLiteral("; "));
		return false;
	}
}
