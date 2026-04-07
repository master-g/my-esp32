# Trading 页面子设计方案

## 1. 目的

本文档细化主设计文档中的 `Trading` 页面，目标是把以下问题定死：

- `market_service` 对页面到底提供什么数据
- P1 价格摘要和 P2 K 线分别怎么组织
- 交易对、周期、缓存、刷新和功耗如何协同
- 网络断开、数据陈旧、前后台切换时页面如何退化

本文档是 [2025-04-06-esp32-dashboard-design.md](./2025-04-06-esp32-dashboard-design.md) 中 `Trading` 页面的补充说明。

## 2. 设计边界

### 2.1 覆盖范围

- `Trading` 页面状态模型
- `market_service` 的页面输入输出
- 交易对和周期切换
- P1 价格摘要展示
- P2 K 线数据与渲染约束

### 2.2 非覆盖范围

- 下单、交易、账户管理
- 多市场、多交易所聚合
- 复杂技术指标系统
- 历史回测或策略引擎

## 3. 市场模型

### 3.1 固定交易对

v1 固定支持三组交易对：

- `BTC/USDT`
- `ETH/USDT`
- `BTC/ETH`

定义为：

```c
typedef enum {
    MARKET_PAIR_BTC_USDT = 0,
    MARKET_PAIR_ETH_USDT,
    MARKET_PAIR_BTC_ETH,
} market_pair_id_t;
```

### 3.2 固定周期

P1 页面只显示价格摘要，但数据模型从一开始就保留周期枚举：

```c
typedef enum {
    MARKET_INTERVAL_1H = 0,
    MARKET_INTERVAL_4H,
    MARKET_INTERVAL_1D,
} market_interval_id_t;
```

原因：

- P2 做 K 线时不需要再改页面状态机
- 顶部交易对切换和底部周期切换可保持同一交互模型

## 4. 服务拆分

`Trading` 页面不直接访问 HTTP/WebSocket，而是只依赖 `market_service`。

`market_service` 内部建议拆成：

| 子模块 | 职责 |
|------|------|
| `market_feed` | 管理上游连接、轮询和重连 |
| `market_cache` | 保存最近一次价格摘要和可选 K 线缓存 |
| `market_aggregate` | 计算涨跌幅、时间戳、新鲜度 |
| `market_scheduler` | 根据 `power_policy` 决定刷新模式 |

关键原则：

- 页面不感知 Binance WebSocket / REST 的细节
- 页面只消费聚合后的快照
- 周期切换不直接触发页面内网络逻辑

## 5. 页面状态模型

### 5.1 页面选择状态

```c
typedef struct {
    market_pair_id_t pair;
    market_interval_id_t interval;
} trading_selection_t;
```

### 5.2 数据状态

```c
typedef enum {
    TRADING_DATA_EMPTY = 0,
    TRADING_DATA_LIVE,
    TRADING_DATA_STALE,
    TRADING_DATA_LOADING,
    TRADING_DATA_ERROR,
} trading_data_state_t;
```

### 5.3 页面快照

```c
typedef struct {
    trading_selection_t selection;
    trading_data_state_t state;
    bool wifi_connected;
    bool has_chart;
    uint32_t updated_at_epoch_s;
    char pair_label[16];
    char price_text[24];
    char change_text[16];
    int32_t change_bp;
    uint32_t last_price_scaled;
    uint16_t candle_count;
} trading_snapshot_t;
```

说明：

- `price_text` 和 `change_text` 已经格式化，页面不再拼接
- `change_bp` 保留数值，便于颜色和箭头判断
- `has_chart` 用于区分 P1 和 P2 能力

## 6. P1 价格摘要设计

P1 仅显示：

- 当前交易对
- 当前价格
- 24h 涨跌幅
- 最近更新时间

### 6.1 新鲜度规则

价格摘要的新鲜度固定为：

- `LIVE`：最近更新在 `30 s` 内
- `STALE`：超过 `30 s` 但仍有缓存
- `EMPTY`：从未拿到数据
- `ERROR`：服务明确报告错误

### 6.2 展示规则

- `LIVE`：正常显示价格和涨跌颜色
- `STALE`：正常显示缓存值，但标注 `cached`
- `EMPTY`：价格区显示 `--`
- `ERROR`：显示 `feed unavailable`

