后端层

音视频处理、网络的底层实现

包含音视频编解码、视频渲染

---

https://chatgpt.com/share/69a1c738-a3e0-8007-a5d4-2a15fd56e796

---
行，给你一套**“分包隔离优先、不为替换而抽象”**的组织结构。目标是：FFmpeg / Qt6 Multimedia / WebRTC（以及网络相关）各自独立，互不污染；上层只碰你自己的头文件。

---

## 推荐目录结构（可直接照搬）

```text
FPlayer-desktop/
  cmake/
  common/                       # 通用工具：log、thread、utils、time、ringbuffer等（不含 ffmpeg/qt/webrtc 头）
    include/fplayer/common/...
    src/...

  core/                         # 你自己的“对外门面”（薄）
    include/fplayer/            # app/widget 只允许 include 这里
      media/player.h            # 你定义的 Player API（尽量薄）
      media/capture.h           # 可选：采集 API（同样薄）
      net/stream.h              # 可选：推拉流/网络播放 API（薄）
      rtc/session.h             # 可选：RTC API（薄）
      core/config.h             # 后端选择（编译期/运行期都可，但你可以先只做编译期）
    src/
      factory.cpp               # createDefaultPlayer / createPlayerXX（用 #if 选择）
      config.cpp

  backends/                     # 真正实现，各自封闭
    media_ffmpeg/
      include/fplayer/backend_ffmpeg/   # 仅内部或仅给测试使用，不给 app 直接用
      src/
      CMakeLists.txt

    media_qt6/
      include/fplayer/backend_qt6/
      src/
      CMakeLists.txt

    net_ffmpeg/                 # RTSP/RTMP/HLS/UDP 推拉流/网络播放（用 FFmpeg avformat）
      include/fplayer/backend_net_ffmpeg/
      src/
      CMakeLists.txt

    net_qt6/                    # 控制面 + 局域网发现 + HTTP/WS 等（QtNetwork）
      include/fplayer/backend_net_qt6/
      src/
      CMakeLists.txt

    rtc_webrtc/                 # WebRTC 会话/track
      include/fplayer/backend_webrtc/
      src/
      CMakeLists.txt

  widget/                       # UI 层（Qt Widgets/QML）
    src/
    resources/

  app/                          # 可执行入口（desktop）
    src/main.cpp

  sample/                       # demo：分别验证各个 backend（强烈建议）
    player_ffmpeg_demo/
    player_qt6_demo/
    rtc_demo/

  external/                     # 第三方（或用 vcpkg/conan）
  doc/
  CMakeLists.txt
```

---

## 关键原则（让“分开”真的有效）

### 1) app/widget 只能 include `core/include/fplayer/*`

**禁止**上层 include 任何 `backends/*` 的头文件。
这样后端依赖（FFmpeg、QtMultimedia、WebRTC）就不会向上传染。

### 2) 每个 backend 只依赖 `core` + `common`

* `media_ffmpeg`：依赖 FFmpeg
* `media_qt6`：依赖 Qt6::Multimedia
* `rtc_webrtc`：依赖 webrtc
* `net_qt6`：依赖 Qt6::Network
* `net_ffmpeg`：依赖 FFmpeg avformat

互相之间尽量别依赖（除非你明确写一个 bridge 包）。

### 3) core 层“薄门面”只做两件事

* 定义你自己的 API（少）
* 用编译选项决定链接/创建哪个实现（别搞复杂插件系统）

---

## CMake target 组织建议（你会很舒服）

顶层建议这些 targets：

* `fplayer_common`
* `fplayer_core`（只含你的头+factory）
* `fplayer_backend_media_ffmpeg`
* `fplayer_backend_media_qt6`
* `fplayer_backend_net_ffmpeg`
* `fplayer_backend_net_qt6`
* `fplayer_backend_rtc_webrtc`
* `fplayer_desktop_app`（链接 core + 你选的 backend）

你想“能分开就行”，那就用选项控制：

* `-DFPLAYER_WITH_FFMPEG_MEDIA=ON`
* `-DFPLAYER_WITH_QT6_MEDIA=ON`
* `-DFPLAYER_WITH_WEBRTC=ON`

`core/src/factory.cpp` 里用 `#if` 返回对应实现即可。

---

## 最小“门面 API”长什么样（别超纲）

你可以只暴露这种级别：

* `createDefaultPlayer()`：由编译开关决定默认后端
* 或 `createPlayer(Backend::FFmpeg / Backend::Qt6)`：如果你想做对比测试

其它更细的（pipeline、frame、clock…）先别抽，先藏到 backend。

---

## 一个很实用的小升级：加 “bridge” 目录（可选）

当你未来需要“QtNetwork 做信令 + WebRTC 传媒体”这种组合，可以加：

```text
bridge/
  webrtc_signaling_qt6/
  ffmpeg_render_qt6/
```

桥接模块允许同时依赖两个后端，但后端本身仍然保持纯净。

---

你照这个结构改，最大的收益是：后端实现怎么折腾都不会污染 UI 和其他后端，工程规模上来以后也不容易失控。接下来你只需要按“先播放器双实现 -> 再 net_ffmpeg 推拉 -> 再 rtc_webrtc”这个顺序逐步填坑就行。
