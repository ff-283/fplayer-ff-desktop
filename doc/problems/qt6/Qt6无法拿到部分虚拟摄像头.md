目前已知的问题：

Qt6无法拿到部分虚拟摄像头：

> 是的，你这个感觉大概率是**“Qt6 的摄像头枚举链路和很多虚拟摄像头的实现链路对不上”**导致的，不是你代码写错。
>
> 在 **Windows 上 Qt 6 的 Qt Multimedia（也就是 `QMediaDevices/QCameraDevice` 那套）默认走的是 Windows Media Foundation（WMF）**，而不是老的 DirectShow。Qt 官方也明确说过：Qt6 Windows 后端是 WMF（Qt5 时代 Windows 上还有 DirectShow 等多套后端） ([qt.io](https://www.qt.io/blog/qt-multimedia-in-qt-6?utm_source=chatgpt.com))
> 所以：**如果你的虚拟摄像头只注册成 DirectShow 设备，Qt6 就“看不见”它**。这就是很多人遇到的 “OBS Virtual Camera 不在 `QMediaDevices::videoInputs()` 里” 的经典坑。([Stack Overflow](https://stackoverflow.com/questions/70706645/qmediadevicesvideoinputs-does-not-list-obs-virtual-camera-as-avaliable-on-wi?utm_source=chatgpt.com))
>
> ### 你怎么快速确认是不是这个原因（30 秒判定法）
>
> 1. 打开 **Windows 自带的“相机(Camera)”应用**
>    - 如果 **系统相机 App 里也看不到**该虚拟摄像头：它极可能是 **DirectShow-only**（或注册有问题）。
>    - 如果 **系统相机 App 里能看到**：它更可能是 **Media Foundation 可见**的设备，Qt6 理论上更有机会枚举到（但仍可能有格式/权限等问题）。
>
> （很多软件会明确说：他们只支持 Media Foundation，所以识别不到 OBS 的虚拟摄像头，因为 OBS 的虚拟摄像头当时只支持 DirectShow——这类解释在别的产品/社区里也反复出现过。）([obsproject.com](https://obsproject.com/forum/threads/windows-obs-studio-virtual-camera-incompatibility.145024/?utm_source=chatgpt.com))
>
> ### 为什么有的虚拟摄像头能看到、有的看不到？
>
> 因为虚拟摄像头的“驱动/注册方式”不一样：
>
> - **DirectShow 虚拟摄像头**：老生态，兼容很多旧软件，但 **Qt6(WMF) 可能看不到**
> - **Media Foundation 虚拟摄像头**：新生态（尤其 Windows 11 更“官方化”），更容易被 Qt6 这类 WMF 后端枚举到
>   （甚至有人专门讨论 “OBS 虚拟摄像头支持 Media Foundation” 这个需求。）([ideas.obsproject.com](https://ideas.obsproject.com/posts/2151/obs-virtual-camera-support-media-foundation?utm_source=chatgpt.com))
>
> ### 实用解决路径（按“务实程度”排序）
>
> **A. 换一个“MF 可见”的虚拟摄像头实现（最省事）**
>
> - 目标：让虚拟摄像头在 **Windows Camera App** 里也能出现（强信号）。
> - OBS 的虚拟摄像头在不同版本/实现方式下表现会不一样；有的阶段是 DirectShow 输出为主，导致 WMF-only 的应用看不到。([GitHub](https://github.com/obsproject/obs-studio/issues/8057?utm_source=chatgpt.com))
>
> **B. 不指望 Qt6 枚举虚拟摄像头：改用 FFmpeg / DirectShow 自己抓（最可控）**
> 你的项目本来就在聊 FFmpeg，这条路线很“工程化”：
>
> - 用 FFmpeg（`-f dshow`）或你自己 DirectShow 抓流
> - 把帧喂给 Qt（例如用 `QVideoFrame` / `QVideoSink` 或者你自己的纹理渲染管线）
> - Qt 只做 UI/显示，不做设备枚举/采集
>
> **C. 在 Qt6 上换后端（看你能不能接受）**
> Qt6 在 Windows 默认是 WMF 后端；理论上你可以走 **GStreamer 后端**（如果你愿意引入它并确保部署），但这会增加依赖和部署复杂度。Qt6 的后端策略就是“收敛到少数可维护后端”。([qt.io](https://www.qt.io/blog/qt-multimedia-in-qt-6?utm_source=chatgpt.com))
>
> ### 顺手给你一个“枚举时自检”的小片段（避免被空 ID/重复描述坑）
>
> ```cpp
> const auto inputs = QMediaDevices::videoInputs();
> qDebug() << "videoInputs size =" << inputs.size();
> for (int i = 0; i < inputs.size(); ++i) {
>     const auto &d = inputs[i];
>     qDebug() << i
>              << "desc=" << d.description()
>              << "id=" << d.id()
>              << "isNull=" << d.isNull();
> }
> ```
>
> 如果这里根本没有虚拟摄像头，那基本就是上面说的“后端不可见”问题，而不是你 `QCameraDevice` 用法问题。
>
> ------
>
> 结论一句话：**Qt6 在 Windows 上用 WMF 枚举摄像头，很多虚拟摄像头（尤其 OBS 某些实现/版本）是 DirectShow-only，所以 `QCameraDevice/QMediaDevices` 获取不到很正常。**([qt.io](https://www.qt.io/blog/qt-multimedia-in-qt-6?utm_source=chatgpt.com))
>
> 继续往下走的话，你这个项目（音视频-bs）反而很适合“采集用 FFmpeg/自定义，显示用 Qt”这种分层：设备世界很脏，UI 世界要干净。