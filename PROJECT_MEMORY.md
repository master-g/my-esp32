# 项目记忆 — esp32

> 跨会话的持久信息。完成任务后回写,约定见 [CLAUDE.md](./CLAUDE.md)。
> 长度上限约 400 行。超长时:**保留**「已验证的事实」与「下次运行」,**淘汰**最旧的「失败尝试」「上次会话」条目。
> 条目用 `- [YYYY-MM-DD]` 前缀,新条目追加在各小节**末尾**,便于按时间淘汰。

## 已验证的事实
<!-- 确认过的技术决策和约束。长期保留,不随压缩淘汰。 -->
- [2026-06-25] U1–U8 提交系列已完成"ESP32 = 只读授权通知器"的转向：设备只显示 Claude 交互的粗类型(权限/Elicitation/AskUserQuestion)，所有决策在 Mac 原生弹窗上做。`device_link` 只走单向 request/dismiss；入站决策帧、`claude.approve` RPC、审批 generation 校验已删除。详见 CLAUDE.md「Important Constraints」与 docs/。
- [2026-06-25] `protocol.error` 与串口行溢出保护与审批无关，是 load-bearing，勿回退。
- [2026-06-25] 锁顺序固定为 LVGL lock → service mutex；service 任务不得直接调 LVGL。

## 失败尝试
<!-- 踩过的坑、走不通的路径、被否决的方案及原因。超长时最旧条目优先淘汰。 -->
- [2026-06-25] 底层 direct-mode 画屏(`bsp_display_show_fatal_screen`)画完若 `end_direct_mode` + 解锁,`lvgl_port_task` 会随后用 `lv_timer_handler` 重绘界面、盖掉刚画的内容——U3 错误屏初版栽在这,红屏闪一下就被覆盖。要让画面留到重启/halt,终态必须保持独占:不 end direct mode、不放 LVGL 锁。(code review 两名审查员印证)
- [2026-06-25] event_bus 事件 payload 一律传 NULL,订阅者以事件类型为信号、自行 query 服务取数据(CLAUDE.md 约定)。`system_state` 原本给 `POWER_CHANGED` 传 `payload=&output`(栈指针);把 publish 移出 `s_mutex` 后这既违约、又是悬垂隐患(仅因同步 dispatch 侥幸安全)。已核实所有订阅者都不读 payload。新发事件别再塞栈指针。
- [2026-09-14] `cargo install` 覆盖 `~/.cargo/bin/esp32dash` 后立刻 `launchctl kickstart -k`,新进程第一次启动会被系统以 `OS_REASON_CODESIGNING` 杀掉,KeepAlive 节流约 10s 后自动拉起成功;这不是二进制损坏。
- [2026-10-13] agent 占着串口时 `esp32dash device screenshot` 会失败(两个进程抢读)。可行做法:`kill -STOP <agent pid>` → 截图 → `kill -CONT`;烧录同理(`PORT_BUSY_OK=1 make flash`),设备复位后 launchd 会自动重启 agent。

## 已验证的事实(续)

