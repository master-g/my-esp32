---
date: 2026-06-25
topic: waiting-alert-beacon
---

# “Claude 等待你”警灯提醒

## Summary

把现有低存在感的 "Claude is waiting" 文字 overlay,换成一个高存在感的警车灯条式全屏警示:左蓝右红分区,在多种真实闪烁 pattern 间自动轮换(cycle),屏幕正中常驻大字 + 粗交互类型,叠加背光同步脉动。仍是只读通知,不在设备上做任何决定。

## Problem Frame

当前 `home_approval.c` 是一个 90% 不透明面板,上面只有两行 11px 小字——灰色的 "Claude is waiting" 加一行粗交互类型。没有动效、没有强对比色、字也不大。坐在桌前余光根本扫不到,Claude 等待用户确认时很容易被错过,而这恰恰是设备作为状态通知器的核心职责。

缺的是运动 + 强色 + 大字。板上没有蜂鸣器/LED(`bsp_board_config.h` 未定义),所以提醒手段只有屏幕本身,外加一个常被忽略的杠杆:背光 PWM(`BSP_LCD_BACKLIGHT`)。

## Key Decisions

- **警车灯条观感。** 左蓝右红固定分区,在多种 pattern 间自动轮换。刻意区别于待机屏保的安静观感,让用户一眼认出"该去 Mac 操作"。
- **走 LVGL,不用 direct 管线。** 用 `lv_anim` 调分区图层透明度 + 大字 label,改造现有 `home_approval` overlay。周期性脉动 LVGL 自己就能干,成本远小于屏保。
- **背光同步脉动。** 用 `BSP_LCD_BACKLIGHT` PWM 让亮度随警示呼吸,作为独立的余光通道。背光是 `power_runtime` 独占写入的(按 power policy 每 200ms 重写同一 LEDC 通道),所以脉动必须经 power_runtime 调制,不能由 overlay 另起一路写;背光不可用时警示视觉照常工作。
- **只读、只显粗类型不变。** 沿用现有约束:绝不显示命令/参数/选项内容,不返回任何决定。
- **触发/消解/自超时不变。** 沿用 `PERMISSION/PROMPT REQUEST/DISMISS`、连接丢失清除、5 分钟设备侧自超时。

## Requirements

**观感与动效**

- R1. 等待警示为全屏警车灯条:左侧蓝、右侧红,位置固定。
- R2. 在多种真实灯条 pattern 间自动轮换(cycle):至少含 wig-wag 双闪轮替、quad 四连、triple 三连、single 快速轮替、simultaneous 同闪、quint 爆闪。光敏性约束:大面积全屏闪烁的有效频率压在光敏阈值以下(大面积明暗变化 <~3 次/秒,参考 WCAG 2.3.1),中央卡片区域保持稳定不闪;闪速是受约束项,不是自由项。
- R3. 屏幕正中常驻卡片:大字提示 + 粗交互类型;在最密的闪烁背景上仍清晰(暗底/描边)。
- R4. 背光(`BSP_LCD_BACKLIGHT` PWM)随警示同步脉动,作为额外余光提示。脉动由 `power_runtime` / power_policy 拥有(警示置一个 alert-active 标志,由 power_runtime 在 policy 亮度之上调制),不由 LVGL overlay 独立写 `bsp_backlight_set_percent`,以免与 power_runtime 每 200ms 重写同一 LEDC 通道相争;警示消解后亮度回到 policy 当前值。背光不可用时警示仍照常工作。

**内容与只读约束**

- R5. 卡片只显示粗交互类型(如 Tool permission / Elicitation / AskUserQuestion),绝不显示命令、参数或选项内容。
- R6. 设备不返回任何决定,警示纯为通知,沿用现有只读语义。

**触发与生命周期**

- R7. 触发与消解沿用现状:`PERMISSION_REQUEST` / `PROMPT_REQUEST` 显示,`*_DISMISS` / 连接丢失 / 5 分钟自超时隐藏。
- R8. 进入警示前沿用现有行为:退出屏保、poke activity。
- R9. 警示走 LVGL 路径,改造现有 `home_approval` overlay,不使用屏保的 direct-mode 管线。
- R10. 并发请求行为:同时有多个待处理请求时,卡片显示最高优先级那一个的粗类型(沿用现有"approval 优先于 prompt"规则),并标注"还有 N 个等待";收到某个 `*_DISMISS` 时,仅当没有其他 pending 才隐藏警示,否则切到下一个仍在等待的类型——避免在还有请求等待时整屏警示消失。

## Acceptance Examples

- AE1. **Covers R2.** 警示持续若干秒 → 可见 pattern 在几种之间轮换,而非单一闪法。
- AE2. **Covers R3, R5.** 在最密的 pattern(quad/quint)下 → 中央大字与粗类型清晰可读;且只显示类型,无命令/参数。
- AE3. **Covers R7.** 收到 `*_DISMISS` 或连接丢失 → 警示立即隐藏,无残留。
- AE4. **Covers R4.** 背光初始化失败/不可用 → 警示视觉仍正常,不崩溃。
- AE5. **Covers R10.** 两个请求同时待处理 → 消解其中一个后,警示对另一个仍在显示(切到其类型),不隐藏。

