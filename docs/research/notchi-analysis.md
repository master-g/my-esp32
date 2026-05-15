# Notchi 架构分析

Notchi 是一个 macOS 菜单栏应用，住在 MacBook 的刘海区域，用 sprite 动画角色实时反映 Claude Code 的工作状态。

## 数据流总览

```
Claude Code 触发 hook event
       ↓
notchi-hook.sh (bash + python3)
       ↓  JSON via Unix Socket
SocketServer (/tmp/notchi.sock)
       ↓  HookEvent
NotchiStateMachine
       ↓
SessionStore → SessionData (task/emotion state)
       ↓
NotchContentView → GrassIslandView → GrassSpriteView
       ↓
SpriteSheetView (帧动画) + BobAnimation (浮动/摇摆)
```

## 一、Hook 注册与执行

### 注册机制

`HookInstaller` 在首次启动时做两件事：

1. 将 bundle 中的 `notchi-hook.sh` 拷贝到 `~/.claude/hooks/notchi-hook.sh`，设置 755 权限
2. 修改 `~/.claude/settings.json`，在 `hooks` 字段中注册 9 个事件

注册的事件清单：

| 事件 | matcher | 用途 |
|------|---------|------|
| `UserPromptSubmit` | 无 | 用户提交了新 prompt |
| `SessionStart` | 无 | 会话开始 |
| `PreToolUse` | `*` | 工具即将执行 |
| `PostToolUse` | `*` | 工具执行完毕 |
| `PermissionRequest` | `*` | Claude 请求用户授权 |
| `PreCompact` | `auto` / `manual` | 上下文压缩即将发生 |
| `Stop` | 无 | 主 agent 回复结束 |
| `SubagentStop` | 无 | 子 agent 回复结束 |
| `SessionEnd` | 无 | 会话结束 |

注册逻辑会检查现有 settings.json 中是否已有 notchi 的 hook 条目，避免重复注册。卸载时逆向操作，清理所有相关条目。

### Hook 脚本执行

`notchi-hook.sh` 的工作流程：

```
1. 检查 /tmp/notchi.sock 是否存在，不存在则静默退出
2. 通过 ps 检测当前会话是否为 non-interactive（-p/--print 模式）
3. 用 python3 读取 stdin 的 JSON（Claude Code 传入的 hook payload）
4. 构造统一格式的 output JSON
5. 通过 Unix domain socket 发送给 Notchi app
```

关键的数据转换在 Python 脚本里完成。Claude Code 传入的原始数据包含 `hook_event_name`、`session_id`、`cwd`、`tool_name`、`tool_input`、`prompt` 等字段，脚本将其映射为 Notchi 内部使用的统一 schema：

```json
{
  "session_id": "...",
  "cwd": "...",
  "event": "UserPromptSubmit",
  "status": "processing",
  "interactive": true,
  "permission_mode": "default",
  "user_prompt": "...",
  "tool": "Bash",
  "tool_use_id": "...",
  "tool_input": {...}
}
```

`status` 字段通过一个 `status_map` 做默认映射：`UserPromptSubmit` → `processing`、`PreToolUse` → `running_tool`、`Stop` → `waiting_for_input` 等。Claude Code 传入的 `status` 字段会覆盖默认值。

## 二、Unix Socket 通信

`SocketServer` 是一个基于 POSIX socket 的服务端，监听 `/tmp/notchi.sock`。

核心设计：

- **非阻塞 accept**：server socket 设为 `O_NONBLOCK`，用 `DispatchSourceRead` 监听可读事件
- **阻塞 client 读取**：accept 后将 client socket 改回阻塞模式，用 `poll()` + 超时（0.5s）逐块读取
- **一次性连接**：每个 hook 事件创建一个新连接，发送完 JSON 后关闭
- **stale socket 检测**：启动时先尝试连接旧 socket，如果 `ECONNREFUSED` 说明是残留文件，删除后重新创建

读取到的 JSON 数据直接用 `JSONDecoder` 解码为 `HookEvent` 结构体。

## 三、状态机与事件处理

`NotchiStateMachine` 是单例，收到 `HookEvent` 后做三件事：

### 1. Session 状态映射（委托给 SessionStore）

`SessionStore.process()` 根据 `event` 字段更新 session 的 task 状态：

```
UserPromptSubmit  → working（清除旧事件和 assistant 消息）
PreToolUse        → working（AskUserQuestion 时 → waiting）
PostToolUse       → working
PermissionRequest → waiting
PreCompact        → compacting
Stop/SubagentStop → idle
SessionEnd        → 移除 session
```

每个 session 独立维护状态，支持多会话并行。

### 2. 事件驱动的副作用

状态机在不同事件下触发额外逻辑：