- [2026-08-31] net_manager 重试链曾永久停摆:`s_ignore_disconnect_event` 无条件置位,而 `esp_wifi_disconnect()` 在上次连接已失败时不发事件,挂着的标志吞掉下一次真实 DISCONNECTED(吞掉的分支不调度重试),设备卡死 CONNECTING 直到重启,连带 NTP 不跑、时钟停 `Time not synced`。修复(98c1ec5):标志以 `esp_wifi_disconnect()==ESP_OK` 为条件。教训:吞事件的标志必须以「确认会产生该事件」为前提。
- [2026-08-31] 诊断通道:launchd agent 占用串口时,走 admin HTTP `POST 127.0.0.1:37125/v1/device/rpc` 发 `device.info`(含 wifi_state/ip/auth_failed/last_disconnect_reason)等 RPC,不用停 agent。固件 ESP_LOG 主 console 在 UART0(115200),usbmodem 口只有 device_link 帧,看日志需接 UART0 那条线。
- [2026-09-14] esp32dash 服务日志:launchd 以 O_APPEND 打开 `~/Library/Logs/esp32dash/{stdout,stderr}.log`(lsof 可见 `AP`),但不创建目录也不轮转;目录缺失时日志静默丢失。agent 每 60s 检查,超 5 MiB 复制为 `.log.1` 后原地截断(rename 无效,launchd 持有 fd)。固件约每秒发一次 hello,`set_connected` 只在连接状态变化时打 INFO。
- [2026-09-14] OMP 不再支持,`hooks install/uninstall` 不再接受 target,只管 Claude Code。hook 脚本中 `$HOME` 下的可执行文件渲染为 `"${HOME}/..."`,与 `~/archdotfiles/ai/.claude/hooks/esp32dash-hook.sh`(被 `~/.claude/hooks/` 软链)逐字一致;`esp32dash hooks install` 返回 `script_written:false` 即为一致。
- [2026-10-13] 架构加深共识(grilling 定稿,术语见 GLOSSARY.md):顺序为 bug 修复 → 候选 2 合并队列 → 3 删 `claude respond` 全走 ingest → 1 pending interaction 以电平随 `claude.update` 下发并删四个 request/dismiss 帧、删 `home_approval` → 5 `protocol/fixtures/` 两侧共用样例帧 → 4 新建 `device_protocol` component 并把 NVS 写入还给 service_time/service_weather。超时只在主机队列,按 kind 取配置(600/120/180 秒);设备只在 pending 从无到有时切回 Home;主机侧 C 测试用 `cc`+`assert`+Homebrew cJSON,入口 `make test-host`;候选 1 完成时写 `docs/adr/0001-pending-interaction-as-snapshot-level.md`。每个候选一个分支,提交/合并/推送等用户发话。
- [2026-10-13] 线协议现状:pending interaction 以电平随 `claude.update` 的 `pending` 字段下发(ADR 0001),无 request/dismiss 帧;`service_claude` 持有该状态并设置 `alert_active`;方法名只在 `device_protocol.h` 的 `DP_METHOD_*` 定义;时区/天气的 NVS 读写都在各自 service 内。协议未带版本号,主机与固件必须一起更新。
- [2026-10-13] Home sprite 存储:源图在 `tools/sprite_assets/*.png`(取自 notchi `ed1e9cf`,不再依赖被 gitignore 的克隆;上游已改名并删了 `sleeping_happy`),`tools/sprite_convert.py` 生成 `generated/sprite_sheets.c`(整张图共用 RGB565 调色板 + 裁剪 + RLE,77 帧共 93 KB,原 ARGB8888 为 1.26 MB),`sprite_codec.c` 每次换帧解到一块 16 KB 的 ARGB8888 缓冲,渲染路径不变。没用 LVGL 原生 I8:它要求 `LV_BIN_DECODER_RAM_LOAD`,且带缩放旋转时每次重绘都从 64 KB 的 LVGL 堆临时分 16 KB。app 分区已从 4 MB 扩到 8 MB(NVS 偏移未变,配置保留),固件 2.19 MB,余 74%。
- [2026-10-13] chibi 情绪链路两端都已实现,但本机从未触发:`~/.config/esp32dash/config.toml` 的 `[emotion]` 是首次启动生成的默认值(`enabled = false`,`api_base_url`/`model`/`api_key` 为空),`EmotionAnalyzer::from_config` 返回 None,`snapshot.emotion` 恒为 `neutral`。启用需填这三项(Anthropic Messages 兼容、https、文件权限 600)并重启 launchd 服务。即使启用:只分析 `UserPromptSubmit`;每 60 秒分数 ×0.75;有 pending interaction 时 presenter 强制中性;compacting 无情绪变体,sleeping 只有 happy。
- [2026-10-13] 情绪提示词与计分照 notchi(`emotion.rs`/`emotion_state.rs`):分类器只答 happy/sad/neutral 加 `intensity`,其余标签按 neutral;每条消息加 `intensity × 0.5`,另一种情绪 ×0.9,neutral 两边 ×0.85,每分钟 ×0.92;happy ≥ 0.6、sad ≥ 0.45 才显示,≥ 0.9 升级为 elated/sob;分析失败不改分数。未用真实接口验证(本机未配 `[emotion]`)。没做:无手填配置时读 Claude Code `settings.json` 的 `ANTHROPIC_AUTH_TOKEN` 等。
- [2026-10-13] 新增情绪 `elated`(设备无对应图时退回 happy)与素材 `idle_elated`(17 帧,143 ms/帧)、`working_sob`、`waiting_sob`,取自 notchi `e6f8b8a`。现共 16 张 106 帧 111 KB。`chibi test --emotion elated` 可直接看。
- [2026-10-13] `idle_happy` 换成上游重画的 20 帧版(143 ms/帧);新增 `waving`(25 帧,104 ms/帧,播一遍 2.6 秒):Home 视图在 `claude_connected` 由假变真时播一次,有 pending interaction 时不播。源图是从画框左边探身进来的,转换脚本的 `MIRRORED` 把它水平翻转,视图再右移 `HOME_SPRITE_WAVE_SHIFT_X` 贴到屏幕右边缘。现共 17 张 145 帧 120 KB。真机看挥手:先 `launchctl bootout` 卸掉代理服务(否则设备重启后 launchd 新拉起的代理抢串口),`esp32dash device reboot` 后重试 `chibi test` 直到发出,立刻截图,最后 `launchctl bootstrap` 恢复。
- [2026-10-13] 电源:板子只走 USB、屏幕常亮 100%。电池/DIM/SLEEP/过热分支和 `system_state` 的 alert 标志、活动计数从未接入,已删;`power_policy` 只按前台 app 给行情 REALTIME/BACKGROUND_CACHE。显示是 `RENDER_MODE_FULL`(每次重绘整帧旋转推送),所以 Home 角色晃动定时器降到 50 ms、屏保直绘限到 33 ms。
- [2026-10-13] Home 布局:时钟保持整行 44 号 `HH:MM:SS`(试过把秒缩成 22 号,两种字体不协调,用户否决);项目名(快照 `workspace`)和 working 已用时长是角色脚下的 11 号小标签(放屏幕中间用 22 号字被用户否决为「出戏」);气泡在时钟与角色之间(`HOME_SESSION_X/W`,底色 `SURFACE_SECONDARY`,左下对齐),不再盖住角色。气泡在无 pending 时显示 `detail`(提示词预览/工具名),这是有意保留的。
- [2026-10-13] 主机:日志一律写 stderr(hook 的 stdout 会进 Claude 上下文);以 `<task-notification>`、`<system-reminder>`、`[SYSTEM NOTIFICATION`、`<agent-message from=` 开头的 UserPromptSubmit 不当作用户提示词;会话失活或被清理时连带清掉它的 pending;`state.json` 0600,pending id 用摘要不含问题文本。`make check` = fmt-check + `make test-host` + `cargo test`(不含固件构建;clippy 有 23 条既有警告未纳入)。
- [2026-10-13] 主机会话表:一个 Claude 进程只保留一条会话。`/clear`、`/resume` 会让同一 pid 换 session id,旧条目收不到事件、pid 又活着,不会被对账清掉;若它停在 waiting_for_input(优先级最高)就会一直占着屏幕。现在同 pid 的新 id 到来时删掉旧条目及其 pending(`apply_event_to_sessions`)。授权后到工具跑完之间 Claude Code 不发 hook,所以长命令期间提示会一直留到 PostToolUse。

