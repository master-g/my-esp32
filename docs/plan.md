# ESP32 Claude Sprite 改进计划（审计版）

## Context

对照 Notchi 源码和当前仓库实现审计后，可以确认这条路线**可行**，但原计划里有几处前提写得过于乐观，尤其是 host 侧情感分析入口、flash 预算、验证手段，以及 Phase 2 的动画架构。

本版计划以当前代码和素材仓现状为准：`make build` 可通过，`cargo test --manifest-path tools/esp32dash/Cargo.toml --quiet` 可通过；Notchi 的 `Assets.xcassets` 里 compacting / emotion 相关 sprite sheet 也都已经在 `docs/research/notchi/notchi/notchi/Assets.xcassets/` 下可用。

## 审计结论

| 项目 | 结论 | 审计说明 |
|---|---|---|
| Compacting 状态链路 | **前提成立，但范围更小** | Host 已产生 `Compacting`，`Snapshot.status` 已经通过 `claude.update` 发到设备，firmware 端 `device_link.c` 也已解析 `compacting`。真正缺的是 Home presenter 不要再把它并进 WORKING，以及补一套 compacting sprite。 |
| Emotion 系统 | **可做，但不是“直接接线”** | 当前 agent 没有独立配置层；`normalizer.rs` 是纯同步映射；hook ingest 只保留了 `prompt_preview`，完整 prompt 在进入 agent 前就被裁断了。Phase 1 需要先补 host-only prompt 传递和配置来源。 |
| 素材可用性 | **前提成立** | 当前素材目录里已经有 `compacting_happy`、`waiting_sob`、`working_sob` 等上游资源，不需要先去外部拉素材。 |
| Flash 预算 | **原表偏乐观，需要改写** | 当前 `build/esp32_dashboard.bin` 为 **2,493,184 B**，app 分区为 **4,194,304 B**，剩余约 **1,701,120 B**。不是原计划里“当前固件约 2MB、还剩约 2MB”。 |
| 验证手段 | **需要补 smoke-test harness** | 现在 `esp32dash chibi` 只支持 `idle/working/waiting/sleeping` 四种状态，也没有 emotion 参数。Phase 0 和 Phase 1 最好顺手补上 `compacting` 和 `--emotion`，这样调试快得多。 |
| Phase 2 动画方案 | **原方案不够平滑** | 现有 sprite timer 只按 2-6 FPS 驱动帧切换。如果 bob / sway 直接挂在同一个 timer 上，运动采样也只有 2-6 FPS，会很顿。Phase 2 应该拆出一个更高频的 motion timer，或改成统一高频 tick 后再按状态门控换帧。 |

## 实施原则

1. Host 侧凡是涉及完整 prompt 的数据，都只在内存里短暂停留，不写入 `state.json`，也不通过 serial 发给 firmware。
2. 远端 emotion provider 默认关闭；如果要把 prompt 发给云端模型，必须是显式 opt-in。
3. 每个 phase 结束都要重新看 app bin 大小。可裁剪的优先级从低到高依次是：额外 emotion 变体、sway、tremble。
4. `home_view.c` 是本次改动最密集的文件。会碰它的 phase 最好成组推进，减少来回重构。

## 基线与修正点

### 当前已经具备的基础

- `tools/esp32dash/src/normalizer.rs` 已把 `PreCompact` 映射成 `RunStatus::Compacting`
- `tools/esp32dash/src/device.rs` 发送 `claude.update` 时直接序列化整个 `Snapshot`
- `src/components/device_link/src/device_link.c` 已把 `"compacting"` 解析成 `CLAUDE_RUN_COMPACTING`
- `src/components/service_claude/include/service_claude.h` 已定义 `CLAUDE_RUN_COMPACTING`
- `tools/sprite_convert.py` 已经接在 Notchi 的 `Assets.xcassets` 上，路径有效

### 原计划里需要改写的地方

