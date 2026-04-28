## Context

当前的 ESP32 设备通过 device_link 协议与主机 agent 通信，显示 PermissionRequest 和 Prompt（Elicitation/AskUserQuestion）overlay。主机端的 `esp32dash` 提供了 `chibi` 命令用于测试 sprite 状态和文本气泡，但缺少交互式选项测试能力。

存在两个核心问题：
1. **无法便捷测试**：开发者需要实际触发 Claude hook 才能看到设备上的 prompt/approval 显示，缺乏独立的测试手段
2. **终端选择后设备不 dismiss**：当用户通过终端（而非设备）完成选择后，设备上的 overlay 不会自动消失，因为缺少从终端向 agent 同步 "已处理" 状态的机制

## Goals / Non-Goals

**Goals:**
- 提供 `chibi prompt` 和 `chibi approval` CLI 命令，允许开发者在终端发起测试并观察设备行为
- 修复终端 hook 处理器中用户选择后设备 overlay 不消失的问题
- 改进设备上 prompt/approval overlay 的布局和可读性

**Non-Goals:**
- 不修改 device_link 协议本身的帧格式
- 不引入新的 hook 类型
- 不修改 approval/prompt store 的核心数据结构

## Decisions

### 1. Chibi 命令使用直接串口通信
**决策**: `chibi prompt` 和 `chibi approval` 通过直接串口发送 `claude.prompt.request` 和 `claude.approval.request` 事件，而非通过 agent 的 HTTP API。
**理由**:
- 保持与现有 `chibi test/demo/approve` 命令一致的通信模式
- 不依赖 agent 是否运行，更便于独立测试
- 发送后等待设备响应并打印结果，形成完整的测试闭环

### 2. 终端选择后通过显式 resolve API 通知 agent
**决策**: 在 `handle_permission_request` 和 `handle_ask_user_question`/`handle_elicitation` 中，当检测到用户通过终端（而非设备）完成选择后，调用新增的 agent HTTP API 显式 resolve pending item。
**理由**:
- 当前只有设备通过串口发送 `claude.approval.resolved`/`claude.prompt.response` 时才会触发 `resolve()`
- 终端 hook 处理器直接输出结果给 Claude Code，但 agent 中的 pending item 仍处于未解决状态
- 新增 `POST /v1/claude/approvals/{id}/resolve` 和 `POST /v1/claude/prompts/{id}/resolve` 端点，允许外部 resolve
- resolve 后会触发 `sync_device_approvals()`，从而发送 dismiss 事件到设备

### 3. 改进设备 UI 的布局参数
**决策**: 调整 approval/prompt overlay 的按钮高度、间距和字体使用，使其在 640x172 横屏上更易阅读和触摸。
**理由**:
- 当前按钮高度 32px 在 3.5 寸屏上偏小
- 增加按钮内边距和间距，提升触摸体验
- 保持现有结构不变，仅调整样式参数

## Risks / Trade-offs

**[Risk]** 新增 resolve API 可能引入安全风险，允许未授权客户端 resolve approval/prompt
→ **Mitigation**: 目前 agent 监听 localhost，且该变更仅用于本地开发测试。后续可考虑添加简单的 token 验证。

**[Risk]** 终端选择后显式 resolve 可能与设备端 resolve 产生竞态
→ **Mitigation**: `ApprovalStore::resolve` 和 `PromptStore::resolve` 已使用 Mutex 保护，且会检查 `entry.resolution.is_some()`，重复 resolve 会返回 None，不会导致错误。

**[Risk]** UI 布局调整后可能与其他 overlay（如 screensaver）产生视觉冲突
→ **Mitigation**: 仅调整内边距、按钮高度等参数，不改变 overlay 的整体层级和尺寸。变更后需在实际设备上验证。

## Migration Plan

无需迁移步骤。此变更为纯新增功能加 bug 修复，不破坏现有 API 或协议。

## Open Questions

1. 是否需要为 chibi 命令添加超时参数，以便测试超时场景？
2. 是否需要在设备 UI 上显示更丰富的选项信息（如选项描述而非仅标签）？
