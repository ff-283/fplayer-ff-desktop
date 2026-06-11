#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QPainter>
#include <QSplashScreen>
#include <QSurfaceFormat>
#include <QTimer>
#include <exception>

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
	auto logEnv = [](const char* key) {
		const QByteArray val = qgetenv(key);
		LOG_INFO("[startup-env]", key, "=", val.isEmpty() ? "<empty>" : val.constData());
	};
	const QStringList libraryPaths = QCoreApplication::libraryPaths();
	LOG_INFO("[startup-env]", "appDir=", QCoreApplication::applicationDirPath().toUtf8().constData());
	LOG_INFO("[startup-env]", "cwd=", QDir::currentPath().toUtf8().constData());
	LOG_INFO("[startup-env]", "platform=", QGuiApplication::platformName().toUtf8().constData());
	LOG_INFO("[startup-env]", "qtPluginsPath=", QLibraryInfo::path(QLibraryInfo::PluginsPath).toUtf8().constData());
	LOG_INFO("[startup-env]", "libraryPaths=", libraryPaths.join(';').toUtf8().constData());
	const QSurfaceFormat defaultFmt = QSurfaceFormat::defaultFormat();
	LOG_INFO("[startup-env]",
	         "defaultSurfaceFormat",
	         " renderableType=",
	         static_cast<int>(defaultFmt.renderableType()),
	         " profile=",
	         static_cast<int>(defaultFmt.profile()),
	         " version=",
	         defaultFmt.majorVersion(),
	         ".",
	         defaultFmt.minorVersion(),
	         " swapBehavior=",
	         static_cast<int>(defaultFmt.swapBehavior()),
	         " swapInterval=",
	         defaultFmt.swapInterval());
	logEnv("PATH");
	logEnv("QT_PLUGIN_PATH");
	logEnv("QT_QPA_PLATFORM");
	logEnv("QT_OPENGL");
	logEnv("QSG_RHI_BACKEND");
	logEnv("QT_ANGLE_PLATFORM");
	// 解析命令行参数
	QCommandLineParser parser;
	parser.addOption({{"b", "backend"}, "Backend type (0=FFmpeg, 1=Qt6)", "backend", "0"});
	parser.process(app);

	// 应用级图标（任务栏/Alt-Tab/托盘等更统一）
	app.setWindowIcon(QIcon(":/icon/icon.png"));

	// 在这里进行转换
	int backendInt = parser.value("backend").toInt();
	fplayer::MediaBackendType backendType;

	switch (backendInt)
	{
	case 0:
		backendType = fplayer::MediaBackendType::FFmpeg;
		LOG_DEBUG("Using FFmpeg backend");
		break;
	case 1:
		backendType = fplayer::MediaBackendType::Qt6;
		LOG_DEBUG("Using Qt6 backend");
		break;
	default:
		qCritical() << "Invalid backend type:" << backendInt;
		return 1;
	}

	// 传递转换后的枚举类型
	try
	{
		// 构建启动画面：紫色磨砂圆角矩形 + 居中图标
		QPixmap splashPixmap(320, 320);
		splashPixmap.fill(Qt::transparent);
		{
			QPainter p(&splashPixmap);
			p.setRenderHint(QPainter::Antialiasing);
			p.setBrush(QColor(120, 80, 200, 200));
			p.setPen(Qt::NoPen);
			p.drawRoundedRect(QRectF(40, 40, 240, 240), 32, 32);
			QPixmap icon(":/icon/icon.png");
			const int iconSz = 100;
			QPixmap scaled = icon.scaled(iconSz, iconSz, Qt::KeepAspectRatio, Qt::SmoothTransformation);
			p.drawPixmap((320 - iconSz) / 2, (320 - iconSz) / 2, scaled);
		}
		QSplashScreen splash(splashPixmap, Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint);
		splash.setAttribute(Qt::WA_TranslucentBackground);
		splash.show();
		QApplication::processEvents();

		CaptureWindow main(nullptr, backendType);
		main.show();

		// 延迟初始化非必要模块，加速首帧显示
		QTimer::singleShot(0, &main, [&main, &splash]() {
			main.performDeferredInit();
			splash.finish(&main);
		});

		app.exec();
		return 0;
	}
	catch (const std::exception& e)
	{
		LOG_ERROR("[startup]", "CaptureWindow startup failed (std::exception): ", e.what());
	}
	catch (...)
	{
		LOG_ERROR("[startup]", "CaptureWindow startup failed (unknown exception)");
	}
	return 2;
}