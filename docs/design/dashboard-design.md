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

基于 Waveshare ESP32-S3-Touch-LCD-3.49 开发一个三页式横屏仪表板，其中包含两个核心页和一个 BTC 应用页：

1. `Home`：时间、日期、联网状态、天气摘要，以及 Claude Code 状态与未读提示
2. `Trading`：BTC/USDT、ETH/USDT、BTC/ETH 的价格与简化行情
3. `Satoshi Slot`：随机生成 BTC 私钥并与目标地址指纹集合进行匹配的趣味页

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
| 屏幕方向 | 物理面板竖屏，运行时 UI 横屏 |
| 显示接口 | QSPI LCD |
| 触摸接口 | I2C |
| 无线 | 2.4GHz Wi-Fi |
| RTC | PCF85063 |
| IMU | QMI8658 |

### 3.2 分辨率和驱动策略

官方资料存在描述不一致的风险，因此本文档明确采用以下实现基线：

- 物理面板分辨率为 `172 x 640`
- UI 逻辑设计基线分辨率为 `640 x 172`
- 当前固件支持两个固定 landscape 方向，默认使用翻转后的 landscape 方向
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
- LVGL 页面壳子可切换三个应用槽位
- 串口日志、异常恢复和基本监控可用

### 4.2 P1: 可演示 MVP

P1 只交付“可用但简化”的用户路径：

- `Home` 显示时间、日期、Wi-Fi 状态、天气占位或缓存值
- `Home` 同时显示 Claude 最新状态、未读提示和审批入口
- `Trading` 显示当前交易对最新价、24h 涨跌幅
- 全局翻页、亮灭屏、唤醒、基础功耗策略可用
- Claude 桥接可获取最新状态快照

P1 不包含：

- K 线蜡烛图
- 背景图片
- IMU 抬手唤醒
- 复杂动画
- `Satoshi Slot`

### 4.3 P2: 增强能力

- 天气正式接入并缓存
- 交易页接入蜡烛图
- `Satoshi Slot` 页面
- 电池模式下的差异化采样和刷新策略
- IMU 抬手唤醒
- IMU 自动翻转
- 背景图和主题

### 4.4 P3: 扩展应用

- 设置页
- 更多桌面小组件

## 5. 总体架构

### 5.1 架构分层

```text
┌──────────────────────────────────────────────┐
│                  Apps                        │
│  home_app  notify_app  trading_app           │
│  satoshi_slot_app                            │
├──────────────────────────────────────────────┤
│                UI Core                       │
│  app_manager  page_router  gesture_router    │
├──────────────────────────────────────────────┤
│              Domain Services                 │
│  time_service    weather_service             │
│  claude_service  market_service              │
│  bitcoin_service power_policy                │
│  net_manager                                 │
├──────────────────────────────────────────────┤
│                 BSP Layer                    │
│  bsp_display  bsp_touch  bsp_rtc  bsp_imu    │
├──────────────────────────────────────────────┤
│              ESP-IDF / LVGL                  │
└──────────────────────────────────────────────┘
```

### 5.2 设计原则

- UI 页面只消费状态，不直接发起 HTTP、USB serial 或其他传输请求
- 所有外部数据先进入 service 层，再分发给页面
- 页面切换与数据采集解耦，避免“页面不在前台就丢数据”
- 复杂性向下沉到 service 和 policy 层，不泄漏到 `app_manager`

`time_service`、`weather_service`、`market_service`、`power_policy` 和 `app_manager/event_bus` 的详细子设计分别见：

- [time-service.md](./services/time-service.md)
- [weather-service.md](./services/weather-service.md)
- [market-service.md](./services/market-service.md)
- [power-policy.md](./system/power-policy.md)
- [app-manager-event-bus.md](./system/app-manager-event-bus.md)

### 5.3 后台服务职责

| 服务 | 职责 | 是否缓存 |
|------|------|----------|
| `time_service` | NTP 校时、RTC 读写、格式化当前时间 | 是 |
| `weather_service` | 获取天气摘要、缓存上次成功结果 | 是 |
| `claude_service` | 获取 Claude 最新状态快照、未读计数、重连 | 是 |
| `market_service` | 获取交易对价格和 K 线源数据 | 是 |
| `bitcoin_service` | BTC 私钥生成、地址指纹比对、哈希核心算法、自检向量 | 否 |
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
- USB 供电时，v1 默认保持 `ACTIVE`，不自动进入 `DIM`/`SLEEP`
- 电池供电时应更积极进入 `DIM`/`SLEEP`

