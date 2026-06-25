# 项目记忆 — esp32

> 跨会话的持久信息。完成任务后回写,约定见 [CLAUDE.md](./CLAUDE.md)。
> 长度上限约 400 行。超长时:**保留**「已验证的事实」与「下次运行」,**淘汰**最旧的「失败尝试」「上次会话」条目。
> 条目用 `- [YYYY-MM-DD]` 前缀,新条目追加在各小节**末尾**,便于按时间淘汰。

## 已验证的事实
<!-- 确认过的技术决策和约束。长期保留,不随压缩淘汰。 -->
- [2026-06-25] U1–U8 提交系列已完成"ESP32 = 只读授权通知器"的转向：设备只显示 Claude 交互的粗类型(权限/Elicitation/AskUserQuestion)，所有决策在 Mac 原生弹窗上做。`device_link` 只走单向 request/dismiss；入站决策帧、`claude.approve` RPC、审批 generation 校验已删除。详见 CLAUDE.md「Important Constraints」与 docs/。
- [2026-06-25] `protocol.error` 与串口行溢出保护与审批无关，是 load-bearing，勿回退。
- [2026-06-25] 锁顺序固定为 LVGL lock → service mutex；service 任务不得直接调 LVGL。

## 失败尝试
<!-- 踩过的坑、走不通的路径、被否决的方案及原因。超长时最旧条目优先淘汰。 -->

## 上次会话
<!-- 上次运行做了什么、停在何处。滚动记录,超长时最旧条目优先淘汰。 -->
- [2026-06-25] 用 /bootstrap-claude 初始化上下文：补全 CLAUDE.md 的技术栈/命令/代码风格/禁止文件/审查规则/项目记忆 六节，创建本文件。AGENTS.md 已是指向 CLAUDE.md 的 symlink。未改动任何源码。

## 下次运行
<!-- 计划要做的任务和优先级。长期保留,不随压缩淘汰。 -->
- [2026-06-25] 仓库尚无 CONTRIBUTING.md / CODEOWNERS / PR 模板，也未见固件单元测试；按需补充。
