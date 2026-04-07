# Claude App 子设计方案

## 1. 目的

本文档细化主设计文档中的 `Claude bridge` 和 `Notify` 页面设计，目标是把以下问题一次性说清楚：

- 开发机侧 hook 输入长什么样
- bridge 如何把 noisy 的 hook 事件归一化成设备可消费的状态
- ESP32 侧 `claude_service` 和 `notify_app` 如何分工
- 什么事件算未读，什么事件只更新状态不打扰用户

本文档是 [2025-04-06-esp32-dashboard-design.md](./2025-04-06-esp32-dashboard-design.md) 第 7 节的补充细化。

## 2. 设计边界

### 2.1 本文档覆盖范围

- 开发机 hook 适配器
- bridge 内部归一化和 WebSocket 输出
- ESP32 侧 `claude_service`
- `Notify` 页的状态展示和已读语义

### 2.2 非覆盖范围

- Claude Code hooks 的安装步骤
- Claude 对话内容存档
- 多设备同步
- 远程互联网访问 bridge

## 3. 输入契约

### 3.1 hook 适配器基线

参考 [notchi-hook.sh](/Users/mg/.claude/hooks/notchi-hook.sh:1)，开发机侧最稳定的输入不是 Claude 内部对象，而是 hook 脚本已经整理过的一层轻量 JSON。

该脚本当前会输出如下字段：

```json
{
  "session_id": "sess_xxx",
  "cwd": "/path/to/project",
  "event": "PreToolUse",
  "status": "running_tool",
  "permission_mode": "default",
  "user_prompt": "optional",
  "tool": "exec_command",
  "tool_use_id": "toolu_xxx",
  "tool_input": {}
}
```

与本项目直接相关的事实：

- `status` 优先取 hook 输入中的显式字段，否则按 `event` 做本地映射
- `cwd` 可稳定用于推导 `workspace`
- `tool_input` 可能很大，也可能包含敏感内容，不适合直接发到 ESP32
- `user_prompt` 对展示有价值，但必须截断和脱敏

### 3.2 现有状态映射基线

参考 [notchi-hook.sh:22](/Users/mg/.claude/hooks/notchi-hook.sh:22) 的 `status_map`，bridge v1 保持以下映射为 canonical baseline：

| hook event | normalized status |
|------------|-------------------|
| `UserPromptSubmit` | `processing` |
| `PreCompact` | `compacting` |
| `SessionStart` | `waiting_for_input` |
| `SessionEnd` | `ended` |
| `PreToolUse` | `running_tool` |
| `PostToolUse` | `processing` |
| `PermissionRequest` | `waiting_for_input` |
| `Stop` | `waiting_for_input` |
| `SubagentStop` | `waiting_for_input` |

这份表不一定是“最终最优语义”，但它和现有 hook 行为一致，因此适合作为 bridge v1 的兼容基线。

### 3.3 bridge 输出目标

主设计文档已经定义了设备侧通用输出：

```json
{
  "seq": 1042,
  "source": "claude_code",
  "event": "notification",
  "status": "waiting_for_input",
  "title": "Awaiting confirmation",
  "workspace": "project-foo",
  "detail": "Tool call requires approval",
  "ts": 1743957123,
  "unread": true
}
```

本子设计的任务，是把 hook 输入稳定地收敛成这个输出，并补足设备侧需要但主设计里没展开的字段和状态机。

## 4. 模块拆分

### 4.1 开发机侧

开发机侧拆成三个模块：

- `hook_adapter`
- `bridge_normalizer`
- `bridge_ws_server`

职责定义：

| 模块 | 职责 |
|------|------|
| `hook_adapter` | 读取 hook stdin，调用本地 CLI 把事件提交给 daemon |
| `bridge_normalizer` | 事件归一化、去噪、生成 snapshot 和 unread 语义 |
| `bridge_ws_server` | 认证、快照响应、增量推送、心跳和重连 |

### 4.2 设备侧

设备侧拆成两个模块：

- `service_claude`
- `app_notify`

职责定义：

| 模块 | 职责 |
|------|------|
| `service_claude` | 维护连接、缓存最新快照、生成事件、管理已读 |
| `app_notify` | 渲染页面、响应触摸、调用 `service_claude_mark_read()` |