1. `normalizer.rs` 不适合直接做异步 LLM 调用。它现在是纯函数，应该保持纯。
2. 不能只靠 `prompt_preview` 做情感分析。它会被截成单行、96 字节、并经过字符集清洗，信息损失太大。
3. `ANTHROPIC_AUTH_KEY` 这个环境变量名不对。Notchi 走的是 `ANTHROPIC_AUTH_TOKEN`，并支持 `ANTHROPIC_BASE_URL` / `ANTHROPIC_DEFAULT_HAIKU_MODEL`。
4. “Ollama 本地模型” 这一条不能写成直接可替换的前提。这里真正需要的是一个**兼容 Anthropic Messages API** 的 endpoint；如果是本地模型，要么自身兼容，要么前面加一个 adapter / proxy。
5. `sprite_frames.h` 现在假定所有状态都是 6 帧。只要引入 5 帧的 compacting，就要把生成逻辑改成“每个资源各自声明帧数”，不能再共用一个 `SPRITE_FRAMES_PER_STATE`。

## Revised plan

### Phase 0: Compacting 独立状态（已完成）

这是最适合先落地的 phase。Host 和 serial protocol 已经把状态传过来了，真正要做的是 firmware 不再吞掉它，并补一套 compacting sprite。

**新增 flash**：`64 * 64 * 4 * 5 = 81,920 B`，约 **80 KiB**

**修改点**

1. `src/apps/app_home/src/home_presenter.h`
   - `sprite_state_t` 新增 `SPRITE_STATE_COMPACTING`
2. `src/apps/app_home/src/home_presenter.c`
   - `map_run_state()` 的 `CLAUDE_RUN_COMPACTING` 改为返回 `SPRITE_STATE_COMPACTING`
3. `src/apps/app_home/src/home_view.c`
   - `s_sprite_anims` 增加 compacting 项
   - compacting 周期设为 `167ms`，保持 6 FPS
4. `src/apps/app_home/src/generated/sprite_frames.h`
   - 为 compacting 单独声明帧数宏和 extern
5. `tools/sprite_convert.py`
   - 增加 `("compacting", "compacting_neutral", 5)`
   - 头文件生成逻辑改成 per-asset frame count
6. `src/apps/app_home/CMakeLists.txt`
   - 增加 `src/generated/sprite_compacting.c`
7. `tools/esp32dash/src/main.rs`（推荐一并做）
   - `ChibiState` 增加 `Compacting`
   - `chibi demo` 增加 compacting 样例，便于直连烟测

**验证**

```bash
cargo run --manifest-path tools/esp32dash/Cargo.toml -- chibi test --state compacting --bubble "Compacting..."
make build
```

如果不改 `chibi` harness，也可以靠真实 `PreCompact` 事件验证，但回归效率会差很多。

**实现备注（2026-04-14）**

- `home_presenter` / `home_view` / `sprite_convert.py` / `esp32dash chibi` 已按本节落地，`compacting` 不再并进 `working`。
- 新增 `src/apps/app_home/src/generated/sprite_compacting.c`，并把 `sprite_frames.h` 改成 per-asset frame count 宏。
- 实机 compacting 动画和 `cargo run --manifest-path tools/esp32dash/Cargo.toml -- chibi test --state compacting` 已验证通过。
- 当前 `build/esp32_dashboard.bin` 为 `2575360 B`，factory app 分区剩余 `1618944 B`。

### Phase 1: Emotion 系统全链路

这个 phase 依然值得做，但要先补 host 基础设施。建议拆成 1A-1F。

#### 1A: Host 配置来源（已完成，agent run 已接入）

这里要把“配置路径”和“emotion provider 来源”分开看。更稳妥的顺序是：

1. `ESP32DASH_CONFIG` 指向显式配置文件（方便测试和 launchd）
2. `~/.config/esp32dash/config.toml` / `$XDG_CONFIG_HOME/esp32dash/config.toml` 覆盖项
3. `agent run` 首次启动时自动创建 `~/.config/esp32dash/` 和默认 `config.toml`