## 上次会话
<!-- 上次运行做了什么、停在何处。滚动记录,超长时最旧条目优先淘汰。 -->
- [2026-06-25] 用 /bootstrap-claude 初始化上下文：补全 CLAUDE.md 的技术栈/命令/代码风格/禁止文件/审查规则/项目记忆 六节，创建本文件。AGENTS.md 已是指向 CLAUDE.md 的 symlink。
- [2026-06-25] 经 /ce-plan + /ce-work 实现固件健壮性加固 U1–U5(分支 `refactor/firmware-robustness-guards`):LVGL 任务断言、UI 锁有限超时、system_state publish 移出锁、丢包串口日志、bootstrap 失败错误屏 + 有界重试。每单元 `make build` 通过(用 `make build`,该 shell 无 idf.py;ESP-IDF 经 `~/.espressif/tools/activate_idf_v6.0.sh`)。随后 /ce-code-review 抓出并修复 5 个问题(见失败尝试前两条,以及 RTC 重启计数改 magic word、magic number 命名)。**未做真机验证**——无固件单测,需 flash 上板。
- [2026-09-14] 检查并调整 esp32dash 主机侧:删除 OMP 支持与仓库里过时的 `tools/esp32dash/hooks/esp32dash-hook.sh`;hook 脚本只让 PermissionRequest 走 `respond`(dotfiles 副本同步更新,未提交);launchd 日志加大小轮转、去 ANSI、去掉每秒 hello 日志。`cargo test` 114/114,已 `cargo install` 并重启服务,设备连接与 hook 事件送达均验证。改动未提交。
- [2026-10-13] 五个架构候选与开头的 bug 修复全部完成,随后又做了素材存储、新情绪素材、挥手和 notchi 式情绪计分;全部已合入 main 并推送(2026-10-13),过程分支已删。每步都烧录并用截图/RPC 在真机核对;`cargo test` 105、`make test-host`、`make build` 通过。设备已刷最终固件,`~/.cargo/bin/esp32dash` 与 hook 脚本也是最终版。新 hook 脚本在 `~/archdotfiles/ai/.claude/hooks/esp32dash-hook.sh`,dotfiles **未提交**;旧脚本会调用已删除的 `claude respond`,不要还原。
- [2026-10-13] /improve 审计后直接实现了 15 条发现和 3 个界面新增(索引在本地 `plans/README.md`,不入库):修 K 线纵坐标 int32 溢出、天气失败后每分钟才重试、Wi-Fi 慢重试轮换 profile、输密码时不重建键盘、Settings 去掉占位行、移除 pipes 屏保。真机截图核对了 Home 新布局、Settings、Trading;K 线溢出(1H/4H/1D)、Wi-Fi 轮换、天气退避、键盘重建只过了编译,未在真机复现。未推送。

