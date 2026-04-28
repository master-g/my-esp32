## Why

当前的 ESP32 设备上的非授权询问（PermissionRequest）和提示（Elicitation/AskUserQuestion）显示效果需要改进，且存在终端选择后设备询问界面不会自动消失的问题。为了给用户提供一个可复现的测试手段并修复该缺陷，需要新增 chibi 交互式选项命令并修复 dismiss 同步逻辑。

## What Changes

- **新增** `chibi prompt` 子命令：支持在终端上发起一个带选项的 prompt/approval 测试，让用户可以在终端选择答案并观察设备行为
- **新增** `chibi approval` 子命令：支持在终端上发起一个权限请求测试
- **修复** 终端 hook 处理器中，用户选择后设备询问界面未 dismiss 的问题
- **改进** 设备上 prompt/approval overlay 的显示效果（布局、字体大小、按钮尺寸）

## Capabilities

### New Capabilities
- `chibi-interactive-prompt`: 新增 chibi CLI 交互式 prompt 测试功能，支持终端用户选择并同步到设备

### Modified Capabilities
- `prompt-response-protocol`: 修复终端选择后设备 overlay 不 dismiss 的问题
- `device-prompt-ui`: 改进设备上 prompt/approval overlay 的显示效果

## Impact

- `tools/esp32dash/src/main.rs`: 新增 chibi 子命令定义和处理逻辑
- `tools/esp32dash/src/agent.rs`: 修复终端选择后的 dismiss 同步逻辑
- `src/apps/app_home/src/home_approval.c`: 改进 approval UI 显示效果
- `src/apps/app_home/src/home_prompt.c`: 改进 prompt UI 显示效果
