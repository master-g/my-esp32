# Market Service 子设计方案

## 1. 目的

本文档细化 `market_service`，目标是固定：

- 行情数据源抽象
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

交易对和周期选择由 `market_service` 持有：

```c
typedef struct {
    market_pair_id_t pair;
    market_interval_id_t interval;
} market_selection_t;
```

原因：

- 页面切走再回来时可恢复
- 不让页面各自维护一套隐藏状态

### 3.2 快照模型

service 对外暴露统一快照：

```c
typedef struct {
    market_selection_t selection;
    trading_data_state_t state;
    bool wifi_connected;
    uint32_t updated_at_epoch_s;
    char pair_label[16];
    char price_text[24];
    char change_text[16];
    int32_t change_bp;
    bool has_chart_data;
} market_snapshot_t;
```

### 3.3 Candle 数据

K 线数据用只读窗口暴露：

```c
const market_candle_t *market_service_get_candles(
    market_pair_id_t pair,
    market_interval_id_t interval,
    uint16_t *count
);
```

## 4. 刷新模式

`market_service` 必须只接受 `power_policy` 给出的刷新模式：

- `REALTIME`
- `INTERACTIVE_POLL`
- `BACKGROUND_CACHE`
- `PAUSED`

行为：

- `REALTIME`：保持实时 feed
- `INTERACTIVE_POLL`：低频轮询
- `BACKGROUND_CACHE`：不主动拉新，只保留缓存
- `PAUSED`：停止前台相关刷新

## 5. 服务内部拆分

| 子模块 | 职责 |
|------|------|
| `market_feed` | 对接上游 feed 或 REST |
| `market_cache` | 保存最近价格摘要与 candle |
| `market_aggregate` | 计算涨跌幅和新鲜度 |
| `market_scheduler` | 根据 `power_policy` 调整刷新模式 |

## 6. 对外接口

```c
esp_err_t market_service_init(void);
void market_service_select_pair(market_pair_id_t pair);
void market_service_select_interval(market_interval_id_t interval);
void market_service_on_refresh_mode_changed(refresh_mode_t mode);
const market_snapshot_t *market_service_get_snapshot(void);
```

## 7. 退化策略

- 无缓存：`EMPTY`
- 有缓存但超时：`STALE`
- 上游错误：`ERROR`
- 页面切走：缓存保留，刷新降级

## 8. 验收用例

- 切换交易对后 service 立即切换选择状态
- 电池模式下 service 自动降级
- 页面返回前台时可先看到缓存，再等待拉新

## 9. 假设

- v1 只对接单一市场源
- 价格格式化由 service 统一完成

---

*文档版本: 1.0*  
*创建日期: 2026-04-07*
