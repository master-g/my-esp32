## Why

当前的 ESP32 设备同时显示 approval（PermissionRequest）和 prompt（Elicitation/AskUserQuestion）overlay。但 prompt 类型的询问在设备小屏幕上显示效果不佳（选项拥挤、描述被截断），且用户更倾向于在终端（terminal）上回答这类问题。

Approval 类型的授权请求（如 Bash 命令执行）在设备上显示是有价值的——用户可以快速看到工具名称和命令摘要，一键允许或拒绝。

此外，当用户在 terminal 上完成选择后，设备上的 prompt overlay 不会自动消失，造成状态不一致。

## What Changes

- **保留** ESP32 设备上的 approval overlay（PermissionRequest）显示和交互逻辑
- **移除** ESP32 设备上的 prompt overlay（Elicitation/AskUserQuestion）显示逻辑
- **新增** sprite 状态提醒：当有 pending prompt 时，sprite 显示 "waiting" emotion 和 "Need input" 气泡
- **修复** 终端选择后设备 prompt overlay 不 dismiss 的问题（通过移除 overlay 解决）
- **新增** `chibi prompt` 测试命令，验证 sprite 提醒行为
- **保留** `chibi approval` 测试命令，测试 approval overlay

## Capabilities

### New Capabilities
- `sprite-prompt-reminder`: chibi sprite 根据 pending prompt 状态显示提醒
- `chibi-interactive-commands`: 新增 chibi CLI 测试命令，支持 prompt 和 approval 测试

### Modified Capabilities
- `device-prompt-ui`: 移除 prompt overlay 的 REQUIREMENTS，保留 approval overlay
- `prompt-response-protocol`: 设备不再显示 prompt overlay，终端选择后通过 snapshot 同步清除 sprite 提醒

## Impact

- `src/apps/app_home/src/home_prompt.c`: 移除显示逻辑，保留数据结构供 sprite 查询
- `src/apps/app_home/src/home_runtime.c`: 移除 APP_EVENT_PROMPT_REQUEST/_DISMISS 的事件处理
- `src/components/service_claude/`: 新增根据 pending prompt 状态更新 sprite emotion 的逻辑
- `src/components/device_link/src/device_link.c`: 保留 prompt 协议解析但不再触发 UI overlay
- `tools/esp32dash/src/agent.rs`: 简化 prompt 到设备的同步，改为 snapshot 更新
- `tools/esp32dash/src/main.rs`: 新增 chibi 测试命令
