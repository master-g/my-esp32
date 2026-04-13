# Market Service 子设计方案

## 1. 目的

本文档细化 `market_service`，目标是固定：

- 行情数据源抽象
- 多源自动回退
- 价格摘要和 K 线缓存生命周期
- 交易对/周期选择的归属
- 前后台与供电模式下的刷新降级规则

本文档与 [trading.md](../apps/trading.md) 互补：后者关注页面，本文关注 service。

## 2. 设计边界

### 2.1 覆盖范围

- `market_feed`
- `market_cache`
- `market_scheduler`
- `market_service` 对外接口

### 2.2 非覆盖范围

- 下单
- 多交易所聚合
- 高级技术指标

## 3. 数据模型

### 3.1 选择状态

交易对和周期选择由 `market_service` 持有；其中 interval 默认值属于持久化偏好：

```c
typedef struct {
    market_pair_id_t pair;
    market_interval_id_t interval;
} market_selection_t;

typedef struct {
    market_interval_id_t default_interval;
    bool binance_price_colors;
} market_preferences_t;
```

原因：

- 页面切走再回来时可恢复
- 不让页面各自维护一套隐藏状态
- Settings 修改默认周期后，可以直接驱动当前前台 Trading 选择
- Trading 的 price digits 颜色风格也由 market 域偏好统一提供；真正的涨跌方向比较留在 Trading runtime，按上一次显示过的 price 计算

### 3.2 快照模型

service 对外暴露统一快照：

```c
typedef struct {
    market_selection_t selection;
    trading_data_state_t state;
    bool wifi_connected;
    uint32_t summary_updated_at_epoch_s;
    uint32_t chart_updated_at_epoch_s;
    char pair_label[16];
    char price_text[24];
    char change_text[16];
    int32_t change_bp;
    int32_t last_price_scaled;
    bool has_chart_data;
    uint16_t candle_count;
    market_transport_hint_t transport_hint;
    bool binance_price_colors;
} market_snapshot_t;
```

### 3.3 Candle 数据

K 线数据用 copy-out 窗口暴露：

```c
typedef struct {
    uint16_t count;
    market_candle_t candles[MARKET_MAX_CANDLES];
} market_candle_window_t;
```

## 4. 刷新模式

`market_service` 必须只接受 `power_policy` 给出的刷新模式：

- `REALTIME`
- `INTERACTIVE_POLL`
- `BACKGROUND_CACHE`
- `PAUSED`

行为：

- `REALTIME`：当前实现为混合模式；先用 REST 做 bootstrap / candle 历史回填，再由 Binance WebSocket 接管前台 summary 和当前可见 kline。默认 interval 为 `5M`，stream 断开时 chart fallback 也保持更短轮询
- `INTERACTIVE_POLL`：低频轮询；当前 chart cadence 比旧方案更短，避免长时间停在旧 candle
- `BACKGROUND_CACHE`：不主动拉新，只保留缓存
- `PAUSED`：停止前台相关刷新

## 5. 服务内部拆分

| 子模块 | 职责 |
|------|------|
| `market_feed` | 对接上游采集；当前为 `Gate REST + Binance REST + Binance WebSocket` |
| `market_cache` | 保存最近价格摘要与 candle |
| `market_aggregate` | 计算涨跌幅和新鲜度 |
| `market_scheduler` | 根据 `power_policy` 调整刷新模式 |
| `market_provider_manager` | 管理主源/备源、失败计数、回退与回切 |

## 6. 对外接口

```c
esp_err_t market_service_init(void);
void market_service_select_pair(market_pair_id_t pair);
void market_service_get_preferences(market_preferences_t *out);
esp_err_t market_service_set_default_interval(market_interval_id_t interval);
esp_err_t market_service_set_binance_price_colors(bool enabled);
void market_service_get_snapshot(market_snapshot_t *out);
bool market_service_has_chart_data(market_pair_id_t pair, market_interval_id_t interval);
bool market_service_get_candles(
    market_pair_id_t pair,
    market_interval_id_t interval,
    market_candle_window_t *out
);
```

额外约束：

- 页面层不能直接感知 REST / WebSocket 的细节
- 页面层不能直接感知 `Gate / Binance` 的切换细节
- WebSocket 只允许替换 `market_feed` 与部分调度逻辑，不能改变 `market_snapshot_t` / `APP_EVENT_DATA_MARKET` 契约
- `market_cache`、`market_snapshot_t` 和 `APP_EVENT_DATA_MARKET` 事件契约保持不变
- interval 默认值在 market 域内通过 NVS 保存，不经由 Wi-Fi 专用的 `service_settings`
- price digits 的 Binance 绿涨红跌开关也在 market 域内通过 NVS 保存，并通过同一个 market event 驱动 UI 刷新

## 7. 退化策略

- 无缓存：`EMPTY`
- 有缓存但超时：`STALE`
- 上游错误：`ERROR`
- 页面切走：缓存保留，刷新降级

## 8. 验收用例

- 切换交易对后 service 立即切换选择状态
- 在 Settings 修改默认 interval 后，service 立即持久化并切换当前选择
- 在 Settings 切换 Binance 价格颜色后，Trading 页立即刷新 price digits，但不触发额外网络刷新
- 电池模式下 service 自动降级
- 页面返回前台时可先看到缓存，再等待拉新

## 9. 假设

- v1 仍维持单页单选择模型，不做多路并行市场聚合
- 价格格式化由 service 统一完成
- 当前代码线采用 `Gate -> Binance` 的 REST 自动回退，同时在 `REALTIME` 模式下为当前选择接入 Binance WebSocket。由于显示栈会长期占用 internal DRAM，TLS 相关分配继续允许使用 external SPIRAM，避免 REST / WSS 都在 `mbedtls_ssl_setup()` 阶段因 internal heap 碎片而失联。

---

*文档版本: 1.0*
*创建日期: 2026-04-07*
