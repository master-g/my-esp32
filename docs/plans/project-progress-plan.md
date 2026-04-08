# 项目进度评估与后续实施计划

## 问题与目标

- 基于现有文档和代码，评估 ESP32 Dashboard 项目当前进度。
- 在不误伤应纳管文件的前提下，整理 `.gitignore`。
- 纳入最新实机反馈：横屏显示已跑通，且当前支持两种固定 landscape 方向，默认值可切换到上下翻转的安装方向。
- 明确项目从“固件骨架已落地”推进到 “P0 板级打通 / P1 MVP” 的实施顺序。

## 执行进展（2026-04-08）

- `repo-hygiene-sync` 已完成首轮收口：
  - `.gitignore` 已明确继续忽略 `.vscode/` 这类机器本地状态，同时保留 `sdkconfig.defaults` 与锁文件纳管
  - `README.md` 已更新为当前代码现实，不再把 LCD / touch / LVGL / Wi-Fi / SNTP 全部描述成“尚未接线”
- `idf-build-baseline` 已完成首轮验证：
  - 已在 `ESP-IDF 6.0` 环境下执行 `idf.py -B build-esp32s3 build`
  - 当前工程可成功完成构建
  - 当前 app 二进制大小为 `0x118870`，相对最小 app 分区 `0x200000` 仍有 `45%` 余量
- `landscape-orientation-bringup` 已完成首轮收口：
  - 已确认物理面板保持 `172x640 portrait`，运行时 UI 采用 `640x172 landscape`
  - 已按官方 `10_LVGL_V9_Test` 路径完成软件旋转显示，而不是继续混用 panel 侧旋转
  - `Home` 已完成横屏验证布局，其他三个 app 已切到横屏根尺寸
  - 当前支持两种固定 landscape 方向，默认值已可切到翻转后的安装方向
  - 最新一次 `idf.py -B build-esp32s3 build` 与实机烧录都已通过
  - 自动翻转明确后置到 `bsp_imu` 落地后的 P2 阶段
- `board-bringup-validation` 已开始首轮闭环：
  - 已补 `power_runtime`，把 `power_policy` 输出真正执行到背光
  - 已根据最新要求改为：USB 供电下保持 `ACTIVE`，不自动进入 `DIM` / `SLEEP`
  - Battery 模式仍保留空闲降级路径，用于后续实机验证
  - 已通过实机串口日志确认：
    - RTC 恢复成功
    - Wi-Fi 自动联网成功
    - 获取 IP 成功
    - SNTP 校时成功
    - NTP 结果已写回 RTC

## 当前状态评估

### 整体判断

项目已经越过“纯设计阶段”，当前更准确的定位是：

- **文档和架构层面**：主设计文档与多个子设计已经基本成型。
- **Host 侧工具**：`tools/esp32dash` 已经是独立 Rust 项目，并带有基础测试。
- **Firmware 侧**：ESP-IDF 工程、组件分层、BSP、服务骨架、四个 app 槽位均已落地。

整体上，项目处于 **“P0 代码骨架已落地，横屏显示基线已稳定并支持双固定方向，但 P0 板级闭环验证与 P1 真实数据/UI 仍未完成”** 的阶段。

### 已完成 / 已有真实骨架

- 顶层 ESP-IDF 工程结构已建立：`CMakeLists.txt`、`src/main/`、`src/components/`、`src/apps/`。
- `bsp_board_config` 已把 Waveshare 板级基线固定为：
  - `AXS15231B`
  - `172 x 640`
  - 双 I2C 总线
  - `touch=0x3B`、`rtc=0x51`、`imu=0x6B`
- `bsp_display` 已存在真实的 `esp_lcd_axs15231b + LVGL` 初始化路径，而不是纯空壳。
- `bsp_touch`、`bsp_rtc` 已有实际 I2C 访问逻辑。
- `app_manager`、`event_bus`、`system_state`、`power_policy` 已构成运行时主干。
- `app_home` 已在实机上跑通，说明 `display + LVGL + Home app` 的最小链路并非纸面设计，而是已有首轮设备验证。
- `net_manager` 已实现：
  - NVS 凭据加载
  - Wi-Fi 事件处理
  - 基础断线重连策略
- `time_service` 已实现：
  - RTC 恢复
  - SNTP 同步触发
  - 时间文本快照更新
- `home_service` 已有聚合层，可汇总 time / weather / claude / net 状态。
- 四个 app 都已注册并能创建 LVGL 根视图。
- `tools/esp32dash` 已具备 agent / claude / device / config / launchd 的 CLI 结构；现有 Rust 测试可通过。

### 部分完成 / 明显仍是占位

- `service_weather` 仍是刷新节流和 snapshot 占位，尚未接真实天气数据源。
- `service_market` 仍是 pair/interval/snapshot 占位，尚未接真实行情与 candle 数据。
- `service_claude` 目前只维护本地状态占位，尚未接入 bridge 快照/增量协议。
- `Notify`、`Trading`、`Satoshi Slot` 仍是占位页；`Home` 也是简化版展示而非最终 MVP 页面。
- 横屏显示已跑通并支持两种固定方向；触摸基础点击正常，RTC、Wi-Fi、IP、SNTP、RTC 回写也已完成首轮串口验证，但背光 / dim / sleep 与更多异常路径仍需继续回归。
- 全局翻页、边缘手势、分页指示器、生产级页面布局尚未落地。
- `power_policy` 已有输出逻辑，但还未看到完整的背光 / dim / sleep 端到端闭环。
- IMU、更多板级外设、复杂功耗策略还未接上。

### 进度漂移与风险