**配置建议**

```toml
[emotion]
enabled = false
api_base_url = "https://api.anthropic.com/v1/messages"
model = "claude-haiku-4-5-20251001"
api_key = ""
timeout_secs = 3
max_tokens = 50
```

`enabled` 默认 false。只有本地 `config.toml` 显式提供 anthropic-compatible API 配置时才启用；没配置、没 token、解析失败、超时、HTTP 非 200、返回 JSON 不合法时，全部回退到 `disabled`，不再回退到 Claude 自身配置。

**修改点**

1. `tools/esp32dash/src/config.rs`（new）
   - `AppConfig`
   - `EmotionConfig`
   - `load()` 保持无副作用；`load_for_agent_run()` 负责默认配置 bootstrap
2. `tools/esp32dash/Cargo.toml`
   - 增加 `toml`
   - 接入 `dir_spec`
3. `tools/esp32dash/src/agent.rs`
   - `Config::from_env()` 接入 `AppConfig`
4. `tools/esp32dash/src/main.rs`
   - `agent run` 启动时加载配置

**实现备注（2026-04-14）**

- `tools/esp32dash/src/config.rs` 已落地，当前用 `dir_spec` 解析默认配置路径，并在首次 `agent run` 时自动写出 `~/.config/esp32dash/config.toml`。
- `agent::Config::from_env()` 已接入 `AppConfig`，文件配置现在可以回退到 `admin_addr` / `state_dir` / `serial_port` / `serial_baud`。
- emotion provider 现在只认本地 `config.toml`；没有显式 anthropic-compatible API 配置就保持 disabled，不再回退到 `~/.claude/settings.json`。
- `emotion api_base_url` 现在会做安全校验：只接受 HTTPS，拒绝 `localhost` 和私网 IP literal；对域名 endpoint，请求前还会解析并 pin 住公开地址，避免 DNS rebinding。
- `launchd` 安装现在会透传显式设置的 `ESP32DASH_CONFIG` / `ESP32DASH_*` 覆盖项；`device` / `chibi` 这些直接 CLI 入口仍沿用现有 env-only 路径，等 Phase 1 后续子项一起收口时再统一。

#### 1B: 完整 prompt 传递与隐私边界（host 侧已完成）

这一步是原计划缺得最明显的部分。当前 `sanitize_raw_event()` 只保留 `prompt_preview`，完整 prompt 进不了 agent。

推荐做法：

1. `RawHookInput.prompt` 继续只在本机使用
2. `LocalHookEvent` 增加一个 **host-only** 的 `prompt_raw: Option<String>`
3. CLI ingest 把 `prompt_raw` 通过本地 HTTP 发给 agent
4. `PersistedState` 仍然只保存 `Snapshot`，不落盘 raw prompt

这样 agent 里可以做完整分类，但设备侧和持久化状态都看不到原始 prompt。

**修改点**

1. `tools/esp32dash/src/model.rs`
   - `LocalHookEvent` 新增 `prompt_raw: Option<String>`
2. `tools/esp32dash/src/main.rs`
   - `sanitize_raw_event()` 同时保留 `prompt_preview` 和 `prompt_raw`
3. `tools/esp32dash/src/text_sanitize.rs`
   - 保持现有展示清洗逻辑，不把它混进 emotion 分析路径

**实现备注（2026-04-15）**

- `LocalHookEvent` 已新增 `prompt_raw`，`sanitize_raw_event()` 现在会同时保留：
  - 展示用 `prompt_preview`
  - host-only 分析用 `prompt_raw`
- raw prompt 目前只在本机 ingest -> agent 路径里流转，不会进入 `state.json`，也不会进设备串口快照。

#### 1C: Emotion analyzer 与 agent 集成（host 侧已完成）

异步 LLM 请求应该挂在 `AppState::process_event()` 附近，而不是 `normalizer.rs`。

推荐流程：

