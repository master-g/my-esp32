# 项目审计报告（2026-04-13）

## 范围与方法

这次审计以固件侧 `src/` 为主体，host 侧只覆盖直接影响设备边界的串口、协议和持久化实现，也就是 `tools/esp32dash/` 里和设备通信直接相关的部分。

方法仍以静态代码审阅为主，但这次补了两条基线验证：

- `make build` 通过，当前环境里固件可以完成构建
- `cargo test --manifest-path tools/esp32dash/Cargo.toml --quiet` 通过，49 个测试全部成功

## 当前状态判断

这个项目已经不再是“只有骨架”的阶段。模块边界是清楚的，代码风格也基本统一：大量使用 `ESP_RETURN_ON_*` 宏、服务层 copy-out getter、UI 线程与后台任务分离，这些都说明工程是按 ESP-IDF 的习惯在收口，而不是杂糅出来的 demo。

真正需要盯的，不是格式和命名，而是**共享状态的并发纪律、队列背压时的退化行为，以及安全边界到底收在哪**。这三类问题现在都还在，而且已经到了会影响长期稳定性的程度。

另外，仓库里 2026-04-09 的那份审计并没有完全过时，但也不能再原样沿用：其中几条关键问题已经被修掉了，下面会单列。

## 正面评价

固件主链路已经比较成型。`bootstrap` 把 `event_bus`、`app_manager`、`system_state`、`power_policy/power_runtime`、BSP 和各类 service 串起来，启动顺序基本合理，失败路径也普遍有显式返回，不是靠静默继续往下跑。相关代码在 `src/main/bootstrap.c:102-146`。

UI 线程边界总体是清楚的。`event_bus` 负责分发事件，`app_manager` 再把大多数事件转进自己的 UI queue，由 LVGL 线程统一消费；切页、screensaver、screenshot 这类 control 请求也单独走 `s_ui_control_queue`，这比把所有事情都塞进一个队列要稳。相关代码在 `src/components/core_app_manager/src/app_manager.c:17-75, 525-594`。

安全方面并不是完全裸奔。默认配置里已经打开了 HMAC 保护的 NVS encryption，`bootstrap` 也会在启动时检查默认安全方案是否真的可用。相关位置在 `sdkconfig.defaults:25-27` 和 `src/main/bootstrap.c:64-79`。

## 主要发现

### 高风险

#### 1. 多个模块的共享状态没有严格遵守“声明了 mutex 就都经由 mutex 访问”的纪律

这不是某一处小疏漏，是几块代码同时存在同类问题。

`weather_service` 明明已经有 `s_mutex`，但 `s_last_request_us`、`s_last_success_us`、`s_refresh_in_progress` 这些关键状态仍在多个上下文里直接读写：声明在 `src/components/service_weather/src/service_weather.c:22-28`，无锁读取分散在 `:134-178`, `:287-301`（`:104-112` 的 `mark_stale_if_needed` 是在锁内的，这条没问题），无锁写入分散在 `:194`, `:219`, `:279`, `:329-331`。这些访问横跨 event handler、worker task 和对外 API。

另外，`weather_service_get_location_config()` 在 `:304-311` 直接 `memcpy` 读取 `s_location_config`，没有任何锁保护；而 `weather_service_apply_location_config()` 在 `:313-339` 会从 device_link 的 reader task 写入同一个结构体。两者可以并发执行。

`time_service` 也是同样的模式。`s_ntp_synced`、`s_sync_in_progress`、`s_rtc_valid` 这些状态由 worker task、event handler、getter 和同步请求路径共同访问，但并没有全部纳入统一锁保护。可以直接看 `src/components/service_time/src/service_time.c:28-36, 115-126, 241-264, 304-318, 357`。

更严重的是 `time_service_apply_timezone_config()` 在 `:340-355`：它直接调用 `time_service_refresh_now()` 修改 `s_snapshot`，没有拿 `s_mutex`，而 `time_service_get_snapshot()` 在另一个线程里持锁调用同一个函数。这是一个教科书式的数据竞争。此外，`time_service_get_timezone_name()` 和 `time_service_get_timezone_tz()` 在 `:336, :338` 直接返回指向 `s_timezone_name` 和 `s_timezone` 静态 buffer 的裸指针，而 `apply_timezone_config` 会从 device_link reader task 写入这些 buffer。

`system_state` 里的 `s_user_activity_seq` 也在绕过锁。读取在 `src/components/core_system_state/src/system_state.c:63`，递增在 `:105`，而消费方 `power_runtime` 会在另一个任务里轮询它，见 `src/components/power_runtime/src/power_runtime.c:85-102`。

