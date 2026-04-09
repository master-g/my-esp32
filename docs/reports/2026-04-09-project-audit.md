# 项目审计报告（2026-04-09）

## 范围与方法

本次审计覆盖两个部分：

- 固件侧：`src/`
- Host 侧：`tools/esp32dash/`

方法以静态代码审阅为主，辅以仓库现有命令的最小验证。

本次已完成的验证：

- `cd tools/esp32dash && cargo test --quiet`，18 个测试通过

本次未完成的验证：

- `idf.py build`

原因很直接：当前执行环境里没有可用的 `idf.py`，因此固件部分的结论主要来自代码路径和线程模型分析，而不是构建产物或实机回归。

## 总体结论

这个仓库已经有了比较清楚的分层和可运行骨架，尤其是：

- 固件侧的 BSP、服务层、四个 app 槽位已经搭起来了
- Host 侧 `esp32dash` 也不是空壳，最小测试是通的

问题不在“有没有骨架”，而在“现在这套骨架能不能长期稳定跑”。按这次看到的实现，答案是否定的。当前最主要的风险集中在固件的共享状态和 UI 事件路径，以及 Host 侧的持久化和串口边界处理。它适合继续开发，不适合当成稳定 release 候选。

## 主要发现

### 高风险

#### 1. 固件共享状态和事件分发没有明确的并发边界

当前代码里，大量运行时状态都以全局静态快照的形式存在，再通过事件或 getter 直接暴露：

- `src/components/core_event_bus/src/event_bus.c:47-59`
- `src/components/core_system_state/src/system_state.c:13-27,42-76`
- `src/components/service_time/src/service_time.c:113-125,200-224,290-314`
- `src/components/service_weather/src/service_weather.c:81-88,174-209,317-320`
- `src/components/service_claude/src/service_claude.c:11-18,43-55`
- `src/components/net_manager/src/net_manager.c:44-52,322-326`
- `src/components/service_home/src/service_home.c:34-62`

现在的 `event_bus_publish()` 是同步调用，谁发布事件，订阅者就在哪个线程里被直接执行。与此同时，各个 service 又把内部静态快照按指针直接返回。这样一来，天气 worker、Wi-Fi 事件线程、USB reader task、power runtime、LVGL task 都可能在不同时间读写同一批快照。

这会带来两个直接后果。第一，`home_service_refresh_snapshot()` 之类的聚合路径可能读到半更新状态，表现成页面数据偶发错位。第二，功耗策略和 UI 刷新顺序容易漂移，问题很难稳定复现。

这不是风格问题，是模型问题。现在缺的不是再补几个判空，而是要给状态流加一个明确的串行化边界。

#### 2. UI 刷新链路里存在无限等待，卡住时会把整机拖死

这条链路可以从 1 秒 tick 一直追到显示 flush：

- `src/main/bootstrap.c:33-42`
- `src/components/core_app_manager/src/app_manager.c:40-49,125-145`
- `src/components/bsp_board/src/bsp_display.c:102-118,147-150`

`tick_1s_cb()` 在 `esp_timer` 回调里直接发事件。`app_manager` 收到事件后会在发布线程里拿显示锁，超时时间是 `UINT32_MAX`。另一方面，`lvgl_flush_cb()` 里又用 `xSemaphoreTake(..., portMAX_DELAY)` 等 DMA flush 完成。

这意味着只要 LCD/SPI/DMA 某一次 flush 没有正常回调，或者 LVGL task 长时间持锁，系统里其他事件路径就会在无限等待上堆住。落到用户手里，就是“看起来还活着，但触摸、定时刷新、页面更新都越来越僵”。

#### 3. 固件后台 task 创建失败会被静默吞掉

下面这些地方都创建了 task，但没有检查返回值：

- `src/components/bsp_board/src/bsp_display.c:271-272`
- `src/components/power_runtime/src/power_runtime.c:150-151`
- `src/components/service_weather/src/service_weather.c:249-250`
- `src/components/service_time/src/service_time.c:267`
- `src/components/device_link/src/device_link.c:863-864`

这类问题最难受的地方在于，它不会把系统立刻炸掉。启动流程会继续往下走，但某个 worker 实际上根本没起来，后面只表现成“某个模块偶尔失灵”或者“某条链路无响应”。对嵌入式 bring-up 来说，这种静默失败很伤。

### 中风险

#### 4. RTC 读写使用的是 local time 语义，不是 UTC

相关代码在这里：

