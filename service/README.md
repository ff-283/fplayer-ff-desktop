服务层

业务集成，调用底层基础功能模块，供UI层直接调用

---
## 如何将两个后端(qt、ffmpeg)的视频输出到QWidget上？

service创建一个公有方法，两个私有方法，公有方法中，根据当前后端摄像头的类型，调用两个私有方法之一

> > 方案二。https://chatgpt.com/share/69a42770-ce94-8007-8726-8fcdf83a728a
>
> 你这个思路里只有一个硬伤（不是“哲学问题”，是“物理问题”）：**“原生窗口句柄 -> QWidget\*”在 Qt 里不可逆**。
>
> - `QWidget::winId()` 给你的是 **native window handle**（Windows 上是 `HWND`，Linux 上可能是 `WId` 对应 X11/Wayland 的东西）。
> - 这个 handle **不是** QWidget 的地址，也不携带“这是哪个 QWidget”的身份信息。
> - Qt 也没有官方 API 让你 “从 HWND 找回 QWidget*”。（你可以自己维护映射表，但那就不是“转换回去”，而是“查表拿回去”。）
>
> 所以：**“只传句柄给 backend，然后 qt6 backend 再转回 QWidget\* 继续用 QMediaCaptureSession + QVideoWidget 那套逻辑”这条路走不通**（除非你额外做一层 registry）。
>
> 下面给你两个可落地方案，按你“句柄路线”来，但把不可逆这点处理干净。
>
> ------
>
> ## 方案 1：仍然只传句柄，但 Qt6 后端别再走 QVideoWidget，而是“按句柄渲染”
>
> 也就是说：你统一接口就是“backend 拿到原生窗口句柄”，然后：
>
> - **FFmpeg backend**：用句柄创建渲染上下文（SDL2 / D3D11 / OpenGL / Vulkan），自己把解码帧画上去。
> - **Qt6 backend**：也不要再依赖 `QVideoWidget`，同样走“按句柄渲染”路线（例如 QRhi / OpenGL / D3D）。
>   这能做到真正统一，但代价是：Qt6 后端你得自己处理摄像头采集到的帧（不用 QMediaCaptureSession 的“自动显示”了）。
>
> 这方案最“架构纯粹”，但你等于把 Qt6 后端从“Qt 多媒体帮我显示”切到“Qt 只帮我采集”。
>
> 如果你现在主要目标是**快速落地**，那我更推荐方案 2。
>
> ------
>
> ## 方案 2（推荐）：句柄只给 FFmpeg 用；Qt6 后端继续用 QWidget*，但不污染抽象
>
> 做法是：你的 `bindCameraPreview(QWidget*)` 还是收 QWidget*，service 内部同时准备两种“目标”：
>
> - 给 FFmpeg：**原生句柄**（跨平台 `NativeWindow`）
> - 给 Qt6：**一个由 service 创建的 QVideoWidget（或 QVideoSink）**，它被嵌进用户传进来的 widget 里
>
> 关键点：你不需要把 QWidget* 传到 backend-ffmpeg；也不需要把句柄“转回 QWidget*”。
>
> ### 为什么这不算“破坏抽象”？
>
> 因为抽象可以是“一个 PreviewTarget”，里面有不同后端需要的字段；backend 自己取它能用的那部分就行。
>
> 例如：
>
> ```cpp
> // runtime/api: 不依赖 QtWidgets
> struct NativeWindow {
> #ifdef _WIN32
>     void* hwnd = nullptr;        // HWND
> #elif __APPLE__
>     void* nsView = nullptr;      // NSView*
> #else
>     void* handle = nullptr;      // X11/Wayland handle (按你的平台策略)
> #endif
> };
> 
> struct PreviewTarget {
>     NativeWindow win;            // 给 FFmpeg 用
>     void* backend_hint = nullptr; // 给特定 backend 的扩展（例如 Qt6）
> };
> ```
>
> 在 **service** 里：
>
> ```cpp
> void Service::bindCameraPreview(QWidget* widget)
> {
>     PreviewTarget t{};
>     t.win.hwnd = reinterpret_cast<void*>(widget->winId()); // Windows 示例
> 
>     // Qt6 专用：在 widget 里创建一个 QVideoWidget 并放进去
>     auto video = new QVideoWidget(widget);
>     if (!widget->layout()) {
>         auto lay = new QVBoxLayout(widget);
>         lay->setContentsMargins(0,0,0,0);
>         lay->setSpacing(0);
>     }
>     widget->layout()->addWidget(video);
> 
>     t.backend_hint = static_cast<void*>(video); // 只给 Qt6 backend 识别使用
> 
>     m_runtime->setPreviewTarget(t);
> }
> ```
>
> - `PreviewTarget` 在 runtime/backend 这层只是个 POD（普通结构体），不引入 QtWidgets 依赖。
> - **backend-qt6**（它本来就依赖 QtWidgets/QtMultimedia）看到 `backend_hint` 就把它当 `QVideoWidget*` 用。
> - **backend-ffmpeg** 完全忽略 `backend_hint`，只用 `NativeWindow`。
>
> 这就是“抽象不是要求所有 backend 用同一招，而是同一接口能喂给不同 backend 足够的信息”。
>
> ------
>
> # 你要的：FFmpeg backend “拿句柄渲染到 QWidget 区域”怎么做？
>
> 核心结论：FFmpeg 负责解码，不负责显示。你需要一个渲染层。最省事且工程上稳的是 **SDL2**（跨平台、能直接从现有窗口句柄创建窗口）。
>
> ## FFmpeg + SDL2（Windows 举例，最常用也最顺滑）
>
> ### 1）用 HWND 绑定到 SDL 窗口
>
> ```cpp
> SDL_Window* sdlWin = SDL_CreateWindowFrom(hwnd); // hwnd 就是 widget->winId()
> SDL_Renderer* ren  = SDL_CreateRenderer(sdlWin, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
> SDL_Texture* tex   = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV,
>                                        SDL_TEXTUREACCESS_STREAMING, width, height);
> ```
>
> ### 2）解码得到 AVFrame（通常是 YUV420P）
>
> ```cpp
> // 每解出一帧：
> SDL_UpdateYUVTexture(tex, nullptr,
>     frame->data[0], frame->linesize[0],
>     frame->data[1], frame->linesize[1],
>     frame->data[2], frame->linesize[2]);
> 
> SDL_RenderClear(ren);
> SDL_RenderCopy(ren, tex, nullptr, nullptr);
> SDL_RenderPresent(ren);
> ```
>
> ### 3）线程纪律（很重要）
>
> - FFmpeg 解码通常在工作线程。
> - SDL 渲染建议固定在同一个线程（很多平台上渲染线程有要求）。
> - 最简单：**backend 内部开“解码线程 + 渲染线程”**，用队列传 `AVFrame` 或者传“已转换好的图像缓冲”。
>
> ### 4）尺寸变化
>
> - 监听目标 widget 的 resize（UI 层发事件给 service/runtime），backend 里 `SDL_SetWindowSize` 或者重新建 texture / viewport。
>
> > 这个方案的好处：**完全不需要把句柄转回 QWidget\***，你只把它当一块“能画的表面”。
>
> ------
>
> # 你原方案里最关键的调整点
>
> > “Qt6 backend 将句柄转换回 QWidget* 然后照常执行”——不成立。
>
> 你可以二选一：
>
> 1. **Qt6 backend 也按句柄渲染**（纯粹统一，但要改采集/渲染链路）
> 2. **Qt6 backend 继续用 QVideoWidget，但这个 QVideoWidget 由 service 创建并通过 backend_hint 传递**（最快落地，抽象也干净）
>
> 我建议你先走 **方案 2**：最快把 FFmpeg 路线跑通，而且不逼你立刻重写 Qt6 后端渲染系统。等以后性能/统一性需求上来，再把 Qt6 后端也切到“按句柄渲染”。
>
> ------
>
> ## 最后给你一句“工程护身符”
>
> 不管你选哪条路：**backend 永远不要持有外部 QWidget\***（除非它只在 Qt6 backend 内部且你用 `QPointer` 监控销毁，并且所有 UI 操作都回 UI 线程）。句柄路线本来就是为了把 UI 生命周期和解码渲染解耦，别又把耦合塞回去。
>
> 你现在这个项目的节奏，走“PreviewTarget（NativeWindow + backend_hint）”会非常顺。下一步你就可以把 `PreviewTarget`、Qt6 backend 的 `backend_hint` 解释、以及 FFmpeg+SDL2 的初始化/销毁时机我帮你按你现有模块（service/runtime/backend）写成一套具体类图+伪代码落地实现。