这类问题平时不一定立刻炸，但它会把系统带进“偶发重复刷新、偶发漏刷新、偶发状态闪烁”的区间，最难查，也最像现场问题。对嵌入式项目来说，这比显式崩溃更麻烦。

建议把这些模块统一收口成一个规则：**凡是影响状态机判断或刷新节流的字段，全部只在锁内读写；如果必须跨任务无锁访问，就改成明确的原子变量。**

#### 2. approval 流程依赖一组无锁全局状态，跨 reader task、等待任务和 LVGL 线程共享

`device_link` 的 approval 状态全部放在 file-static 变量里：`s_approval_req`、`s_approval_decision`、`s_approval_uses_rpc`、`s_approval_rpc_id`，定义在 `src/components/device_link/src/device_link.c:58-63`。

这组状态会被至少三条路径同时读写：

- 串口 reader task 收到 `claude.approval.request` / `claude.approve` / `claude.approval.dismiss` 时更新它，见 `src/components/device_link/src/device_link.c:1014-1049, 1169-1205`
- `approval_rpc_wait_task` 超时或收到 semaphore 后读取并复位它，见 `:409-425`
- Home 页 UI 通过 `device_link_get_pending_approval()`、`device_link_resolve_approval()`、`device_link_cancel_approval()` 读写它，见 `:1458-1514` 和 `src/apps/app_home/src/home_approval.c:113-145`

这里没有 mutex，也没有版本号。只要 host dismiss approval、用户本地点按钮、或者 RPC/event 两种 approval 路径交错，状态就可能被覆盖或提前复位。表现出来会是 overlay 和 host 状态不一致、approval 被吞、或者错误的请求拿到响应。

这条线建议单独加一个小的状态机保护层：至少要把 approval state 收进 mutex，外加 request id / generation 校验，避免旧请求和新请求互相污染。

### 中风险

#### 3. 队列背压时的退化策略过于“掉了就掉了”，对上层几乎不可见

`app_manager` 的 UI 事件队列长度只有 16，满了就直接丢事件，只打一个 warning。代码在 `src/components/core_app_manager/src/app_manager.c:18, 423, 544-547`。

`market_service` 更明显。内部 `queue_command()` 只是简单 `xQueueSend(..., 0)`，失败就返回 `false`，但流式回调里大量直接 `(void)queue_command(&command);`，也就是失败后没有恢复、没有计数、没有回传。代码在 `src/components/service_market/src/service_market.c:526-533, 545-606`。

这两处叠在一起，意味着市场数据最忙的时候，恰恰最容易出现“service 内部已经丢了更新，而 UI 侧完全不知道”的情况。相关的刷新与发布路径在 `src/components/service_market/src/service_market.c:894-1010`。

这类问题不一定需要“一个都不能丢”，但至少要有可观测性。更合理的做法是：

- 区分可合并事件和不可丢事件
- 对 market stream 这类高频事件做 coalescing
- 增加 overflow 计数和最后一次溢出的时间，暴露到调试快照里

#### 4. 串口协议对超长输入的处理是静默复位，调试和排障成本偏高

设备侧 `reader_task` 使用 `DEVICE_LINK_LINE_MAX=768` 的行缓冲。超过长度以后，当前实现直接把 `line_len` 清零，并不会返回显式协议错误，也不会写日志。代码在 `src/components/device_link/src/device_link.c:39, 1370-1413`。

这不是传统意义上的安全漏洞，但它很容易把现场问题变成黑盒：一旦 host 发出超长行，设备侧就悄悄丢掉当前帧，双方都只看到“协议好像偶尔没反应”。

如果这条协议只跑结构很固定的帧，这个问题暂时还不会频繁出事；但一旦 `claude.update` 的 `title/detail/workspace` 变长，或者以后加了更复杂的 RPC，这个上限很容易被打到。建议至少补两件事：一是日志，二是一个明确的 `protocol.error` 事件。

#### 5. 当前构建配置的安全硬化只做到 NVS encryption，离 production 还差一截

这套配置已经启用了 NVS encryption，见 `sdkconfig.defaults:25-27`。但当前实际构建配置里，`CONFIG_SECURE_BOOT`、`CONFIG_SECURE_FLASH_ENC_ENABLED`、`CONFIG_FLASH_ENCRYPTION_ENABLED` 都还是关闭状态，见 `sdkconfig:802, 804, 4380`。

