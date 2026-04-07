# ESP32-S3-Touch-LCD-3.49 智能仪表板设计文档

## 1. 文档目的

本文档用于定义项目的实现合同，而不是仅描述产品愿景。重点回答以下问题：

- 板卡能力与软件范围如何对齐
- 页面、服务、功耗和网络连接如何协同
- Claude Code、市场数据、天气数据分别由谁采集、缓存和展示
- MVP 先做什么，哪些能力明确延期

本文档版本已根据首轮审计结果重写，目标是减少后续实现阶段的返工。

## 2. 项目目标

### 2.1 核心目标

基于 Waveshare ESP32-S3-Touch-LCD-3.49 开发一个三页式竖屏仪表板：

1. `Home`：时间、日期、联网状态、天气摘要
2. `Notify`：Claude Code 最新状态和未读提示
3. `Trading`：BTC/USDT、ETH/USDT、BTC/ETH 的价格与简化行情

### 2.2 非目标

以下内容不纳入当前设计范围：

- 设备侧保存完整 Claude 对话或日志
- 设备侧执行交易、下单或账户操作
- 音频、语音、摄像头等多媒体功能
- 跨公网直接暴露 ESP32 控制接口

## 3. 硬件基线

### 3.1 板卡假设

| 项目 | 基线 |
|------|------|
| MCU | ESP32-S3R8 |
| 内存 | 512KB SRAM + 8MB PSRAM |
| Flash | 16MB |
| 屏幕方向 | 竖屏 |
| 显示接口 | QSPI LCD |
| 触摸接口 | I2C |
| 无线 | 2.4GHz Wi-Fi |
| RTC | PCF85063 |
| IMU | QMI8658 |

### 3.2 分辨率和驱动策略

官方资料存在描述不一致的风险，因此本文档明确采用以下实现基线：

- UI 设计基线分辨率为 `172 x 640`
- 像素格式按 `RGB565` 预算
- 触摸控制器型号不在本设计中写死，统一抽象为 `bsp_touch`
- 显示控制器初始化参数以 Waveshare 官方示例和实测驱动为准，统一抽象为 `bsp_display`

如果后续实测板卡只能稳定运行在其他窗口尺寸或旋转模式，需要重新评审 UI 布局，不允许在实现阶段临时魔改页面。

### 3.3 内存预算约束

按 `172 x 640 x 2` 计算，单全屏缓冲约 `220160 bytes`。因此：

- 不采用常驻双全屏 framebuffer 作为默认方案
- LVGL 默认使用局部双缓冲，建议起始值为 `172 x 80 x 2` 像素块
- 图表、图片和网络 JSON 必须分配预算，不允许各模块各自申请“大块试试看”
- 关键控制路径优先使用内部 SRAM，PSRAM 主要承载图形缓冲和较大数据块

## 4. 产品范围与阶段划分

### 4.1 P0: 板级打通

P0 的目标是把板卡从开发板状态变成“可运行产品底座”：

- 显示驱动可稳定刷新
- 触摸输入可上报坐标和手势方向
- Wi-Fi 连接和重连机制可用
- NTP 同步后写入 RTC，重启后可从 RTC 恢复时间
- LVGL 页面壳子可切换三页
- 串口日志、异常恢复和基本监控可用

### 4.2 P1: 可演示 MVP

P1 只交付“可用但简化”的用户路径：

- `Home` 显示时间、日期、Wi-Fi 状态、天气占位或缓存值
- `Notify` 显示 Claude 最新状态、来源工作区、时间戳
- `Trading` 显示当前交易对最新价、24h 涨跌幅
- 全局翻页、亮灭屏、唤醒、基础功耗策略可用
- Claude 桥接可获取最新状态快照

P1 不包含：

- K 线蜡烛图
- 背景图片
- IMU 抬手唤醒
- 复杂动画

### 4.3 P2: 增强能力

- 天气正式接入并缓存
- 交易页接入蜡烛图
- 电池模式下的差异化采样和刷新策略
- IMU 抬手唤醒
- 背景图和主题

### 4.4 P3: 扩展应用

