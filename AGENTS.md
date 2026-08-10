# Mineworld 开发说明

## 工作环境

- 当前环境是 Windows PowerShell。`apply_patch.bat` 不能可靠接收包含中文等非 ASCII 文本的多行 UTF-8 参数。
- 包含非 ASCII 文本的补丁不要先经过 here-string、管道或 `apply_patch.bat`；应解析包装器中的 `codex.exe`，通过支持 UTF-8 的方式直接调用 `codex.exe --codex-run-as-apply-patch <patch>`。
- 补丁内容统一使用 UTF-8 和 LF，并确保正确结束于 `*** End Patch`。将此视为已知环境约束，除非兼容方式也失败，否则不要向用户报告中间尝试。
- 每个会话最多解析一次底层补丁程序路径；后续补丁复用该路径，不要反复读取工作区外的 `.codex` 或 VS Code 扩展目录。
- 需要临时保存补丁时，只在当前仓库内使用固定临时文件，并在命令结束时删除；不要使用系统 `%TEMP%`。
- 尽量将同一逻辑阶段的相关修改合并为一个补丁，避免因补丁过碎而重复启动工作区外程序或触发多次沙箱审批。
- 不为普通仓库修改主动请求沙箱升级。只有任务确实需要访问工作区外资源时才请求用户批准，并说明具体原因。

## 构建与生成文件

- 项目使用 C++20、CMake、Ninja 和 vcpkg，生成同一个 `mineworld` 可执行文件，通过启动参数选择客户端或服务端。
- 修改代码后的首选验证命令是 `cmake --build build --config Debug`。
- Codex 的普通 PowerShell 可能没有加载 MSVC 的 INCLUDE/LIB 环境。若构建报告找不到 `algorithm` 等标准库头文件，先调用 Visual Studio 的 `VsDevCmd.bat`，再执行首选构建命令；不要把该问题误判为源码错误。
- 不要直接修改 `build/`、`bin/` 或其他生成产物。
- FlatBuffers 协议源文件是 `src/net_protocol.fbs`。协议变更同时更新 `src/net_protocol.h` 和 `src/net_protocol.cpp`，不要修改 `build/src/generated/net_protocol_generated.h`。

## 项目协作

- `README.md` 负责记录项目架构、当前实现状态和后续计划。开始实现前先阅读相关章节，并以实际代码确认 README 描述是否仍然有效。
- 不在 `AGENTS.md` 重复具体模块设计、状态机、协议字段或阶段计划，避免两处内容不同步。
- 保持改动聚焦，遵循现有命名、文件布局和 `.clang-format` 风格，不顺手重构无关代码。
- 修改跨客户端、服务端或协议边界的功能时，检查所有相关调用方和关闭流程是否保持一致。
- 使用现有 Profiler 宏观察性能，避免在热路径增加高频普通日志。
- 临时验证代码必须在任务结束前删除；最终至少执行首选构建命令，无法验证时明确说明原因。
