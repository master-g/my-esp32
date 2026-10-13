# ESP32 Dashboard

一块常亮的桌面状态屏，加上 Mac 上的宿主代理 esp32dash。设备显示时间、天气、行情和 Claude Code 的运行状态；它只做通知，不参与任何授权决定。

## Language

### Claude 交互

**Pending interaction**:
一条正在等用户在 Mac 上处理的 Claude 交互。它有两个 kind：approval 和 prompt。同一时刻设备上最多显示一条。
_Avoid_: waiting、alert、overlay request、pending request

**Approval**:
pending interaction 的一个 kind，来自 Claude Code 的 tool permission 请求。它优先于 prompt 占用设备上的显示位置。
_Avoid_: permission、authorization

**Prompt**:
pending interaction 的一个 kind，来自 Elicitation 或 AskUserQuestion。
_Avoid_: question、input request

**Coarse type**:
pending interaction 在设备上显示的粗略类别，例如「Bash command」「File edit」「Question」。它不含命令、参数或选项内容。
_Avoid_: type label、tool name