- **UserPromptSubmit**：启动文件监听器跟踪对话文件变化；调用 EmotionAnalyzer 分析情感
- **PreToolUse（waiting_for_input）**：播放通知音
- **PostToolUse**：触发对话文件同步解析（带 100ms debounce）
- **PermissionRequest**：播放通知音
- **Stop**：播放通知音，停止文件监听
- **SessionEnd**：清理所有资源

### 3. 文件监听器

对每个 interactive session，`startFileWatcher` 用 `DispatchSource` 监听 Claude Code 的 JSONL 对话文件（`~/.claude/projects/{cwd-hash}/{session-id}.jsonl`）的写入事件。文件变化时触发 `ConversationParser` 做增量解析，提取 assistant 的文本回复。

如果解析到新消息且 session 处于 idle/sleeping 状态，会自动切换回 working 状态——这是为了捕获 Claude 在 "thinking" 阶段写入文件但尚未触发 PostToolUse 的情况。

## 四、Sprite 动画系统

### 动画状态模型

两个维度组合决定最终的 sprite 表现：

- **Task**（5 种）：`idle`、`working`、`sleeping`、`compacting`、`waiting`
- **Emotion**（4 种）：`neutral`、`happy`、`sad`、`sob`

Sprite sheet 命名规则：`{task}_{emotion}`，如 `working_happy`、`idle_neutral`。查找时有 fallback 链：精确匹配 → sad（sob 的降级）→ neutral。

### SpriteSheetView 帧动画

```
SpriteSheetView
  └── TimelineView(.animation, fps)     // 定时刷新
        └── SpriteFrameView             // 单帧渲染
              └── Image(spriteSheet)    // 整张 sprite sheet
                    .frame(width * columns, height * rows)  // 放大到完整尺寸
                    .offset(-col * width, -row * height)     // 偏移到当前帧
                    .clipped()                               // 裁剪为单帧
```

每个 task 有不同的 FPS：
- `compacting`: 6 FPS
- `working`: 4 FPS
- `idle` / `waiting`: 3 FPS
- `sleeping`: 2 FPS

### 运动效果层

在 GrassIslandView 中的 sprite 视图叠加了三层运动：

1. **Bob（上下浮动）**：cubic easeInOut 曲线，amplitude 和 duration 随 task 变化。working 时快速小幅浮动（0.4s, 0.5pt），idle 时慢速中幅浮动（1.5s, 1.5pt），sleeping 时不浮动
2. **Sway（左右摇摆）**：正弦波旋转，以 sprite 底部为锚点。amplitude 由 emotion 决定：happy 1°、neutral 0.5°、sad 0.25°
3. **Tremble（颤抖）**：sob 状态特有的水平高频抖动（2Hz, 0.3pt）

另外还有一个 **walk（散步）** 机制——每个 sprite 有随机位置的 X 偏移，通过 hash 计算分配，多 session 之间有碰撞避让（最小间距 0.15），形成散落在草地上的效果。

### 草地岛屿

`GrassIslandView` 是一个视觉层（.background，不响应点击），用 `ImagePaint` 平铺一张 512x512 的草地纹理。多个 session 的 sprite 按 Y 深度排序渲染（远近关系），被点击选中的 sprite 下方有蓝色椭圆光晕。

交互层 `GrassTapOverlay` 是独立的透明覆盖层（.overlay），处理点击和 hover。

## 五、情感分析系统

`EmotionAnalyzer` 在用户提交 prompt 时，调用 Anthropic API（Haiku 模型）做情感分类：

1. 从 Keychain 或 `~/.claude/settings.json` 获取 API key
2. 发送用户 prompt 给 Haiku，要求返回 JSON `{"emotion": "happy|sad|neutral", "intensity": 0.0-1.0}`
3. 结果交给 `EmotionState` 维护累积分数

`EmotionState` 不是简单的 "最后一次判断"，而是一个**累积衰减**模型：

- happy/sad 分数独立累积，intensity 乘以 0.5 的 dampen 因子后叠加
- 非 neutral 判断会让其他 emotion 分数乘以 0.9 衰减
- neutral 判断会让所有非 neutral 分数乘以 0.85 衰减
- 每 60 秒全局衰减一次（× 0.92）
- sad 分数达到 0.9 触发 sob 升级

阈值：sad ≥ 0.45 → sad，happy ≥ 0.6 → happy，sad ≥ 0.9 → sob。

## 六、通知音效

`SoundService` 在以下场景播放音效：
- 工具需要用户输入（PreToolUse with waiting_for_input）
- 权限请求（PermissionRequest）
- 回复完成（Stop）

有智能跳过逻辑：
- Non-interactive session 不播放
- Terminal 当前获得焦点时不播放（用户已经在看了）
- 同一 session 2 秒内去重

## 七、对我们项目的借鉴点

### Hook 事件映射

