# Windows 打包说明

本文说明 `fplayer-ff-desktop` 在 Windows 下如何生成安装包（`.exe` / `.msi`）。

## 1. 目标与产物

- 构建产物：应用可执行文件及依赖（Qt 运行时由安装阶段自动部署）
- 安装包产物：
  - NSIS：`setup.exe`
  - WiX：`msi`
- 打包中间目录：`disk/package-build`（避免污染 `build`）
- 安装暂存目录：`disk/package-install`（默认，不写入 `Program Files`）
- 统一输出目录：`disk/windows`（可在脚本参数中覆盖）

## 2. 前置依赖

必需：

- CMake（含 CPack）
- Visual Studio 2022（MSVC v143 + Windows SDK）
- Qt 6.10.2（与工程配置一致）

按需（决定打包类型）：

- NSIS（提供 `makensis`，用于生成 `.exe`）
- WiX Toolset（提供 `candle` 或 `wix`，用于生成 `.msi`）

## 3. 一键打包脚本

项目已提供脚本：

- `scripts/package-windows.ps1`

在项目根目录执行：

```powershell
.\scripts\package-windows.ps1
```

注意：脚本启动时会校验 Qt 参数。若未传入或路径不正确，会直接报错并给出示例命令。
请至少传入以下其一（建议两个都传）：

- `-Qt6Dir`
- `-CMakePrefixPath`

默认行为：

1. `cmake -S . -B disk/package-build`
2. `cmake -S . -B disk/package-build -DCMAKE_INSTALL_PREFIX=disk/package-install`
3. `cmake --build disk/package-build --config Release`
4. `cmake --install disk/package-build --config Release`
5. `cpack --config disk/package-build/CPackConfig.cmake -C Release`
6. 将安装包复制到 `disk/windows`

## 4. 常用参数

```powershell
# Qt 参数（建议两个都传）
.\scripts\package-windows.ps1 `
  -Qt6Dir "D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64/lib/cmake/Qt6" `
  -CMakePrefixPath "D:/SoftWare/Qt/Qt6.10.2/6.10.2/msvc2022_64"

# 指定配置（Debug/Release/RelWithDebInfo/MinSizeRel）
.\scripts\package-windows.ps1 -Config Release

# 指定构建目录
.\scripts\package-windows.ps1 -BuildDir build-win

# 指定安装暂存目录（避免写入系统目录）
.\scripts\package-windows.ps1 -InstallDir disk/custom-install

# 指定 CMake 生成器
.\scripts\package-windows.ps1 -Generator "Visual Studio 17 2022"

# 跳过 install 阶段
.\scripts\package-windows.ps1 -SkipInstall

# 自定义产物汇总目录
.\scripts\package-windows.ps1 -OutputDir disk/release/windows
```

## 5. CPack 配置说明

根 `CMakeLists.txt` 已启用：

- `CPACK_GENERATOR "NSIS;WIX"`
- `CPACK_WIX_UPGRADE_GUID`（固定值，用于升级识别）
- `CPACK_WIX_PRODUCT_GUID "*"`（每次构建自动生成）

注意事项：

- `CPACK_WIX_UPGRADE_GUID` 应保持稳定，不要随版本改动
- 若缺失 NSIS/WiX，对应类型安装包不会生成，脚本会给出告警

## 6. 常见问题

- 启动即报“缺少 Qt 参数”
  - 需要传入 `-Qt6Dir` 或 `-CMakePrefixPath`（建议两个都传）
- 报“`-Qt6Dir` 目录中未找到 `Qt6Config.cmake`”
  - 检查 `-Qt6Dir` 是否指向 `.../lib/cmake/Qt6`
- `Install` 阶段提示无权限写入 `Program Files`
  - 使用脚本默认安装暂存目录 `disk/package-install`，或显式传 `-InstallDir`
- `cmake not found in PATH`
  - 把 CMake 加入 PATH，或使用 VS 开发者终端
- 只有 `.exe` 没有 `.msi`
  - 检查是否安装 WiX Toolset，且 `candle`/`wix` 在 PATH
- 只有 `.msi` 没有 `.exe`
  - 检查是否安装 NSIS，且 `makensis` 在 PATH
