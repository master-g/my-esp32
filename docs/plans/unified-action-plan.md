# 统一行动计划

从现状出发、按优先级排列的完整行动路线。合并自审计报告、整改方案、进度评估与天气计划。

## 现状

### 已经站住的部分

项目已越过纯设计阶段。ESP-IDF 工程结构、组件分层、BSP、服务骨架、四个 app 槽位都已落地。横屏显示基线已稳定。Host 侧 `esp32dash` 是独立 Rust 项目，18 个测试通过。

2026-04-09 审计暴露了 3 个高风险和 5 个中风险问题，第一阶段加固已全部完成（见下文）。

### 还没站住的部分

板级闭环的异常路径（断线重连、背光 dim/sleep 端到端、触摸异常）尚未回归。`service_weather`、`service_market`、`service_claude` 仍是占位。Notify、Trading、Satoshi Slot 页面仍是空壳。全局翻页、边缘手势、分页指示器未落地。IMU 未接入。

## 行动路线

### ~~第一阶段：固件地基加固~~ ✅ 已完成（2026-04-09）

四个动作全部落地并通过编译验证。改动覆盖 27 个源文件（+1007 / -120 行）。

动作 1（状态边界）：8 个 service 的 getter 从指针返回改为 copy-out（`void get(X *out)`），6 个 service 加了 FreeRTOS mutex，event payload 统一设为 NULL。动作 2（UI 单线程）：app_manager 引入 16 深 FreeRTOS queue，事件先入队再由 LVGL task 统一消费，app 的 resume/handle_event 不再自己拿显示锁。动作 3（有限超时）：DMA flush 等待从 portMAX_DELAY 改为 1000ms + error recovery，LVGL task 拿锁从 UINT32_MAX 改为 100ms。动作 4（task 创建检查）：5 个文件、6 个 xTaskCreatePinnedToCore 调用点加了返回值检查。

### 第二阶段：板级闭环与时间语义（部分完成）

#### 动作 5：P0 板级打通验证

范围：实机全链路。验证项：LCD 刷新稳定性、触摸坐标读数、RTC 读写恢复、Wi-Fi 联网与断线重连、NTP 校时写回 RTC、`power_policy` 对亮度/dim/sleep 的实际驱动效果。大部分已通过串口日志确认，背光 dim/sleep 端到端仍需回归。

#### ~~动作 6：把 RTC 统一成 UTC 语义~~ ✅ 已完成（2026-04-09）

`bsp_rtc.c` 的 `localtime_r`→`gmtime_r`、`mktime`→`timegm`。RTC 芯片现在存储 UTC calendar，timezone 变更不影响 epoch 恢复。

### 第三阶段：数据服务接线

板级闭环稳定后，补齐真实数据链路。

#### ~~动作 7：天气服务落地~~ ✅ 已就绪（2026-04-09）

审查确认：weather_client 已用 cJSON 结构化解析（非 strstr），4KB 响应上限 + overflow 检测已到位，worker task + event bus 接线完整，Home 页面消费路径畅通。Kconfig 默认配置上海坐标。需要实机联网验证。

#### 动作 8：Claude 服务接线

范围：`service_claude`、`device_link`。

做法：接入 `esp32dash` 的快照/增量协议。发布 `APP_EVENT_DATA_CLAUDE` 事件。

#### 动作 9：行情服务接线

范围：`service_market`。

做法：接入价格与 K 线源数据。发布 `APP_EVENT_DATA_MARKET` 事件。

#### 统一要求

所有 service 统一通过 `APP_EVENT_DATA_*` 事件发布数据变更，app 不直接依赖 service 内部细节。

### ~~第四阶段：Host 侧加固~~ ✅ 已完成（2026-04-09）

两个动作全部落地，18 个 Rust 测试通过。

动作 10（state.json 原子写入）：agent.rs 的 `persist_state` 改为 write→fsync→rename 三步，`load_state` 区分文件不存在和损坏并输出 tracing::warn。动作 11（串口缓冲护栏）：device.rs 的 `read_buf` 加 4KB 上限超限丢弃，`send_snapshot` 不再吞 send 错误，compat.rs 的 baud rate 解析失败输出明确警告。

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

1. ~~固件 UI 更新只从单一线程进入 LVGL~~ ✅
2. ~~显示锁和 flush 路径不再有无限等待~~ ✅
3. ~~关键后台 task 创建失败会让初始化直接失败~~ ✅
4. ~~RTC 采用 UTC 语义，timezone 只影响展示~~ ✅
5. ~~Host `state.json` 是原子写入~~ ✅
6. ~~串口读缓冲有上限，worker 异常不会被静默吞掉~~ ✅
7. `idf.py build` 和 Rust 现有测试都能稳定通过
8. Home 页面展示真实天气数据（代码就绪，待实机验证）

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
