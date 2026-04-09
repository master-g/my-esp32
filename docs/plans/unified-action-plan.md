# 统一行动计划

本文档合并了以下四份文件的内容，形成一份从现状出发、按优先级排列的完整行动路线：

- `reports/2026-04-09-project-audit.md`（审计报告）
- `plans/2026-04-09-audit-action-plan.md`（审计整改行动报告）
- `plans/project-progress-plan.md`（项目进度评估与实施计划）
- `plans/weather-system-plan.md`（天气系统实施计划）

## 现状

### 已经站住的部分

项目已越过纯设计阶段。ESP-IDF 工程结构、组件分层、BSP、服务骨架、四个 app 槽位都已落地。横屏显示基线已稳定，支持两种固定 landscape 安装方向。Host 侧 `esp32dash` 是独立 Rust 项目，18 个测试通过。

具体完成项：

- 仓库卫生与文档同步（`.gitignore` 收口、`README.md` 对齐代码现实）
- 固件编译基线（`ESP-IDF 6.0` 下 `idf.py build` 通过，二进制占用约 55% app 分区）
- 横屏方向基线（软件旋转到 640×172 landscape，`app_home` 实机回归通过）
- RTC 恢复、Wi-Fi 联网、IP 获取、SNTP 校时、NTP 写回 RTC 均已串口验证
- `power_runtime` 已把 `power_policy` 输出执行到背光，USB 供电下保持 ACTIVE

### 还没站住的部分

板级闭环的异常路径（断线重连、背光 dim/sleep 端到端、触摸异常）尚未回归。`service_weather`、`service_market`、`service_claude` 仍是占位。Notify、Trading、Satoshi Slot 页面仍是空壳。全局翻页、边缘手势、分页指示器未落地。IMU 未接入。

### 审计暴露的结构性风险

2026-04-09 审计发现了 3 个高风险和 5 个中风险问题。核心判断是：当前骨架"能跑起来，但扛不住现场"。最主要的风险集中在固件共享状态缺乏并发边界、UI 刷新链路存在无限等待、以及 Host 侧持久化和串口边界处理不够硬。

这些问题不解决就继续铺新功能，只会把不稳定点放大。所以行动路线把地基加固排在功能接线之前。

## 行动路线

### 第一阶段：固件地基加固

这是全局阻塞项，优先级最高。不完成这一阶段，后续所有板级验证和数据接线都建立在不稳定模型上。

#### 动作 1：给运行时状态引入单一写入边界

对应审计发现 #1（高风险）。

范围：`event_bus`、`system_state`、`service_time`、`service_weather`、`service_claude`、`net_manager`、`service_home`。

做法：不再把内部静态快照按裸指针暴露给外部长期读取。改成"复制快照到调用方 buffer"或"通过消息队列投递不可变副本"。每个 service 只在自己的 task 内修改状态，对外只给副本。

完成标准：不再存在多个 task 直接读写同一个可变静态快照。`home_service` 聚合时拿到的是一致快照，而不是若干 live pointer。

#### 动作 2：把 UI 更新收敛到单一线程

对应审计发现 #1（高风险，UI 侧表现）和 #2（高风险）。

范围：`bootstrap`、`app_manager`、所有 `app_*`、`bsp_display`。

做法：引入 UI command queue。Wi-Fi 事件线程、USB reader task、天气 worker、`esp_timer` 回调不再直接驱动 LVGL 更新，只投递"需要刷新"的命令。由 LVGL 所在线程统一消费。

完成标准：app 的 `handle_event()` 不再在任意发布线程里直接跑 LVGL 更新。定时器和网络事件线程里不再出现无限等待显示锁的路径。

#### 动作 3：移除显示链路的无限等待

对应审计发现 #2（高风险）。

范围：`app_manager`、`bsp_display`、`app_notify`、其他直接拿 `bsp_board_lock()` 的路径。

做法：显示锁改成有限超时，超时时记录明确日志。`lvgl_flush_cb()` 等待 flush done 时增加故障超时和错误状态。超时时优先保系统可恢复。

完成标准：关键显示路径不存在无限阻塞。flush 失败时能留下足够的诊断信息。

#### 动作 4：补齐 task 创建失败处理

对应审计发现 #3（高风险）。

范围：`bsp_display`、`power_runtime`、`service_weather`、`service_time`、`device_link`。

做法：检查所有 `xTaskCreatePinnedToCore()` 返回值。启动失败时让初始化链路报错，不默默继续。`bootstrap_start()` 应该能把这类错误明确冒出来。

完成标准：所有关键后台 task 都有失败检测。初始化不会在"半起半不起来"的状态下继续前进。

### 第二阶段：板级闭环与时间语义

第一阶段修完地基之后，进入板级端到端验证。RTC 语义修正放在这个阶段，因为它属于板级正确性，不是业务功能。

#### 动作 5：P0 板级打通验证

对应项目进度计划中的 `board-bringup-validation`。

范围：实机全链路。

验证项：LCD 刷新稳定性、触摸坐标读数、RTC 读写恢复、Wi-Fi 联网与断线重连、NTP 校时写回 RTC、`power_policy` 对亮度/dim/sleep 的实际驱动效果。

完成标准：在实机上完成所有验证项的至少一轮回归记录。

#### 动作 6：把 RTC 统一成 UTC 语义

对应审计发现 #4（中风险）。

范围：`bsp_rtc`、`service_time`。

做法：RTC 里只存 UTC 对应的 calendar time。timezone 只影响展示和本地格式化，不影响 RTC 持久化语义。timezone 变更后，系统显示文本更新即可，不要让已存 RTC 数据重新解释成另一套 epoch。