1. 仅在 `UserPromptSubmit` 且 `prompt_raw` 存在时调用 analyzer
2. 按 `session_key_for(event)` 找到 session 对应的 `EmotionState`
3. `record_emotion()` 后，把当前判定结果写入将要发布的 `Snapshot.emotion`
4. 60 秒周期衰减同样由 agent 内部 task 驱动

`EmotionState` 建议只存在内存里，不写入 `state.json`。agent 重启后回到 neutral 是可以接受的，也更安全。

**修改点**

1. `tools/esp32dash/src/emotion.rs`（new）
   - `Emotion` enum：`neutral/happy/sad/sob`
   - `EmotionAnalyzer`
   - 调用兼容 Anthropic Messages API 的 `POST {base}/v1/messages`
   - 剥 markdown code fence，再解 JSON
2. `tools/esp32dash/src/emotion_state.rs`（new）
   - `happy_score`
   - `sad_score`
   - `record_emotion()`
   - `decay_all()`
   - `current_emotion()`
3. `tools/esp32dash/src/agent.rs`
   - 增加 `HashMap<String, EmotionState>`
   - 启动 60s decay task
   - `SessionEnd`、session prune、liveness lost 时清理对应 emotion state
4. `tools/esp32dash/src/model.rs`
   - `Snapshot` 新增 `#[serde(default)] emotion: String`
   - `Snapshot::empty()` 默认 `"neutral"`
5. `tools/esp32dash/src/normalizer.rs`
   - 保持纯函数
   - `materially_equal()` 必须把 `emotion` 纳入比较，否则 emotion 变化不会推送到设备
6. `tools/esp32dash/src/main.rs`
   - `build_test_snapshot()` 和 `chibi` 测试路径补 emotion

**实现备注（2026-04-15）**

- `tools/esp32dash/src/emotion.rs` / `emotion_state.rs` 已落地，agent 现在会在 `UserPromptSubmit` 且 `prompt_raw` 存在时异步请求 emotion analyzer。
- direct HTTP 调用会把 `api_base_url` 归一化到 `/v1/messages`；像 MiniMax 这种 bare base path 兼容服务不需要手工把 endpoint 写死到 config。
- response parser 不再假设 `content[0]` 一定是 `text`；它会跳过 `thinking`，找到第一个 `text` block，再剥 markdown code fence 解 JSON。
- 对 MiniMax 这类会先输出较长 `thinking` 的兼容服务，单纯依赖较小的 `max_tokens` 很容易只拿到 `thinking` 而拿不到最终 `text`。当前 analyzer 已把有效 token 预算抬到一个安全下限，默认模板也同步提高到 `256`。
- `Snapshot` 已新增 `emotion`，`normalizer::materially_equal()` 也已把它纳入比较，所以 emotion 变化会触发新的 host snapshot。
- agent 内部 emotion state 只保留在内存里；session 结束、prune、liveness lost 会清理对应状态。agent 重启后显示 emotion 会回到 `neutral`。

#### 1D: Serial protocol 扩展

这一步风险不高，因为 host 发设备快照本来就是 `serde_json::to_value(snapshot)`。

**修改点**

1. `src/components/service_claude/include/service_claude.h`
   - `claude_snapshot_t` 新增 `char emotion[16]`
2. `src/components/device_link/src/device_link.c`
   - `parse_claude_update()` 增加 `copy_json_string(payload, "emotion", ...)`
3. `src/components/service_home/include/service_home.h`
   - `home_snapshot_t` 新增 `char claude_emotion[16]`
4. `src/components/service_home/src/service_home.c`
   - 拷贝 `claude_snap.emotion`

**向后兼容**

- 老 firmware：忽略 host 多出来的 JSON 字段
- 老 host：不发 `emotion`，firmware 读到空串后按 neutral 处理
- 老 `state.json`：靠 `#[serde(default)] emotion` 继续可读

**实现备注（2026-04-15）**

- `service_claude` / `device_link` / `service_home` 已接通 `emotion` 字段：
  - `claude_snapshot_t` 新增 `emotion[16]`
  - `parse_claude_update()` 已读取 `"emotion"`
  - `home_snapshot_t` 已新增 `claude_emotion`
