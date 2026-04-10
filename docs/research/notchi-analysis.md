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
