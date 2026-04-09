/****************************************************************************
**
**  FPlayer_Backend_Media_FFmpeg Export Header
**
**  本文件定义了 FPLAYER_BACKEND_MEDIA_FFMPEG 模块的导入导出宏。
**  在构建动态库时，FPLAYER_BACKEND_MEDIA_FFMPEG_LIBRARY 会被定义，
**  此时 FPLAYER_BACKEND_MEDIA_FFMPEG_EXPORT 用于导出符号。
**  在使用该库时，没有定义 FPLAYER_BACKEND_MEDIA_FFMPEG_LIBRARY，
**  此时 FPLAYER_BACKEND_MEDIA_FFMPEG_EXPORT 用于导入符号。
**
**  适用于跨平台 (Windows/Linux/macOS) 的 Qt 动态库构建。
****************************************************************************/
#ifndef FPLAYER_BACKEND_MEDIA_FFMPEG_EXPORT_H
#define FPLAYER_BACKEND_MEDIA_FFMPEG_EXPORT_H

#include <QtCore/qglobal.h>

#if defined(FPLAYER_BACKEND_MEDIA_FFMPEG_LIBRARY)
#  define FPLAYER_BACKEND_MEDIA_FFMPEG_EXPORT Q_DECL_EXPORT
#else
#  define FPLAYER_BACKEND_MEDIA_FFMPEG_EXPORT Q_DECL_IMPORT
#endif

#endif // FPlayer_Backend_Media_FFmpeg_EXPORT_H