## 下次运行
<!-- 计划要做的任务和优先级。长期保留,不随压缩淘汰。 -->
- [2026-06-25] 仓库尚无 CONTRIBUTING.md / CODEOWNERS / PR 模板，也未见固件单元测试；按需补充。
- [2026-06-25] 真机验证 `refactor/firmware-robustness-guards`:重点确认 U3 错误屏留存到重启、RTC 重启计数 N=3 按预期 halt、U5 死锁路径不再死锁。验证后再决定 merge/PR。`BOOT_FAIL_RESTART_LIMIT`(N=3)若不合实际启动失败模式可调。
- [2026-08-31] 实机「Time not synced」已修(见已验证的事实):98c1ec5 在 main 本地、**未 push**。RTC 电池疑似缺失(掉电即丢时间,靠 NTP 回写)——若在意断电守时可考虑装电池,非 bug。
- [2026-10-13] 待用户做:提交 dotfiles 里的 hook 脚本。遗留:`openspec/specs/` 下 `prompt-store-cleanup`、`device-prompt-ui`、`device-link-atomicity` 描述的是已删除的结构,未处理;`parse_set_many_params` 仍在 `device_link.c`(依赖 service 的配置类型,未搬进 `device_protocol`);行溢出保护只有主机测试,未在真机上发超长行验证。notchi 跟进未做:过滤 `<task-notification>` 等系统注入的 UserPromptSubmit、去 `<pasted_content>` 标签;情绪分析等用户填 `[emotion]` 配置后才会生效,尚未用真实接口验证。
- [2026-10-13] /improve 里考虑过未做的:管理端口鉴权、串口 worker 写超时与积压折叠、持久化移出请求路径、`app_settings.c` 拆分、可选服务初始化失败不致命、cJSON/NVS 句柄泄漏、截图超时悬空写、渲染模式改 partial(先测帧耗时)。dotfiles 里的 hook 脚本副本还没加 `>/dev/null 2>&1`(二进制已不往 stdout 写日志,非必需)。
