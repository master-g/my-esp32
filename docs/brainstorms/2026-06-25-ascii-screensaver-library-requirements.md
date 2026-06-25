---
date: 2026-06-25
topic: ascii-screensaver-library
---

# ASCII 屏保效果库

## Summary

把当前单一的渐变 shader 屏保,换成一组可选的 ASCII 字符效果,每个效果共用同一个固定居中的时钟。每次进入屏保时随机抽一个效果,运行到退出为止。复用现有的 direct 推屏管线、字模 blitter 和生命周期,只替换背景渲染器。

## Problem Frame

现在的屏保是 `screensaver_renderer.c` —— 一个从 `docs/plan/screensaver.glsl` 移植的定点化逐像素 shader,只有一种波动渐变观感。用户想要 omarchy 那种终端 ASCII 屏保的多样性和复古质感,并担心 ESP32-S3 撑不撑得住。

事实大体相反:这个 shader 是目前最重的负载档位(每帧逐像素算噪声、旋转、smoothstep、多次混色)。满帧 RGB565 推屏成本与画什么内容无关(push 恒定),现有屏保已经每帧推满整屏;真正的变量是每效果的 compose。字符格子的 compose 通常比逐像素 shader 轻,但 Game of Life、等离子/流场这类不保证更轻,需逐效果用现有 `compose_us` 仪表实测(见 R12)。所以总体性能风险低,但"不超过 shader"对每个效果都要实测,不是无条件成立。真正要定的是观感与取舍。

## Key Decisions

- **固定居中时钟,公共合成。** 时钟不再是某个效果的一部分;它由共享代码每帧盖在背景之上,所有效果一致。这把"画背景"和"画时钟"解耦,新增效果只需关心背景。
- **每次进入随机抽一个效果。** 不做轮播、不做用户手选;进屏保掷一次骰子,选中的效果一直跑到退出。
- **退役现有渐变 shader。** 屏保背景不再使用 `screensaver_renderer.c` 的渐变路径,完全由效果库提供。
- **生命周期与触发时机完全不动。** 仍是空闲 30 分钟触发(`HOME_SCREENSAVER_IDLE_US`)、触摸退出、enter/exit/suspend 不变。只换背景渲染器。
- **复用 direct 管线,不重写。** 双缓冲 framebuffer、专用推屏任务、DMA、5×7 字模 blitter(`draw_char_with_writer`)都留用。

## Requirements

**效果库**

- R1. 提供一组可选的 ASCII / 字符格子背景效果。初始集合:字符雨(Matrix)、ASCII 等离子/流场、生命游戏、ASCII 火焰、星空跃迁、管道(pipes)、斜雨、正弦波带。
- R2. 退役现有渐变 shader 背景(`screensaver_renderer.c` 的渐变路径),屏保背景完全由效果库提供。
- R3. 每个效果是一个自包含的背景渲染器:给定原生 framebuffer 与流逝时间,填满一帧背景;不触碰时钟,不触碰生命周期。

**时钟与合成**

- R4. 所有效果在屏幕正中显示同一个时钟,由共享合成代码盖在背景之上,而非各效果各画一份。
- R5. 共享合成步骤在时钟字形所在矩形区域先做暗化 scrim(固定压暗 / alpha 混合),再贴时钟字形,使可读性与效果内容无关。
- R6. 时钟显示当前 HH:MM,沿用现有每秒刷新。

**选择与生命周期**

- R7. 每次进入屏保随机选一个效果,运行到退出。
- R8. 连续两次进入不重复上一次的效果。
- R9. 触发阈值(空闲 30 分钟)、触摸退出、enter/exit/suspend 行为与当前完全一致;唯一变化是背景渲染器。进入屏保时效果从各自的空态 / seed 立即起渲(无全局淡入),触摸时硬切退出——所有效果统一,沿用现状。

**渲染与字模**

- R10. 复用现有 direct 推屏管线与 LVGL fallback 路径;所有效果都经过同一套合成步骤输出。
- R11. 扩充 bitmap 字模表以覆盖各效果用到的字符(如 pipes 的 box-drawing 字符、等离子/火焰的明暗 ramp 标点)。效果应只使用字模表中存在的字形(设计意图);任何缺失字形必须经 `find_glyph_rows` 安全回退到空格,绝不输出乱码(安全网)。
- R12. 每个效果上线前用现有 `compose_us` 仪表确认其每帧 compose 不超过当前 shader 的 compose(push 恒定,不计入比较);屏保帧率不低于现状。Game of Life、等离子/流场最可能逼近上限,重点验证。