Notchi 的 hook 脚本展示了 Claude Code 完整的事件 schema。对于 ESP32 固件，我们可以复用相同的事件注册方式（在 `settings.json` 的 `hooks` 字段注册），但传输通道从 Unix socket 改为 USB serial。

关键事件及其对 UI 的含义：

- `UserPromptSubmit`（user_prompt 字段）→ 显示 prompt 摘要
- `PreToolUse` / `PostToolUse`（tool + tool_input 字段）→ 工具执行指示器
- `Stop` / `SubagentStop` → 回复完成通知
- `PreCompact` → 压缩中指示器

### 状态简化

Notchi 的 5 个 task 状态对我们有直接参考价值：idle、working、waiting（需要用户输入）、compacting、sleeping（超时休眠）。ESP32 上可以进一步精简，去掉 compacting，保留 idle/working/waiting 三个核心状态。

### Sprite 动画方案

Notchi 的 sprite sheet 方案（横向排列多帧，通过 offset + clipped 切换）可以直接在 LVGL 上用类似方式实现。LVGL 的 `lv_img_set_offset` 配合遮罩可以实现同样的帧动画效果。bob/sway 运动在嵌入式上可以简化为正弦波查表。

### 数据格式

Hook 脚本输出的 JSON 格式已经很干净，可以直接作为 esp32dash host agent 的输入 schema。Host agent 从 stdin 读取同样的 hook 数据，通过 serial 发送给 ESP32。

### 多 session 策略：单展示 + 计数

Notchi 为每个 Claude Code session 创建独立的 sprite，散落在草地岛屿上。这在桌面应用上行得通，但在 ESP32 的 640×172 小屏幕上既没空间也没必要。

我们的方案：**只展示一个活跃 session 的状态，附带并发 session 计数**。esp32dash 作为 host agent 拥有全局视角，它知道当前有多少活跃 session，把计数作为 UI 状态的一部分下发给 ESP32。UI 上一个 sprite + 一个数字 badge，简洁且信息充足。

### Session 生命周期防御

Notchi 有一个实际缺陷：如果 Claude Code 兼容工具（如 opencode）没有正确发送 `SessionEnd` 事件，或者 hook 脚本自身执行失败，session 会无限积累，sprite 越来越多。Notchi 对此没有任何防御。

我们的架构需要在两层做防御，主要职责放在 host agent：

**第一层：esp32dash（host agent）**

这是防御的主阵地，ESP32 不应该承担 session 生命周期管理的复杂度。

- **超时淘汰**：每个 session 记录最后事件时间（`last_activity`），超过阈值（建议 10 分钟）无任何事件自动视为结束。Claude Code 即使在长时间思考，也会有 `PreToolUse`、`PostToolUse` 等事件，10 分钟完全没有事件大概率是异常 session。
- **PID 存活检查**：hook 脚本已经带有 `pid` 字段（notchi 的 schema 定义了但未使用）。esp32dash 可以周期性检查 session 对应的进程是否还活着——进程不存在就清理 session。这比纯超时更精确，能区分 "Claude 在长时间编译" 和 "进程已经 crash"。
- **上限熔断**：设定最大并发 session 数（建议 5），超过时按 `last_activity` 排序淘汰最旧的那个。这是兜底策略，防止极端情况下的资源耗尽。

**第二层：ESP32 固件**

最小防御，作为最后防线。如果 host agent 因为自身 bug 没有发送 session 清理指令，ESP32 对长时间无心跳的 session 做本地超时。正常情况下不应该走到这一层。

**综合策略的优先级**：PID 存活检查 > 超时淘汰 > 上限熔断 > ESP32 本地超时。PID 检查最精确，超时淘汰最通用，上限熔断最粗暴但最安全，ESP32 本地超时是最后的保险。

## 八、情感分析系统（深入分析）

### 触发时机与前置条件

Emotion 分析只在一个场景触发：interactive session 里用户提交 prompt（`UserPromptSubmit` 事件）。两个前提条件缺一不可：`session.isInteractive` 为 true（排除 `claude -p` / `--print` 模式），且 `event.userPrompt` 非空。

分析在 `NotchiStateMachine.handleEvent()` 中以 `Task {}` 异步发起，不阻塞 UI 事件循环。调用链：`EmotionAnalyzer.shared.analyze(prompt)` → `EmotionState.recordEmotion(emotion, intensity, prompt)`。

### API 调用细节

#### 模型选择

默认模型 `claude-haiku-4-5-20251001`（Claude Haiku 4.5），可通过 `~/.claude/settings.json` 的 `env.ANTHROPIC_DEFAULT_HAIKU_MODEL` 环境变量覆盖。选 Haiku 的理由：分类任务不需要强推理，Haiku 廉价且延迟低。

#### System Prompt

硬编码在 `EmotionAnalyzer.systemPrompt`（`EmotionAnalyzer.swift:84-93`），关键设计：