- 旧 host 不发 `emotion` 时，firmware 侧会保留空串；presenter 再统一把空串映射到 `neutral`。

#### 1E: Firmware 情绪枚举与 sprite 选择

这里不只是 `home_presenter`，`home_view` 自身的状态缓存也要扩。

**修改点**

1. `src/apps/app_home/src/home_presenter.h`
   - 新增 `sprite_emotion_t`
   - `home_present_model_t` 新增 `sprite_emotion`
2. `src/apps/app_home/src/home_presenter.c`
   - `emotion_from_text()`
   - 缺失或未知值统一映射 `NEUTRAL`
3. `src/apps/app_home/src/home_view.h`
   - `home_view_t` 增加 `sprite_emotion`
4. `src/apps/app_home/src/home_view.c`
   - `s_sprite_anims` 改成 `[SPRITE_STATE_COUNT][SPRITE_EMOTION_COUNT]`
   - `refresh_sprite()` 比较 state + emotion，而不是只比较 state
   - fallback 链建议做成：`exact -> sad (仅 sob) -> neutral`

**实现备注（2026-04-15）**

- `home_presenter` 已新增 `sprite_emotion_t`，并通过 `emotion_from_text()` 把 `happy/sad/sob/unknown` 映射到 UI 枚举。
- `home_view` 现在按 `state x emotion` 选择动画，并缓存当前展示的 emotion 变体。
- fallback 实现为：
  - 先找精确匹配
  - `sob` 没资源时退到同 task 的 `sad`
  - 再退到同 task 的 `neutral`
  - 最后才回 `idle_neutral` 兜底
- 这意味着最小资源矩阵可以先缺省 `working_sob` / `waiting_sob` / `sleeping_sad` 这类低优先级组合，不会把 UI 弄坏。

#### 1F: Sprite 转换与 smoke test

最小可交付矩阵建议保留原计划的 8 个高可见度变体：

| Task | 新增 emotion |
|---|---|
| idle | happy / sad / sob |
| working | happy / sad |
| waiting | happy / sad |
| sleeping | happy |

这 8 个资源新增 **786,432 B**，约 **768 KiB**。如果把 Phase 0 的 compacting neutral 算进去，Phase 0 + Phase 1 最小集累计是 **868,352 B**。

上游素材里还已有这些低成本扩展项：

- `compacting_happy`
- `working_sob`
- `waiting_sob`

它们适合作为 Phase 1.5；在 Phase 1 最小链路跑通后，再根据实际剩余空间决定要不要一起带上。

**修改点**

1. `tools/sprite_convert.py`
   - 扩展 `SPRITES`
   - 允许不同资源声明不同帧数
2. `src/apps/app_home/CMakeLists.txt`
   - 编入所有新增生成文件
3. `tools/esp32dash/src/main.rs`
   - `chibi test --emotion happy|sad|sob|neutral`
   - `chibi demo` 增加几组 emotion case

**实现备注（2026-04-15）**

- 当前已按最小矩阵生成并编入以下新增资源：
  - `idle_happy` / `idle_sad` / `idle_sob`
  - `working_happy` / `working_sad`
  - `waiting_happy` / `waiting_sad`
  - `sleeping_happy`
- `tools/sprite_convert.py` 已扩展为按资源名生成独立的 frame count 宏和 extern 声明；neutral 资产继续保留原来的无后缀符号名。
- `esp32dash chibi test` 已支持 `--emotion neutral|happy|sad|sob`；`chibi demo` 也已经带几组 emotion 场景。
- 当前 `make build` 结果：
  - app binary size: `0x335300`
  - smallest app partition free: `0x0cad00`（约 20%）

**验证**

