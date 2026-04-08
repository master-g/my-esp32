# Power Policy 子设计方案

## 1. 目的

本文档细化系统级 `power_policy` 模块，目标是把以下问题固定下来：

- 系统根据哪些输入决定亮度、刷新模式和任务暂停
- 页面如何受统一功耗策略约束
- 前台页面、供电状态、低功耗状态如何协同
- 哪些复杂度必须收敛在 policy 层，而不能散落到各页面

本文档是 [dashboard-design.md](../dashboard-design.md) 中运行时状态机与功耗策略的补充说明。

## 2. 设计边界

### 2.1 覆盖范围

- 显示状态决策
- 页面刷新模式决策
- 计算密集型任务允许/禁止规则
- 与 `app_manager`、`event_bus`、各 service 的协作边界

### 2.2 非覆盖范围

- 底层 PM 寄存器配置细节
- RTC / 深睡眠唤醒底层驱动实现
- 电池电量估算算法

## 3. 输入模型

`power_policy` 只接收聚合后的系统输入，不直接读取页面内部状态。输入定义为：

```c
typedef enum {
    POWER_SOURCE_USB = 0,
    POWER_SOURCE_BATTERY,
} power_source_t;

typedef enum {
    DISPLAY_STATE_ACTIVE = 0,
    DISPLAY_STATE_DIM,
    DISPLAY_STATE_SLEEP,
} display_state_t;

typedef enum {
    APP_ID_HOME = 0,
    APP_ID_NOTIFY,
    APP_ID_TRADING,
    APP_ID_SATOSHI_SLOT,
} app_id_t;

typedef struct {
    power_source_t power_source;
    display_state_t display_state;
    app_id_t foreground_app;
    bool wifi_connected;
    bool thermal_throttled;
    bool voltage_guard_triggered;
    bool user_interacting;
} power_policy_input_t;
```

约束：

- `foreground_app` 只有一个有效值
- policy 只关心“哪个页面在前台”，不关心页面内部 widget
- 热保护和掉压保护视为高优先级硬限制

## 4. 输出模型

`power_policy` 输出一个统一动作快照：

```c
typedef enum {
    REFRESH_MODE_REALTIME = 0,
    REFRESH_MODE_INTERACTIVE_POLL,
    REFRESH_MODE_BACKGROUND_CACHE,
    REFRESH_MODE_PAUSED,
} refresh_mode_t;

typedef struct {
    uint8_t brightness_percent;
    refresh_mode_t claude_mode;
    refresh_mode_t market_mode;
    bool weather_refresh_allowed;
    bool slot_compute_allowed;
    bool slot_compute_throttled;
    bool should_enter_sleep;
} power_policy_output_t;
```

设计原则：

- 输出是系统动作，不是建议文本
- 页面和 service 只消费结果，不重复推导
- 所有“是否允许运行”的最终决策都在 policy 层完成

## 5. 基本策略

### 5.1 亮度策略

固定规则：

- `ACTIVE`
  - USB：`100%`
  - Battery：`60%`
- `DIM`
  - USB：`35%`
  - Battery：`20%`
- `SLEEP`
  - 背光关闭

说明：

- 百分比是默认值，可在将来由设置页参数化
- 当前文档不要求页面自己调背光
- USB 供电模式下，v1 不再自动进入 `DIM` / `SLEEP`；空闲降级仅在 Battery 模式启用

### 5.2 数据刷新模式策略

默认映射：

| 条件 | Claude | Market | Weather |
|------|--------|--------|---------|
| USB + Notify + ACTIVE | `REALTIME` | `BACKGROUND_CACHE` | `BACKGROUND_CACHE` |
| USB + Trading + ACTIVE | `BACKGROUND_CACHE` | `REALTIME` | `BACKGROUND_CACHE` |
| USB + Home + ACTIVE | `BACKGROUND_CACHE` | `BACKGROUND_CACHE` | `BACKGROUND_CACHE` |
| USB + Satoshi Slot + ACTIVE | `BACKGROUND_CACHE` | `BACKGROUND_CACHE` | `BACKGROUND_CACHE` |
| Battery + Notify + ACTIVE | `INTERACTIVE_POLL` | `PAUSED` | `BACKGROUND_CACHE` |
| Battery + Trading + ACTIVE | `BACKGROUND_CACHE` | `INTERACTIVE_POLL` | `BACKGROUND_CACHE` |
| Battery + Home + ACTIVE | `BACKGROUND_CACHE` | `PAUSED` | `BACKGROUND_CACHE` |
| 任何页面 + DIM | `BACKGROUND_CACHE` | `PAUSED` | `BACKGROUND_CACHE` |
| 任何页面 + SLEEP | `PAUSED` | `PAUSED` | `PAUSED` |