关键原则：

- `app_notify` 不解析 WebSocket JSON
- `service_claude` 不拼 UI 文案和颜色
- 桥接侧吸收 hook 差异，设备侧只接受统一协议

## 5. 状态模型

### 5.1 连接状态

`service_claude` 维护连接状态：

```c
typedef enum {
    CLAUDE_CONN_DISCONNECTED,
    CLAUDE_CONN_CONNECTING,
    CLAUDE_CONN_SYNCING,
    CLAUDE_CONN_READY,
    CLAUDE_CONN_STALE,
    CLAUDE_CONN_AUTH_FAILED,
} claude_conn_state_t;
```

说明：

- `DISCONNECTED`：未建连或连接断开
- `CONNECTING`：TCP/WebSocket 建连中
- `SYNCING`：已连接，等待 snapshot
- `READY`：已拿到合法 snapshot
- `STALE`：连接仍在，但超过超时阈值未收到任何 bridge 活动
- `AUTH_FAILED`：token 校验失败

### 5.2 运行状态

bridge 和设备共享统一的运行状态枚举：

```c
typedef enum {
    CLAUDE_RUN_UNKNOWN = 0,
    CLAUDE_RUN_WAITING_FOR_INPUT,
    CLAUDE_RUN_PROCESSING,
    CLAUDE_RUN_RUNNING_TOOL,
    CLAUDE_RUN_COMPACTING,
    CLAUDE_RUN_ENDED,
} claude_run_state_t;
```

设计理由：

- 设备侧不关心底层 hook 名字，只关心“现在是什么状态”
- 运行状态数量要少，避免 UI 层塞满分支
- `waiting_for_input` 代表“Claude 当前没有继续推进，可能在等用户”

### 5.3 页面状态

`Notify` 页还需要维护显示层状态：

- `EMPTY`：尚未收到任何有效 snapshot
- `LIVE`：连接正常且有当前状态
- `STALE`：展示的是旧数据
- `ERROR`：鉴权失败或桥接不可达

页面状态由 `claude_conn_state_t` 和快照是否存在共同决定，不单独和 bridge 协议耦合。

## 6. 归一化规则

### 6.1 原始事件到展示状态

bridge v1 使用如下归一化表：

| raw event | run state | title 模板 | detail 模板 | 默认是否记未读 |
|-----------|-----------|-------------|-------------|----------------|
| `SessionStart` | `WAITING_FOR_INPUT` | `Session ready` | `Workspace available` | 否 |
| `UserPromptSubmit` | `PROCESSING` | `Processing prompt` | 用户提示词预览 | 否 |
| `PreToolUse` | `RUNNING_TOOL` | `Running tool` | 工具名 | 否 |
| `PostToolUse` | `PROCESSING` | `Tool finished` | 工具名 | 否 |
| `PermissionRequest` | `WAITING_FOR_INPUT` | `Awaiting approval` | 工具名 + 权限模式 | 是 |
| `PreCompact` | `COMPACTING` | `Compacting context` | `Preparing context window` | 否 |
| `Stop` | `WAITING_FOR_INPUT` | `Ready for input` | 最近一次动作完成 | 是 |
| `SubagentStop` | `WAITING_FOR_INPUT` | `Subagent finished` | 最近一次子任务结束 | 否 |
| `SessionEnd` | `ENDED` | `Session ended` | 工作区结束 | 是 |
| unknown | 保持原状态或 `UNKNOWN` | `Unknown event` | raw event 名称 | 否 |

这里有两个故意和原始 hook 不同的设计：

- `Stop` 和 `SubagentStop` 的 run state 相同，但 unread 策略不同
- `PostToolUse` 不直接显示原始工具输入，避免把大块参数和敏感信息推到屏幕上

### 6.2 文本生成规则

bridge 负责生成人类可读的 `title` 和 `detail`，设备侧不再二次拼装。规则如下：

- `workspace` 取 `cwd` 的最后一级目录名
- `title` 最大 `32` 字节
- `detail` 最大 `96` 字节
- `user_prompt` 只取首行，并安全截断
- `tool_input` 默认不下发，仅在 bridge 本地日志中保留
- 所有多行文本在 bridge 侧压成单行