## 7. P2 K 线设计

### 7.1 数据模型

单根 candle 定义为：

```c
typedef struct {
    uint32_t open_time_epoch_s;
    int32_t open_scaled;
    int32_t high_scaled;
    int32_t low_scaled;
    int32_t close_scaled;
    uint32_t volume_scaled;
} market_candle_t;
```

### 7.2 页面约束

P2 页面最多渲染最近 `32` 根 candle。

约束：

- 图表刷新最多 `1 FPS`
- 周期变化时先复用缓存，再等待新数据
- K 线数据由 service 层维护，不在页面中长期持有副本

### 7.3 缩放策略

K 线图只支持自动缩放：

- Y 轴范围由当前窗口内最高/最低值计算
- 页面不支持手势缩放或平移

原因：

- 屏幕宽度有限
- 交互复杂度和收益不成比例

## 8. 刷新与功耗策略

### 8.1 USB 模式

- 前台 `Trading` 页：优先实时更新
- 可使用 WebSocket 或秒级刷新
- K 线数据按节流频率重绘

### 8.2 电池模式

- 前台 `Trading` 页：降级为低频轮询
- 若已有缓存，优先显示缓存，不因短暂断网清空页面
- 页面离开前台后，停止高频刷新

### 8.3 页面切换

页面切走时：

- 不重置当前 pair / interval 选择
- 不清空缓存
- 只降低刷新频率

重新回到页面时：

- 立即显示最近缓存
- 再由 `market_service` 决定是否拉新

## 9. 交互设计

### 9.1 顶部交易对切换

顶部使用 tab 或显式按钮切换：

- `BTC/USDT`
- `ETH/USDT`
- `BTC/ETH`

切换后：

1. 立即更新页面选择状态
2. 先显示对应缓存
3. 再等待 service 更新

### 9.2 底部周期切换

底部按钮切换：

- `1H`
- `4H`
- `1D`

P1 即使没有 K 线，也保留该选择模型，但可视上弱化或禁用。

### 9.3 禁止的交互

- 内容区左右滑动切换交易对
- 图表内拖拽平移
- 双指缩放

## 10. 页面布局

页面固定分为四区：

1. 顶部交易对区
2. 中部价格区
3. 图表区或占位区
4. 底部周期区

布局原则：

- 价格是第一视觉焦点
- 图表区在 P1 可退化成占位，不影响价格阅读
- 交易对和周期切换控件必须足够清晰，不与全局翻页冲突

## 11. 对外接口

`Trading` 页面只依赖以下接口：

```c
const trading_snapshot_t *market_service_get_snapshot(void);
void market_service_select_pair(market_pair_id_t pair);
void market_service_select_interval(market_interval_id_t interval);
bool market_service_has_chart_data(market_pair_id_t pair, market_interval_id_t interval);
const market_candle_t *market_service_get_candles(
    market_pair_id_t pair,
    market_interval_id_t interval,
    uint16_t *count
);
```

设计原则：

- 页面只读快照和只读 candle 视图
- 所有选择变更都通过 service 接口完成
- 页面不维护自己的隐藏数据缓存

## 12. 错误与退化处理

### 12.1 无缓存

- 显示 `--`
- 图表区显示空占位
- 不崩溃、不闪屏

### 12.2 有缓存但断网

- 保留最近价格和图表
- 标注 `cached`
- 更新时间继续可见

### 12.3 上游返回错误

- 页面进入 `ERROR`
- 若有缓存则继续显示缓存
- 若无缓存则显示占位和错误文案

## 13. 验收用例

### 13.1 P1

- 交易对切换后页面能立即显示对应缓存或占位
- 前台时价格摘要按策略刷新
- 电池模式下降级后页面仍然可用

### 13.2 P2

- 周期切换后可显示对应 K 线缓存
- 图表刷新不超过 `1 FPS`
- 网络波动时页面可退化为缓存显示

## 14. 假设

- v1 只有单一市场源，不做多源校验
- 价格格式化和涨跌颜色规则在 `market_service` 内统一
- 图表渲染由页面层完成，但数据生命周期由 `market_service` 管理

---

*文档版本: 1.0*  
*创建日期: 2026-04-07*
