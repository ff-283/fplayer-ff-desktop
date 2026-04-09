/****************************************************************************
**
**  FPlayer_Common Export Header
**
**  本文件定义了 FPLAYER_COMMON 模块的导入导出宏。
**  在构建动态库时，FPLAYER_COMMON_LIBRARY 会被定义，
**  此时 FPLAYER_COMMON_EXPORT 用于导出符号。
**  在使用该库时，没有定义 FPLAYER_COMMON_LIBRARY，
**  此时 FPLAYER_COMMON_EXPORT 用于导入符号。
**
**  适用于跨平台 (Windows/Linux/macOS) 的 Qt 动态库构建。
****************************************************************************/
#ifndef FPLAYER_COMMON_EXPORT_H
#define FPLAYER_COMMON_EXPORT_H

#include <QtCore/qglobal.h>

#if defined(FPLAYER_COMMON_LIBRARY)
#  define FPLAYER_COMMON_EXPORT Q_DECL_EXPORT
#else
#  define FPLAYER_COMMON_EXPORT Q_DECL_IMPORT
#endif

#endif // FPlayer_Common_EXPORT_H