- 三个分类的边界定义得非常精确。Happy 仅限"对 AI 或结果的正面情感"（explicit praise、gratitude、celebration），Sad 仅限"沮丧/愤怒/受挫"（frustration、anger、feeling stuck）
- 特别把"对工作内容的热情"和"感叹号表达紧迫感"排除在 happy 之外——编程场景中 `gotta fix this bug!` 不算 happy
- 大写文本（ALL CAPS）作为 intensity 增强的信号，额外加 0.2-0.3
- 明确要求"拿不准就 neutral"，避免过度解读

#### API Key 解析链

`EmotionAnalyzer.resolveAPIConfig()` 按优先级查找凭证：

1. **Keychain**：`KeychainManager.getAnthropicApiKey(allowInteraction: false)`，用户在 Notchi 设置面板手动输入的 key
2. **Claude settings.json**：读取 `~/.claude/settings.json` 的 `env` 字段，提取 `ANTHROPIC_AUTH_TOKEN`

如果两条路都走不通，直接返回 `("neutral", 0.0)`，不弹窗不报错。

同时读取 `ANTHROPIC_BASE_URL` 并做路径规范化——支持用户通过代理访问 API（如中转服务器）。路径规范化逻辑（`buildMessagesURL`）处理了常见写法：裸域名自动追加 `/v1/messages`，已有 `/v1` 追加 `/messages`，已有完整路径则保留。

#### 请求构造

```json
{
  "model": "claude-haiku-4-5-20251001",
  "max_tokens": 50,
  "system": "<分类指令>",
  "messages": [{"role": "user", "content": "<用户 prompt>"}]
}
```

`max_tokens` 设为 50，因为分类输出只是一个短 JSON 对象。HTTP header 带 `x-api-key` 和 `anthropic-version: 2023-06-01`。

#### 响应解析与容错

`callHaiku` 的解析流程：

1. 解码 Haiku 的标准响应（`HaikuResponse.content[0].text`）
2. `extractJSON` 剥离 LLM 输出中常见的 markdown code block（` ```json ... ``` `），并在前后有多余文字时提取第一个 `{` 到最后一个 `}` 之间的内容
3. `JSONDecoder` 解码为 `EmotionResponse { emotion: String, intensity: Double }`
4. `emotion` 做白名单校验（`["happy", "sad", "neutral"]`），不在列表中的值降级为 neutral
5. `intensity` 裁剪到 `[0.0, 1.0]` 区间

任何环节失败（网络错误、HTTP 非 200、JSON 解析失败），一律返回 `("neutral", 0.0)`。没有重试逻辑。这意味着 API 不可用时，emotion 系统对用户体验没有负面影响——sprite 保持 neutral。

### EmotionState 累积衰减模型

这是整个 emotion 系统最精巧的部分。`EmotionState` 维护的不是"当前情绪"这个单一变量，而是 happy 和 sad 两个独立的累积分数（字典 `[NotchiEmotion: Double]`，只存 `.happy` 和 `.sad` 两个 key）。

#### 参数一览

| 参数 | 值 | 含义 |
|------|-----|------|
| `intensityDampen` | 0.5 | 单次判断的 intensity 在叠加前先减半 |
| `interEmotionDecay` | 0.9 | 非 neutral 判断压制其他 emotion 的系数 |
| `neutralCounterDecay` | 0.85 | neutral 判断主动消解所有情绪的系数 |
| `decayRate` | 0.92 | 全局衰减系数（每 60 秒触发一次） |
| `decayInterval` | 60s | 全局衰减周期 |
| `sadThreshold` | 0.45 | sad 的触发阈值 |
| `happyThreshold` | 0.6 | happy 的触发阈值 |
| `sobEscalationThreshold` | 0.9 | sad 升级为 sob 的阈值 |

#### 记录新判断（`recordEmotion`）

- **非 neutral 判断**（happy 或 sad）：`intensity × 0.5` 叠加到对应 emotion 的累积分数上（上限 1.0）。同时对另一个 emotion 的分数乘以 0.9——"如果你 happy 了，你之前的 sad 分数会被轻微压制"
- **neutral 判断**：所有非 neutral 分数乘以 0.85。neutral 不是"什么都不做"，而是主动消解已有情绪

#### 全局衰减（`decayAll`）

状态机初始化时创建一个永不停止的 `Task`（`startEmotionDecayTimer`），每 60 秒对所有活跃 session 调用 `decayAll()`：每个分数乘以 0.92，低于 0.01 直接归零。衰减有变化时才更新 `currentEmotion`，避免无效的 UI 刷新。

#### 阈值判定（`updateCurrentEmotion`）

取分数最高的那个 emotion，看是否达到阈值。sad 阈值更低（0.45），happy 更高（0.6）——表达不满比表达满意更容易被检测到。如果 sad 分数 ≥ 0.9，升级为 `sob`。

