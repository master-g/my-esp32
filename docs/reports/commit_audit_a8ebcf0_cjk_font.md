# Commit 审计报告：a8ebcf0 —— 添加 Noto Sans CJK 中文字体支持

**审计日期**：2026-04-09
**Commit**：`a8ebcf0de22763360762f1aa9310762474b6b388`
**作者**：masterg (Co-authored-by: Copilot)
**主题**：feat: add Noto Sans CJK font for Chinese text in bubble/approval

---

## 变更概览

本次提交为 `app_home` 引入了 Noto Sans CJK（GB2312 Level 1，3855 字符，12px/2bpp）的嵌入式字体，以实现气泡和审批弹窗的中文渲染。同时移除了 `esp32dash` 端的 `ascii_safe()` 过滤，允许原始 CJK 文本下发到设备。

改动涉及 5 个文件：

- `src/apps/app_home/CMakeLists.txt` — 添加生成字体源文件
- `src/apps/app_home/src/app_home.c` — 构建 Montserrat 12 + Noto Sans CJK 的 fallback 复合字体
- `src/apps/app_home/src/generated/noto_sans_cjk_12.h` — 字体外部声明头文件
- `src/apps/app_home/src/generated/noto_sans_cjk_12.c` — 生成的字体位图数据（~21k 行，~2.0MB）
- `tools/esp32dash/src/normalizer.rs` — 移除 `ascii_safe()` 调用

---

## 审计发现

### 1. 编译器警告：const 限定符丢失（核实后不成立）

在 `app_home.c` 中：

```c
static lv_font_t s_cjk_font;

static lv_obj_t *app_home_create_root(lv_obj_t *parent)
{
    s_cjk_font = lv_font_montserrat_12;   // const → non-const 赋值
    s_cjk_font.fallback = &noto_sans_cjk_12;
    ...
}
```

这条结论不成立。这里发生的是 `const lv_font_t` 结构体值拷贝到一个独立的 `lv_font_t` 变量，不是把 `const lv_font_t *` 赋给 `lv_font_t *`。在本仓库的 `ESP-IDF 6.0` 环境下重新执行 `idf.py -B build-esp32s3 build`，该位置没有出现 `-Wdiscarded-qualifiers` 或 `-Werror` 失败。

**结论**：无需为这一点改代码。现有写法在当前 LVGL / 编译器组合下可接受。

### 2. 二进制体积显著增长（已知，需持续关注）

生成的 `noto_sans_cjk_12.c` 包含约 2.0MB 的静态字模数据。按提交说明，这已占到 4MB app 分区的 50%。当前 16MB Flash 的设备虽然足够，但留给未来功能扩展（更多 app、更大固件、OTA 双分区需求）的裕量被大幅压缩。如果后续需要支持 GB2312 Level 2 或更大字号，体积会进一步膨胀。

**建议**：将字体数据放入单独的分区（如 `spiffs` 或自定义 `font` 分区），通过文件系统或内存映射按需加载，而非直接链接进 app binary。这样可以在不重新烧录固件的情况下更新字体，也能缓解 app 分区压力。

### 3. 生成文件缺少许可证声明（合规风险）

`noto_sans_cjk_12.c` 头部仅记录了 `lv_font_conv` 的生成参数，没有包含源字体 Noto Sans CJK 的 SIL Open Font License 版权声明和版权声明文本（该许可证要求分发衍生作品时附带许可文本）。

**建议**：在生成文件头部追加必要的许可证说明，或在 `third_party/` 目录下放置 `NOTO_SANS_CJK_LICENSE.txt` 并在文档中引用。

### 4. `normalizer.rs` 的测试覆盖与行为错配，以及 UTF-8 字节边界风险（中风险）

`normalizer.rs` 中保留了 `ascii_safe()` 函数本身，但 `normalize()` 已不再调用它。现有单元测试（如 `ascii_safe_strips_cjk`、`ascii_safe_pure_chinese_becomes_empty`）仍在验证一个“已退役”的函数行为。更关键的是，由于 `ascii_safe` 被移除，下发到设备的文本可能包含 GB2312 Level 1 未覆盖的字符（Emoji、生僻字、繁体中文等），而固件侧并没有兜底过滤，LVGL 只会显示空白或乱码。

此外，原报告漏掉了一个更实际的问题：host 侧原本按“字符数”裁剪，firmware 侧通过 `strlcpy` 等接口按“字节数”落进定长缓冲。对于中文和 emoji，长文本会在设备侧发生 UTF-8 半截截断，结果可能不是简单的“少几个字”，而是直接出现乱码或空白。

**建议**：
- 删除 `ascii_safe` 及其旧测试。
- 改成统一的“设备出站文本清洗层”：按当前字体覆盖表保留字符，把不支持字符替换成 `?`，并且按 UTF-8 字节边界裁剪到 firmware 字段上限。
- 这层逻辑要同时覆盖 snapshot 文本和 approval RPC 文本，不能只修 `normalize()`。

### 5. 生成文件对仓库可维护性的影响

一个 21,002 行的机器生成文件会直接纳入 Git 历史。每次字体参数微调（增删字符、调整 bpp、更换字号）都会产生同等量级的 diff，显著拖慢 `git clone`、`git blame` 和代码审查流程。

**建议**：长远来看，应将字体源文件（TTF）和转换脚本纳入仓库，在构建阶段（CMake 自定义命令）调用 `lv_font_conv` 生成 `.c` 文件，而不是提交生成产物。如果当前构建环境不方便安装 Node.js/lv_font_conv，至少应将生成脚本和参数记录在 `src/apps/app_home/src/generated/README.md` 中。

---

## 总结

本次提交的功能方向正确，解决了中文显示的刚需，但实现上留下了编译警告、体积管理和许可证合规的隐患。建议按优先级处理：

1. **立即修复** host 侧统一文本清洗和 UTF-8 安全裁剪，覆盖 snapshot 与 approval 两条出站路径。
2. **短期** 补充 Noto 字体的许可证与来源说明；清理 `normalizer.rs` 中退役的 `ascii_safe` 代码和测试。
3. **中期** 继续观察字体体积。如果未来进入 OTA 双分区或需要更大字集，再评估独立字体分区；本轮不必为了 50% 空闲量提前改分区。

---

*报告生成者：Kimi Code CLI*
*生成时间：2026-04-09T22:01+08:00*