### 6.2 数据采集模式

定义三种采集模式：

- `REALTIME`：串口推送或秒级前台刷新
- `INTERACTIVE_POLL`：前台低频轮询
- `BACKGROUND_CACHE`：仅维护缓存，不追求实时

采集模式由 `power_policy` 统一决策：

| 条件 | Claude | Market | Weather |
|------|--------|--------|---------|
| USB + Trading 前台 | `BACKGROUND_CACHE` | `REALTIME` | `BACKGROUND_CACHE` |
| USB + Home 前台 | `REALTIME` | `BACKGROUND_CACHE` | 30 分钟更新 |
| 电池 + Trading 前台 | 快照或低频轮询 | `INTERACTIVE_POLL` | 缓存 |
| 电池 + Home 前台 | 快照拉取 + 增量订阅 | 暂停 | 缓存 |
| 电池 + 其他状态 | 快照缓存 | 暂停 | 缓存 |

关键约束：

- Home 内的 Claude 区域不能依赖“前台时才开始采集”来保证状态完整性
- `Trading` 页在电池模式下降级为低频刷新，不维持高频市场流
- 天气始终属于缓存型数据，不进入高频更新路径

### 6.3 计算密集型页面策略

`Satoshi Slot` 属于本地高计算负载页面，必须受 `power_policy` 统一约束：

| 条件 | Satoshi Slot |
|------|--------------|
| USB + 前台 | 连续批量运行 |
| 电池 + 前台 | 降级为 burst 模式 |
| `DIM` / `SLEEP` | 停止 |
| 温度/掉压异常 | 停止并提示 |

关键约束：

- `Satoshi Slot` 必须支持显式开始/暂停，不允许后台偷偷持续扫描
- 计算过程不能阻塞 UI 主线程

## 7. Claude Code 桥接协议

### 7.1 设计目标

`esp32dash` 运行在开发机，负责把 Claude Code hooks 事件转成设备可消费的状态流，并通过 USB serial 把当前状态和配置 RPC 送到 ESP32。ESP32 只负责展示和管理，不直接解析 Claude 内部行为。

Claude app 的详细子设计见 [claude-app.md](./apps/claude-app.md)。

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

为避免漏状态，采用“agent 保留当前快照 + 串口推送完整快照”模型：

1. `esp32dash` 内存中保留最新状态和最新 `seq`
2. ESP32 打开 USB serial 会话后先发送 `hello`
3. host 识别设备能力后，把当前 Claude snapshot 作为 `claude.update` 事件发送
4. 后续只在状态发生实质变化时再发送新的完整 snapshot

这意味着：

- 即使设备平时不在 `Home` 页，也不会丢失“最后状态”
- 未读提示和最近更新时间可以从快照恢复
- 页面展示与数据采集完全解耦

### 7.5 安全边界

- agent 的 admin HTTP 只监听本机 loopback 地址，不暴露公网
- host 和设备之间的管理链路默认走本地 USB serial，不依赖局域网
- 设备通过 `hello` 声明 `protocol_version` 和 `capabilities`
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

### 8.3 BTC 趣味应用策略

#### Satoshi Slot

`Satoshi Slot` 是“随机私钥 + 指纹比对”的趣味页，不是现实可行的密钥恢复方案。

详细子设计见 [satoshi-slot.md](./apps/satoshi-slot.md)。

设计边界：

- 页面随机生成 secp256k1 私钥，派生公钥和地址指纹
- 设备只和本地预置的“目标地址指纹集合”做比对，不依赖联网
- 目标集合应抽象成 `hash160` 或等价短指纹，不在 UI 层硬编码一组地址字符串
- 命中概率在现实中近乎为零，因此页面必须内置 `self-test / forced-hit` 模式，用于验证保存和提醒链路

命中后的动作：

- 立即暂停扫描
- 亮屏并弹出全屏告警
- 将命中记录保存到受保护存储
- 若启用了 `claude_bridge` 或其他通知桥接，可额外发送主机提醒

安全要求：

- 只在启用了安全存储能力时允许落盘保存原始私钥
- 若安全存储不可用，页面应降级为“仅演示 / 仅自检”模式

## 9. UI 和交互设计

### 9.1 页面模型