这个模型的直觉：用户情绪在多次交互中逐渐累积或消退。单次 happy 不会立刻让角色跳起来，但连续几次 happy 的语气，角色的 happy 分数会稳步上升。反过来，持续沮丧会让 sad 分数越积越高直到触发 sob。一段时间的 neutral 交互或单纯的空闲（靠全局衰减），情绪会自然回归 neutral。

### Emotion 对视觉表现的影响

Emotion 通过两条途径影响 sprite 的最终呈现。

#### Sprite Sheet 覆盖矩阵

Notchi 为每种 `(task, emotion)` 组合准备了独立的 sprite sheet 图片。实际 assets 目录中的覆盖情况：

| | neutral | happy | sad | sob |
|---|---|---|---|---|
| idle | ✅ | ✅ | ✅ | ✅ |
| working | ✅ | ✅ | ✅ | ✅ |
| waiting | ✅ | ✅ | ✅ | ✅ |
| sleeping | ✅ | ✅ | ❌ | ❌ |
| compacting | ✅ | ✅ | ❌ | ❌ |

sleeping 和 compacting 状态没有 sad/sob 的 sprite sheet。`NotchiState.spriteSheetName` 的 fallback 链兜底：精确匹配 → 如果是 sob 且没有对应 sprite 就降级到 sad → 最终降级到 neutral。所以 sleeping + sad 的组合实际显示 `sleeping_neutral`。

每个 sprite sheet 是一张横向排列多帧的图片。大多数状态 6 帧 6 列，compacting 是 5 帧 5 列。

#### 运动参数调节

Emotion 不仅换贴图，还直接修改角色的运动行为：

| 参数 | neutral | happy | sad | sob |
|------|---------|-------|-----|-----|
| Sway（摇摆幅度） | 0.5° | 1.0° | 0.25° | 0.15° |
| Bob（上下浮动） | task 原始值 | task 原始值 | task 值 × 0.5 | 0（停止浮动） |
| Tremble（水平颤抖） | 无 | 无 | 无 | ~2Hz, 0.2pt |
| Walk（散步） | 由 task 决定 | 由 task 决定 | 由 task 决定 | 禁止 |

Sway 是以 sprite 底部为锚点的正弦波旋转，越开心摇得越欢。Tremble 通过 `sin(date * 2π * 2) * amplitude` 计算，只在 sob 状态激活，通过 `SessionSpriteView` 的 30fps `TimelineView` 驱动。Bob 在 sob 时完全停止（角色"僵住了"），sad 时减半（动作迟缓）。

### 对 ESP32 项目的借鉴

#### 分析位置：host agent 侧

Notchi 把 emotion 分析放在 macOS app 侧。我们的架构里，对应位置是 esp32dash host agent。ESP32 不应该承担 LLM API 调用，原因：

- ESP32 的 Wi-Fi 连接不稳定，API 调用可能超时
- JSON 解析和 HTTP 请求占用 RAM 和 CPU
- API key 管理在嵌入式设备上更复杂

Host agent 已经能拿到 `UserPromptSubmit` 事件的 `user_prompt` 字段，在那里做分析然后把 emotion 分类结果通过 serial 下发给 ESP32 即可。

#### 替代方案：不做 LLM 调用

Notchi 选 Haiku 是因为桌面 app 有稳定网络和充足资源。在 host agent 侧可以考虑更轻量的方案：

- **关键词匹配**：一个简单的正则/词表就能覆盖大多数场景——"thank"、"great job"、"awesome" → happy，"wrong"、"broken"、"doesn't work"、"wtf" → sad。优点是零延迟零成本，缺点是对隐含情感和反讽无能为力
- **本地小模型**：如果 host agent 机器上有 Ollama 或类似服务，可以用本地模型做分类，避免 API 费用和网络延迟

两种方案可以共存：默认用关键词，用户配置了 API key 时升级到 LLM 调用。

#### 累积衰减模型值得移植

`EmotionState` 的累积衰减模型不依赖任何 Apple 框架，是一个纯数值算法，可以直接在 Rust（host agent 侧）中实现。它的价值在于避免了"单次判断决定情绪"的抖动问题——用户偶尔抱怨一句不会立刻让角色哭起来，需要持续的情绪信号才能改变角色的表情。

核心参数（dampen、decay rate、threshold）可以通过 serial 作为配置项下发给 host agent，用户在 ESP32 的设置页面可以调节情绪灵敏度。但初期建议直接硬编码 Notchi 的参数，经过验证再考虑可配置性。

## 九、多 Provider 架构（Codex 集成）★ 2026年4月新增

这是本次更新最大的架构变化。Notchi 从单 provider（仅 Claude Code）演进为双 provider 系统，同时支持 Claude Code 和 Codex CLI（OpenAI 的 coding agent）。

### Provider 抽象层

`AgentProviderAdapter` 协议定义了统一的 provider 接口：

