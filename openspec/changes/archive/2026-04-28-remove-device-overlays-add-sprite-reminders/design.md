## Context

当前的 hook 脚本（`hooks.rs`）将 `PermissionRequest`、`Elicitation` 和 `AskUserQuestion` 事件标记为 `IS_RESPOND_EVENT`，并通过 `exec` 执行 `esp32dash claude respond --event-from-stdin`。这导致：

1. **阻塞终端**：Claude Code 阻塞等待 hook 返回，终端上不会显示 Claude Code 自己的询问对话框
2. **设备优先**：用户必须在设备上做出选择，终端交互被完全挂起
3. **状态不一致**：终端选择完成后，设备上的 overlay 不会自动消失

根本原因是：**hook handler 的阻塞执行与设备 overlay 的全屏抢占存在冲突**。设备屏幕不适合作为终端询问的替代界面。

## Goals / Non-Goals

**Goals:**
- 移除 ESP32 设备上 approval/prompt overlay 的显示
- 让 chibi sprite 在有 pending approval/prompt 时显示视觉提醒（emotion + 气泡）
- 确保终端上正常显示 Claude Code 的询问，不被阻塞
- 提供独立的 chibi 测试命令验证 sprite 状态变化

**Non-Goals:**
- 不修改 device_link 协议帧格式（保留向后兼容）
- 不修改 Claude Code hook 脚本的阻塞执行模式（这是 hooks 的设计）
- 不修改 approval/prompt store 的核心数据结构
- 不在设备上实现任何新的输入交互方式

## Decisions

### 1. 完全移除设备 overlay，改为 sprite 状态提示
**决策**: 不再在设备上显示 approval/prompt overlay。当 agent 收到 PermissionRequest/Elicitation/AskUserQuestion 时，仅更新 chibi sprite 的 emotion 为 "thinking" 或 "waiting"，并显示一个简短的气泡文本（如 "Need your input"）。终端上 Claude Code 正常显示询问对话框，用户可以在终端选择。
**理由**:
- 设备屏幕尺寸（640x172）不适合展示复杂的权限/选项信息
- 阻塞式 hook 设计下，设备 overlay 会抢占终端交互
- sprite 状态提示是非侵入式的，不影响终端操作

### 2. 保留 device_link 协议但简化设备端处理
**决策**: 保留 `claude.approval.request`、`claude.prompt.request` 等协议事件，但设备端收到后仅更新内部状态（用于 sprite 显示），不再创建 LVGL overlay。
**理由**:
- 保留协议兼容性，未来如需恢复 overlay 功能可轻松实现
- 设备仍需知道 pending 状态以更新 sprite
- 避免大规模修改 device_link 的协议解析代码

### 3. 终端选择后自动清除 sprite 提醒
**决策**: 当终端 hook handler（`handle_permission_request`、`handle_elicitation`、`handle_ask_user_question`）完成选择后，通过 agent 的 HTTP API 或事件机制通知设备清除 pending 状态，sprite 恢复为 "idle" 或 "happy" 状态。
**理由**:
- 用户在终端完成选择后，设备上的提醒应立即消失
- 与现有的 `sync_device_approvals()` 机制一致
- 无需新增复杂的同步逻辑

### 4. Chibi 测试命令直接操作 sprite 状态
**决策**: `chibi prompt` 和 `chibi approval` 命令通过直接串口发送 `claude.approval.request`/`claude.prompt.request` 事件，触发 sprite 状态变化，但不阻塞等待响应。
**理由**:
- 测试 sprite 提醒状态变化，无需完整的交互闭环
- 与现有的 `chibi test/demo` 命令模式一致
- 不依赖 agent 是否运行

## Risks / Trade-offs

**[Risk]** 移除设备 overlay 后，用户可能完全依赖终端，忽略设备上的 sprite 提醒
→ **Mitigation**: sprite 的 emotion 变化应足够明显（如闪烁、表情动画），气泡文本应简洁清晰。同时保留声音提醒（如有扬声器）。

**[Risk]** 多个 pending approval/prompt 同时存在时，sprite 状态可能频繁切换
→ **Mitigation**: sprite 状态应基于 "是否有任何 pending item" 而非具体数量，避免频繁变化。当最后一个 pending item 解决后，状态才恢复。

**[Risk]** 设备端的 pending 状态与 agent 端可能不同步（如设备重启后）
→ **Mitigation**: 设备连接时，agent 应发送当前 snapshot（已包含 pending 状态），设备根据 snapshot 更新 sprite。

## Migration Plan

1. **Phase 1**: 在设备端移除 overlay 显示逻辑，保留 pending 状态跟踪
2. **Phase 2**: 在 service_claude 中添加 sprite emotion 更新逻辑
3. **Phase 3**: 在 agent 端简化设备同步，终端选择后发送状态更新
4. **Phase 4**: 添加 chibi 测试命令并验证

回滚策略：如需恢复 overlay 功能，只需恢复 `home_runtime.c` 中的事件处理和 overlay 显示代码。

## Open Questions

1. sprite 的 "thinking" 和 "waiting" emotion 在 UI 上如何区分？是否需要新增 emotion 类型？
2. 是否需要为 pending approval 和 pending prompt 显示不同的 sprite 状态？
3. 设备断线重连后，如何确保 sprite 状态与 agent 同步？