## Scope Boundaries

- 不改触发来源、`device_link` 协议、只读语义。
- 不加声音(板上无蜂鸣器);唯一的非屏幕通道是背光 PWM。
- 不在设备上做决定,不显示命令/参数/选项。
- 不做"先低调横幅、N 秒后升级"的渐进升级;固定就是警灯 cycle。
- ASCII 屏保是另一件事,见 `docs/brainstorms/2026-06-25-ascii-screensaver-library-requirements.md`。

## Dependencies / Assumptions

- 沿用现有 `device_link` 的 pending approval/prompt 查询与 coarse `type_label`。
- 背光可经 `bsp_backlight_set_percent` 编程脉动(API 已存在)。
- 板上无蜂鸣器/LED(GPIO 配置未定义),提醒仅靠屏幕 + 背光。

## Sources / Research

- 现状 overlay:`src/apps/app_home/src/home_approval.c`(90% 面板 + 两行 11px 文字;`HOME_APPROVAL_SELF_TIMEOUT_MS` 5 分钟自超时;连接丢失清除;只读、无触摸)。
- 触发接线:`src/apps/app_home/src/home_runtime.c:339-354`(`PERMISSION/PROMPT REQUEST/DISMISS`;进入前 `exit_screensaver` + `home_screensaver_poke_activity`)。
- 只读约束:`CLAUDE.md`(设备是只读通知器;仅显示粗类型;`claude.approve` 等决定路径已移除)。
- 背光:`src/components/bsp_board/include/bsp_board_config.h:45`(`BSP_LCD_BACKLIGHT` GPIO8);`bsp_display.c` 的 `bsp_display_set_backlight_percent` / `bsp_backlight_set_percent`。无 `BUZZER`/`LED` 定义。
- 警灯 pattern 参考:真实 emergency lightbar 的命名与闪速 —— wig-wag(左双闪→右双闪)、single/double/triple/quad/quint、simultaneous、cycle;FPM = 每分钟闪数(如 75 / 150 / 210 FPM)。来源:SpeedTech Alpha flash pattern list、Federal Signal MicroPulse、strobesnmore "Decoding Flash Patterns"。

## Outstanding Questions

**Deferred to Planning**

- cycle 的具体子集、每种 pattern 的停留时长与切换节奏。
- 卡片大字用中文还是英文(LVGL CJK 字体可用;英文可直接复用现有粗类型串)。
- 背光脉动的幅度/频率,以及与警示视觉如何同步。

## Deferred / Open Questions

### From 2026-06-25 review

- **六种 pattern + cycle 是否对二元信号过度** — Requirements / R2 (P1, scope-guardian, confidence 100)

  salience 目标已由全屏红蓝 + 运动 + 大字(R1/R3/R4)给足;R2 再加六种真实灯条 pattern + cycle 调度(每 pattern 停留、切换节奏、六套定义)。对"有人在等"这个二元信号,wig-wag/quad/quint 没有可区分含义,cycle 机制成了实现要背的状态与动画调度,AE1("pattern 可见轮换")更像为特性而非目标设的验收项。1-2 个 pattern 即可给到同样不可错过的 salience。需确认六 pattern 轮换是刻意的审美目标(区别于纯 salience),还是可砍。

- **屏幕 salience 是否是症结,背光才是真正的余光通道** — Problem Frame / Key Decisions (P1, adversarial, confidence 75)

  文档自陈问题是"余光扫不到"=请求触发时不在用户视线内。但每个决定都做在 Mac 的原生 prompt 上,用户视线本就在 Mac;小桌面设备上的全屏频闪仍落在中央视野外,和现状一样会被错过。唯一真正进余光的是背光脉动,文档却把它降为 R4 次要。若此前提成立,六 pattern 警灯可能让漏接率基本不变,而真正的杠杆(背光)反成附属。建议显式陈述并验证视线假设;若用户视线常在 Mac,把背光脉动提为与屏幕视觉并列的主通道。

- **固定最大强度 vs alarm fatigue / 升级设计** — Key Decisions / Scope (P1, adversarial, confidence 75)

  设计把警示固定在最大强度(全屏红蓝、六 pattern 爆闪、背光脉动)且无升级。其载重假设是"越强越被注意",但每次请求都满强度触发会习惯化:几天后用户把这套警灯滤成背景噪声(alarm fatigue),此时它反不如一个会升级的较平静基线——升级保留了 salience 梯度。文档恰恰拒绝了渐进升级,所以若习惯化占主导,被拒的那个方案才是更高 salience 的设计。建议把习惯化记为该决定的已知风险,并定义随时间衡量的成功标准(如一周正常使用后的漏接率),而非只看第一印象可见性。
