# Weather Service 子设计方案

## 1. 目的

本文档细化 `weather_service`，目标是固定：

- 天气缓存模型
- 自动刷新与手动刷新节流
- 对首页输出的快照格式

## 2. 设计边界

### 2.1 覆盖范围

- 上游天气 API 抽象
- 缓存状态
- 刷新节流

### 2.2 非覆盖范围

- 多城市管理
- 历史天气记录

## 3. 状态模型

```c
typedef enum {
    WEATHER_EMPTY = 0,
    WEATHER_LIVE,
    WEATHER_STALE,
    WEATHER_REFRESHING,
    WEATHER_ERROR,
} weather_state_t;
```

## 4. 快照模型

```c
typedef struct {
    weather_state_t state;
    uint32_t updated_at_epoch_s;
    char city[24];
    char text[24];
    int16_t temperature_c_tenths;
    uint8_t icon_id;
} weather_snapshot_t;
```

## 5. 刷新策略

- 自动刷新周期：`30 min`
- 手动刷新节流：`60 s`
- 失败后保留旧缓存并标记 `STALE`

## 6. 对外接口

```c
esp_err_t weather_service_init(void);
void weather_service_request_refresh(void);
bool weather_service_can_refresh(void);
const weather_snapshot_t *weather_service_get_snapshot(void);
```

## 7. 退化规则

- 无缓存：`EMPTY`
- 有缓存但过期：`STALE`
- 无网：保留缓存，不清空页面

## 8. 验收用例

- 自动刷新后快照更新
- 手动刷新在节流窗口内被拒绝
- 无网时继续返回旧缓存

## 9. 假设

- v1 只有单一上游 API
- 首页只消费聚合后的天气快照

---

*文档版本: 1.0*  
*创建日期: 2026-04-07*
