# App Manager 与 Event Bus 子设计方案

## 1. 目的

本文档细化 `app_manager` 和 `event_bus` 的系统职责，目标是把以下问题定死：

- 页面如何注册、创建、切换和暂停
- 事件总线如何把系统/网络/数据更新送到页面
- 哪些职责属于 manager，哪些属于页面或 service

本文档是 [dashboard-design.md](../dashboard-design.md) 中 UI Core 的补充说明。

## 2. 设计边界

### 2.1 覆盖范围

- 应用注册表
- 前台页面切换
- 页面生命周期
- 统一事件分发

### 2.2 非覆盖范围

- LVGL widget 细节
- 底层触摸驱动
- 复杂脚本式路由

## 3. 核心约束

- 系统任意时刻只有一个前台 app
- 所有页面共享同一个根导航上下文
- 页面切换不销毁 service 缓存
- 页面不直接互相调用
- 页面间通信一律走 `event_bus` 或共享 service 快照

## 4. App Manager 模型

### 4.1 注册描述符

```c
typedef struct {
    app_id_t id;
    const char *name;
    esp_err_t (*init)(void);
    lv_obj_t *(*create_root)(lv_obj_t *parent);
    void (*resume)(void);
    void (*suspend)(void);
    void (*handle_event)(const app_event_t *event);
} app_descriptor_t;
```

约束：

- `create_root()` 只调用一次
- `resume()` / `suspend()` 允许多次调用
- 页面不拥有导航控制权

### 4.2 生命周期

固定生命周期：

1. `init`
2. `create_root`
3. 首次前台：`resume`
4. 离开前台：`suspend`
5. 再次进入前台：`resume`

v1 不实现页面销毁。

## 5. 前台切换规则

### 5.1 切换入口

前台切换只允许来自：

- 全局翻页手势
- 页面指示器点击
- 系统强制跳转（未来可选）

### 5.2 切换流程

切换流程固定为：

1. 当前页面 `suspend`
2. 更新 `foreground_app`
3. 新页面 `resume`
4. 广播 `APP_EVENT_ENTER` 给新页面
5. 广播 `APP_EVENT_LEAVE` 给旧页面

约束：

- 不允许在 `handle_event()` 内直接递归切页
- 若目标页面与当前页面相同，则 no-op

## 6. Event Bus 模型

### 6.1 事件类型

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
```

### 6.2 分发规则

- `ENTER` / `LEAVE`：只发给相关页面
- `TOUCH`：只发给前台页面
- `TICK_1S`：只发给前台页面，用于低频状态刷新
- `NET_CHANGED` / `POWER_CHANGED`：发给前台页面，必要时允许广播给全部页面
- `DATA_*`：默认只发给前台页面；页面切回前台后通过 service 快照补状态

设计原则：

- 页面主要通过拉快照恢复，不依赖事件历史重放
- `event_bus` 不做复杂持久队列
- 显式 UI 控制命令（例如强制切页、进入 Home screensaver）不走 `event_bus`，改由 `app_manager` 的 UI control queue 送入 LVGL 线程

## 7. 与 Service 的边界

- service 负责持有真状态
- `event_bus` 只负责通知“状态有变化”
- 页面收到事件后，去对应 service 拉最新快照

禁止：

- 在事件 payload 中塞完整大型数据结构
- 让页面依赖事件顺序恢复业务状态

## 8. 对外接口

```c
esp_err_t app_manager_init(void);
esp_err_t app_manager_switch_to(app_id_t app_id);
app_id_t app_manager_get_foreground_app(void);
const app_descriptor_t *app_manager_get_descriptor(app_id_t app_id);

esp_err_t event_bus_init(void);
esp_err_t event_bus_publish(const app_event_t *event);
```

## 9. 错误与退化

### 9.1 页面初始化失败

- 初始化失败的页面不得进入前台
- manager 必须保留到 `Home` 的安全回退路径

### 9.2 事件处理失败

- 单个页面处理失败不得阻断 manager 主循环
- 错误只记日志，不传播成系统崩溃

## 10. 验收用例

- 从 `Home` 切到 `Trading` 时，旧页 `suspend`、新页 `resume` 顺序正确
- 重复切到当前页时不触发重复切换
- `APP_EVENT_DATA_MARKET` 到来后，前台 `Trading` 能刷新，而其他页面靠快照恢复
- `APP_EVENT_POWER_CHANGED` 到来后，前台页面能立即按 policy 退化

## 11. 假设

- v1 只支持四个固定页面
- 不引入动态插件式 app 注册
- 页面状态恢复主要依靠 service 快照而不是事件历史

---

*文档版本: 1.0*
*创建日期: 2026-04-07*