天气特殊规则：

- `weather_refresh_allowed` 在 `ACTIVE` 和 `DIM` 可为真
- `SLEEP` 时禁止天气刷新

## 6. Satoshi Slot 特殊规则

`Satoshi Slot` 是唯一的本地高计算页面，因此需要单独策略：

### 6.1 允许运行条件

正常批量扫描仅在以下条件下允许：

- `display_state == ACTIVE`
- `foreground_app == APP_ID_SATOSHI_SLOT`
- `thermal_throttled == false`
- `voltage_guard_triggered == false`

### 6.2 USB / Battery 差异

- USB + ACTIVE：连续运行
- Battery + ACTIVE：只允许节流运行
- `DIM / SLEEP`：禁止运行

### 6.3 节流策略

Battery 模式下：

- `slot_compute_allowed = true`
- `slot_compute_throttled = true`

页面或 service 必须据此切换到更小批次或插入更长 `yield`。

## 7. 热保护与掉压保护

### 7.1 优先级

热保护和掉压保护优先级高于前台页面需求。

一旦触发：

- 计算任务立即暂停
- 高刷新模式全部降级
- 可选地请求系统进入 `DIM`

### 7.2 行为

若 `thermal_throttled == true` 或 `voltage_guard_triggered == true`：

- `slot_compute_allowed = false`
- `market_mode != REALTIME`
- `brightness_percent` 不得高于当前状态允许值

## 8. 与 event_bus 的关系

`power_policy` 不是主动拉取型模块，而是响应输入变化并广播输出变化。

发事件时机：

- 供电模式变化
- 显示状态变化
- 前台 app 变化
- 热保护状态变化
- 掉压保护状态变化

统一广播：

- `APP_EVENT_POWER_CHANGED`

约束：

- 如果输出快照无变化，不重复广播
- 页面不直接监听原始电源中断，而只监听 policy 层事件

## 9. 对外接口

```c
esp_err_t power_policy_init(void);
void power_policy_on_input_changed(const power_policy_input_t *input);
const power_policy_output_t *power_policy_get_output(void);
bool power_policy_is_refresh_mode(refresh_mode_t expected, app_id_t app);
```

设计原则：

- 输入改变时统一重算一次
- 输出快照可被 service 和页面读取
- 不向页面暴露底层电源细节

## 10. 错误与退化

### 10.1 输入不完整

如果输入不完整：

- 保守退化为低功耗输出
- 不进入 `REALTIME`
- 不允许高计算任务运行

### 10.2 未知前台页面

未知前台页面时：

- 默认走 `BACKGROUND_CACHE`
- 禁止本地计算任务

## 11. 验收用例

### 11.1 亮度

- USB `ACTIVE` 时亮度输出为高亮
- Battery `DIM` 时亮度明显下降

### 11.2 页面模式切换

- 切到 `Trading` 前台时，`market_mode` 正确提升
- 切到 `Notify` 前台时，`claude_mode` 正确提升

### 11.3 低功耗

- 进入 `SLEEP` 时，所有刷新模式变为 `PAUSED`
- `Satoshi Slot` 在 `DIM` 时立即停止计算

### 11.4 保护逻辑

- 热保护触发后，`slot_compute_allowed=false`
- 掉压保护触发后，不再允许高计算任务继续

## 12. 假设

- v1 不引入用户可配置功耗档位页
- v1 只考虑 USB / Battery 两种电源来源
- `power_policy` 是所有刷新和计算许可的唯一真源

---

*文档版本: 1.0*  
*创建日期: 2026-04-07*
