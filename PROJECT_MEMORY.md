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

## 上次会话
<!-- 上次运行做了什么、停在何处。滚动记录,超长时最旧条目优先淘汰。 -->
- [2026-06-25] 用 /bootstrap-claude 初始化上下文：补全 CLAUDE.md 的技术栈/命令/代码风格/禁止文件/审查规则/项目记忆 六节，创建本文件。AGENTS.md 已是指向 CLAUDE.md 的 symlink。
- [2026-06-25] 经 /ce-plan + /ce-work 实现固件健壮性加固 U1–U5(分支 `refactor/firmware-robustness-guards`):LVGL 任务断言、UI 锁有限超时、system_state publish 移出锁、丢包串口日志、bootstrap 失败错误屏 + 有界重试。每单元 `make build` 通过(用 `make build`,该 shell 无 idf.py;ESP-IDF 经 `~/.espressif/tools/activate_idf_v6.0.sh`)。随后 /ce-code-review 抓出并修复 5 个问题(见失败尝试前两条,以及 RTC 重启计数改 magic word、magic number 命名)。**未做真机验证**——无固件单测,需 flash 上板。
- [2026-09-14] 检查并调整 esp32dash 主机侧:删除 OMP 支持与仓库里过时的 `tools/esp32dash/hooks/esp32dash-hook.sh`;hook 脚本只让 PermissionRequest 走 `respond`(dotfiles 副本同步更新,未提交);launchd 日志加大小轮转、去 ANSI、去掉每秒 hello 日志。`cargo test` 114/114,已 `cargo install` 并重启服务,设备连接与 hook 事件送达均验证。改动未提交。
- [2026-10-13] 已提交(本地分支,未合并未推送):`fix/claude-update-preserve-pending-prompt`(f07162a,真机验证:Need input 气泡 20 秒后仍在)→ `refactor/pending-interaction-queue`(9dece4e,`pending.rs` 的 `PendingQueue`)→ `refactor/hooks-single-ingest`(删 `claude respond` 与四条 approvals/prompts 路由,PermissionRequest 在 ingest 内入队,hook 脚本不再分流)。三者都经截图在真机核对。新 hook 脚本已写入 `~/archdotfiles/ai/.claude/hooks/esp32dash-hook.sh`(dotfiles 未提交)。

## 下次运行
<!-- 计划要做的任务和优先级。长期保留,不随压缩淘汰。 -->
- [2026-06-25] 仓库尚无 CONTRIBUTING.md / CODEOWNERS / PR 模板，也未见固件单元测试；按需补充。
- [2026-06-25] 真机验证 `refactor/firmware-robustness-guards`:重点确认 U3 错误屏留存到重启、RTC 重启计数 N=3 按预期 halt、U5 死锁路径不再死锁。验证后再决定 merge/PR。`BOOT_FAIL_RESTART_LIMIT`(N=3)若不合实际启动失败模式可调。
- [2026-08-31] 实机「Time not synced」已修(见已验证的事实):98c1ec5 在 main 本地、**未 push**。RTC 电池疑似缺失(掉电即丢时间,靠 NTP 回写)——若在意断电守时可考虑装电池,非 bug。
- [2026-10-13] 剩余:候选 1(电平下发、删四帧与 `home_approval`、显示 coarse type、写 ADR 0001)→ 5(`protocol/fixtures/`)→ 4(`device_protocol` component、NVS 归还)。`openspec/specs/` 下 `prompt-store-cleanup`、`device-prompt-ui` 也已过时,未处理。
