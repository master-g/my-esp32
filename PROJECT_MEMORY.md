# 项目记忆 — esp32

> 跨会话的持久信息，只写代码、Git 和文档推导不出的内容。回写约定见 [AGENTS.md](./AGENTS.md)。

## 已验证的事实
<!-- 确认过的技术决策和约束。每条一行：结论 + 原因 + 来源或适用范围。 -->
- [2026-11-02] `esp_wifi_disconnect()` 的返回值判断不了事件会不会来：空闲时返回 ESP_OK 却不发事件，打断在途连接时原因码是 36 而非 8；所以 `s_ignore_disconnect_event` 只吞这两个原因码。真机验证。
- [2026-11-02] 连接尝试进行中 `wifi.scan` 会返回空列表（约一半），未联网时要先取消在途连接再扫。设备重试时 `last_disconnect_reason` 应循环 201；一直是 0 说明重试链停了。
- [2026-09-14] launchd 不创建日志目录，`~/Library/Logs/esp32dash/` 缺失时日志静默丢失；轮转只能原地截断，rename 无效（launchd 持有 fd）。
- [2026-09-14] hook 脚本的真身在 `~/archdotfiles/ai/.claude/hooks/esp32dash-hook.sh`（`~/.claude/hooks/` 是软链）；`esp32dash hooks install` 返回 `script_written:false` 表示与模板一致。
- [2026-10-13] sprite 没用 LVGL 原生 I8：它要求 `LV_BIN_DECODER_RAM_LOAD`，且带缩放旋转时每次重绘都从 64 KB 的 LVGL 堆临时分 16 KB；所以自带调色板 + RLE，每帧解到一块 ARGB8888 缓冲。
- [2026-10-13] 素材取自 notchi（`e6f8b8a`），源图已复制进 `tools/sprite_assets/`，不依赖上游克隆；`waving` 源图朝左，转换脚本的 `MIRRORED` 把它翻转。
- [2026-10-13] 情绪链路两端已实现但本机从未启用（用户因隐私搁置）；notchi 式计分未用真实接口验证。即使启用也只分析 UserPromptSubmit，有 pending interaction 时强制中性。
- [2026-10-13] `waving` 在主机由断开变为连上时播一次，这个时机是 Claude 代选的，用户未表态；想改成每次 SessionStart 之类要先问。
- [2026-10-13] 气泡在没有 pending interaction 时显示 `detail`（提示词预览或工具名）是用户确认保留的；「只显示粗类型」只约束 pending。
- [2026-10-13] 主机会话表按「一个 Claude 进程一条会话」处理：`/clear`、`/resume` 会让同一 pid 换 session id，旧条目若停在等待状态会一直占屏。同进程多会话各报 id 的模式会互相顶掉。
- [2026-10-13] 回合计时由主机按会话维护并以时长下发（设备不需要和主机对时）；设备本地计时在项目互相抢占屏幕时会归零，用户反馈后改掉。
- [2026-10-13] 屏保命令不能经代理转发：代理的下一帧快照会把屏幕唤醒，所以 `chibi screensaver` 先让代理让出串口再直连。
- [2026-11-02] macOS 把当前 SSID 显示成 `<redacted>`（`ipconfig`、`system_profiler` 都读不到），`networksetup -listpreferredwirelessnetworks` 和钥匙串条目可读；所以 `wifi-import` 用设备扫描与已保存网络的交集选网，也避开 Mac 在 5 GHz 而设备只有 2.4 GHz 的问题。
- [2026-11-02] 设备所在位置 `Mico_SZ` 只有 5 GHz（Mac 连它在 40 信道，用户确认），ESP32-S3 连不上，所以时间靠 agent 的 `time.sync`（连上即发，每 60 秒一次）；天气、行情没有网络，仍无数据。

## 失败尝试
<!-- 走不通的路径和原因，防止下次重走。 -->
- [2026-06-25] 致命错误屏画完后若结束 direct mode 并放 LVGL 锁，LVGL 任务会立刻重绘盖掉它；终态画面必须保持独占直到重启。
- [2026-06-25] 事件 payload 一律 NULL：`POWER_CHANGED` 曾传栈上指针，publish 移出锁后成了悬垂隐患。新事件不要带指针。
- [2026-10-13] Home 时钟把秒缩成 22 号字：两套字体笔画风格对不上，用户否决，恢复整行 44 号。
- [2026-10-13] 项目名用 22 号字放在屏幕中间：用户认为出戏，改成角色脚下的 11 号小标签。
- [2026-10-13] 气泡宽度超过 190 会压到日期行的星期；放在角色正下方则会盖住角色下半身。

## 上次会话
<!-- 整节改写：停在何处、验证结果。 -->
- [2026-11-02] main。修了 Wi-Fi 重试链停摆（真机验证 201 循环、扫描 10/10），加了 `device wifi-import`（真机用假 `security` 走通写入并清理，真实钥匙串读取退出码 0，两者未合在一次运行里）；`make check` 通过。设备当前扫不到已保存的网络，时间仍未同步。
  「连上后 NTP 成功」未验证。K 线溢出（1H/4H/1D）、Wi-Fi profile 轮换、天气失败退避、输密码时不重建键盘只过了编译，未在真机复现。本地索引在 `plans/README.md`（不入库）。

<!-- 整节改写：接下来的计划与优先级。 -->
- [2026-11-02] 若路由器开了 2.4 GHz，确认设备连上后 NTP 成功、天气行情恢复。可选的后续，按价值排：串口 worker 写超时与积压折叠；`app_settings.c` 拆分；可选服务初始化失败不致命；渲染模式改 partial 前先测帧耗时。
  遗留：`openspec/specs/` 下三份规格描述的是已删除的结构；clippy 有 23 条既有警告未纳入 `make check`；dotfiles 的 hook 脚本副本未同步输出屏蔽（非必需）。