```bash
cargo test --manifest-path tools/esp32dash/Cargo.toml --quiet
cargo run --manifest-path tools/esp32dash/Cargo.toml -- chibi test --state working --emotion happy --bubble "Nice!"
cargo run --manifest-path tools/esp32dash/Cargo.toml -- chibi test --state waiting --emotion sad --bubble "Need help"
make build
```

### Phase 2: 辅助动画

原计划里“复用现有 sprite timer，不引入新 timer”这件事不建议照做。当前状态帧率只有 2-6 FPS，把 bob / sway 挂在同一个 timer 上，观感会像逐帧跳，不像 Notchi。

更稳的结构是：

- **frame timer**：继续负责 sprite sheet 换帧，频率保持状态 FPS
- **motion timer**：新增一个 33-50ms 的 LVGL timer，只做 transform / position 更新

这样 Bob、Sway、Tremble 可以平滑一些，且不需要把 sprite sheet 自己也提到高帧率。

#### 2A: Bob

**实现建议**

1. `home_view_t` 增加：
   - `lv_timer_t *motion_timer`
   - `uint32_t motion_tick`
   - `lv_coord_t sprite_base_x`
   - `lv_coord_t sprite_base_y`
2. 新增 `sprite_motion_def_t`：
    - `bob_period_ms`
    - `bob_amplitude_px_x10`
3. `motion_timer_cb` 用 sine lookup table 更新 `lv_obj_set_style_translate_y()`

**参数建议**

- idle：1.5s，1.5-2.0px
- working：0.4s，0.5-1.0px
- waiting：1.5s，1.0px
- sleeping / compacting：0
- sad：幅度减半
- sob：关闭 bob

**实现备注（2026-04-15）**

- `home_view_t` 现已增加独立 `motion_timer`、motion tick、sprite base offset 和当前 translate offset；frame timer 继续只负责换帧。
- 实现里把“语义 emotion”和“实际显示资源的 emotion fallback”分开了：这样即使 `sob` 资产退回到 `sad` 资源，motion 也仍然保持 `sob = no bob`。
- bob profile 已先落在 `idle / working / waiting` 三种状态；`sleeping`、`compacting` 和 `sob` 全部保持静止。
- `home_view_resume()` / `home_view_suspend()` / screensaver enter/exit 现在都会一起管理 frame timer 和 motion timer。
- 已通过以下设备侧 smoke：
  - `cargo run --manifest-path tools/esp32dash/Cargo.toml --quiet -- chibi test --state idle --emotion happy --bubble "Nice"`
  - `cargo run --manifest-path tools/esp32dash/Cargo.toml --quiet -- chibi test --state working --emotion sad --bubble "Need help"`
  - `cargo run --manifest-path tools/esp32dash/Cargo.toml --quiet -- chibi screensaver enter`
  - `cargo run --manifest-path tools/esp32dash/Cargo.toml --quiet -- chibi screensaver exit`
- 当前 `make build` 结果：
  - app binary size: `0x335750`
  - smallest app partition free: `0x0ca8b0`（约 20%）

#### 2B: Sway

**实现结论（2026-04-15）**

- 这一版用 **LVGL object rotation** 做 sway 的方案已经实机证伪，当前已回退到 **bob-only**。
- 具体症状不是“观感不好”，而是稳定性出问题：只要先发一次 `chibi test` 更新 Home snapshot，几秒后就会出现 `app_manager: ui event queue full, dropping tick_1s`，随后这条日志会直接混进 USB serial protocol，导致后续 `device info` / `home.screensaver` 的 hello 或 rpc 帧损坏。
- 根因落在 `lv_obj_set_style_transform_rotation()` 这条路径。把 rotation style 写入去掉后，同一组 smoke（多次 `chibi test`、`device info`、`chibi screensaver enter/exit`）恢复稳定；单纯放宽 timeout 不能解决这个问题。
- 这和 LVGL 官方文档、官方 demo 的方向也是一致的：
  - `docs/src/common-widget-features/styles/overview.rst` 明确写了 style transformation 会为 widget 及其 children 创建 **intermediate layer / snapshot**；
  - `examples/styles/lv_example_style_16.c` 也直接注释了 transformed widget 会先 render to a layer，再对 layer 做 transform；
  - `docs/src/main-modules/display/rotation.rst` 又明确提醒 display rotation 在 `FULL` / software rotation 链路里本来就更贵。
  这三点叠在一起，意味着 **style/object transform-based sway** 和我们当前这套 full render + software rotation 的板级显示路径天然冲突。