- Feeling Lucky Today
- 设置页
- 更多桌面小组件

## 5. 总体架构

### 5.1 架构分层

```text
┌──────────────────────────────────────────────┐
│                  Apps                        │
│  home_app  notify_app  trading_app           │
├──────────────────────────────────────────────┤
│                UI Core                       │
│  app_manager  page_router  gesture_router    │
├──────────────────────────────────────────────┤
│              Domain Services                 │
│  time_service    weather_service             │
│  claude_service  market_service              │
│  power_policy    net_manager                 │
├──────────────────────────────────────────────┤
│                 BSP Layer                    │
│  bsp_display  bsp_touch  bsp_rtc  bsp_imu    │
├──────────────────────────────────────────────┤
│              ESP-IDF / LVGL                  │
└──────────────────────────────────────────────┘
```

### 5.2 设计原则

- UI 页面只消费状态，不直接发起 HTTP/WebSocket 请求
- 所有外部数据先进入 service 层，再分发给页面
- 页面切换与数据采集解耦，避免“页面不在前台就丢数据”
- 复杂性向下沉到 service 和 policy 层，不泄漏到 `app_manager`

### 5.3 后台服务职责

| 服务 | 职责 | 是否缓存 |
|------|------|----------|
| `time_service` | NTP 校时、RTC 读写、格式化当前时间 | 是 |
| `weather_service` | 获取天气摘要、缓存上次成功结果 | 是 |
| `claude_service` | 获取 Claude 最新状态快照、未读计数、重连 | 是 |
| `market_service` | 获取交易对价格和 K 线源数据 | 是 |
| `power_policy` | 根据供电、亮屏、前台页决定刷新频率 | 是 |
| `net_manager` | Wi-Fi 连接、断线重连、网络事件广播 | 否 |

## 6. 运行时状态机

### 6.1 显示状态

定义三种显示状态：

- `ACTIVE`：正常亮屏，允许触摸和前台刷新
- `DIM`：降低亮度，保留 UI 上下文
- `SLEEP`：关闭背光，停止高频刷新

状态迁移规则：

| 当前状态 | 触发条件 | 下一状态 |
|----------|----------|----------|
| `ACTIVE` | 用户无操作超时 | `DIM` |
| `DIM` | 再次超时 | `SLEEP` |
| `DIM` | 触摸/IMU/关键通知 | `ACTIVE` |
| `SLEEP` | 触摸/IMU/关键通知 | `ACTIVE` |

说明：

- `Home` 不是严格意义上的 always-on 页面，而是默认首页
- USB 供电时可以延长 `DIM`/`SLEEP` 超时
- 电池供电时应更积极进入 `DIM`/`SLEEP`

### 6.2 数据采集模式

定义三种采集模式：

- `REALTIME`：WebSocket 或秒级前台刷新
- `INTERACTIVE_POLL`：前台低频轮询
- `BACKGROUND_CACHE`：仅维护缓存，不追求实时

采集模式由 `power_policy` 统一决策：

| 条件 | Claude | Market | Weather |
|------|--------|--------|---------|
| USB + Notify 前台 | `REALTIME` | `BACKGROUND_CACHE` | `BACKGROUND_CACHE` |
| USB + Trading 前台 | `BACKGROUND_CACHE` | `REALTIME` | `BACKGROUND_CACHE` |
| USB + Home 前台 | `BACKGROUND_CACHE` | `BACKGROUND_CACHE` | 30 分钟更新 |
| 电池 + Notify 前台 | 快照拉取 + 增量订阅 | 暂停 | 缓存 |
| 电池 + Trading 前台 | 快照或低频轮询 | `INTERACTIVE_POLL` | 缓存 |
| 电池 + 其他状态 | 快照缓存 | 暂停 | 缓存 |

关键约束：

- `Notify` 页不能依赖“前台时才开始采集”来保证状态完整性
- `Trading` 页在电池模式下降级为低频刷新，不维持高频市场流
- 天气始终属于缓存型数据，不进入高频更新路径

## 7. Claude Code 桥接协议

### 7.1 设计目标

