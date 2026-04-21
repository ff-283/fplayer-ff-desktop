#include "audioinputprobe.h"

#include <QSet>
#include <QStringList>

extern "C"
{
#include <libavdevice/avdevice.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

namespace fplayer::windows_api
{
	namespace
	{
		QStringList listDshowAudioDeviceNames()
		{
			QStringList result;
			const AVInputFormat* dshowFmt = av_find_input_format("dshow");
			if (!dshowFmt)
			{
				return result;
			}
			AVDeviceInfoList* devList = nullptr;
			const int ret = avdevice_list_input_sources(dshowFmt, nullptr, nullptr, &devList);
			if (ret < 0 || !devList)
			{
				return result;
			}
			for (int i = 0; i < devList->nb_devices; ++i)
			{
				const AVDeviceInfo* info = devList->devices[i];
				if (!info || !info->device_name)
				{
					continue;
				}
				const QString name = QString::fromUtf8(info->device_name).trimmed();
				if (name.isEmpty())
				{
					continue;
				}
				result << name;
			}
			avdevice_free_list_devices(&devList);
			return result;
		}
	}

	bool openDshowAudioInputWithFallback(const QString& requestedSource,
	                                     std::atomic<bool>& stopRequest,
	                                     AVFormatContext*& outIfmt,
	                                     QString& openedDeviceLabel,
	                                     QString& detailLog)
	{
		outIfmt = nullptr;
		openedDeviceLabel.clear();
		(void)stopRequest;
		struct Candidate
		{
			QString fmtName;
			QString deviceSpec;
			bool loopback = false;
		};
		QList<Candidate> candidates;
		const QString req = requestedSource.trimmed();
		if (req == QStringLiteral("system"))
		{
			candidates.push_back({QStringLiteral("dshow"), QStringLiteral("audio=virtual-audio-capturer"), false});
			const QStringList names = listDshowAudioDeviceNames();
			const auto addMatched = [&candidates, &names](const QStringList& keys) {
				for (const QString& n : names)
				{
					const QString lower = n.toLower();
					bool matched = false;
					for (const QString& k : keys)
					{
						if (lower.contains(k))
						{
							matched = true;
							break;
						}
					}
					if (matched)
					{
						candidates.push_back({QStringLiteral("dshow"), QStringLiteral("audio=") + n, false});
					}
				}
			};
			addMatched({QStringLiteral("stereo mix"), QStringLiteral("立体声混音"), QStringLiteral("what u hear"),
			            QStringLiteral("wave out"), QStringLiteral("loopback"), QStringLiteral("mixage st")});
			const QStringList commonLoopbackNames{
				QStringLiteral("Stereo Mix"),
				QStringLiteral("Stereo Mix (Realtek(R) Audio)"),
				QStringLiteral("立体声混音"),
				QStringLiteral("What U Hear"),
				QStringLiteral("Wave Out Mix"),
				QStringLiteral("WaveOut Mix")
			};
			for (const QString& n : commonLoopbackNames)
			{
				candidates.push_back({QStringLiteral("dshow"), QStringLiteral("audio=") + n, false});
			}
			candidates.push_back({QStringLiteral("wasapi"), QStringLiteral("default"), true});
		}
		else
		{
			QString reqName = req;
			if (reqName.startsWith(QStringLiteral("audio="), Qt::CaseInsensitive))
			{
				reqName = reqName.mid(QStringLiteral("audio=").size()).trimmed();
			}
			if (reqName.startsWith('"') && reqName.endsWith('"') && reqName.size() >= 2)
			{
				reqName = reqName.mid(1, reqName.size() - 2).trimmed();
			}
			candidates.push_back({QStringLiteral("dshow"), QStringLiteral("audio=") + reqName, false});
			candidates.push_back({QStringLiteral("dshow"), QStringLiteral("audio=\"") + reqName + QStringLiteral("\""), false});

			const QStringList names = listDshowAudioDeviceNames();
			for (const QString& n : names)
			{
				const QString nl = n.trimmed().toLower();
				const QString rl = reqName.toLower();
				if (nl == rl || nl.contains(rl) || rl.contains(nl))
				{
					candidates.push_back({QStringLiteral("dshow"), QStringLiteral("audio=") + n, false});
					candidates.push_back({QStringLiteral("dshow"), QStringLiteral("audio=\"") + n + QStringLiteral("\""), false});
				}
			}
		}
		{
			QSet<QString> dedup;
			QList<Candidate> uniq;
			for (const Candidate& c : candidates)
			{
				const QString key = c.fmtName + QStringLiteral("|") + c.deviceSpec + (c.loopback ? QStringLiteral("|1") : QStringLiteral("|0"));
				if (dedup.contains(key))
				{
					continue;
				}
				dedup.insert(key);
				uniq.push_back(c);
			}
			candidates = uniq;
		}

		QStringList errors;
		for (const Candidate& c : candidates)
		{
			const AVInputFormat* audioFmt = av_find_input_format(c.fmtName.toUtf8().constData());
			if (!audioFmt)
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
			if (c.loopback)
			{
				av_dict_set(&inOpts, "loopback", "1", 0);
			}
			// 统一降低输入侧缓冲，避免直播场景音频累积延迟。
			av_dict_set(&inOpts, "thread_queue_size", "64", 0);
			av_dict_set(&inOpts, "audio_buffer_size", "20", 0);
			const QByteArray devUtf8 = c.deviceSpec.toUtf8();
			const int ret = avformat_open_input(&ifmt, devUtf8.constData(), audioFmt, &inOpts);
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