- 当前保留的修正有两类：
  - Home 仍保持 2A 的独立 `motion_timer` + bob；
  - `home.screensaver` 的 host / firmware control timeout 放宽到 5 秒，避免 direct-mode stop/start 正好撞到 2 秒边界。

**如果以后还要重新做 sway**

不要再走 LVGL **style/object transform rotation**。这块板子的现有渲染链路是 full render + software rotation，style transform 的代价和风险都太高。

**补充验证（2026-04-15，第二轮）**

- `src/apps/app_home/src/home_screensaver.c` 现在只在 host 驱动的 `home.screensaver` enter / exit 控制窗口里，临时静音 `screensaver_perf` 日志；这样保住了 shared serial protocol，又不需要把 perf log 永久降级。
- 这条修正之后，`chibi screensaver enter -> chibi screensaver exit -> device info` 连跑 3 轮已经恢复干净，之前稳定复现的 `home_screensaver: screensaver_perf` 污染 hello frame 不再出现。
- 在这个干净 baseline 上，重新打开了最小 **`lv_image_set_rotation` spike**：继续沿用现有 `motion_timer`，只在 `lv_image` sprite 本身加小角度 rotation，并通过 `lv_image_set_pivot()` 把 pivot 固定在 sprite 下缘附近。
- 这版 image-widget rotation 固件通过了两类设备 smoke：
  1. 常规 Home 链路：重复 `chibi test --state idle --emotion neutral|happy|sob`、`chibi test --state working --emotion sad`，以及随后 `device info`；
  2. 修复后的 screensaver 链路：`chibi screensaver enter -> exit -> device info` 连跑 3 轮。
- 在这轮 smoke 里，没有再看到旧的 `ui event queue full` / serial 脏帧症状。

**当前结论**

- **LVGL style/object transform-based sway：继续排除**
- **`lv_image_set_rotation` + `lv_image_set_pivot`：当前已通过真机 smoke，可作为 2B 的保留实现路径**
- 如果后续只需要继续打磨，应当围绕 **小角度 image-widget rotation 的幅度 / pivot 调整** 展开，不要重新引入 style transform。

**其它配套改动**

- `device_link.c` 的 stdout mutex 升级为 recursive mutex，并通过 `esp_log_set_vprintf(locked_log_vprintf)` 把所有 ESP_LOG* 输出也收到同一把锁下。每条 log 写完后会 `fflush(stdout)` + `usb_serial_jtag_wait_tx_done(20ms)`，和 protocol frame 互斥，从源头减少 serial interleaving。代价是所有 log 调用路径都多了一次 mutex take/give + 最长 20ms 的 TX drain wait。
- `HOME_SPRITE_Y_OFFSET` 从 `0` 调到 `10`：原来 sprite 垂直居中偏高，部分情绪状态下 sprite 会超出屏幕上缘。正值向下偏移。

**当前验证结果**

- 现有 Home motion 固件已经同时通过：
  1. `chibi test --state idle --emotion neutral`
  2. `chibi test --state idle --emotion happy`
  3. `chibi test --state idle --emotion sob`
  4. `chibi test --state working --emotion sad`
  5. `device info`
  6. `chibi screensaver enter`
  7. `chibi screensaver exit`
  8. `device info`
- flash 后第一次 `device info` 偶尔仍可能被 boot / Wi-Fi 日志打脏，需要 host retry；这条噪声现在和 Home saver / sway 无关，是 shared serial line 的独立问题。

#### 2C: Tremble

sob 的 tremble 依赖高频 motion tick，放在这一版结构里反而更容易做。