## Acceptance Examples

- AE1. **Covers R7, R8.** 连续进入屏保两次 → 第二次选中的效果与第一次不同。
- AE2. **Covers R4, R5.** 在背景最密最亮的效果(如火焰/字符雨)下 → 时钟字形以全对比度压在 scrim 之上,每个效果都清晰可读(可测,而非主观判断)。
- AE3. **Covers R11.** 某效果引用字模表里不存在的字符 → 退回安全字形(空格),绝不出现乱码(沿用 `find_glyph_rows` 现有回退)。
- AE4. **Covers R9.** 屏保期间触摸 → 立即退出,与当前一致。

## Scope Boundaries

- 不改 power policy 的任何时机与 DIM/SLEEP 阈值。电池供电下背光在 45 秒降到 0%,屏保实际看不到 —— 这个现状不变,本次不处理。
- 不引入新的矢量/TTF 字体,只用 bitmap 字模。
- 不做"手动指定某个效果"的用户设置;只随机。
- 不做按效果的配置界面或参数调节 UI。

## Dependencies / Assumptions

- 屏保仅在 USB 供电时有意义可见(显示被强制 ACTIVE、常亮)。接受这一点。
- 现有 direct 管线与字模 blitter 仍是底座,效果在其上实现。

## Sources / Research

- 当前屏保实现:`src/apps/app_home/src/home_screensaver.c`(生命周期、direct/LVGL 双路径、perf 仪表)、`src/apps/app_home/src/screensaver_renderer.c`(GLSL shader 定点移植)、`src/apps/app_home/src/screensaver_direct.c`(direct 管线 + 5×7 字模表 `s_glyphs[]` + `draw_char_with_writer` + `find_glyph_rows` 回退)。
- 触发阈值:`HOME_SCREENSAVER_IDLE_US` = 30 分钟,见 `src/apps/app_home/src/home_internal.h:38`;direct 任务参数(core/优先级/周期)同文件。
- 电源时机:`src/components/power_runtime/src/power_runtime.c`(USB 强制 ACTIVE;电池 DIM 15s / SLEEP 45s);`src/components/power_policy/src/power_policy.c:30-37`(SLEEP 亮度 0%)。
- 显示与带宽:172×640 面板,QSPI 40 MHz quad(约 20 MB/s),满帧 RGB565 215 KB → 推屏天花板约 11 ms/帧,见 `src/components/bsp_board/src/bsp_display.c`。字符效果比逐像素 shader 更轻,性能不是约束。

## Outstanding Questions

**Deferred to Planning**

- "逐字符揭示"在固定居中时钟前提下的角色:是进入屏保时的一次性揭示/转场动效,还是一个循环背景效果?规划阶段定。
- 各效果具体需要哪些字符、要往字模表加哪些 bitmap。

## Deferred / Open Questions

### From 2026-06-25 review

- **30 分钟触发可能让多样性从不被看到** — Problem Frame / Dependencies (P1, adversarial, confidence 75)

  整个效果库被 30 分钟连续空闲触发门控,USB 下显示常亮且用户此时多半已离开屏幕。8 个效果、随机选取、防重复带来的多样性只会跨多次 30 分钟空闲分别出现,从不在一次观看中体验到。若触发太罕见,用户实际只会看到一两个效果,那么为基本不被观察的差异建 8 个效果就不划算。文档接受"仅 USB 可见",却没问 30 分钟阈值本身是否才是多样性能否落地的瓶颈,也未把"缩短触发 / 改为进入时一次性转场"与"做满 8 个效果"做权衡。

- **8 个效果对一个装饰性目标是否右尺寸** — Requirements / R1 (P2, scope-guardian, confidence 75)

  目标是观感多样性与复古质感,任意小集合即可满足——数量本身不服务于任何随规模增长的目标。R1 一次承诺全部 8 个,而它们实现成本差异巨大:Game of Life 需持久细胞网格 + 演化步,ASCII 火焰需热扩散缓冲,pipes 需有状态路径追踪 + 新 box-drawing 字模,正弦波带几乎免费。每个有状态效果都是 R3 无状态渲染器签名之外的新抽象,使这套在一个非功能性屏保上触发复杂度警戒线(>2 个新抽象)。把 8 个全列入更像把头脑风暴的探索原样搬进 spec,而非一个定好尺寸的首版。可考虑先上 3-4 个便宜的(字符雨、等离子/流场、正弦波带、星空),GoL/火焰/pipes 列为后续。
