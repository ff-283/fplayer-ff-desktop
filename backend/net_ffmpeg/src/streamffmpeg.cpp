#include <fplayer/backend/net_ffmpeg/streamffmpeg.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

fplayer::StreamFFmpeg::StreamFFmpeg(QObject* parent) : QObject(parent)
{
}

fplayer::StreamFFmpeg::~StreamFFmpeg()
{
	stop();
}

bool fplayer::StreamFFmpeg::startPush(const QString& inputUrl, const QString& outputUrl)
{
	if (inputUrl.trimmed().isEmpty() || outputUrl.trimmed().isEmpty())
	{
		m_lastError = QStringLiteral("推流输入或输出地址为空");
		return false;
	}
	// 最小可用策略：转封装直推（copy）。
	return startWithArgs({"-re", "-i", inputUrl, "-c", "copy", "-f", "flv", outputUrl});
}

bool fplayer::StreamFFmpeg::startPull(const QString& inputUrl, const QString& outputUrl)
{
	if (inputUrl.trimmed().isEmpty() || outputUrl.trimmed().isEmpty())
	{
		m_lastError = QStringLiteral("拉流输入或输出地址为空");
		return false;
	}
	// 拉流默认落地为 mp4，保持简单可验证。
	return startWithArgs({"-i", inputUrl, "-c", "copy", "-y", outputUrl});
}

void fplayer::StreamFFmpeg::stop()
{
	if (!m_ffmpegProcess)
	{
		return;
	}
	if (m_ffmpegProcess->state() != QProcess::NotRunning)
	{
		m_ffmpegProcess->terminate();
		if (!m_ffmpegProcess->waitForFinished(1500))
		{
			m_ffmpegProcess->kill();
			m_ffmpegProcess->waitForFinished(800);
		}
	}
	delete m_ffmpegProcess;
	m_ffmpegProcess = nullptr;
}

bool fplayer::StreamFFmpeg::isRunning() const
{
	return m_ffmpegProcess && m_ffmpegProcess->state() != QProcess::NotRunning;
}

QString fplayer::StreamFFmpeg::lastError() const
{
	return m_lastError;
}

QString fplayer::StreamFFmpeg::recentLog() const
{
	return m_recentLog;
}

int fplayer::StreamFFmpeg::lastExitCode() const
{
	return m_lastExitCode;
}

bool fplayer::StreamFFmpeg::startWithArgs(const QStringList& args)
{
	stop();
	m_lastError.clear();
	m_recentLog.clear();
	m_lastExitCode = 0;
	const QString ffmpegProgram = resolveFfmpegProgram();
	if (ffmpegProgram.isEmpty())
	{
		m_lastError = QStringLiteral("未找到 ffmpeg 可执行文件");
		return false;
	}
	m_ffmpegProcess = new QProcess(this);
	m_ffmpegProcess->setProgram(ffmpegProgram);
	m_ffmpegProcess->setArguments(args);
	m_ffmpegProcess->setProcessChannelMode(QProcess::MergedChannels);
	QObject::connect(m_ffmpegProcess, &QProcess::readyReadStandardOutput, this, [this]() {
		if (!m_ffmpegProcess)
		{
			return;
		}
		m_recentLog += QString::fromUtf8(m_ffmpegProcess->readAllStandardOutput());
		if (m_recentLog.size() > 16000)
		{
			m_recentLog = m_recentLog.right(16000);
		}
	});
	QObject::connect(m_ffmpegProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
	                 [this](int exitCode, QProcess::ExitStatus) {
		                 m_lastExitCode = exitCode;
	                 });
	m_ffmpegProcess->start();
	if (!m_ffmpegProcess->waitForStarted(1200))
	{
		m_lastError = QStringLiteral("ffmpeg 启动失败");
		return false;
	}
	return true;
}

QString fplayer::StreamFFmpeg::resolveFfmpegProgram() const
{
	const QString appDir = QCoreApplication::applicationDirPath();
	const QString localExe = QDir(appDir).filePath("ffmpeg.exe");
	if (QFileInfo::exists(localExe))
	{
		return localExe;
	}
	return QStringLiteral("ffmpeg");
}