完成标准：断网重启后，RTC 恢复结果不依赖当前 `TZ`。timezone 修改不会让 RTC 恢复出的 epoch 漂移。

### 第三阶段：数据服务接线

板级闭环稳定后，补齐真实数据链路。天气系统的结构化解析要求（审计发现 #5）在这一阶段随 weather_client 重新实现时一并解决。

#### 动作 7：天气服务落地

对应天气系统实施计划全文和审计发现 #5（中风险）。

上游：Open-Meteo Forecast API，请求 `current=temperature_2m,weather_code,is_day&timezone=auto`。v1 不做运行时城市搜索，不做多城市。地点配置先从 `sdkconfig` 默认值来，后续允许 NVS 覆盖。

实现拆分：

- `weather_service`：缓存快照、刷新调度（自动 30min、手动节流 60s）、启动时 NVS 恢复、失败保留旧缓存标记 STALE
- `weather_client`：HTTP 请求 + **cJSON 结构化解析**（不再用字符串查找），响应体大小增加显式上限，区分"响应过大"和"结构不匹配"
- `weather_mapper`：`weather_code` → icon_id / text，v1 压到 9 类图标（晴、多云、阴、小雨、大雨、雪、雷暴、雾、未知）

完成标准：上游字段顺序变化不影响解析。响应过长、字段缺失、类型不匹配能被明确区分。Home 页面展示真实天气数据。

#### 动作 8：Claude 服务接线

范围：`service_claude`、`device_link`。

做法：接入 `esp32dash` 的快照/增量协议。发布 `APP_EVENT_DATA_CLAUDE` 事件。

#### 动作 9：行情服务接线

范围：`service_market`。

做法：接入价格与 K 线源数据。发布 `APP_EVENT_DATA_MARKET` 事件。

#### 统一要求

所有 service 统一通过 `APP_EVENT_DATA_*` 事件发布数据变更，app 不直接依赖 service 内部细节。

### 第四阶段：Host 侧加固

Host 侧的问题不阻塞固件推进，但在真实数据流跑起来之前必须修完，否则长时间运行时会出问题。

#### 动作 10：把 `state.json` 改成原子写入

对应审计发现 #6（中风险）。

范围：`tools/esp32dash/src/agent.rs`。

做法：先写临时文件，`fsync` 后再 rename 覆盖正式文件。读取失败时把"文件损坏"单独记日志，不直接静默当作首次启动。

完成标准：agent 异常退出不会留下半截状态文件。状态文件损坏时有明确可见的错误信息。

#### 动作 11：给串口读缓冲和 worker 通道加护栏

对应审计发现 #7 和 #8（中风险）。

范围：`tools/esp32dash/src/device.rs`、`tools/esp32dash/src/compat.rs`。

做法：给 `read_buf` 增加最大行长，超限后丢弃当前帧并记录日志。`send_snapshot()` 不再吞掉 `send()` 错误。`ESP32DASH_SERIAL_BAUD` 解析失败时输出警告。

完成标准：异常串口帧不会导致 host 内存持续增长。worker 失效和配置错误能被操作方看见。

### 第五阶段：MVP UI 收口

数据服务就绪后，把 UI 从占位屏推进到可演示 MVP。

#### 动作 12：四个页面收口到 P1

- Home：时间、日期、Wi-Fi 状态、天气、Claude 未读角标
- Notify：Claude 状态、标题、工作区、更新时间、断线/无数据态
- Trading：价格、涨跌幅、更新时间，保留 P2 图表扩展口
- Satoshi Slot：保持受控占位，延后到 P2

#### 动作 13：全局交互补齐

- 全局翻页 / 分页指示器
- 页面切换与 service 状态更新的协同
- 与 `power_policy` 的前后台刷新策略联动

### 第六阶段：回归与放行

这一步不解决新问题，只负责确认前面的修补和功能真的站住了。

#### 动作 14：构建与回归验证

- 在可用的 ESP-IDF 环境里执行 `idf.py build`
- 保持 `tools/esp32dash` 现有 Rust 测试常态通过
- 回归项：高频事件下页面刷新不死锁、Wi-Fi 断开/重连后系统仍可交互、串口收到超长无换行数据时 host 不膨胀、timezone 修改后 RTC 恢复语义不漂移

#### 放行标准

在进入下一轮功能迭代之前，至少满足：

1. 固件 UI 更新只从单一线程进入 LVGL
2. 显示锁和 flush 路径不再有无限等待
3. 关键后台 task 创建失败会让初始化直接失败
4. RTC 采用 UTC 语义，timezone 只影响展示
5. Host `state.json` 是原子写入
6. 串口读缓冲有上限，worker 异常不会被静默吞掉
7. `idf.py build` 和 Rust 现有测试都能稳定通过
8. Home 页面展示真实天气数据

## 依赖关系

```
第一阶段（固件地基）
    ↓
第二阶段（板级闭环 + RTC）
    ↓
第三阶段（数据服务）──→ 第四阶段（Host 加固）可并行
    ↓
第五阶段（MVP UI）
    ↓
第六阶段（回归放行）
```

`tools/esp32dash` 的加固（第四阶段）可以独立于固件推进，但应在真实数据流端到端跑通前完成。IMU 自动翻转属于 P2 体验增强项，不在本计划范围内。

## 备注

这份行动计划覆盖了从"开发骨架"到"可演示 MVP"的完整路径。它有意没有扩到 CI 搭建、多城市天气、图表渲染等 P2 项，因为那会把优先级打散。当前最值钱的动作序列是：先浇地基，再打闭环，再接数据，最后收口 UI。
