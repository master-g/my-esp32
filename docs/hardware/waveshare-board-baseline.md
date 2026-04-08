# Waveshare ESP32-S3-Touch-LCD-3.49 板级基线说明

## 目的

这份说明用于把当前项目固件骨架所采用的官方板级基线固定下来，避免后续实现时继续在“产品页参数”和“官方示例代码”之间摇摆。

## 采用的主基线

当前实现优先以 Waveshare 官方 GitHub 的 ESP-IDF 示例为准，而不是仅以产品页摘要文字为准。

选择理由：

- 官方 ESP-IDF 示例直接决定了可复用的驱动、引脚和组件依赖
- 产品页存在明显文本冲突
- 我们当前目标是搭建可实现的固件底座，而不是复述宣传页参数

## 固定参数

截至 2026-04-08，固件骨架采用以下参数：

- LCD 控制器：`AXS15231B`
- 显示接口：`QSPI`
- 物理面板分辨率：`172 x 640`
- 物理面板方向：竖屏
- 运行时 UI 逻辑分辨率：`640 x 172`
- 运行时 UI 方向：横屏
- 当前支持两种固定横屏方向：`landscape_90` / `landscape_270`
- 当前默认方向：`landscape_270`
- LCD Host：`SPI3_HOST`
- LCD 引脚：
  - `CS = GPIO9`
  - `PCLK = GPIO10`
  - `DATA0..3 = GPIO11..14`
  - `RST = GPIO21`
  - `BK_LIGHT = GPIO8`
- 传感器 I2C 总线：
  - `I2C_NUM_0`
  - `SCL = GPIO48`
  - `SDA = GPIO47`
- RTC 地址：`0x51`
- IMU 地址：`0x6B`
- 触摸 I2C 总线：
  - `I2C_NUM_1`
  - `SCL = GPIO18`
  - `SDA = GPIO17`
- 触摸地址：`0x3B`

## 组件依赖基线

从官方 `10_LVGL_V9_Test` 与 `11_FactoryProgram` 示例可确认：

- `10_LVGL_V9_Test`：
  - `idf: >5.0.4, !=5.1.1`
  - `lvgl/lvgl: ^9`
  - `espressif/esp_lcd_axs15231b: ^1.0.1`
- `11_FactoryProgram`：
  - `espressif/esp_lcd_axs15231b: ^1.0.1`
  - `espressif/esp_io_expander_tca9554: ^2.0.1`

当前项目第一版骨架没有立即引入这些依赖，而是先把板级 contract 和分层结构固定下来。真实 LCD/LVGL bring-up 时再接入。

在当前项目的真实 bring-up 阶段，还需要额外注意一件事：

- Waveshare 示例里锁定的 `espressif/esp_lcd_axs15231b 1.0.1` 在本机 `ESP-IDF 6.0` 下会因旧字段 `color_space` 与新版 `esp_lcd_panel_dev_config_t` 不匹配而编译失败
- 因此当前项目的实际工程依赖改为 `espressif/esp_lcd_axs15231b 2.1.0`

这属于为了适配当前 `ESP-IDF 6.0` 环境所做的兼容性升级，不改变板级硬件基线本身。

## 发现的官方信息冲突

Waveshare 产品页同一页面存在相互矛盾的信息：

- 页面简介写了 `3.49-inch capacitive high-definition IPS screen`
- 规格摘要又写了 `172 × 320`, `Resistive touch screen`

但官方 ESP-IDF 示例、FactoryProgram 生成图片和 `user_config.h` 都统一指向：

- `172 x 640`
- 独立触摸 I2C 总线，地址 `0x3B`

因此当前项目明确采用“官方示例代码基线优先”。

## 对项目架构的影响

- `bsp_board` 必须支持双 I2C 总线模型，而不是把 RTC/IMU/Touch 混在一条线上
- `bsp_display` 需要为 `AXS15231B QSPI LCD` 保留专用初始化路径
- 触摸层先抽象为自定义 `bsp_touch`，不要直接假设能套用通用驱动
- 物理缓冲区预算仍按 `172 x 640` 计算，但页面布局基线应按 `640 x 172` landscape 设计
- 当前横屏实现优先复用官方 `10_LVGL_V9_Test` 的软件旋转路径，而不是自行引入额外的 panel 侧旋转
- 若未来引入 IMU 自动翻转，应在 `bsp_imu` 落地后再补“去抖 + 锁向 + 状态广播”策略，不应和当前 P0 bring-up 混做

## 后续动作

- 接入真实 `esp_lcd_axs15231b`
- 验证触摸读数协议是否继续沿用官方示例里的自定义读命令
- 基于这套板级基线实现 `bsp_display`、`bsp_touch`、`bsp_rtc`、`bsp_imu`