桥接服务运行在开发机，负责把 Claude Code hooks 事件转成局域网可消费的状态流。ESP32 只负责展示，不直接解析 Claude 内部行为。

Claude app 的详细子设计见 [2026-04-07-claude-app-subdesign.md](./2026-04-07-claude-app-subdesign.md)。

### 7.2 事件来源

桥接服务优先消费 Claude Code 官方 hooks 事件，例如：

- `Notification`
- `Stop`
- `SubagentStop`

如果未来接入更多 hooks，必须先在桥接层完成语义归一化，再暴露给设备侧。

### 7.3 设备侧只依赖统一协议

桥接输出统一消息结构：

```json
{
  "seq": 1042,
  "source": "claude_code",
  "event": "notification",
  "status": "waiting_for_input",
  "title": "Awaiting confirmation",
  "workspace": "project-foo",
  "detail": "Tool call requires approval",
  "ts": 1743957123,
  "unread": true
}
```

字段约束：

- `seq`：单调递增，用于断线补偿
- `status`：设备 UI 使用的归一化状态，不直接暴露原始 hooks 结构
- `workspace`：工作区名称，可由桥接根据路径推导
- `detail`：可显示的短文本，长度需受控
- `ts`：桥接生成的 Unix 时间戳

### 7.4 交付语义

为避免漏消息，采用“当前快照 + 实时增量”模型：

1. 桥接服务内存中保留最新状态和最新 `seq`
2. ESP32 建立连接时先拿当前快照
3. 建连成功后只接收该快照之后的实时增量
4. 若订阅中断，重新连接后再次拉当前快照，再继续增量

这意味着：

- 即使设备平时不在 `Notify` 页，也不会丢失“最后状态”
- 未读提示和最近更新时间可以从快照恢复
- 页面展示与数据采集完全解耦

### 7.5 安全边界

- 桥接只监听局域网地址，不暴露公网
- 设备连接需带预共享 token
- token 存储在 NVS
- 桥接和设备都需要重连退避

## 8. 市场和天气数据策略

### 8.1 市场数据

P1 只做简化行情：

- 交易对：`BTC/USDT`、`ETH/USDT`、`BTC/ETH`
- 指标：最新价、24h 涨跌幅、最后更新时间

P2 再做蜡烛图：

- 时间周期：`1H`、`4H`、`1D`
- 图表刷新节流到最多 `1 FPS`
- 数据采样和渲染频率分离，避免每个 tick 都触发重绘

### 8.2 天气数据

天气属于低频缓存型数据：

- 正常更新周期为 30 分钟
- 失败后保留上次成功结果并标记“缓存”
- 若网络不可用，`Home` 页面仍显示时间，不因天气接口失败阻塞首页

## 9. UI 和交互设计

### 9.1 页面模型

三页结构固定：

- `Home`
- `Notify`
- `Trading`

三页之间允许全局切换，但页面内部交互不得与全局翻页冲突。

### 9.2 全局翻页规则

全局翻页只允许以下两种方式：

- 屏幕左右边缘区域滑动
- 页脚分页指示器点击

中间内容区不承担全局翻页手势，以避免和页面内部交互冲突。

建议值：

- 左右边缘各 `20 px` 作为全局翻页触发区
- 中间区域手势优先留给当前页面

### 9.3 Home 页面

显示内容：

- 当前时间
- 日期和星期
- Wi-Fi 状态
- 天气摘要
- Claude 未读点或状态角标

行为：

- 点击天气区域可触发手动刷新
- 不承载复杂手势

### 9.4 Notify 页面

显示内容：

- 当前 Claude 状态图标
- 标题
- 工作区名
- 详细短文本
- 更新时间

行为：

- 页面进入时先拉取快照
- 若收到新状态，短暂高亮但不强制切页
- 保留“无数据 / 网络断开 / 桥接不可达”三种显式状态

### 9.5 Trading 页面

显示内容：

- 当前交易对
- 最新价
- 24h 涨跌幅
- 最后更新时间
- P2 后增加 K 线区域

交互规则：

