# 发包与发布规范（Desktop / Windows）

本文档用于约定 `fplayer-ff-desktop` 的标准发包流程、脚本行为和发布检查项。

## 1. 适用范围

- Windows 安装包（NSIS：`.exe`）
- Windows 安装包（WiX：`.msi`，当本机已安装 WiX）

## 2. 统一入口

统一使用脚本：`scripts/package-windows.ps1`

示例：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\package-windows.ps1 -Qt6Dir "D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64/lib/cmake/Qt6" -CMakePrefixPath "D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64"
```

## 3. 脚本行为约定

### 3.1 构建行为

- 仅支持 Windows
- 仅在 MSVC 工具链下启用 CPack 的 Windows 打包配置（见根 `CMakeLists.txt`）
- 默认构建配置：`Release`
- 默认构建目录：`disk/package-build`
- 默认安装暂存目录：`disk/package-install`
- 默认发布目录：`disk/windows`

### 3.2 Qt 参数校验（启动即校验）

脚本启动时会校验 Qt 参数，以下至少传一个（建议两个都传）：

- `-Qt6Dir`
- `-CMakePrefixPath`

若参数缺失、路径不存在、或 `-Qt6Dir` 下缺少 `Qt6Config.cmake`，脚本会直接失败并输出示例命令。

### 3.3 打包生成器选择

- 检测到 `makensis`：执行 NSIS 打包（`.exe`）
- 检测到 `candle` 或 `wix`：执行 WiX 打包（`.msi`）
- 两者都检测不到：直接失败并提示安装工具

说明：

- 如果仅安装了 NSIS，脚本会只生成 `.exe`，不会因为缺 WiX 而失败
- 如果同时安装 NSIS + WiX，脚本会同时尝试生成 `.exe` 与 `.msi`

### 3.4 失败即停

- `Configure` / `Build` / `Install` / `Package` 任一步失败会立即退出
- 不再执行后续步骤，避免“连锁噪音报错”

### 3.5 产物收集行为

- 脚本会扫描 `disk/package-build` 下的安装包文件（`.exe` / `.msi` / `.zip` / `.7z`）
- 并复制到 `disk/windows`（或你传入的 `-OutputDir`）

## 4. 参数矩阵

- `-Config <Debug|Release|RelWithDebInfo|MinSizeRel>`：构建配置
- `-BuildDir <path>`：构建目录（默认 `disk/package-build`）
- `-InstallDir <path>`：安装暂存目录（默认 `disk/package-install`）
- `-OutputDir <path>`：安装包汇总目录（默认 `disk/windows`）
- `-Generator <cmake-generator>`：CMake 生成器（如 `Visual Studio 17 2022`）
- `-SkipInstall`：跳过 install 阶段
- `-CMakePath <path>`：显式指定 `cmake.exe`
- `-CPackPath <path>`：显式指定 `cpack.exe`
- `-Qt6Dir <path>`：Qt6 CMake 包目录（通常到 `.../lib/cmake/Qt6`）
- `-CMakePrefixPath <path>`：Qt 安装前缀（通常到 `.../msvc2022_64`）

## 5. 推荐发布流程

1. 确认使用 MSVC 工具链
2. 准备 Qt 路径（`-Qt6Dir` + `-CMakePrefixPath`）
3. 执行打包脚本（建议使用 `Release`）
4. 检查 `disk/windows` 中是否出现目标安装包
5. 分发前进行安装验证（含“可自定义安装路径”验证）

## 5.1 `build/` 与 `disk/` 目录职责

- `build/`：你项目原有的常规构建目录，不用于本脚本的打包流程
- `disk/package-build`：打包中间构建目录（脚本默认）
- `disk/package-install`：安装暂存目录（脚本默认）
- `disk/windows`：可分发安装包目录（脚本默认）

## 5.2 成功判定标准

- 以 `disk/windows/` 为主判定
- 成功标准（按目标）：
  - NSIS 目标：存在 `*.exe` 安装包
  - WiX 目标：存在 `*.msi` 安装包

## 6. 常见命令

```powershell
# 标准打包（推荐）
powershell -ExecutionPolicy Bypass -File .\scripts\package-windows.ps1 -Qt6Dir "D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64/lib/cmake/Qt6" -CMakePrefixPath "D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64"

# 自定义目录（不污染默认目录）
powershell -ExecutionPolicy Bypass -File .\scripts\package-windows.ps1 -Qt6Dir "D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64/lib/cmake/Qt6" -CMakePrefixPath "D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64" -BuildDir "disk/custom/build" -InstallDir "disk/custom/install" -OutputDir "disk/custom/out"

# 指定 CMake/CPack 路径
powershell -ExecutionPolicy Bypass -File .\scripts\package-windows.ps1 -Qt6Dir "D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64/lib/cmake/Qt6" -CMakePrefixPath "D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64" -CMakePath "D:/SoftWare/CMake/bin/cmake.exe" -CPackPath "D:/SoftWare/CMake/bin/cpack.exe"
```
