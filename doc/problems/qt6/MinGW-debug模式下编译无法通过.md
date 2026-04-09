报错：

```
[97/100] Building CXX object app/CMakeFiles/FPlayer_App.dir/__/api/src/ifvideoview.cpp.obj
D:/Code/FPlayer-desketop/api/src/ifvideoview.cpp:3:1: warning: 'virtual fplayer::IFVideoView::~IFVideoView()' redeclared without dllimport attribute: previous dllimport ignored [-Wattributes]
    3 | fplayer::IFVideoView::~IFVideoView() = default;
      | ^~~~~~~
[98/100] Building CXX object app/CMakeFiles/FPlayer_App.dir/main.cpp.obj
[99/100] Linking CXX executable bin\FPlayer_App.exe; Deploying Qt runtime for target 'FPlayer_App' via windeployqt...
FAILED: bin/FPlayer_App.exe 
C:\Windows\system32\cmd.exe /C "cd . && "D:\SoftWare\CLion 2025.1.3\bin\mingw\bin\g++.exe" -g  app/CMakeFiles/FPlayer_App.dir/FPlayer_App_autogen/mocs_compilation.cpp.obj app/CMakeFiles/FPlayer_App.dir/__/api/src/ifvideoview.cpp.obj app/CMakeFiles/FPlayer_App.dir/main.cpp.obj -o bin\FPlayer_App.exe -Wl,--out-implib,bin\libFPlayer_App.dll.a -Wl,--major-image-version,0,--minor-image-version,0  D:/Code/Qt6.10/6.10.2/mingw_64/lib/libQt6Widgets.a  D:/Code/Qt6.10/6.10.2/mingw_64/lib/libQt6Gui.a  bin/libFPlayer_Widgetd.dll.a  D:/Code/Qt6.10/6.10.2/mingw_64/lib/libQt6Core.a  bin/libFPlayer_Serviced.dll.a  bin/libFPlayer_Commond.dll.a  bin/libFPlayer_Runtimed.dll.a  bin/libFPlayer_Backend_Media_Qt6d.dll.a  bin/libFPlayer_Apid.dll.a  bin/libLoggerd.dll.a  -ld3d11  -ldxgi  -ldxguid  -ld3d12  -lmpr  -luserenv  -lkernel32 -luser32 -lgdi32 -lwinspool -lshell32 -lole32 -loleaut32 -luuid -lcomdlg32 -ladvapi32 && C:\Windows\system32\cmd.exe /C "cd /D D:\Code\FPlayer-desketop\build\mingw\debug\bin && "D:\SoftWare\CLion 2025.1.3\bin\cmake\win\x64\bin\cmake.exe" -E env QT_PLUGIN_PATH=D:/Code/Qt6.10/6.10.2/mingw_64/plugins/platforms;D:/Code/Qt6.10/6.10.2/mingw_64/plugins QT_QPA_PLATFORM_PLUGIN_PATH=D:/Code/Qt6.10/6.10.2/mingw_64/plugins/platforms D:/Code/Qt6.10/6.10.2/mingw_64/bin/windeployqt.exe --debug --plugindir plugins --verbose 2 --no-compiler-runtime D:/Code/FPlayer-desketop/build/mingw/debug/bin/FPlayer_App.exe && "D:\SoftWare\CLion 2025.1.3\bin\cmake\win\x64\bin\cmake.exe" -E copy_directory D:/Code/Qt6.10/6.10.2/mingw_64/plugins/platforms D:/Code/FPlayer-desketop/build/mingw/debug/bin/plugins/platforms""
Running: qtpaths -query
Trying to read translation catalogs from "D:/Code/Qt6.10/6.10.2/mingw_64/translations/catalogs.json".
Found catalog "qtbase".
Found catalog "qtdeclarative".
Found catalog "qtmultimedia".
Found catalog "qtconnectivity".
Found catalog "qtlocation".
Found catalog "qtwebsockets".
Found catalog "qtserialport".
Found catalog "qtwebengine".
Found catalog "designer".
Found catalog "linguist".
Found catalog "assistant".
Found catalog "qt_help".
Found module "Qt63DAnimation".
Found module "Qt63DCore".
Found module "Qt63DExtras".
Found module "Qt63DInput".
Found module "Qt63DLogic".
Found module "Qt63DQuick".
Found module "Qt63DQuickAnimation".
Found module "Qt63DQuickExtras".
Found module "Qt63DQuickInput".
Found module "Qt63DQuickLogic".
Found module "Qt63DQuickRender".
Found module "Qt63DQuickScene2D".
Found module "Qt63DQuickScene3D".
Found module "Qt63DRender".
Found module "Qt6ActiveQt".
Found module "Qt6AxBase".
Found module "Qt6AxContainer".
Found module "Qt6AxServer".
Found module "Qt6Bluetooth".
Found module "Qt6Charts".
Found module "Qt6ChartsQml".
Found module "Qt6Concurrent".
Found module "Qt6Connectivity".
Found module "Qt6Core".
Found module "Qt6Core5Compat".
Found module "Qt6DataVisualization".
Found module "Qt6DataVisualizationQml".
Found module "Qt6DBus".
Found module "Qt6Designer".
Found module "Qt6DesignerComponents".
Found module "Qt6DeviceDiscoverySupport".
Found module "Qt6EntryPoint".
Found module "Qt6ExampleIcons".
Found module "Qt6ExamplesAssetDownloader".
Found module "Qt6FbSupport".
Found module "Qt6FFmpegMediaPluginImpl".
Found module "Qt6Freetype".
Found module "Qt6Graphs".
Found module "Qt6GraphsWidgets".
Found module "Qt6Grpc".
Found module "Qt6GrpcQuick".
Found module "Qt6Gui".
Found module "Qt6Harfbuzz".
Found module "Qt6Help".
Found module "Qt6HttpServer".
Found module "Qt6Jpeg".
Found module "Qt6JsonRpc".
Found module "Qt6LabsAnimation".
Found module "Qt6LabsFolderListModel".
Found module "Qt6LabsPlatform".
Found module "Qt6LabsQmlModels".
Found module "Qt6LabsSettings".
Found module "Qt6LabsSharedImage".
Found module "Qt6LabsSynchronizer".
Found module "Qt6LabsWavefrontMesh".
Found module "Qt6LanguageServer".
Found module "Qt6Linguist".
Found module "Qt6Location".
Found module "Qt6Lottie".
Found module "Qt6LottieVectorImageGenerator".
Found module "Qt6LottieVectorImageHelpers".
Found module "Qt6Multimedia".
Found module "Qt6MultimediaQuick".
Found module "Qt6MultimediaTestLib".
Found module "Qt6MultimediaWidgets".
Found module "Qt6Network".
Found module "Qt6NetworkAuth".
Found module "Qt6Nfc".
Found module "Qt6OpenGL".
Found module "Qt6OpenGLWidgets".
Found module "Qt6PacketProtocol".
Found module "Qt6Png".
Found module "Qt6Positioning".
Found module "Qt6PositioningQuick".
Found module "Qt6PrintSupport".
Found module "Qt6Protobuf".
Found module "Qt6ProtobufQtCoreTypes".
Found module "Qt6ProtobufQtGuiTypes".
Found module "Qt6ProtobufQuick".
Found module "Qt6ProtobufWellKnownTypes".
Found module "Qt6QDocCatchConversions".
Found module "Qt6QDocCatchGenerators".
Found module "Qt6QDocCatch".
Found module "Qt6Qml".
Found module "Qt6QmlAssetDownloader".
Found module "Qt6QmlCompiler".
Found module "Qt6QmlCore".
Found module "Qt6QmlDebug".
Found module "Qt6QmlDom".
Found module "Qt6QmlFormat".
Found module "Qt6QmlIntegration".
Found module "Qt6QmlLocalStorage".
Found module "Qt6QmlLS".
Found module "Qt6QmlMeta".
Found module "Qt6QmlModels".
Found module "Qt6QmlNetwork".
Found module "Qt6QmlToolingSettings".
Found module "Qt6QmlTypeRegistrar".
Found module "Qt6QmlWorkerScript".
Found module "Qt6QmlXmlListModel".
Found module "Qt6Quick".
Found module "Qt6Quick3D".
Found module "Qt6Quick3DAssetImport".
Found module "Qt6Quick3DAssetUtils".
Found module "Qt6Quick3DEffects".
Found module "Qt6Quick3DGlslParser".
Found module "Qt6Quick3DHelpers".
Found module "Qt6Quick3DHelpersImpl".
Found module "Qt6Quick3DIblBaker".
Found module "Qt6Quick3DParticleEffects".
Found module "Qt6Quick3DParticles".
Found module "Qt6Quick3DPhysics".
Found module "Qt6Quick3DPhysicsHelpers".
Found module "Qt6Quick3DRuntimeRender".
Found module "Qt6Quick3DSpatialAudio".
Found module "Qt6Quick3DUtils".
Found module "Qt6Quick3DXr".
Found module "Qt6QuickControls2".
Found module "Qt6QuickControls2Basic".
Found module "Qt6QuickControls2BasicStyleImpl".
Found module "Qt6QuickControls2FluentWinUI3StyleImpl".
Found module "Qt6QuickControls2Fusion".
Found module "Qt6QuickControls2FusionStyleImpl".
Found module "Qt6QuickControls2Imagine".
Found module "Qt6QuickControls2ImagineStyleImpl".
Found module "Qt6QuickControls2Impl".
Found module "Qt6QuickControls2Material".
Found module "Qt6QuickControls2MaterialStyleImpl".
Found module "Qt6QuickControls2Universal".
Found module "Qt6QuickControls2UniversalStyleImpl".
Found module "Qt6QuickControls2WindowsStyleImpl".
Found module "Qt6QuickControlsTestUtils".
Found module "Qt6QuickDialogs2".
Found module "Qt6QuickDialogs2QuickImpl".
Found module "Qt6QuickDialogs2Utils".
Found module "Qt6QuickEffects".
Found module "Qt6QuickLayouts".
Found module "Qt6QuickParticles".
Found module "Qt6QuickShapesDesignHelpers".
Found module "Qt6QuickShapes".
Found module "Qt6QuickTemplates2".
Found module "Qt6QuickTest".
Found module "Qt6QuickTestUtils".
Found module "Qt6QuickTimeline".
Found module "Qt6QuickTimelineBlendTrees".
Found module "Qt6QuickVectorImage".
Found module "Qt6QuickVectorImageGenerator".
Found module "Qt6QuickVectorImageHelpers".
Found module "Qt6QuickWidgets".
Found module "Qt6RemoteObjects".
Found module "Qt6RemoteObjectsQml".
Found module "Qt6RepParser".
Found module "Qt6Scxml".
Found module "Qt6ScxmlGlobal".
Found module "Qt6ScxmlQml".
Found module "Qt6Sensors".
Found module "Qt6SensorsQuick".
Found module "Qt6SerialBus".
Found module "Qt6SerialPort".
Found module "Qt6ShaderTools".
Found module "Qt6SpatialAudio".
Found module "Qt6Sql".
Found module "Qt6StateMachine".
Found module "Qt6StateMachineQml".
Found module "Qt6Svg".
Found module "Qt6SvgWidgets".
Found module "Qt6Test".
Found module "Qt6TestInternals".
Found module "Qt6TextToSpeech".
Found module "Qt6Tools".
Found module "Qt6UiPlugin".
Found module "Qt6UiTools".
Found module "Qt6VirtualKeyboard".
Found module "Qt6VirtualKeyboardQml".
Found module "Qt6VirtualKeyboardSettings".
Found module "Qt6WebChannel".
Found module "Qt6WebChannelQuick".
Found module "Qt6WebSockets".
Found module "Qt6WebView".
Found module "Qt6WebViewQuick".
Found module "Qt6Widgets".
Found module "Qt6Xml".
Found module "Qt6Zlib".
Qt binaries in D:\Code\Qt6.10\6.10.2\mingw_64\bin
readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\FPlayer_App.exe 64 bit, MinGW, dependent libraries: 10, release
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\bin\Qt6Core.dll 64 bit, MinGW, dependent libraries: 18
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\bin\Qt6Gui.dll 64 bit, MinGW, dependent libraries: 14
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\bin\Qt6Widgets.dll 64 bit, MinGW, dependent libraries: 10
Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libFPlayer_Apid.dll
readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libFPlayer_Apid.dll 64 bit, MinGW, dependent libraries: 4
Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libFPlayer_Commond.dll
readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libFPlayer_Commond.dll 64 bit, MinGW, dependent libraries: 6
Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libFPlayer_Widgetd.dll
readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libFPlayer_Widgetd.dll 64 bit, MinGW, dependent libraries: 10
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\bin\Qt6MultimediaWidgets.dll 64 bit, MinGW, dependent libraries: 7
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\bin\Qt6Multimedia.dll 64 bit, MinGW, dependent libraries: 12
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\bin\Qt6Network.dll 64 bit, MinGW, dependent libraries: 12
Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libLoggerd.dll
readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libLoggerd.dll 64 bit, MinGW, dependent libraries: 7
Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libFPlayer_Serviced.dll
readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libFPlayer_Serviced.dll 64 bit, MinGW, dependent libraries: 7
Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libspdlogd.dll
readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libspdlogd.dll 64 bit, MinGW, dependent libraries: 5
Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libyaml-toold.dll
readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libyaml-toold.dll 64 bit, MinGW, dependent libraries: 5
Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libFPlayer_Runtimed.dll
readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libFPlayer_Runtimed.dll 64 bit, MinGW, dependent libraries: 6
Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libFPlayer_Backend_Media_Qt6d.dll
readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libFPlayer_Backend_Media_Qt6d.dll 64 bit, MinGW, dependent libraries: 7
D:\Code\FPlayer-desketop\build\mingw\debug\bin\FPlayer_App.exe 64 bit, debug executable
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\bin\Qt6Core.dll 64 bit, dependent libraries: 18
Adding in plugin type generic for module: Qt6Gui
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\generic\qtuiotouchplugin.dll 64 bit, MinGW, release
Adding in plugin type iconengines for module: Qt6Gui
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\iconengines\qsvgicon.dll 64 bit, MinGW, release
Adding in plugin type imageformats for module: Qt6Gui
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qgif.dll 64 bit, MinGW, release
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qicns.dll 64 bit, MinGW, release
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qico.dll 64 bit, MinGW, release
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qjpeg.dll 64 bit, MinGW, release
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qsvg.dll 64 bit, MinGW, release
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qtga.dll 64 bit, MinGW, release
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qtiff.dll 64 bit, MinGW, release
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qwbmp.dll 64 bit, MinGW, release
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qwebp.dll 64 bit, MinGW, release
Adding in plugin type multimedia for module: Qt6Multimedia
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\multimedia\ffmpegmediaplugin.dll 64 bit, MinGW, release
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\multimedia\windowsmediaplugin.dll 64 bit, MinGW, release
Adding in plugin type networkinformation for module: Qt6Network
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\networkinformation\qnetworklistmanager.dll 64 bit, MinGW, release
Adding in plugin type platforminputcontexts for module: Qt6Gui
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\platforminputcontexts\qtvirtualkeyboardplugin.dll 64 bit, MinGW, release
Adding in plugin type platforms for module: Qt6Gui
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\platforms\qdirect2d.dll 64 bit, MinGW, release
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\platforms\qminimal.dll 64 bit, MinGW, release
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\platforms\qoffscreen.dll 64 bit, MinGW, release
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\platforms\qwindows.dll 64 bit, MinGW, release
Adding in plugin type styles for module: Qt6Widgets
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\styles\qmodernwindowsstyle.dll 64 bit, MinGW, release
Adding in plugin type tls for module: Qt6Network
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\tls\qcertonlybackend.dll 64 bit, MinGW, release
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\tls\qopensslbackend.dll 64 bit, MinGW, release
readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\tls\qschannelbackend.dll 64 bit, MinGW, release
Direct dependencies: Qt6Core Qt6Gui Qt6Multimedia Qt6MultimediaWidgets Qt6Network Qt6Widgets
All dependencies   : Qt6Core Qt6Gui Qt6Multimedia Qt6MultimediaWidgets Qt6Network Qt6Widgets
To be deployed     : Qt6Core Qt6Gui Qt6Multimedia Qt6MultimediaWidgets Qt6Network Qt6Widgets
Plugins: 
Unable to find the platform plugin.
ninja: build stopped: subcommand failed.

```

