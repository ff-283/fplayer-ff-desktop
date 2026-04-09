/*************************************************
  * 描述：
  *
  * File：previewtarget.h
  * Date：2026/3/1
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_PREVIEWTARGET_H
#define FPLAYER_DESKETOP_PREVIEWTARGET_H

namespace fplayer
{
	// 原生窗口信息（跨平台扩展用）
	struct NativeWindow // 没什么大用
	{
#ifdef _WIN32
		void* hwnd = nullptr;// HWND
#elif __APPLE__
		void* nsView = nullptr;// NSView*
#else
		void* handle = nullptr;// X11/Wayland handle (按你的平台策略)
#endif
		int width = 0;
		int height = 0;
		double device_pixel_ratio = 1.0;// 高 DPI 可用
	};

	struct PreviewTarget
	{
		NativeWindow window;// 给 FFmpeg 用
		// 可选：给特定 backend 的 hint（例如 Qt6 可以用 QVideoSink*）
		// 注意：API 层不包含 Qt 头文件，只能用 void*。
		void* backend_hint = nullptr;

		// 扩展字段（例如：是否保持宽高比、渲染模式等）
		// uint32_t flags = 0;
	};
}
#endif //FPLAYER_DESKETOP_PREVIEWTARGET_H