- `src/components/bsp_board/src/bsp_rtc.c:42-54,103-152`
- `src/components/service_time/src/service_time.c:48-56,95,127-145,300-314`

`bsp_rtc_read_epoch()` 和 `bsp_rtc_write_epoch()` 都通过 `localtime_r()` / `mktime()` 走本地时区语义。与此同时，`time_service_apply_timezone_config()` 又允许在运行时修改 `TZ`。

结果是 RTC 里保存的并不是稳定的 UTC 时间，而是“当前时区解释下的本地时间”。设备如果在改过 timezone 之后重启，或者以后遇到 DST 规则变化，就可能在 NTP 之前先恢复出一个偏移的 epoch。

这个问题平时不一定露头，但一旦遇到“断网重启 + RTC 恢复”的场景，时间会显得很怪，而且不好追。

#### 5. 天气客户端对上游响应过于脆弱

相关代码在这里：

- `src/components/service_weather/src/weather_client.c:14-42`
- `src/components/service_weather/src/weather_client.c:45-83`
- `src/components/service_weather/src/weather_client.c:117-171`

当前实现有三个明显特征：

- 响应缓冲固定为 `2048` 字节
- 超出容量时不会给出专门的“响应被截断”错误
- JSON 解析靠字符串查找和 `strtod()`，不是结构化解析

Open-Meteo 现在的响应刚好还能被这套逻辑吃下去，不代表以后也行。只要上游字段顺序、空格、嵌套形式或者响应体长度稍有变化，这段代码就可能开始报 `ESP_ERR_INVALID_RESPONSE`，但日志里很难看出根因到底是 API 变了、响应过大，还是网络截断。

#### 6. Host agent 的状态文件不是原子写入，损坏时还会静默回退

相关代码在这里：

- `tools/esp32dash/src/agent.rs:306-314`

`persist_state()` 直接 `fs::write()` 覆盖 `state.json`。如果进程刚好在写文件过程中退出，文件可能只写了一半。更麻烦的是，`load_state()` 在读取或反序列化失败时直接返回 `None`，不会把“状态文件损坏”作为一个显式错误抛出来。

这样一来，用户看到的效果不会是明确报错，而是 agent 悄悄把自己当成第一次启动。对于一个要把 Claude 状态推到设备上的 host 进程来说，这种静默退化是不够稳的。

#### 7. Host 串口读取没有行长上限

相关代码在这里：

- `tools/esp32dash/src/device.rs:480-566`

`UnixSerialPort::read_line()` 会不断往 `read_buf` 里追加字节，直到碰到 `\n`。问题在于，这个缓冲没有上限。如果设备端因为 bug、串口噪声或者协议错位一直不发换行，`read_buf` 就会一直涨。

这条边界在 demo 阶段常常被忽略，但它对长期运行很关键。坏设备、坏线缆、坏帧都不该把 host 进程拖到内存膨胀。

#### 8. Host 对串口 worker 和配置错误的反馈还不够硬

相关代码在这里：

- `tools/esp32dash/src/device.rs:85-87`
- `tools/esp32dash/src/compat.rs:26-29`

`DeviceManager::send_snapshot()` 丢弃了 `mpsc::Sender::send()` 的错误。如果 worker 因为 panic 或其他异常退出，后续 snapshot 更新会直接被吞掉，调用方拿不到明确反馈。

另外，`ESP32DASH_SERIAL_BAUD` 解析失败时会静默退回 `115200`。这不会立刻把程序搞崩，但会让配置错误变得很难察觉，尤其是在用户自己改环境变量的时候。

## 验证限制

这次结论需要和下面几个限制一起看：

1. 固件部分没有在可用的 ESP-IDF 环境里做构建验证，`idf.py build` 没跑起来。
2. 仓库里当前没有 `.github/workflows`，也就没有现成的 CI 去压这些并发和边界问题。
3. 这次没有接实机，没有做长时间运行、断线重连、串口异常帧、天气接口异常响应等压力场景。

所以这份报告更准确的定位，是“把容易在现场出事的点先圈出来”，不是一份替代实机验证的放行结论。

## 收口判断

如果只看工程骨架，这个项目已经过了“从零到一”的阶段；如果看稳定性，它还停在“能跑起来，但还扛不住现场”的阶段。

接下来的工作不该继续优先铺新功能，应该先把三件事做扎实：

1. 固件状态流和 UI 线程模型收口
2. 显示链路的超时与失败路径补齐
3. Host 持久化和串口边界加固

这些事不做完，后面接入更多真实数据，只会把现在这些不稳定点放大。