三页结构固定：

- `Home`
- `Trading`
- `Satoshi Slot`

页面之间允许全局切换，但页面内部交互不得与全局翻页冲突。

### 9.2 全局翻页规则

全局翻页只允许以下两种方式：

- 屏幕左右边缘区域滑动
- 页脚分页指示器点击

中间内容区不承担全局翻页手势，以避免和页面内部交互冲突。

建议值：

- 左右边缘各 `20 px` 作为全局翻页触发区
- 中间区域手势优先留给当前页面

### 9.3 Home 页面

详细子设计见 [home.md](./apps/home.md)。

显示内容：

- 当前时间
- 日期和星期
- Wi-Fi 状态
- 天气摘要
- Claude 未读点、状态角标和审批弹层入口

行为：

- 点击天气区域可触发手动刷新
- 不承载复杂手势

### 9.4 Claude 状态入口

Claude 状态不再单列为独立页面，而是并入 `Home`：

- Claude 未读点、运行状态和短文本提示直接显示在首页
- 审批请求在 `Home` 上以全屏 overlay 呈现
- 页面切换不再承担“进入 Claude 专页才开始看状态”的职责

### 9.5 Trading 页面

详细子设计见 [trading.md](./apps/trading.md)。

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

### 9.6 Satoshi Slot 页面

显示内容：

- 当前运行状态：`idle / running / hit / self-test`
- 已尝试次数
- 当前批次速度：`keys/s`
- 最近一次生成的短地址指纹
- 目标集合标签：`Satoshi candidate set`

交互规则：

- `Start / Pause`
- `Self-test`，用于注入一次可验证命中
- `Reset counter`

行为约束：

- 页面进入时默认不自动开始
- 只在前台运行
- 一旦命中或进入自检命中态，必须暂停并等待用户确认

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
    APP_EVENT_DATA_BITCOIN,
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
├── src/
│   ├── main/
│   │   ├── main.c
│   │   ├── bootstrap.c
│   │   ├── bootstrap.h
│   │   └── CMakeLists.txt
│   ├── components/
│   │   ├── bsp_board/
│   │   │   ├── include/
│   │   │   └── src/
│   │   ├── service_time/
│   │   ├── service_weather/
│   │   ├── service_claude/
│   │   ├── service_market/
│   │   ├── net_manager/
│   │   ├── power_policy/
│   │   └── power_runtime/
│   └── apps/
│       ├── app_home/
│       ├── app_trading/
│       └── app_satoshi_slot/
│
├── tools/
│   ├── claude_bridge/
│   │   ├── Cargo.toml
│   │   ├── src/
│   │   ├── hooks/
│   │   ├── rust-toolchain.toml
│   │   └── README.md
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
| 用户对 “Satoshi 私钥命中” 存在不现实预期 | 高 | 文档和 UI 明确标注为趣味页，提供 self-test 而非承诺实际命中 |
| 命中后原始私钥保存涉及安全风险 | 高 | 只有安全存储可用时才允许持久化，否则降级为演示模式 |
| K 线渲染占用内存和 CPU | 中 | P2 再做，先做价格摘要 |
| 网络不稳定导致页面卡死 | 中 | service 层缓存最近一次成功结果，UI 永远可降级显示 |

## 13. 验收标准

### 13.1 P1 验收

- 开机后 3 秒内进入首页
- Wi-Fi 已配置时，30 秒内完成联网和校时
- 三个应用槽位可稳定切换，无明显误触冲突
- `Home` 页可在断线重连后恢复最近一次 Claude 状态
- `Trading` 页可显示至少一个交易对的当前价格和更新时间
- 电池模式下设备能够进入 `DIM` 和 `SLEEP`

### 13.2 P2 验收

- 天气缓存策略生效
- K 线图切换周期时无明显卡顿
- `Satoshi Slot` 可以通过 self-test 验证命中保存和提醒链路
- IMU 抬手唤醒可靠率达到可接受水平

## 14. 参考资料

- Waveshare Wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.49
- Claude Code hooks: https://docs.anthropic.com/en/docs/claude-code/hooks
- Binance Spot API: https://binance-docs.github.io/apidocs/spot/en/
- OpenWeatherMap API: https://openweathermap.org/api
- secp256k1: https://github.com/bitcoin-core/secp256k1

---

*文档版本: 2.4*
*修订日期: 2026-04-07*
