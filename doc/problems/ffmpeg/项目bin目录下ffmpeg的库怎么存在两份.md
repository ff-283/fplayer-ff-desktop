这是执行cmake脚本之后的。我的backend-ffmpeg的cmake脚本，是将3rd/ffmpeg下的win/bin中的内容拷贝过来。
刚执行完cmake脚本之后，bin目录下是正常的。

![1.png](../../img/problems/ffmpeg/1.png)

执行完构建之后，发现多了很多低版本的ffmepg库：

![2.png](../../img/problems/ffmpeg/2.png)

想了想，发现可能是windeployqt安装的qt中以来的ffmpeg库