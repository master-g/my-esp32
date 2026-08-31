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

## 已验证的事实(续)

- [2026-08-31] net_manager 重试链曾永久停摆:`s_ignore_disconnect_event` 无条件置位,而 `esp_wifi_disconnect()` 在上次连接已失败时不发事件,挂着的标志吞掉下一次真实 DISCONNECTED(吞掉的分支不调度重试),设备卡死 CONNECTING 直到重启,连带 NTP 不跑、时钟停 `Time not synced`。修复(98c1ec5):标志以 `esp_wifi_disconnect()==ESP_OK` 为条件。教训:吞事件的标志必须以「确认会产生该事件」为前提。
- [2026-08-31] 诊断通道:launchd agent 占用串口时,走 admin HTTP `POST 127.0.0.1:37125/v1/device/rpc` 发 `device.info`(含 wifi_state/ip/auth_failed/last_disconnect_reason)等 RPC,不用停 agent。固件 ESP_LOG 主 console 在 UART0(115200),usbmodem 口只有 device_link 帧,看日志需接 UART0 那条线。

## 上次会话
<!-- 上次运行做了什么、停在何处。滚动记录,超长时最旧条目优先淘汰。 -->
- [2026-06-25] 用 /bootstrap-claude 初始化上下文：补全 CLAUDE.md 的技术栈/命令/代码风格/禁止文件/审查规则/项目记忆 六节，创建本文件。AGENTS.md 已是指向 CLAUDE.md 的 symlink。
- [2026-06-25] 经 /ce-plan + /ce-work 实现固件健壮性加固 U1–U5(分支 `refactor/firmware-robustness-guards`):LVGL 任务断言、UI 锁有限超时、system_state publish 移出锁、丢包串口日志、bootstrap 失败错误屏 + 有界重试。每单元 `make build` 通过(用 `make build`,该 shell 无 idf.py;ESP-IDF 经 `~/.espressif/tools/activate_idf_v6.0.sh`)。随后 /ce-code-review 抓出并修复 5 个问题(见失败尝试前两条,以及 RTC 重启计数改 magic word、magic number 命名)。**未做真机验证**——无固件单测,需 flash 上板。

## 下次运行
<!-- 计划要做的任务和优先级。长期保留,不随压缩淘汰。 -->
- [2026-06-25] 仓库尚无 CONTRIBUTING.md / CODEOWNERS / PR 模板，也未见固件单元测试；按需补充。
- [2026-06-25] 真机验证 `refactor/firmware-robustness-guards`:重点确认 U3 错误屏留存到重启、RTC 重启计数 N=3 按预期 halt、U5 死锁路径不再死锁。验证后再决定 merge/PR。`BOOT_FAIL_RESTART_LIMIT`(N=3)若不合实际启动失败模式可调。
- [2026-08-31] 实机「Time not synced」已修(见已验证的事实):98c1ec5 在 main 本地、**未 push**。RTC 电池疑似缺失(掉电即丢时间,靠 NTP 回写)——若在意断电守时可考虑装电池,非 bug。
