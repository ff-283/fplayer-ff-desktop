#include <QApplication>
#include <QCommandLineParser>
#include <QSurfaceFormat>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <logger/logger.h>

#include <fplayer/widget/capturewindow.h>
#include <fplayer/common/qtloggeradapter/qtloggeradapter.h>
#include <fplayer/api/media/mediabackendtype.h>

int main(int argc, char* argv[])
{
	// 设置Windows控制台编码
#ifdef Q_OS_WIN
	// 设置控制台输出代码页为 UTF-8
	SetConsoleOutputCP(CP_UTF8);
	// 可选：设置控制台输入为 UTF-8
	SetConsoleCP(CP_UTF8);
#endif

	// 将 Qt 默认的日志系统重定向到 Logger，
	qInstallMessageHandler(fplayer::qtToLoggerHandler);

	// 在创建 QApplication / 任何 OpenGL 窗口之前设置默认表面格式，否则 swapInterval 在部分平台上不生效，易出现画面撕裂。
	{
		QSurfaceFormat fmt;
		fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
		fmt.setSwapInterval(1);
		QSurfaceFormat::setDefaultFormat(fmt);
	}

	// QApplication::setStyle("Fusion");
	QApplication app(argc, argv);
	// 解析命令行参数
	QCommandLineParser parser;
	parser.addOption({{"b", "backend"}, "Backend type (0=Qt6, 1=FFmpeg)", "backend", "0"});
	parser.process(app);

	// 应用级图标（任务栏/Alt-Tab/托盘等更统一）
	app.setWindowIcon(QIcon(":/icon/img.png"));

	// 在这里进行转换
	int backendInt = parser.value("backend").toInt();
	fplayer::MediaBackendType backendType;

	switch (backendInt)
	{
	case 0:
		backendType = fplayer::MediaBackendType::Qt6;
		LOG_DEBUG("Using Qt6 backend");
		break;
	case 1:
		backendType = fplayer::MediaBackendType::FFmpeg;
		LOG_DEBUG("Using FFmpeg backend");
		break;
	default:
		qCritical() << "Invalid backend type:" << backendInt;
		return 1;
	}

	// 传递转换后的枚举类型
	CaptureWindow main(nullptr, backendType);

	main.show();
	app.exec();
	return 0;
}