```swift
protocol AgentProviderAdapter {
    func installIfNeeded() -> Bool
    func isProviderAvailable() -> Bool
    func isInstalled() -> Bool
    func configureForLaunch()
    func normalize(_ envelope: AgentHookEnvelope) -> HookEvent?
}
```

每个 provider 实现自己的 hook 安装、可用性检测、事件规范化逻辑。`ClaudeProviderAdapter` 和 `CodexProviderAdapter` 各自封装差异，上层 `IntegrationCoordinator` 通过字典 `[AgentProvider: any AgentProviderAdapter]` 统一调度。

### ProviderCapabilities：用能力标记代替类型检查

`ProviderCapabilities` 结构体用四个 bool 标记编码 provider 间的功能差异，而非到处写 `if provider == .claude`：

| 标记 | Claude | Codex | 含义 |
|------|--------|-------|------|
| `supportsPermissionPrompts` | ✅ | ✅ | 是否发出权限请求事件 |
| `supportsUsageResumeTriggers` | ✅ | ❌ | 是否通过 resume 事件触发用量刷新 |
| `supportsPromptEmotionAnalysis` | ✅ | ❌ | 是否做 prompt 情感分析 |
| `supportsDerivedTranscriptFallback` | ✅ | ❌ | 是否从转录内容派生事件 |

下游代码用 `provider.capabilities.supportsX` 查询，不需要知道具体是哪个 provider。这比枚举 switch 更灵活——将来添加第三个 provider 只需要填一个 struct 实例，不用改现有代码。

### 事件规范化管线

`IntegrationCoordinator` 的核心流水线：

```
Socket 原始数据 → AgentHookEnvelope
  → 遍历所有 adapter.normalize()
    → nil（未知事件）→ 丢弃
    → HookEvent → enqueue 到 AsyncStream
      → serial DispatchQueue 串行化
        → MainActor 上调用 onEvent 闭包 → UI 层
```

关键设计：`AsyncStream<HookEvent>` + 专用串行队列 `"com.ruban.notchi.integration.delivery"`。套接字在自己的并发上下文中读取，规范化是同步的，投递在串行队列上序列化，UI 回调在主 actor 上执行。四个并发域各司其职。

### Codex 的 3-event 钩子模型

Codex 只注册 3 个 hook（对比 Claude 的 9 个）：`SessionStart`（匹配器 `startup|resume`）、`UserPromptSubmit`、`Stop`。缺失的 `PreToolUse` / `PostToolUse` / `PreCompact` 等事件通过以下方式弥补：

- **Transcript 驱动的事件合成**：`CodexProviderAdapter` 解析 Codex 的 transcript JSONL 文件，从 `assistant_message` 类型的行中提取 `tool_calls` 字段，合成 `preToolUse` / `postToolUse` 事件
- **Transcript 路径门控**：`CodexProviderAdapter` 维护一个 `transcriptBackedSessionIDs` 集合。只有带 `transcriptPath` 的 `SessionStart` 才会创建 session；`SessionStart` 不带路径的（resume/continue 模式）被抑制。但如果一个 session 曾经有过路径（在集合中），后续不带路径的事件仍可通过——防止中途丢失 transcript 引用导致 session 异常终止
- **Compaction 检测**：通过解析 Codex 的日志输出搜索 compaction 特征行来检测压缩状态。带 stale signal 过滤和去重

### Process 生命周期跟踪

Codex hook 脚本（`notchi-codex-hook.sh`）内联了一段 Python，它遍历进程树（`/bin/ps -axo pid=,ppid=,tty=,comm=`）向上查找 8 层，找到包含 "codex" 的进程。输出 JSON 中带上 `codex_process_id` 和 `codex_origin`（`cli` 或 `desktop`）。Notchi app 利用这个 PID 来检测进程退出——Codex session 随进程消亡而结束，比纯超时淘汰更精确。

Codex 的 hook installer 写入 `~/.codex/hooks.json`（注册 3 个 hook 事件）和 `~/.codex/config.toml`（启用 `codex_hooks = true` 功能开关），清理逻辑会先 `pruneManagedHooks` 移除旧条目再写入新条目。

## 十、Session 生命周期防御（更新）

上次分析我们批评 Notchi "对 session 积累没有任何防御"。这次更新中 Codex provider 引入了实质性的防御机制，Claude provider 的积累问题仍然存在：

### Codex 侧已实现的防御

- **Transcript 门控**：Session 只在有 `transcriptPath` 时才创建，过滤掉 resume/continue 模式的无 transcript 假 session
- **Process 存活检测**：Session 随 codex 进程退出自动结束
- **Abort 检测**：Codex 会话在 turn 被 abort（没有用户 prompt）时自动 idle
- **Archived session 终结**：Codex 的 archived session 不再报告为活跃

