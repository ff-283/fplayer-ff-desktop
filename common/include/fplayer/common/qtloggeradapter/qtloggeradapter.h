/*************************************************
  * 描述：接管qDebug输出内容为Logger日志打印
  *
  * File：qtloggeradapter.h
  * Date：2026/2/27
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_QTLOGGERADAPTER_H
#define FPLAYER_DESKETOP_QTLOGGERADAPTER_H

#include <fplayer/common/export.h>

namespace fplayer
{
    // 防止递归：如果 spdlog 的 sink/formatter 间接触发 Qt 日志，会无限递归
    static thread_local bool g_inQtHandler = false;

    void FPLAYER_COMMON_EXPORT qtToLoggerHandler(QtMsgType type,
                                                 const QMessageLogContext& ctx,
                                                 const QString& msg);
}
#endif //FPLAYER_DESKETOP_QTLOGGERADAPTER_H
