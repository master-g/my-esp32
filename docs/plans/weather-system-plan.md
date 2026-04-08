# 天气系统实施计划

## 目标

为 `Home` 页面提供一个稳定、低功耗、可缓存的天气摘要能力，v1 只覆盖：

- 城市名
- 当前温度
- 简短天气文本
- 基础图标映射

## 推荐上游

v1 推荐使用 Open-Meteo 官方接口：

- Forecast API: https://open-meteo.com/en/docs
- Geocoding API: https://open-meteo.com/en/docs/geocoding-api

选择理由：

- 接口简单，适合单地点摘要
- 支持 `timezone=auto`
- 不要求设备端先引入额外鉴权流

## v1 范围

v1 不做运行时城市搜索，也不做多城市。

设备侧只持有单地点配置：

```c
typedef struct {
    double latitude;
    double longitude;
    char timezone[32];
    char city_label[24];
} weather_location_config_t;
```

地点配置建议来源：

1. `sdkconfig` 默认值
2. 后续再允许 NVS 覆盖

## 请求模型

首页摘要只请求当前天气：

```text
GET /v1/forecast?latitude=31.23&longitude=121.47&current=temperature_2m,weather_code,is_day&timezone=auto
```

v1 不建议同时接入 hourly / daily / alerts。

## 设备侧实现拆分

建议拆成三层：

1. `weather_service`
   - 缓存快照
   - 刷新调度
   - 状态机
2. `weather_client`
   - HTTP 请求
   - JSON 解析
3. `weather_mapper`
   - `weather_code -> icon_id / text`

## 缓存策略

- 自动刷新：`30 min`
- 手动刷新节流：`60 s`
- 启动时先从 NVS 恢复上次成功快照
- 失败后保留旧缓存，标记为 `STALE`

## 图标映射

v1 先压成少量本地图标：

- 晴
- 多云
- 阴
- 小雨
- 大雨
- 雪
- 雷暴
- 雾
- 未知

不要在 UI 层直接写 `weather_code` 判断。

## 推荐实施顺序

1. 增加地点配置来源
2. 实现 `weather_client`
3. 为 `weather_service` 增加 worker task / 自动刷新 / NVS 缓存
4. 发布 `APP_EVENT_DATA_WEATHER`
5. 收口 `Home` 的真实天气展示

## 当前判断

在功能优先级上，天气系统适合放在 `Home` 首页视觉收口之后、`Notify`/`Trading` 真实数据接线之前。
