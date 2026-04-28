## Context

当前的 ESP32 设备通过 `device_link` 协议接收两类用户交互事件：

1. **Approval（PermissionRequest）**：工具执行权限请求，显示 Accept/Decline/Always 按钮
2. **Prompt（Elicitation/AskUserQuestion）**：选项询问，显示选项按钮列表

这两类事件共享相同的设备端处理流程：存储状态 → 强制切换到 Home app → 显示 overlay。但 prompt 类型在小屏幕上体验不佳，且用户更倾向于在 terminal 上回答。

Approval 类型在设备上显示有价值——用户可以快速授权或拒绝工具调用。

## Goals / Non-Goals

**Goals:**
- 保留设备上 approval overlay 的完整显示和交互逻辑
- 移除设备上 prompt overlay 的显示，改为 sprite 状态提醒
- 确保 terminal 上正常显示 prompt 询问（不被设备 overlay 干扰）
- 提供独立的 chibi 测试命令验证两种行为

**Non-Goals:**
- 不修改 approval overlay 的 UI 结构或交互方式
- 不修改 device_link 协议帧格式
- 不修改 Claude Code hook 脚本的执行模式
- 不修改 approval/prompt store 的核心数据结构

## Decisions

### 1. Approval overlay 保留，Prompt overlay 移除
**决策**:
- `claude.approval.request` → 继续显示 approval overlay（Accept/Decline/Always）
- `claude.prompt.request` → 不显示 overlay，仅更新 sprite emotion 为 "waiting" + 气泡 "Need input"

**理由**:
- Approval 是二元决策（允许/拒绝），适合设备快速操作
- Prompt 通常包含多个选项和描述，在小屏幕上显示效果差
- 用户在 terminal 上回答 prompt 更自然（可以阅读完整描述）

### 2. Prompt 状态通过 snapshot 同步到设备
**决策**: 当 agent 收到 prompt 时，不再发送 `claude.prompt.request` 事件到设备，而是在 snapshot 中包含 `has_pending_prompt` 标志。设备根据 snapshot 更新 sprite 状态。

**理由**:
- snapshot 已经定期发送到设备，复用现有机制
- 避免 device_link 协议中新增事件类型
- terminal 选择完成后，snapshot 更新为 `has_pending_prompt: false`，sprite 自动恢复

### 3. Approval 状态继续通过 device_link 事件同步
**决策**: `claude.approval.request` 和 `claude.approval.dismiss` 事件继续通过 device_link 发送，保持现有的阻塞式交互流程。

**理由**:
- Approval 需要设备端实时响应（用户点击按钮）
- 现有事件机制工作良好，无需修改
- 移除 prompt 事件后，approval 的处理更加清晰

### 4. Chibi 测试命令区分类型
**决策**:
- `chibi approval` → 发送 `claude.approval.request`，测试设备 overlay
- `chibi prompt` → 发送 `claude.prompt.request`，测试 sprite 提醒

**理由**:
- 两种行为不同，需要独立的测试命令
- 与现有的 `chibi approve` 命令（RPC 方式）区分开

## Risks / Trade-offs

**[Risk]** 用户可能混淆：为什么 approval 有 overlay 而 prompt 没有？
→ **Mitigation**: 在文档中明确说明两种类型的区别。sprite 的气泡文本也帮助区分（"Need permission" vs "Need input"）。

**[Risk]** Prompt 在 terminal 上显示时，用户可能没看到设备上的 sprite 提醒
→ **Mitigation**: sprite 的 "waiting" emotion 应足够明显（如动画效果）。同时 terminal 上的 Claude Code 询问是主要交互渠道。

**[Risk]** 同时有 approval 和 prompt pending 时，sprite 状态如何显示？
→ **Mitigation**: approval 优先级高于 prompt。如果两者都有，sprite 显示 "thinking"（approval 状态）。

## Migration Plan

1. **Phase 1**: 在设备端移除 prompt overlay 显示，保留 approval overlay
2. **Phase 2**: 在 snapshot 中添加 `has_pending_prompt` 字段，设备端解析并更新 sprite
3. **Phase 3**: 在 agent 端修改 prompt 同步逻辑（通过 snapshot 而非 device_link 事件）
4. **Phase 4**: 添加 chibi 测试命令并验证

回滚策略：如需恢复 prompt overlay，只需恢复 `home_runtime.c` 中的事件处理代码。

## Open Questions

1. 是否需要在 snapshot 中包含 prompt 的具体内容（如问题文本），以便设备显示更详细的气泡？
2. 设备端的 pending prompt 状态是否需要超时清除？
3. 当 approval 和 prompt 同时存在时，sprite 气泡文本应如何显示？