这样可以把文本清洗复杂度压在 bridge 层，避免 ESP32 上做字符串垃圾处理。

### 6.3 去噪和合并

Claude hook 事件会非常频繁，bridge 必须做 coalescing：

- 若新的 `run_state`、`title`、`detail` 与当前 snapshot 完全相同，则只更新时间戳，不推新 `seq`
- `PreToolUse` 紧跟 `PostToolUse` 且耗时低于 `300 ms` 时，可只保留 `PostToolUse`
- 同一 `tool_use_id` 的重复事件按最后一条覆盖

目标不是保留每个底层事件，而是维护“用户当前最应该看到的状态”。

## 7. bridge 协议细化

### 7.1 握手

ESP32 建立 WebSocket 后，先发送 `hello`：

```json
{
  "type": "hello",
  "token": "psk-from-nvs",
  "device_id": "esp32-dashboard",
  "last_seq": 1038
}
```

bridge 校验成功后，返回：

1. 一个 `snapshot`
2. 然后进入实时推送

说明：

- 当前实现优先保证“拿到最新快照 + 继续接收实时 delta”
- 不会在发送当前 `snapshot` 之后，再回放早于该 `snapshot` 的历史 `delta`

### 7.2 snapshot 消息

```json
{
  "type": "snapshot",
  "payload": {
    "seq": 1042,
    "source": "claude_code",
    "session_id": "sess_xxx",
    "event": "PermissionRequest",
    "status": "waiting_for_input",
    "title": "Awaiting approval",
    "workspace": "project-foo",
    "detail": "exec_command requires approval",
    "permission_mode": "default",
    "ts": 1743957123,
    "unread": true,
    "attention": "high"
  }
}
```

### 7.3 delta 消息

```json
{
  "type": "delta",
  "payload": {
    "seq": 1043,
    "source": "claude_code",
    "session_id": "sess_xxx",
    "event": "Stop",
    "status": "waiting_for_input",
    "title": "Ready for input",
    "workspace": "project-foo",
    "detail": "Previous action completed",
    "permission_mode": "default",
    "ts": 1743957128,
    "unread": true,
    "attention": "medium"
  }
}
```

### 7.4 字段约束

| 字段 | 说明 |
|------|------|
| `type` | `snapshot` 或 `delta` |
| `seq` | 全局单调递增 |
| `session_id` | 仅用于去重和调试，设备默认不展示 |
| `event` | 原始事件名，便于调试和规则追踪 |
| `status` | 归一化后的运行状态字符串 |
| `title` | 设备直显标题 |
| `workspace` | 从 `cwd` 派生的短工作区名 |
| `detail` | 一行短说明 |
| `permission_mode` | 便于在等待授权场景下显示提示 |
| `unread` | bridge 给出的建议未读位 |
| `attention` | `low` / `medium` / `high` |

### 7.5 设备侧忽略规则

设备侧可以直接忽略以下字段变化，不触发整页重排：

- `ts`
- `seq`
- `unread` 从 `true` 变 `false`

这能减少 LVGL 无意义重绘。

## 8. 设备侧 service 设计

### 8.1 内部缓存结构

```c
#define CLAUDE_STR_WORKSPACE_MAX  24
#define CLAUDE_STR_TITLE_MAX      40
#define CLAUDE_STR_DETAIL_MAX     96
#define CLAUDE_STR_SESSION_MAX    40

typedef struct {
    uint32_t seq;
    claude_conn_state_t conn_state;
    claude_run_state_t run_state;
    bool unread;
    bool stale;
    uint32_t updated_at_epoch_s;
    char session_id[CLAUDE_STR_SESSION_MAX];
    char workspace[CLAUDE_STR_WORKSPACE_MAX];
    char title[CLAUDE_STR_TITLE_MAX];
    char detail[CLAUDE_STR_DETAIL_MAX];
    char event[24];
    char permission_mode[16];
} claude_snapshot_t;
```

设计要求：

- `service_claude` 始终只维护一个当前 snapshot
- 增量事件处理完成后，用新 snapshot 覆盖旧 snapshot
- 不在 MCU 侧维护完整事件历史

### 8.2 对外接口