这意味着当前方案的安全边界比较明确：**配置和命中记录在 NVS 里是加密存储的，但设备本身还不具备完整的 anti-tamper / anti-extraction / anti-rollback 保护。**

如果它目前只是开发设备，这个取舍可以接受；如果后面要长期离开受信环境，或者设备上开始存放更有价值的数据，这一条迟早要补。

#### 6. `slot.export_hit` 直接把 WIF 通过串口协议导出，必须明确写进威胁模型

`slot.export_hit` RPC 在读取到命中记录后，会直接把 `wif`、`label`、`hash160` 和时间戳返回给 host，代码在 `src/components/device_link/src/device_link.c:1309-1336`。

从功能角度看，这显然是为了运营或调试便利；从安全角度看，它等于告诉你：**只要串口协议通了，命中私钥就能被导出，而且这里没有再加一层确认、授权或物理存在校验。**

这不一定要删，但至少要在报告里写明白它的前提：它只适合“USB host 完全可信”的场景。否则，前面做的 NVS encryption 就只保护了静态存储，没保护运行时导出面。

### 低风险

#### 7. 文档已经和真实 app 结构脱节

实际注册的 app 是 `Home / Trading / Satoshi Slot / Settings`，见 `src/main/bootstrap.c:84-99` 和 `src/components/core_types/include/core_types/app_id.h:6-13`。

但 `README.md` 和 `CLAUDE.md` 里还在写 `Notify`，甚至还保留了 `app_notify` 的目录描述，见 `README.md:16, 120` 和 `CLAUDE.md:7, 16, 118`。这会直接误导后续维护者判断系统结构。

#### 8. event_bus 的订阅表上限只有 8，当前已经接近打满

`event_bus` 的订阅上限是固定 8，见 `src/components/core_event_bus/src/event_bus.c:7, 14-16, 37-38`。

当前实际订阅点已经有 7 个：`bootstrap/app_manager`、`time_service`、`weather_service`、`settings_service`、`power_runtime`、`market_service`、`bitcoin_service`，对应 `src/main/bootstrap.c:112`，`src/components/service_time/src/service_time.c:282`，`src/components/service_weather/src/service_weather.c:261`，`src/components/service_settings/src/service_settings.c:209`，`src/components/power_runtime/src/power_runtime.c:152`，`src/components/service_market/src/service_market.c:1139`，`src/components/service_bitcoin/src/service_bitcoin.c:664`。

它现在还没出问题，但扩展余量已经非常薄。再加一个订阅者，就要开始考虑“为什么初始化会失败”这类很不值当的问题。

## 和 2026-04-09 报告相比，已经修掉的项

有几条旧问题这次不该再重复写进结论里，因为代码已经改过来了。

RTC 的 UTC / local time 语义现在已经收正。`bsp_rtc_read_epoch()` 用的是 `timegm()`，`bsp_rtc_write_epoch()` 用的是 `gmtime_r()`，不再把本地时区语义写进 RTC。证据在 `src/components/bsp_board/src/bsp_rtc.c:103-153`。

天气客户端也已经不再靠字符串硬抠 JSON。现在是 `cJSON_Parse()`，并且对响应缓冲做了 `overflow` 检测，容量也从旧报告里的 2048 提到了 4096。证据在 `src/components/service_weather/src/weather_client.c:15-23, 46-73, 131-138`。

Host 侧状态持久化也加固了。`persist_state()` 现在走的是临时文件 + `fsync` + `rename`，不再是直接覆盖原文件。证据在 `tools/esp32dash/src/agent.rs:685-699`。

Host 串口读缓冲也补了上限。`read_line()` 现在有 `MAX_LINE_BUFFER=4096`，超过会 warning 并清空，不再无限增长。证据在 `tools/esp32dash/src/device.rs:32, 923-960`。

## 收口判断

这个项目现在的状态，我会这样概括：**架构已经成型，工程习惯也基本站住了，但并发纪律和安全边界还没有完全收口。**

如果接下来要继续推进，我建议优先级按这个顺序来：

1. 先把 `device_link` 的 approval 状态改成有锁、有代次校验的状态机
2. 再把 `service_time` / `service_weather` / `system_state` 这些共享状态的访问规则收紧
3. 然后补队列溢出和协议超长帧的可观测性
4. 最后再决定 secure boot / flash encryption 与 `slot.export_hit` 的正式威胁模型

这些事做完，这个项目才算从“能稳定开发”走到“能稳定运行”。
