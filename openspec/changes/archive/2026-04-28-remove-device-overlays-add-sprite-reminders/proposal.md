## Why

当前 ESP32 设备上的非授权询问（PermissionRequest）和提示（Elicitation/AskUserQuestion）会先于 terminal 的询问出现，并**阻塞** terminal 的选项显示。这是因为 `claude respond` hook 使用阻塞模式执行，设备 overlay 显示期间终端交互被挂起。如果设备不在手边或用户更倾向于终端操作，就无法继续。

此外，当用户在终端完成选择后，设备上的询问 overlay 不会自动消失，造成状态不一致。

## What Changes

- **移除** ESP32 设备上的 approval 和 prompt overlay 显示逻辑（home_approval.c、home_prompt.c 的显示触发）
- **保留** device_link 协议中的 approval/prompt 存储和事件处理，但不再强制切换到 Home app 显示 overlay
- **新增** chibi sprite 提醒状态：当有 pending approval/prompt 时，sprite 显示特定的 emotion（如 "thinking"、"waiting"）和气泡提示
- **修复** 终端选择后设备 overlay 不消失的问题（通过移除 overlay 机制本身解决）
- **新增** `chibi prompt` 和 `chibi approval` 测试命令，方便开发者验证 sprite 提醒行为

## Capabilities

### New Capabilities
- `sprite-reminder-state`: chibi sprite 根据 pending approval/prompt 状态自动切换表情和气泡
- `chibi-test-commands`: 新增 chibi CLI 测试命令，无需触发真实 hook 即可验证行为

### Modified Capabilities
- `device-prompt-ui`: **移除**设备上 approval/prompt overlay 的 REQUIREMENTS，改为 sprite 提醒
- `prompt-response-protocol`: 简化协议，设备不再发送 device-side response（终端已处理）

## Impact

- `src/apps/app_home/src/home_approval.c`: 移除显示逻辑，保留数据结构
- `src/apps/app_home/src/home_prompt.c`: 移除显示逻辑，保留数据结构
- `src/apps/app_home/src/home_runtime.c`: 移除 APP_EVENT_PERMISSION_REQUEST/_PROMPT_REQUEST 的事件处理
- `src/components/device_link/src/device_link.c`: 保留协议解析，但不再触发 UI 事件
- `src/components/service_claude/`: 新增根据 pending 状态更新 sprite emotion 的逻辑
- `tools/esp32dash/src/agent.rs`: 简化设备同步逻辑（无需同步 approval/prompt 到设备）
- `tools/esp32dash/src/main.rs`: 新增 chibi 测试命令
