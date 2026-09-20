# Mineworld 开发说明

## 工作环境

- 当前环境是 Windows PowerShell。`apply_patch.bat` 不能可靠接收包含中文等非 ASCII 文本的多行 UTF-8 参数。
- 包含非 ASCII 文本的补丁不要经过 here-string、管道或 `apply_patch.bat`；应直接调用底层 `codex.exe --codex-run-as-apply-patch <patch>`。
- 每个会话最多解析一次底层 `codex.exe` 路径，优先直接读取 `(Get-Command codex.exe).Source` 的完整字符串，避免表格输出截断路径；后续补丁复用该路径，不要反复读取工作区外的 `.codex` 或 VS Code 扩展目录。
- 补丁内容统一使用 UTF-8 和 LF，并确保正确结束于 `*** End Patch`。将这些限制视为已知环境约束，除非兼容方式也失败，否则不要向用户报告中间尝试。
- Windows PowerShell 调用原生程序时会重新解析参数，可能移除多行补丁中的双引号。已验证的方式是让 Python 读取仓库内 UTF-8 补丁文件，再用 `subprocess.run([codex_path, '--codex-run-as-apply-patch', patch])` 直接传递参数列表，并传播退出码；不要把补丁正文重新拼成 shell 命令。
- 同一逻辑阶段的相关修改应尽量合并，减少重复启动补丁程序；如果接近 Windows 命令行长度限制，则按文件或完整逻辑阶段拆分，不要原样重试超长命令。
- 多文件或多 hunk 补丁失败时，前面的修改可能已经生效。重试前必须检查所有目标文件，不要假设补丁应用是原子的。
- 需要临时保存补丁时，只在当前仓库内使用固定临时文件，不要使用系统 `%TEMP%`；补丁完成后立即删除。
- 如果命令策略不允许把补丁应用和临时文件清理组合在同一命令中，应分开执行，并在应用完成后立即清理仓库内临时文件。
- 不为普通仓库修改主动请求沙箱升级。只有任务确实需要访问工作区外资源且当前权限不足时，才请求用户批准并说明具体原因。

## 构建与生成文件

- 项目使用 C++20、CMake、Ninja 和 vcpkg，生成同一个 `mineworld` 可执行文件，通过启动参数选择客户端或服务端。
- 修改代码后的首选验证命令是 `cmake --build build --config Debug`。
- 先检查 `build/CMakeCache.txt` 的生成器与实际构建配置。单配置 `Ninja` 使用 `CMAKE_BUILD_TYPE`，`--config Debug` 不会覆盖它；不要仅根据命令参数宣称通过了 Debug 构建，也不要为验证擅自切换现有构建配置。
- Codex 在普通 Windows PowerShell 中统一使用下方“Windows 构建命令”准备 MSVC 环境并构建；不新建临时 `.cmd` / `.bat`。后续构建和临时编译沿用这一方式，不依赖上一次会话的环境。
- 临时 C++ 验证文件放在仓库内、`src/` 外的专用目录，避免被源码 glob 编入正式程序。编译时复用目标所需的 C++20、`/utf-8`、宏定义、运行库及依赖设置；fmt 的 Unicode 支持要求 `/utf-8`。
- 临时验证优先使用公开接口。不要通过 `#define private public` 调用另一个编译单元中的私有方法；MSVC 的符号修饰包含访问级别，这种做法会导致链接失败。
- 不要直接修改 `build/`、`bin/` 或其他生成产物。
- FlatBuffers 协议源文件是 `src/net_protocol.fbs`。协议变更同时更新 `src/net_protocol.h` 和 `src/net_protocol.cpp`，不要修改 `build/src/generated/net_protocol_generated.h`。

### Windows 构建命令

在仓库根目录执行以下 PowerShell 命令。从 CMake 缓存中的编译器路径定位同一 Visual Studio 安装的 `VsDevCmd.bat`，不写死用户名、安装目录或 Visual Studio 版本。环境初始化与构建在同一个 `cmd` 进程中完成；初始化失败时不继续构建，构建失败时向调用方报错。标准错误在 `cmd` 内合并到标准输出，按退出码判断是否成功。

```powershell
$buildCompiler = (Select-String -LiteralPath build/CMakeCache.txt -Pattern '^CMAKE_CXX_COMPILER:FILEPATH=(.+)$').Matches.Groups[1].Value
$buildVsDevCmd = $buildCompiler.Replace('\', '/') -replace '/VC/Tools/MSVC/.*$', '/Common7/Tools/VsDevCmd.bat'
if (-not (Test-Path -LiteralPath $buildVsDevCmd)) { throw 'Cannot locate VsDevCmd.bat for the cached compiler.' }
& $env:ComSpec /d /s /c ('call "{0}" -arch=x64 -host_arch=x64 >nul && cmake --build build --config Debug 2>&1' -f $buildVsDevCmd)
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
```

换机器或 Visual Studio 安装路径变更后，需要用本机工具链重新配置 CMake，不能沿用另一台机器的 `build/CMakeCache.txt`。首次配置在 Visual Studio 的 Developer PowerShell / Command Prompt 中进行，并指定本机的 vcpkg 工具链。已有完整开发环境时可以直接执行首选构建命令。

## 项目协作

- `README.md` 负责记录项目架构、当前实现状态和后续计划。开始实现前先阅读相关章节，并以实际代码确认 README 描述是否仍然有效。
- 不在 `AGENTS.md` 重复具体模块设计、状态机、协议字段或阶段计划，避免两处内容不同步。
- 保持改动聚焦，遵循现有命名、文件布局和 `.clang-format` 风格，不顺手重构无关代码。补丁后注意 LF/CRLF 混用；格式检查出现大量行尾报错时，先统一改动文件的换行符，避免误判为大量代码风格问题。
- 修改 C/C++ 源文件后，对所有改动过的 C/C++ 文件执行 `clang-format -i <files...>`；不要对未改动文件做全量格式化。
- 修改跨客户端、服务端或协议边界的功能时，检查所有相关调用方和关闭流程是否保持一致。
- 使用现有 Profiler 宏观察性能，避免在热路径增加高频普通日志。
- 性能优化验证同时检查总耗时和单次调用峰值，覆盖大批量任务同时到期等集中处理场景。报告基准时说明规模、实际构建配置和测量范围；仅调度的微基准不能当作完整游戏帧率。
- 临时验证代码及其编译产物必须在任务结束前删除。修改代码后最终至少执行首选构建命令，无法验证时明确说明原因；仅修改文档时检查差异与文本格式即可。