### Claude 侧仍然缺失的防御

Claude provider 依然没有超时淘汰、PID 检查或上限熔断。两个 provider 的防御水平不一致。

### 对 ESP32 的影响

我们的 host agent (esp32dash) 设计要同时支持 Claude Code 和 opencode，所以 provider 差异防御是必经之路。上次提出的三层防御策略（PID 存活检查 > 超时淘汰 > 上限熔断）仍然有效，但需要跟 ProviderCapabilities 模式结合——不同 provider 能提供的防御信息不同。例如，opencode 如果像 Codex 一样只发 3 个 hook，我们也要做 transcript 事件合成和进程跟踪。

## 十一、Emotion 分析多 Provider 支持

### 双 LLM Provider 策略

Emotion 分析从纯 Anthropic 扩展为支持两种 API：

```swift
protocol EmotionAnalysisProviding {
    func analyze(prompt: String, systemPrompt: String) async throws -> EmotionAnalysisResult
}
```

`resolveProvider()` 根据 `AppSettings.emotionAnalysisProvider` 分发到 `ClaudeEmotionAnalysisProvider` 或 `OpenAIEmotionAnalysisProvider`。两种 provider 的请求差异由各自实现封装，上层调用者无感。

### OpenAI Structured Output 的启发

OpenAI 路径的关键差异：使用 `response_format: json_schema` + `strict: true`，模型被强制输出符合 schema 的结构化数据，而不是靠 prompt 引导。Claude 路径仍然靠文本 JSON 提取 + 容错解析。

对 esp32dash 的启示：如果 host agent 使用 OpenAI 兼容的 API 做 emotion 分析，structured output 模式更可靠。解析侧不需要 `extractJSON` 那一套剥离 markdown code block 的逻辑。

### API Key 解析链

| 来源 | Claude | OpenAI |
|------|--------|--------|
| Keychain（用户手动输入） | ✅ 优先 | ✅ 唯一 |
| 本地 settings.json | ✅ 回退 | ❌ |

Claude 有双重认证路径——如果 Keychain 中没有 key，自动读取 `~/.claude/settings.json` 的 `ANTHROPIC_AUTH_TOKEN`。用户不需要手动输入 API key 就能启用 emotion 分析（只要 Claude Code 已经认证过）。这种"零配置"体验值得借鉴。

### 设置 UI 的智能默认

`emotionAnalysisProvider` 的 getter 有自动回退逻辑：检查两个 Keychain 哪个有 key，优先选有 key 的。两者都有或都没有则默认 claude。测试功能用 snapshot 模式避免竞态——启动测试时捕获当前参数，异步返回后比对是否为同一组，防止用户中途切换配置导致结果错位。

## 十二、启动动画设计

### Iridescent Glow

Notchi 在启动时播放一个 6.5 秒的刘海屏轮廓光效动画：淡入 1s → 保持 4s → 淡出 1.5s。视觉分四层 stroke 叠加：

| 层 | 线宽 | 模糊 | 效果 |
|---|---|---|---|
| 外光晕 | 18pt | 12pt | AngularGradient 彩虹色 |
| 中间层 | 8pt | 4pt | 同上 |
| 高光扫动 | 5pt | 2pt | LinearGradient 金属色 |
| 内芯 | 1.6pt | 无 | 白色半透明 |

高光扫动通过 `LaunchIridescentGlowMotion` 驱动，包含三个独立的循环动画：颜色旋转（6s）、高光位置（3.8s）、呼吸效果（4.5s）。通过 mask 裁剪，光效只出现在刘海屏轮廓外侧。

### Timing 与 Motion 分离

`LaunchIridescentGlowTiming`（opacity 三段缓动，由 `Animatable` protocol 的 `animatableData: progress` 驱动）和 `LaunchIridescentGlowMotion`（相位动画，由 SwiftUI `withAnimation` 驱动）被拆成两个独立的 enum。这种分离让每个动画维度独立演进，互不干扰。

### 对 ESP32 的启示

ESP32 固件可以做简化版的启动动画：LVGL 的 `lv_anim` 支持 opacity 渐变，配合 `lv_canvas` 可以画径向渐变光晕。但 ESP32 的计算能力有限，做不到 4 层模糊叠加。可行方案：预设一张光晕图片，用 `lv_img_set_angle` 做旋转动画 + opacity 淡入淡出。或者干脆跳过——嵌入式设备的"启动"已经是硬件级冷启动，屏幕点亮本身就是最大的启动动画。

## 十三、设置 UI 模式

### Master/Detail 导航

Emotion Analysis 设置在设置面板中是一个入口行，点击后滑入二级页面。转场用 `ZStack` + `@State` 手工实现推入效果，不依赖 NavigationStack：

```
HStack（一级列表）向左滑出
EmotionAnalysisSettingsView（二级页面）从右滑入
```

### 内联下拉选择器