```c
esp_err_t claude_service_init(void);
esp_err_t claude_service_start(void);
void claude_service_stop(void);
const claude_snapshot_t *claude_service_get_snapshot(void);
bool claude_service_has_unread(void);
void claude_service_mark_read(uint32_t seq);
claude_conn_state_t claude_service_get_conn_state(void);
```

接口原则：

- 页面总是拉当前快照，不订阅底层 socket
- 已读动作只接受 `seq`，避免 UI 需要知道内部细节
- `mark_read()` 若 `seq` 不是当前快照，直接 no-op

### 8.3 事件输出

`service_claude` 在以下时机向 `event_bus` 发事件：

- 首次拿到 snapshot
- `run_state` 变化
- `unread` 从 `false` 变 `true`
- `conn_state` 变化

不因为心跳或时间戳变化发页面事件。

### 8.4 重连与 stale 判定

参数建议：

- 初始重连退避：`1 s`
- 最大重连退避：`30 s`
- `STALE` 判定：`45 s` 内无任何合法消息
- `AUTH_FAILED` 后不自动狂重试，改为每 `60 s` 低频重试

## 9. Notify 页面子设计

### 9.1 页面布局

页面信息密度控制在“一个 glance 能读完”：

1. 顶部状态条：连接状态、工作区
2. 中部状态卡片：图标、标题、detail
3. 底部辅助区：最后更新时间、权限模式、未读标记

布局原则：

- 标题最大两行
- `detail` 默认一到两行
- 工作区和更新时间优先保证可见

### 9.2 颜色和图标映射

| run state | 颜色建议 | 图标语义 |
|-----------|----------|----------|
| `WAITING_FOR_INPUT` | amber | 等待 |
| `PROCESSING` | blue | 处理中 |
| `RUNNING_TOOL` | cyan | 工具运行 |
| `COMPACTING` | purple | 压缩上下文 |
| `ENDED` | gray | 会话结束 |
| `UNKNOWN` | red | 未知状态 |

### 9.3 已读规则

为避免过多噪声，采用如下规则：

- 页面进入前有 `unread=true`，显示角标
- 页面稳定停留 `1 s` 后，对当前 `seq` 调用 `mark_read`
- 若收到 `attention=high` 的新消息且页面不在前台，可触发短暂高亮
- `PreToolUse` / `PostToolUse` 默认不产生未读角标

### 9.4 错误页语义

`Notify` 页必须区分以下三类错误：

- `No data yet`：bridge 尚未给过任何 snapshot
- `Bridge offline`：连接断开或 stale
- `Auth failed`：token 不匹配

不要把这三种情况都显示成“连接失败”。

## 10. 安全与隐私

### 10.1 不下发的字段

以下内容默认不下发到设备：

- 原始 `tool_input`
- 完整 `user_prompt`
- 本地文件绝对路径
- 任何可能包含密钥的参数

### 10.2 允许下发的派生字段

允许发送到设备的仅包括：

- 工作区短名
- 状态枚举
- 标题
- 截断后的短说明
- 权限模式

这能让设备具备足够展示价值，同时降低信息泄漏。

## 11. 失败场景

| 场景 | bridge 行为 | 设备行为 |
|------|-------------|----------|
| hook 输入非法 JSON | 丢弃事件并记日志 | 无感知 |
| hook 事件未知 | 生成低优先级 unknown snapshot 或仅记日志 | 继续显示旧状态 |
| bridge 重启 | `seq` 从持久化恢复，至少保留最后 snapshot | 重新 hello 并拉快照 |
| 设备重启 | 从 `last_seq=0` 开始 | 先拿 snapshot |
| token 不匹配 | 返回 auth failed | 页面显示显式错误 |

## 12. 实现建议

### 12.1 开发机侧优先级

1. 先做 `hook_adapter` 和 `bridge_normalizer`
2. 再做 `snapshot + 实时 delta` 协议
3. 最后再做 attention、高亮和 coalescing 优化

### 12.2 设备侧优先级

1. 先把 `service_claude` 跑通并能显示静态 snapshot
2. 再接入重连和 stale 判定
3. 最后再做已读和高亮动画

---

*文档版本: 1.0*  
*创建日期: 2026-04-07*