- `README.md` 与硬件/设计文档已完成横屏首轮同步，但板级闭环和真实数据链路接入后仍需要继续跟进更新，避免再次落后于代码。
- `.gitignore` 已完成首轮收口；如果后续决定纳管共享 IDE 配置，需要再单独细化 `.vscode/` 的白名单策略。
- 已确认当前工程可在 `ESP-IDF 6.0` 环境下完成 `idf.py -B build-esp32s3 build`，且横屏固件与 RTC/Wi-Fi/NTP 主链均已完成首轮实机验证；当前阻塞点已转向剩余板级异常路径与真实业务数据接线。
- IMU 自动翻转暂不应插队；在 `bsp_imu` 尚未落地前继续推进会把 P0 bring-up 和 P2 体验特性混在一起。
- 当前工作区存在大量未提交新增文件，后续改动必须避免误碰用户已有变更。

## 建议实施顺序

### 1. 仓库卫生与文档同步

先解决“仓库现状表达不一致”的问题：

- 完成 `.gitignore` 规则收口。
- 重新评估 `.vscode/` 等 IDE 配置是否应该继续忽略，还是转为部分纳管。
- 确认哪些文件应长期纳管：
  - `sdkconfig.defaults`
  - `dependencies.lock`
  - `tools/esp32dash/Cargo.lock`
- 更新 `README.md`，让文档准确反映当前代码进度。
- 标记哪些模块已是真实 bring-up，哪些仍是 placeholder。

### 2. 固件编译基线确认

在真实 ESP-IDF shell 中确认当前工程可重复编译：

- `idf.py set-target esp32s3`
- `idf.py build`

目标是尽快确认：

- 组件依赖是否完整
- Kconfig / sdkconfig 默认值是否合理
- `esp_lcd_axs15231b`、LVGL、board 组件是否都能稳定编译

### 3. 横屏方向基线收口

在继续做更多页面和服务接线之前，先把显示方向从当前竖屏实现统一到目标横屏模式：

- 明确运行时 UI 坐标系以 `landscape` 为准，而不是继续沿用当前 `172 x 640 portrait` 的页面假设
- 调整 LCD 初始化/旋转配置，让显示输出符合横屏安装方向
- 同步修正 `bsp_touch` 的 `x/y` 映射与方向翻转
- 重新确认全局翻页、边缘热区和页面布局的基准方向
- 在实机上重新回归 `app_home`，把它作为横屏切换后的第一块验证页

这一步已经完成，后续所有 UI 和交互工作可直接建立在当前 landscape 基线上。

### 4. P0 板级打通验证

在实机上验证最关键的板级闭环：

- LCD 刷新
- 触摸坐标读数
- RTC 读写恢复
- Wi-Fi 联网与断线重连
- NTP 校时写回 RTC
- `power_policy` 对亮度 / dim / sleep 的实际驱动效果

这是从“能编译的骨架”变成“可运行底座”的分水岭。

### 5. 数据服务接线

在板级闭环稳定后，补齐真实数据链路：

- `claude_service` 接入 `claude_bridge` 的快照/增量协议
- `weather_service` 接入真实天气接口与缓存状态
- `market_service` 接入价格与 K 线源数据
- 统一发布 `APP_EVENT_DATA_*` 事件，避免 app 直接依赖服务内部细节

### 6. MVP UI 收口

最后再把 UI 从“占位屏”推进到可演示 MVP：

- Home：时间、日期、Wi-Fi、天气、Claude 未读角标
- Notify：状态、标题、工作区、更新时间、断线/无数据态
- Trading：价格、涨跌幅、更新时间，保留 P2 图表扩展口
- Satoshi Slot：保持受控占位，延后到 P2

同时补上：

- 全局翻页/分页指示器
- 页面切换与 service 状态更新的协同
- 与 `power_policy` 的前后台刷新策略联动

## Todo 列表

1. `repo-hygiene-sync`（已完成）
   - 完成 `.gitignore` 策略确认。
   - 修正文档与代码当前状态不一致的问题。
2. `idf-build-baseline`（已完成）
   - 在真实 IDF 环境下建立可重复的固件编译基线。
3. `landscape-orientation-bringup`（已完成）
   - 已完成 landscape 显示基线、双固定方向支持和 `app_home` 横屏回归。
4. `board-bringup-validation`（下一步）
   - 实机验证显示、触摸、RTC、Wi-Fi、NTP 与功耗主链。
5. `service-transport-wiring`
   - 接入 Claude bridge、Market、Weather 的真实数据流和事件发布。
6. `mvp-ui-integration`
   - 将 Home / Notify / Trading 收口到 P1 可演示状态，并保留 Satoshi Slot 为 P2。

## 依赖关系

- `landscape-orientation-bringup` 依赖 `idf-build-baseline`
- `board-bringup-validation` 依赖 `landscape-orientation-bringup`
- `service-transport-wiring` 依赖 `board-bringup-validation`
- `mvp-ui-integration` 依赖 `service-transport-wiring`

## 备注

- `tools/esp32dash` 可以独立推进，不必等待所有固件 UI 完成。
- 当前最值得优先推进的不是“再加新功能”，而是先让：
  - 仓库状态清晰
  - 文档与代码对齐
  - 固件在真实 IDF 环境中可编译
  - 板级主链路可验证
- 已确认：横屏显示基线已稳定，且当前支持两种固定 landscape 安装方向。
- 已确认：`.gitignore` 不只处理生成物和本地状态，还要一并复核 IDE 配置（尤其是 `.vscode/`）是否应部分入库。
- 已确认：虽然屏幕硬件基线仍是 `172 x 640`，但产品显示目标已经切到 `landscape`；后续页面设计和输入映射都应围绕这个目标展开。
- 已确认：IMU 自动翻转属于 P2 体验增强项，不建议在 `board-bringup-validation` 完成前插入。