Provider 和 Model 选择器都是手工实现的内联下拉列表（`isProviderPickerExpanded` / `isModelPickerExpanded`），而非系统 Picker。选中项用绿色圆点指示，hover 高亮。Picker 高度按 `28pt × 行数` 动态计算，最多显示 6 行。

### 焦点感知的 Affordance

API key 输入框获得焦点时，旁边的 "Get API Key" 外链按钮会高亮（背景 + 边框），同时触发 `HorizontalShake` 动画（用 `GeometryEffect` + sin 做的微小水平抖动）来引导注意力。多个操作（切换 provider、切换 model、保存 key、开始测试、点击外链）都会自动收起键盘。

### Provider-Specific 文案

`apiKeyPlaceholder` 和 `apiKeyURL` 是 `EmotionAnalysisProvider` 枚举的计算属性。切换 provider 时，placeholder 文字（如 "sk-..." / "sk-ant-..."）和 "Get API Key" 的 URL 自动跟随变化。状态 badge 的显示逻辑也有层次：有存储 key → 显示 provider 名 + 绿色；Claude 且有本地 settings.json → 显示 "Claude Code" + 绿色；否则 "No Key" + 红色。

## 十四、Shimmer 指示器与 Provider-Specific 细节

### ProcessingSpinner

一个 24 行的通用指示器组件：6 个 Unicode 符号（`·✢✳∗✻✽`）每 150ms 循环切换，通过 `Timer.publish(every: 0.15)` + `@State phase` 驱动。特点是极简、无外部依赖、视觉上与终端主题融合。

### Provider-Specific Spinner Verbs

Session 启动时，Spinner 旁的文本用 provider-specific 的动词种子："thinking"（Claude）和 "coding"（Codex）。这是个很小的细节，但体现了 provider 差异不仅体现在架构层，也渗透到 UX 的微观层面。

## 十五、对 ESP32 项目的新借鉴点

### 1. Provider 抽象层应立即实施

我们的 host agent（esp32dash）需要同时支持 Claude Code 和 opencode，两边的 hook 事件格式、数量、字段名可能不同。Notchi 的 `AgentProviderAdapter` 协议 + `ProviderCapabilities` 能力标记模式是最佳实践。

Rust 中的等价实现：

```rust
trait AgentProvider {
    fn install_hooks(&self) -> Result<()>;
    fn is_available(&self) -> bool;
    fn normalize(&self, raw: &RawHookPayload) -> Option<AppEvent>;
}

struct ProviderCapabilities {
    supports_permission_prompts: bool,
    supports_emotion_analysis: bool,
    // ...
}
```

### 2. 事件合成模式

opencode 的 hook 事件可能比 Claude Code 少（类似 Codex 只发 3 个事件）。Notchi 展示了从 transcript 文件合成缺失事件的模式。esp32dash 应该为每个 provider 实现独立的 normalize 逻辑——Claude provider 直接透传丰富的 hook 事件，opencode provider 从 transcript 或 log 中补充缺失的工具执行 / 压缩等事件。

### 3. Process 跟踪替代纯超时

Codex hook 脚本的进程树查找（`/bin/ps` 向上追溯 8 层）是比纯超时更精确的 session 存活判断。我们的上次分析已经提到 PID 存活检查，Notchi 的实际实现验证了这个方向的可行性。esp32dash 可以在 hook 脚本中加入类似的进程查找逻辑，把 PID 附加到事件 payload 中。

### 4. Transcript 路径门控

`transcriptBackedSessionIDs` 集合的设计很精妙——它区分了"有持久 transcript 的真 session"和"resume/continue 的临时 session"。opencode 也可能有类似的 resume 模式，我们应该在 host agent 中实现类似的 gating 逻辑，避免临时 session 干扰 UI。

### 5. 设置 UI 的模式可迁移

ESP32 的设置页面（app_settings）目前功能简单。Notchi 的 master/detail 导航、内联下拉选择器、smart defaults（自动检测哪个 provider 有 API key）、snapshot 测试防竞态——这些模式可以直接指导 ESP32 设置页面的设计。

### 6. 零配置优先

Notchi 的 emotion 分析对 Claude 用户是零配置——自动从 settings.json 读取 token。esp32dash 也应该对 Claude Code 用户做到开箱即用：检测 `~/.claude/settings.json` 中的认证信息，不需要用户在 ESP32 上手动输入任何 key。

### 7. Capability-Driven UI

ESP32 的三个 app 页面在面对不同 agent 时应该有不同的表现能力。例如，如果当前连接的 agent 不支持 permission prompt，设置页面中的权限相关选项就应该灰掉或隐藏。`ProviderCapabilities` 的 bool 标记模式可以直接指导这个逻辑。

---

*本文档基于 Notchi 代码库截至 2026-04-30 的状态更新。新增第九至第十五节。*
