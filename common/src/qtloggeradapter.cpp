#include <fplayer/common/qtloggeradapter/qtloggeradapter.h>
#include <QCoreApplication>
#include <QDateTime>
#include <QMutex>
#include <QThread>
#include <QMessageLogContext>
#include <logger/logger.h>

void fplayer::qtToLoggerHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (g_inQtHandler) return;
    g_inQtHandler = true;

    // 你可以按需把这些信息拼进日志：文件/行号/函数名/分类(category)
    const char* file = ctx.file ? ctx.file : "";
    const char* func = ctx.function ? ctx.function : "";
    const char* cat = ctx.category ? ctx.category : "";

    // QString -> std::string（UTF-8）
    std::string text = msg.toUtf8().constData();

    // 结构化一点：把上下文带上
    // 注意：不要在这里调用 qDebug 之类，会递归
    switch (type)
    {
        case QtDebugMsg:
            LOG_DEBUG(cat, text, file, ctx.line, func);
            break;
        case QtInfoMsg:
            LOG_INFO(cat, text, file, ctx.line, func);
            break;
        case QtWarningMsg:
            LOG_WARN(cat, text, file, ctx.line, func);
            break;
        case QtCriticalMsg:
            LOG_ERROR(cat, text, file, ctx.line, func);
            break;
        case QtFatalMsg:
            LOG_CRITI(cat, text, file, ctx.line, func);
            g_inQtHandler = false;
            abort(); // Qt 约定：fatal 要终止
    }

    g_inQtHandler = false;
}