- 交易对切换使用顶部 tab 或显式左右按钮
- 时间周期切换使用底部按钮
- 不使用内容区左右滑动切换交易对

该规则用于消除与全局翻页的冲突。

## 10. 应用框架设计

### 10.1 事件驱动接口

应用层采用事件驱动，而不是零散的 `show/hide/on_notify` 回调：

```c
typedef enum {
    APP_EVENT_ENTER,
    APP_EVENT_LEAVE,
    APP_EVENT_TICK_1S,
    APP_EVENT_TOUCH,
    APP_EVENT_NET_CHANGED,
    APP_EVENT_POWER_CHANGED,
    APP_EVENT_DATA_CLAUDE,
    APP_EVENT_DATA_MARKET,
    APP_EVENT_DATA_WEATHER,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    void *payload;
} app_event_t;

typedef struct {
    const char *id;
    esp_err_t (*init)(void);
    void (*resume)(void);
    void (*suspend)(void);
    void (*handle_event)(const app_event_t *event);
    lv_obj_t *(*create_root)(lv_obj_t *parent);
} app_descriptor_t;
```

### 10.2 设计原因

这样做的目的：

- 把网络、功耗、时间、数据更新统一建模成事件
- 避免 `app_manager` 内出现大量布尔分支和特判
- 后续增加应用时，不需要再为每类数据新加专用回调

## 11. 推荐目录结构

```text
project/
├── main/
│   ├── main.c
│   ├── app_manager.c
│   ├── app_manager.h
│   ├── system_state.c
│   ├── system_state.h
│   ├── event_bus.c
│   └── event_bus.h
│
├── components/
│   ├── bsp_board/
│   │   ├── bsp_display.c
│   │   ├── bsp_touch.c
│   │   ├── bsp_rtc.c
│   │   └── bsp_imu.c
│   ├── service_time/
│   ├── service_weather/
│   ├── service_claude/
│   ├── service_market/
│   ├── net_manager/
│   └── power_policy/
│
├── apps/
│   ├── home/
│   ├── notify/
│   └── trading/
│
├── tools/
│   └── claude_bridge/
│       ├── hook_handler.py
│       ├── ws_server.py
│       └── README.md
│
├── sdkconfig.defaults
├── CMakeLists.txt
└── README.md
```

约束：

- 驱动代码不放进 app 目录
- 外部协议解析不放进页面文件
- 页面文件只负责布局、绑定和轻量状态转换

## 12. 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 显示驱动初始化复杂 | 高 | 先完成 P0 板级打通，再接业务页面 |
| 分辨率和触控型号描述不一致 | 高 | 以 Waveshare 示例和实测为准，抽象统一 BSP |
| Claude hooks 语义不足以直接驱动 UI | 高 | 在桥接层做状态归一化，不让设备依赖原始事件 |
| 电池模式下实时行情过于耗电 | 中 | 前台低频轮询，后台暂停 |
| K 线渲染占用内存和 CPU | 中 | P2 再做，先做价格摘要 |
| 网络不稳定导致页面卡死 | 中 | service 层缓存最近一次成功结果，UI 永远可降级显示 |

## 13. 验收标准

### 13.1 P1 验收

- 开机后 3 秒内进入首页
- Wi-Fi 已配置时，30 秒内完成联网和校时
- 三页可稳定切换，无明显误触冲突
- `Notify` 页可在断线重连后恢复最近一次 Claude 状态
- `Trading` 页可显示至少一个交易对的当前价格和更新时间
- 电池模式下设备能够进入 `DIM` 和 `SLEEP`

### 13.2 P2 验收

- 天气缓存策略生效
- K 线图切换周期时无明显卡顿
- IMU 抬手唤醒可靠率达到可接受水平

## 14. 参考资料

- Waveshare Wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.49
- Claude Code hooks: https://docs.anthropic.com/en/docs/claude-code/hooks
- Binance Spot API: https://binance-docs.github.io/apidocs/spot/en/
- OpenWeatherMap API: https://openweathermap.org/api

---

*文档版本: 2.0*  
*修订日期: 2026-04-07*