主要意思就是只找到了Release版本的MinGW的platforms插件，没有Debug版本的：

![image-20260303171636301](https://map--depot.oss-cn-hangzhou.aliyuncs.com/image/image-20260303171636301.png)

MSVC版本的，Debug和Release都有：

![image-20260303171606574](https://map--depot.oss-cn-hangzhou.aliyuncs.com/image/image-20260303171606574.png)

所以说，可能是MinGW只安装了Release版本的原因导致的(....吗？)

总结一下下面的说法，问题就在于MinGW版本的Qt默认是不集成Release版本的，Debug构件中也可以直接引用Release版本的Qt库（与MSVC不一样，构建类型没那么严格）。而我此前的CMaqke脚本中，针对Debug和Release构建进行了严格区分，导致MinGW的时候，去寻找Debug的插件，但其实这个不存在。修改方式就是，MinGW构建寻找平台插件的时候，不严格区分debug还是release。

> > https://chatgpt.com/share/69a6b14a-2fa0-8007-adfe-10b72d23caa5
> >
> > 为什么我是用qt online安装包安装的qt6.10.2中，MSVC2022版本是有debug和release两个版本的插件库，而MingGW的只有release版本的
>
> 这是 **Qt 在线安装器的设计策略 + MinGW 构建方式差异** 导致的现象，不是你安装出问题。
>
> 我直接给你讲核心原因。
>
> ------
>
> ## 一、MSVC 为什么有 Debug / Release 两套插件
>
> 在 Windows 上：
>
> - **MSVC Debug 版本的 ABI 与 Release 不兼容**
> - Debug 版使用：
>   - 不同的 CRT（MSVCPxxxD.dll）
>   - 不同的符号布局
>   - 额外调试信息
>   - `_ITERATOR_DEBUG_LEVEL` 不同
> - Debug 可执行程序 **只能加载 Debug 插件**
> - Release 可执行程序 **只能加载 Release 插件**
>
> 因此 Qt 官方必须：
>
> ```
> plugins/
>     imageformats/
>         qjpeg.dll          (release)
>         qjpegd.dll         (debug)
> ```
>
> 否则 Debug 程序运行时会直接报：
>
> ```
> Cannot load library ... : The specified module could not be found.
> ```
>
> 👉 所以 MSVC 必须同时提供两套插件。
>
> ------
>
> ## 二、MinGW 为什么通常只有 Release 插件？
>
> 因为 **MinGW 的 Debug 和 Release ABI 通常兼容**。
>
> 具体原因：
>
> 1. MinGW 没有像 MSVC 那样区分 Debug CRT
> 2. 没有 `_ITERATOR_DEBUG_LEVEL` 这种 ABI 差异
> 3. Debug 只是加了 `-g` 调试信息
> 4. 二进制接口基本一致
>
> 所以：
>
> ```
> MinGW Debug 可执行程序
> 可以加载
> MinGW Release 插件
> ```
>
> 👉 官方为了减少体积，直接只提供 Release 插件。
>
> 这在 Qt5 时代就已经是这样。
>
> ------
>
> ## 三、为什么 Qt 6 仍然这样？
>
> Qt6 的 Windows 构建策略：
>
> | 编译器 | 是否区分 Debug 插件 |
> | ------ | ------------------- |
> | MSVC   | 必须区分            |
> | MinGW  | 不强制区分          |
>
> Qt 官方为了减小下载体积：
>
> - MinGW 默认只打包 release 插件
> - Debug 模式只包含 `.pdb` 或 `.debug` 信息
>
> ------
>
> ## 四、你可以验证一下
>
> 看你的 MinGW 插件目录：
>
> ```
> Qt/6.10.2/mingw_64/plugins/
> ```
>
> 你会发现：
>
> ```
> qwindows.dll
> ```
>
> 但没有：
>
> ```
> qwindowsd.dll
> ```
>
> 而 MSVC 目录：
>
> ```
> Qt/6.10.2/msvc2022_64/plugins/
> ```
>
> 会有：
>
> ```
> qwindows.dll
> qwindowsd.dll
> ```
>
> 这是预期行为。
>
> ------
>
> ## 五、什么时候 MinGW 需要 Debug 插件？
>
> 几乎不会。
>
> 除非：
>
> - 你自己用 `-DQT_BUILD_DEBUG` 重新编译 Qt
> - 或者你手动构建插件为 Debug
>
> 但正常开发完全不需要。
>
> ------
>
> ## 六、会不会影响你？
>
> ✔ 不影响开发
> ✔ Debug 程序能正常加载插件
> ✔ 不会导致性能问题
>
> 你只需要确保：
>
> ```
> mingw 编译的程序
> 使用 mingw 的 Qt
> ```
>
> 不要混用 MSVC 和 MinGW。
>
> ------
>
> ## 七、额外一个高级点（你是程序员，这个对你重要）
>
> 如果你强行用 MSVC 的 Debug 程序加载 Release 插件：
>
> - 可能直接加载失败
> - 或者运行时崩溃
> - 或者 STL 容器跨模块崩溃
>
> 但 MinGW 基本不会出现这种问题。
>
> ------
>
> ## 八、总结
>
> > MSVC Debug/Release ABI 不兼容 → 必须双插件
> > MinGW Debug/Release ABI 基本兼容 → 可以共用 Release 插件
>
> Qt 官方只是利用了这个事实来减小安装包体积。
>
> ------
>
> 如果你愿意，我可以再给你讲：
>
> - Qt 插件加载机制（QFactoryLoader 细节）
> - ABI 差异到底发生在哪一层
> - 如何自己构建完整 Debug Qt
> - 或者如何减小 MSVC 版本体积
>
> 你现在是在做什么项目？桌面？嵌入式？Qt Quick？
>
> > ```
> > [95/100] Automatic MOC and UIC for target FPlayer_App
> > [96/100] Building CXX object app/CMakeFiles/FPlayer_App.dir/FPlayer_App_autogen/mocs_compilation.cpp.obj
> > [97/100] Building CXX object app/CMakeFiles/FPlayer_App.dir/__/api/src/ifvideoview.cpp.obj
> > D:/Code/FPlayer-desketop/api/src/ifvideoview.cpp:3:1: warning: 'virtual fplayer::IFVideoView::~IFVideoView()' redeclared without dllimport attribute: previous dllimport ignored [-Wattributes]
> >     3 | fplayer::IFVideoView::~IFVideoView() = default;
> >       | ^~~~~~~
> > [98/100] Building CXX object app/CMakeFiles/FPlayer_App.dir/main.cpp.obj
> > [99/100] Linking CXX executable bin\FPlayer_App.exe; Deploying Qt runtime for target 'FPlayer_App' via windeployqt...
> > FAILED: bin/FPlayer_App.exe 
> > C:\Windows\system32\cmd.exe /C "cd . && "D:\SoftWare\CLion 2025.1.3\bin\mingw\bin\g++.exe" -g  app/CMakeFiles/FPlayer_App.dir/FPlayer_App_autogen/mocs_compilation.cpp.obj app/CMakeFiles/FPlayer_App.dir/__/api/src/ifvideoview.cpp.obj app/CMakeFiles/FPlayer_App.dir/main.cpp.obj -o bin\FPlayer_App.exe -Wl,--out-implib,bin\libFPlayer_App.dll.a -Wl,--major-image-version,0,--minor-image-version,0  D:/Code/Qt6.10/6.10.2/mingw_64/lib/libQt6Widgets.a  D:/Code/Qt6.10/6.10.2/mingw_64/lib/libQt6Gui.a  bin/libFPlayer_Widgetd.dll.a  D:/Code/Qt6.10/6.10.2/mingw_64/lib/libQt6Core.a  bin/libFPlayer_Serviced.dll.a  bin/libFPlayer_Commond.dll.a  bin/libFPlayer_Runtimed.dll.a  bin/libFPlayer_Backend_Media_Qt6d.dll.a  bin/libFPlayer_Apid.dll.a  bin/libLoggerd.dll.a  -ld3d11  -ldxgi  -ldxguid  -ld3d12  -lmpr  -luserenv  -lkernel32 -luser32 -lgdi32 -lwinspool -lshell32 -lole32 -loleaut32 -luuid -lcomdlg32 -ladvapi32 && C:\Windows\system32\cmd.exe /C "cd /D D:\Code\FPlayer-desketop\build\mingw\debug\bin && "D:\SoftWare\CLion 2025.1.3\bin\cmake\win\x64\bin\cmake.exe" -E env QT_PLUGIN_PATH=D:/Code/Qt6.10/6.10.2/mingw_64/plugins/platforms;D:/Code/Qt6.10/6.10.2/mingw_64/plugins QT_QPA_PLATFORM_PLUGIN_PATH=D:/Code/Qt6.10/6.10.2/mingw_64/plugins/platforms D:/Code/Qt6.10/6.10.2/mingw_64/bin/windeployqt.exe --debug --plugindir plugins --verbose 2 --no-compiler-runtime D:/Code/FPlayer-desketop/build/mingw/debug/bin/FPlayer_App.exe && "D:\SoftWare\CLion 2025.1.3\bin\cmake\win\x64\bin\cmake.exe" -E copy_directory D:/Code/Qt6.10/6.10.2/mingw_64/plugins/platforms D:/Code/FPlayer-desketop/build/mingw/debug/bin/plugins/platforms""
> > Running: qtpaths -query
> > Trying to read translation catalogs from "D:/Code/Qt6.10/6.10.2/mingw_64/translations/catalogs.json".
> > Found catalog "qtbase".
> > Found catalog "qtdeclarative".
> > Found catalog "qtmultimedia".
> > Found catalog "qtconnectivity".
> > Found catalog "qtlocation".
> > Found catalog "qtwebsockets".
> > Found catalog "qtserialport".
> > Found catalog "qtwebengine".
> > Found catalog "designer".
> > Found catalog "linguist".
> > Found catalog "assistant".
> > Found catalog "qt_help".
> > Found module "Qt63DAnimation".
> > Found module "Qt63DCore".
> > Found module "Qt63DExtras".
> > Found module "Qt63DInput".
> > Found module "Qt63DLogic".
> > Found module "Qt63DQuick".
> > Found module "Qt63DQuickAnimation".
> > Found module "Qt63DQuickExtras".
> > Found module "Qt63DQuickInput".
> > Found module "Qt63DQuickLogic".
> > Found module "Qt63DQuickRender".
> > Found module "Qt63DQuickScene2D".
> > Found module "Qt63DQuickScene3D".
> > Found module "Qt63DRender".
> > Found module "Qt6ActiveQt".
> > Found module "Qt6AxBase".
> > Found module "Qt6AxContainer".
> > Found module "Qt6AxServer".
> > Found module "Qt6Bluetooth".
> > Found module "Qt6Charts".
> > Found module "Qt6ChartsQml".
> > Found module "Qt6Concurrent".
> > Found module "Qt6Connectivity".
> > Found module "Qt6Core".
> > Found module "Qt6Core5Compat".
> > Found module "Qt6DataVisualization".
> > Found module "Qt6DataVisualizationQml".
> > Found module "Qt6DBus".
> > Found module "Qt6Designer".
> > Found module "Qt6DesignerComponents".
> > Found module "Qt6DeviceDiscoverySupport".
> > Found module "Qt6EntryPoint".
> > Found module "Qt6ExampleIcons".
> > Found module "Qt6ExamplesAssetDownloader".
> > Found module "Qt6FbSupport".
> > Found module "Qt6FFmpegMediaPluginImpl".
> > Found module "Qt6Freetype".
> > Found module "Qt6Graphs".
> > Found module "Qt6GraphsWidgets".
> > Found module "Qt6Grpc".
> > Found module "Qt6GrpcQuick".
> > Found module "Qt6Gui".
> > Found module "Qt6Harfbuzz".
> > Found module "Qt6Help".
> > Found module "Qt6HttpServer".
> > Found module "Qt6Jpeg".
> > Found module "Qt6JsonRpc".
> > Found module "Qt6LabsAnimation".
> > Found module "Qt6LabsFolderListModel".
> > Found module "Qt6LabsPlatform".
> > Found module "Qt6LabsQmlModels".
> > Found module "Qt6LabsSettings".
> > Found module "Qt6LabsSharedImage".
> > Found module "Qt6LabsSynchronizer".
> > Found module "Qt6LabsWavefrontMesh".
> > Found module "Qt6LanguageServer".
> > Found module "Qt6Linguist".
> > Found module "Qt6Location".
> > Found module "Qt6Lottie".
> > Found module "Qt6LottieVectorImageGenerator".
> > Found module "Qt6LottieVectorImageHelpers".
> > Found module "Qt6Multimedia".
> > Found module "Qt6MultimediaQuick".
> > Found module "Qt6MultimediaTestLib".
> > Found module "Qt6MultimediaWidgets".
> > Found module "Qt6Network".
> > Found module "Qt6NetworkAuth".
> > Found module "Qt6Nfc".
> > Found module "Qt6OpenGL".
> > Found module "Qt6OpenGLWidgets".
> > Found module "Qt6PacketProtocol".
> > Found module "Qt6Png".
> > Found module "Qt6Positioning".
> > Found module "Qt6PositioningQuick".
> > Found module "Qt6PrintSupport".
> > Found module "Qt6Protobuf".
> > Found module "Qt6ProtobufQtCoreTypes".
> > Found module "Qt6ProtobufQtGuiTypes".
> > Found module "Qt6ProtobufQuick".
> > Found module "Qt6ProtobufWellKnownTypes".
> > Found module "Qt6QDocCatchConversions".
> > Found module "Qt6QDocCatchGenerators".
> > Found module "Qt6QDocCatch".
> > Found module "Qt6Qml".
> > Found module "Qt6QmlAssetDownloader".
> > Found module "Qt6QmlCompiler".
> > Found module "Qt6QmlCore".
> > Found module "Qt6QmlDebug".
> > Found module "Qt6QmlDom".
> > Found module "Qt6QmlFormat".
> > Found module "Qt6QmlIntegration".
> > Found module "Qt6QmlLocalStorage".
> > Found module "Qt6QmlLS".
> > Found module "Qt6QmlMeta".
> > Found module "Qt6QmlModels".
> > Found module "Qt6QmlNetwork".
> > Found module "Qt6QmlToolingSettings".
> > Found module "Qt6QmlTypeRegistrar".
> > Found module "Qt6QmlWorkerScript".
> > Found module "Qt6QmlXmlListModel".
> > Found module "Qt6Quick".
> > Found module "Qt6Quick3D".
> > Found module "Qt6Quick3DAssetImport".
> > Found module "Qt6Quick3DAssetUtils".
> > Found module "Qt6Quick3DEffects".
> > Found module "Qt6Quick3DGlslParser".
> > Found module "Qt6Quick3DHelpers".
> > Found module "Qt6Quick3DHelpersImpl".
> > Found module "Qt6Quick3DIblBaker".
> > Found module "Qt6Quick3DParticleEffects".
> > Found module "Qt6Quick3DParticles".
> > Found module "Qt6Quick3DPhysics".
> > Found module "Qt6Quick3DPhysicsHelpers".
> > Found module "Qt6Quick3DRuntimeRender".
> > Found module "Qt6Quick3DSpatialAudio".
> > Found module "Qt6Quick3DUtils".
> > Found module "Qt6Quick3DXr".
> > Found module "Qt6QuickControls2".
> > Found module "Qt6QuickControls2Basic".
> > Found module "Qt6QuickControls2BasicStyleImpl".
> > Found module "Qt6QuickControls2FluentWinUI3StyleImpl".
> > Found module "Qt6QuickControls2Fusion".
> > Found module "Qt6QuickControls2FusionStyleImpl".
> > Found module "Qt6QuickControls2Imagine".
> > Found module "Qt6QuickControls2ImagineStyleImpl".
> > Found module "Qt6QuickControls2Impl".
> > Found module "Qt6QuickControls2Material".
> > Found module "Qt6QuickControls2MaterialStyleImpl".
> > Found module "Qt6QuickControls2Universal".
> > Found module "Qt6QuickControls2UniversalStyleImpl".
> > Found module "Qt6QuickControls2WindowsStyleImpl".
> > Found module "Qt6QuickControlsTestUtils".
> > Found module "Qt6QuickDialogs2".
> > Found module "Qt6QuickDialogs2QuickImpl".
> > Found module "Qt6QuickDialogs2Utils".
> > Found module "Qt6QuickEffects".
> > Found module "Qt6QuickLayouts".
> > Found module "Qt6QuickParticles".
> > Found module "Qt6QuickShapesDesignHelpers".
> > Found module "Qt6QuickShapes".
> > Found module "Qt6QuickTemplates2".
> > Found module "Qt6QuickTest".
> > Found module "Qt6QuickTestUtils".
> > Found module "Qt6QuickTimeline".
> > Found module "Qt6QuickTimelineBlendTrees".
> > Found module "Qt6QuickVectorImage".
> > Found module "Qt6QuickVectorImageGenerator".
> > Found module "Qt6QuickVectorImageHelpers".
> > Found module "Qt6QuickWidgets".
> > Found module "Qt6RemoteObjects".
> > Found module "Qt6RemoteObjectsQml".
> > Found module "Qt6RepParser".
> > Found module "Qt6Scxml".
> > Found module "Qt6ScxmlGlobal".
> > Found module "Qt6ScxmlQml".
> > Found module "Qt6Sensors".
> > Found module "Qt6SensorsQuick".
> > Found module "Qt6SerialBus".
> > Found module "Qt6SerialPort".
> > Found module "Qt6ShaderTools".
> > Found module "Qt6SpatialAudio".
> > Found module "Qt6Sql".
> > Found module "Qt6StateMachine".
> > Found module "Qt6StateMachineQml".
> > Found module "Qt6Svg".
> > Found module "Qt6SvgWidgets".
> > Found module "Qt6Test".
> > Found module "Qt6TestInternals".
> > Found module "Qt6TextToSpeech".
> > Found module "Qt6Tools".
> > Found module "Qt6UiPlugin".
> > Found module "Qt6UiTools".
> > Found module "Qt6VirtualKeyboard".
> > Found module "Qt6VirtualKeyboardQml".
> > Found module "Qt6VirtualKeyboardSettings".
> > Found module "Qt6WebChannel".
> > Found module "Qt6WebChannelQuick".
> > Found module "Qt6WebSockets".
> > Found module "Qt6WebView".
> > Found module "Qt6WebViewQuick".
> > Found module "Qt6Widgets".
> > Found module "Qt6Xml".
> > Found module "Qt6Zlib".
> > Qt binaries in D:\Code\Qt6.10\6.10.2\mingw_64\bin
> > readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\FPlayer_App.exe 64 bit, MinGW, dependent libraries: 10, release
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\bin\Qt6Core.dll 64 bit, MinGW, dependent libraries: 18
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\bin\Qt6Gui.dll 64 bit, MinGW, dependent libraries: 14
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\bin\Qt6Widgets.dll 64 bit, MinGW, dependent libraries: 10
> > Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libFPlayer_Apid.dll
> > readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libFPlayer_Apid.dll 64 bit, MinGW, dependent libraries: 4
> > Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libFPlayer_Commond.dll
> > readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libFPlayer_Commond.dll 64 bit, MinGW, dependent libraries: 6
> > Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libFPlayer_Widgetd.dll
> > readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libFPlayer_Widgetd.dll 64 bit, MinGW, dependent libraries: 10
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\bin\Qt6MultimediaWidgets.dll 64 bit, MinGW, dependent libraries: 7
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\bin\Qt6Multimedia.dll 64 bit, MinGW, dependent libraries: 12
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\bin\Qt6Network.dll 64 bit, MinGW, dependent libraries: 12
> > Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libLoggerd.dll
> > readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libLoggerd.dll 64 bit, MinGW, dependent libraries: 7
> > Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libFPlayer_Serviced.dll
> > readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libFPlayer_Serviced.dll 64 bit, MinGW, dependent libraries: 7
> > Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libspdlogd.dll
> > readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libspdlogd.dll 64 bit, MinGW, dependent libraries: 5
> > Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libyaml-toold.dll
> > readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libyaml-toold.dll 64 bit, MinGW, dependent libraries: 5
> > Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libFPlayer_Runtimed.dll
> > readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libFPlayer_Runtimed.dll 64 bit, MinGW, dependent libraries: 6
> > Adding local dependencyD:/Code/FPlayer-desketop/build/mingw/debug/bin/libFPlayer_Backend_Media_Qt6d.dll
> > readPeExecutable: D:\Code\FPlayer-desketop\build\mingw\debug\bin\libFPlayer_Backend_Media_Qt6d.dll 64 bit, MinGW, dependent libraries: 7
> > D:\Code\FPlayer-desketop\build\mingw\debug\bin\FPlayer_App.exe 64 bit, debug executable
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\bin\Qt6Core.dll 64 bit, dependent libraries: 18
> > Adding in plugin type generic for module: Qt6Gui
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\generic\qtuiotouchplugin.dll 64 bit, MinGW, release
> > Adding in plugin type iconengines for module: Qt6Gui
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\iconengines\qsvgicon.dll 64 bit, MinGW, release
> > Adding in plugin type imageformats for module: Qt6Gui
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qgif.dll 64 bit, MinGW, release
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qicns.dll 64 bit, MinGW, release
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qico.dll 64 bit, MinGW, release
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qjpeg.dll 64 bit, MinGW, release
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qsvg.dll 64 bit, MinGW, release
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qtga.dll 64 bit, MinGW, release
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qtiff.dll 64 bit, MinGW, release
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qwbmp.dll 64 bit, MinGW, release
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\imageformats\qwebp.dll 64 bit, MinGW, release
> > Adding in plugin type multimedia for module: Qt6Multimedia
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\multimedia\ffmpegmediaplugin.dll 64 bit, MinGW, release
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\multimedia\windowsmediaplugin.dll 64 bit, MinGW, release
> > Adding in plugin type networkinformation for module: Qt6Network
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\networkinformation\qnetworklistmanager.dll 64 bit, MinGW, release
> > Adding in plugin type platforminputcontexts for module: Qt6Gui
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\platforminputcontexts\qtvirtualkeyboardplugin.dll 64 bit, MinGW, release
> > Adding in plugin type platforms for module: Qt6Gui
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\platforms\qdirect2d.dll 64 bit, MinGW, release
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\platforms\qminimal.dll 64 bit, MinGW, release
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\platforms\qoffscreen.dll 64 bit, MinGW, release
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\platforms\qwindows.dll 64 bit, MinGW, release
> > Adding in plugin type styles for module: Qt6Widgets
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\styles\qmodernwindowsstyle.dll 64 bit, MinGW, release
> > Adding in plugin type tls for module: Qt6Network
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\tls\qcertonlybackend.dll 64 bit, MinGW, release
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\tls\qopensslbackend.dll 64 bit, MinGW, release
> > readPeExecutable: D:\Code\Qt6.10\6.10.2\mingw_64\plugins\tls\qschannelbackend.dll 64 bit, MinGW, release
> > Direct dependencies: Qt6Core Qt6Gui Qt6Multimedia Qt6MultimediaWidgets Qt6Network Qt6Widgets
> > All dependencies   : Qt6Core Qt6Gui Qt6Multimedia Qt6MultimediaWidgets Qt6Network Qt6Widgets
> > To be deployed     : Qt6Core Qt6Gui Qt6Multimedia Qt6MultimediaWidgets Qt6Network Qt6Widgets
> > Plugins: 
> > Unable to find the platform plugin.
> > ninja: build stopped: subcommand failed.
> > ```
> >
> > 
> >
> > 当我使用MinGW debug模式构建我的qt项目的时候，我这里报了这个错误，我的插件列表在给你的图像中列出来了，确实缺少Debug的platform的插件。另外，下面是我相关的cmake脚本：
> >
> > 
> >
> > ```cmake
> > # ------------------------------------------------------------
> > # qt_deploy_runtime(<target>
> > #     [DEST_DIR <dir>]               # 部署目录（默认：目标exe所在目录）
> > #     [PLUGIN_DIR <dir>]             # 插件目录名（默认：plugins）
> > #     [QML_DIR <dir>]                # 如有QML可传入（可选）
> > #     [EXTRA_ARGS <...>]             # 额外 windeployqt 参数（可选）
> > # )
> > #
> > # 示例：
> > #   qt_deploy_runtime(FPlayer_App
> > #       EXTRA_ARGS --multimedia
> > #   )
> > #
> > #   qt_deploy_runtime(MyApp
> > #       DEST_DIR "${CMAKE_BINARY_DIR}/deploy/$<CONFIG>"
> > #       PLUGIN_DIR "plugins"
> > #       EXTRA_ARGS --multimedia --no-translations
> > #   )
> > # ------------------------------------------------------------
> > function(qt_deploy_runtime target)
> >     if (NOT WIN32)
> >         return()
> >     endif ()
> > if (NOT TARGET ${target})
> >     message(FATAL_ERROR "qt_deploy_runtime: target '${target}' does not exist")
> > endif ()
> > 
> > set(options)
> > set(oneValueArgs DEST_DIR PLUGIN_DIR QML_DIR)
> > set(multiValueArgs EXTRA_ARGS)
> > cmake_parse_arguments(QTDEPLOY
> >         "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
> > 
> > # 默认插件输出目录
> > if (NOT QTDEPLOY_PLUGIN_DIR)
> >     set(QTDEPLOY_PLUGIN_DIR "plugins")
> > endif ()
> > 
> > # 1) 定位 qmake 和 windeployqt
> > get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
> > if (NOT _qmake_exe)
> >     message(FATAL_ERROR "qt_deploy_runtime: cannot locate Qt6::qmake. Did you call find_package(Qt6 ...)?")
> > endif ()
> > get_filename_component(_qt_bin_dir "${_qmake_exe}" DIRECTORY)
> > set(_windeployqt "${_qt_bin_dir}/windeployqt.exe")
> > if (NOT EXISTS "${_windeployqt}")
> >     message(FATAL_ERROR "qt_deploy_runtime: windeployqt not found at: ${_windeployqt}")
> > endif ()
> > 
> > # 2) 获取 Qt 核心路径（修复：显式指定 MinGW 插件路径）
> > get_target_property(_qt_core_lib Qt6::Core IMPORTED_LOCATION)
> > get_filename_component(_qt_root_dir "${_qt_core_lib}" DIRECTORY)
> > get_filename_component(_qt_root_dir "${_qt_root_dir}" DIRECTORY) # 上一级到 Qt 安装根目录
> > 
> > # 显式指定 MinGW 平台插件路径（关键修复）
> > set(_qt_platform_plugin_dir "${_qt_root_dir}/plugins/platforms")
> > if (NOT EXISTS "${_qt_platform_plugin_dir}")
> >     message(FATAL_ERROR "Qt platforms plugin dir not found: ${_qt_platform_plugin_dir}")
> > endif ()
> > 
> > # 3) 部署目录配置
> > set(_work_dir "$<TARGET_FILE_DIR:${target}>")
> > set(_dir_arg)
> > if (QTDEPLOY_DEST_DIR)
> >     set(_dir_arg --dir "${QTDEPLOY_DEST_DIR}")
> > endif ()
> > 
> > # 4) QML 配置（可选）
> > set(_qml_arg)
> > if (QTDEPLOY_QML_DIR)
> >     set(_qml_arg --qmldir "${QTDEPLOY_QML_DIR}")
> > endif ()
> > 
> > # 5) Debug/Release 配置
> > set(_cfg_arg $<$<CONFIG:Debug>:--debug>$<$<NOT:$<CONFIG:Debug>>:--release>)
> > 
> > # 6) 修复：增强环境变量 + 显式指定平台插件路径
> > # 核心改进：
> > # - 同时设置 QT_PLUGIN_PATH 和 QT_QPA_PLATFORM_PLUGIN_PATH
> > # - 强制包含 platforms 插件目录
> > # - 增加 --verbose 便于调试
> > add_custom_command(TARGET ${target} POST_BUILD
> >         COMMAND "${CMAKE_COMMAND}" -E env
> >         # 双重保险：设置平台插件专用环境变量
> >         "QT_PLUGIN_PATH=${_qt_platform_plugin_dir};${_qt_root_dir}/plugins"
> >         "QT_QPA_PLATFORM_PLUGIN_PATH=${_qt_platform_plugin_dir}"
> >         # 执行 windeployqt，增加 --verbose 便于调试
> >         "${_windeployqt}"
> >         ${_cfg_arg}
> >         ${_dir_arg}
> >         --plugindir "${QTDEPLOY_PLUGIN_DIR}"
> >         --verbose 2  # 输出详细日志，便于排查问题
> >         --no-compiler-runtime  # 避免重复部署编译器运行时（MinGW 不需要）
> >         ${_qml_arg}
> >         ${QTDEPLOY_EXTRA_ARGS}
> >         "$<TARGET_FILE:${target}>"
> >         # 额外步骤：手动复制 platforms 插件（兜底方案）
> >         COMMAND "${CMAKE_COMMAND}" -E copy_directory
> >         "${_qt_platform_plugin_dir}"
> >         "${_work_dir}/${QTDEPLOY_PLUGIN_DIR}/platforms"
> >         WORKING_DIRECTORY "${_work_dir}"
> >         COMMENT "Deploying Qt runtime for target '${target}' via windeployqt..."
> >         VERBATIM
> > )
> > 
> > # 输出调试信息，帮助确认路径是否正确
> > message(STATUS "Qt deploy config for ${target}:")
> > message(STATUS "  Qt root dir: ${_qt_root_dir}")
> > message(STATUS "  Platform plugin dir: ${_qt_platform_plugin_dir}")
> > message(STATUS "  Target exe path: $<TARGET_FILE:${target}>")
> > endfunction()
> > ```
>
> 你这个报错**不是因为 MinGW 缺少 debug 版 platform 插件本身**（那是正常现象），而是因为你在 **MinGW 的 Debug 构建里强行对 `windeployqt` 传了 `--debug`**，导致 `windeployqt` 在“部署/校验阶段”按 **debug 规则**去找 platform plugin（倾向找 `*d.dll` / debug 变体），结果它认为“平台插件没部署成功”，于是直接退出非 0，Ninja 就把构建停了：
>
> > `Plugins: Unable to find the platform plugin.`
> > `ninja: build stopped: subcommand failed.`
>
> 注意你日志里其实已经扫描到了 `qwindows.dll`（MinGW、release）：
>
> - `...plugins\platforms\qwindows.dll 64 bit, MinGW, release`
>
> 但你同时又传了 `--debug`，`windeployqt` 在某些版本/组合下会把 **debug 部署**当成“必须匹配 debug 插件/依赖”的语义来做最终判定，于是就报“找不到 platform plugin”（它不是说 Qt 安装目录没有，而是说**按它的 debug 判定，部署结果不满足**）。
>
> ------
>
> ## 关键结论
>
> ### ✅ MinGW：Debug 可执行程序 **可以**加载 Release 插件
>
> 所以**MinGW 下即使是 Debug 构建，也应该让 `windeployqt` 走 release 插件的部署逻辑**（或者干脆不指定 `--debug/--release` 让它自判）。
>
> ### ❌ 你现在的逻辑：Debug 构建就传 `--debug`
>
> 这对 MSVC 是对的；对 MinGW 常常会踩坑。
>
> ------
>
> ## 你该怎么改（推荐顺序）
>
> ### 方案 A（最推荐）：MinGW 下不要传 `--debug/--release`
>
> 让 `windeployqt` 自己判断，或者至少不要强制 debug。
>
> 把这行：
>
> ```cmake
> set(_cfg_arg $<$<CONFIG:Debug>:--debug>$<$<NOT:$<CONFIG:Debug>>:--release>)
> ```
>
> 改成“只有 MSVC 才区分 debug/release；MinGW 直接空”：
>
> ```cmake
> if (MSVC)
>   set(_cfg_arg $<$<CONFIG:Debug>:--debug>$<$<NOT:$<CONFIG:Debug>>:--release>)
> else()
>   set(_cfg_arg) # MinGW: 不强制 --debug/--release
> endif()
> ```
>
> ### 方案 B：MinGW 下即使 Debug 构建也传 `--release`
>
> 更激进但很稳：
>
> ```cmake
> if (MSVC)
>   set(_cfg_arg $<$<CONFIG:Debug>:--debug>$<$<NOT:$<CONFIG:Debug>>:--release>)
> else()
>   set(_cfg_arg --release)  # MinGW debug 也按 release 部署插件
> endif()
> ```
>
> ------
>
> ## 你当前脚本里还有一个“隐藏雷点”
>
> 你设置了：
>
> ```cmake
> --plugindir "${QTDEPLOY_PLUGIN_DIR}"
> ```
>
> 这会把插件部署到 `bin/plugins/...`（相对 `--dir` 或 exe 目录）。然后你又额外 copy：
>
> ```cmake
> copy_directory "${_qt_platform_plugin_dir}"
>               "${_work_dir}/${QTDEPLOY_PLUGIN_DIR}/platforms"
> ```
>
> 这一步没问题，但**windeployqt 在它自己的校验阶段报错会先退出**，后面的 copy 根本执行不到（因为 `&&` 链式执行，前一步失败后就短路了）。所以你看到“我都兜底 copy 了怎么还失败”，原因是：**失败发生在 copy 之前**。
>
> 只要你把 `--debug` 从 MinGW 路径里拿掉，`windeployqt` 不再失败，后面的 copy 才有机会跑。
>
> ------
>
> ## 最小改动版本（直接贴可用补丁）
>
> 只改 `_cfg_arg` 一处就能救活：
>
> ```cmake
> # 5) Debug/Release 配置
> if (MSVC)
>     set(_cfg_arg $<$<CONFIG:Debug>:--debug>$<$<NOT:$<CONFIG:Debug>>:--release>)
> else()
>     set(_cfg_arg)  # MinGW 不要强制 --debug
> endif()
> ```
>
> ------
>
> ## 额外建议（让部署更“确定”）
>
> 1. 你已经显式设了 `QT_QPA_PLATFORM_PLUGIN_PATH`，很好。
> 2. 运行时确保目录结构是：
>
> ```
> FPlayer_App.exe
> plugins/
>   platforms/
>     qwindows.dll
> ```
>
> 1. 如果你想让 Debug 运行时也更好调试 Qt 插件加载，运行时加：
>
> - 环境变量：`QT_DEBUG_PLUGINS=1`
>
> CLion Run Configuration 里加一下，能直接看到它到底在找哪个路径、为什么加载失败。
>
> ------
>
> ## 你现在这个错误该怎么快速验证已修复？
>
> 改完后重新构建，`windeployqt` 输出里应该能看到它最终把 `platforms/qwindows.dll` 部署进你的 `bin/plugins/platforms`，并且不会再出现：
>
> - `Unable to find the platform plugin.`
>
> ------
>
> 如果你愿意把你那张“插件列表截图”也贴出来（或把 `build/mingw/debug/bin/plugins/platforms` 的目录列表贴出来），我可以再帮你确认：到底是 `windeployqt` 的“校验逻辑挂了”，还是你的部署目录/plugindir 组合导致它检查错了位置。你现在这份日志更像是 **`--debug` 触发了 windeployqt 的错误判定**。