**参数建议**

- 2Hz 左右
- 0.2-0.3px 横向抖动
- 只在 `SOB` 时开启

**验证**

1. `home_view_resume()` / `home_view_suspend()` 同时管理 frame timer 和 motion timer
2. 现在 `lv_image_set_rotation` 路径已经通过 smoke，tremble 可以用同一条 motion timer + `apply_sprite_transform` 的 x 分量来实现，不需要 `transform_rotation`
3. 当前优先级仍然是保住 bob 与 screensaver 稳定性

## 依赖与推荐顺序

推荐顺序如下：

```text
Phase 0 (Compacting)
  -> Phase 1A (Config source)
  -> Phase 1B (Full prompt path)
  -> Phase 1C (Analyzer + EmotionState)
  -> Phase 1D/1E/1F (Protocol + Firmware + Assets + Smoke test)
  -> Phase 2 (Motion)
```

说明：

- Phase 2 从概念上能独立，但它和 1E 会同时重写 `home_view.c/h`。放到 1E 之后更省返工。

## 降级行为

| 场景 | 行为 |
|---|---|
| emotion 未启用 | 直接返回 `neutral + 0.0` |
| 找不到配置 / token | 同上 |
| 远端超时 / 非 200 / JSON 解析失败 | 同上，不重试 |
| `emotion` 字段为空或未知 | firmware 按 neutral 显示 |
| emotion asset 缺失 | `exact -> sad (仅 sob) -> neutral` fallback |
| agent 重启 | emotion state 清空，所有 session 回到 neutral |

## Flash 预算（按当前构建产物重算）

| 项目 | 大小 |
|---|---|
| 当前 `build/esp32_dashboard.bin` | **2,493,184 B** |
| app 分区 (`partitions.csv`) | **4,194,304 B** |
| 当前剩余空间 | **1,701,120 B** |
| Phase 0 | **81,920 B** |
| Phase 1 最小集（8 个 emotion 变体） | **786,432 B** |
| Phase 0 + Phase 1 最小集累计 | **868,352 B** |
| 全部上游 sprite 额外变体 | **1,146,880 B** |

按当前 bin 大小估算，如果把当前上游已有的额外 sprite 变体也都一起带上，还剩约 **554,240 B**。所以这条路线**能装下**，但余量没有原计划写得那么松。后续每个 phase 都要重新看一次 bin。

## 关键文件清单

| 文件 | 备注 |
|---|---|
| `src/apps/app_home/src/home_presenter.h` | 增加 sprite state / emotion enum |
| `src/apps/app_home/src/home_presenter.c` | state / emotion 映射 |
| `src/apps/app_home/src/home_view.h` | 视图缓存字段、motion timer |
| `src/apps/app_home/src/home_view.c` | sprite 选择与 motion 动画 |
| `src/components/service_home/include/service_home.h` | 透传 emotion |
| `src/components/service_home/src/service_home.c` | 透传 emotion |
| `src/components/service_claude/include/service_claude.h` | snapshot schema 扩展 |
| `src/components/device_link/src/device_link.c` | `claude.update` 解析 emotion |
| `tools/esp32dash/src/config.rs` | host 配置加载 |
| `tools/esp32dash/src/emotion.rs` | analyzer |
| `tools/esp32dash/src/emotion_state.rs` | 累积衰减模型 |
| `tools/esp32dash/src/model.rs` | `LocalHookEvent` / `Snapshot` schema |
| `tools/esp32dash/src/agent.rs` | emotion state 生命周期、异步调用 |
| `tools/esp32dash/src/main.rs` | ingest、`chibi` smoke test |
| `tools/esp32dash/src/normalizer.rs` | 保持纯函数，但更新 `materially_equal()` |
| `tools/esp32dash/Cargo.toml` | `toml` 依赖 |
| `tools/sprite_convert.py` | sprite 生成逻辑 |
| `src/apps/app_home/CMakeLists.txt` | 新增生成资源文件 |
