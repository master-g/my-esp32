# WiFi 凭据管理分析与后续优化

## 当前实现

Wi-Fi 凭据链路已经不是旧的单 profile 方案了。

1. **Host 端** (`tools/esp32dash`)
   - 普通配置仍通过 `config.export` / `config.set_many` 处理
   - Wi-Fi profile 改走专门的 RPC：
     - `wifi.scan`
     - `wifi.profiles.list`
     - `wifi.profile.add`
     - `wifi.profile.remove`

2. **device_link**
   - 暴露上面这组 `wifi.*` 方法
   - `config.export` 继续返回通用配置（例如 time / weather），不再承载 Wi-Fi profile 列表

3. **net_manager**
   - 负责多 profile 存储、NVS 持久化、scan-and-match、候选网络切换和重连
   - `net_manager_add_or_update_profile()` / `net_manager_remove_profile()` 自己负责持久化，不再依赖调用方额外写 NVS

4. **Firmware settings UI**
   - `app_settings` + `service_settings` 已接入 firmware
   - on-device settings 已改成多级页面，和 host 侧 profile 流程保持同一套语义

## 已经落地的变化

### 1. 旧的 build-time Wi-Fi fallback 已经移除

仓库里已经没有 `CONFIG_DASH_WIFI_SSID` / `CONFIG_DASH_WIFI_PASSWORD`、`seed_credentials_from_defaults()`、`load_credentials_from_nvs()` 这类单 profile fallback 路径了。

设备现在以 NVS 中的已保存 profile 为准；没有 profile 时安静等待外部配置。

### 2. 多 profile 存储与自适应连接已经落地

当前实现已经具备：

- `NET_PROFILE_MAX = 5`
- 按 SSID 去重的 profile 存储
- 启动时 scan + match + 候选列表重建
- auth failure / 重连失败后的候选切换
- 删除当前 profile 后的连接协调

### 3. Host / device 协议已经拆成专门的 `wifi.*` RPC

这部分已经不再通过 `config.set_many` 的 `wifi.ssid` / `wifi.password` 承载。

`config.export` 也不再作为 Wi-Fi profile 的主要读取接口；host 现在直接用 `wifi.profiles.list`。

### 4. settings_service 的短期密码驻留窗口已收紧

`service_settings` 当前用队列异步处理保存/删除请求。队列里涉及明文密码的命令在处理完成后会 zeroize 并释放，不再长期留在队列内存里。

## 现行协议

### `wifi.scan`

请求：

```json
{ "method": "wifi.scan", "params": {} }
```

返回：

```json
{
  "aps": [
    {
      "ssid": "Office-5G",
      "rssi": -48,
      "auth_mode": "wpa2_psk",
      "auth_required": true
    }
  ]
}
```

### `wifi.profiles.list`

请求：

```json
{ "method": "wifi.profiles.list", "params": {} }
```

返回：

```json
{
  "profiles": [
    {
      "ssid": "Home-5G",
      "has_password": true,
      "hidden": false,
      "active": true
    }
  ]
}
```

### `wifi.profile.add`

请求：

```json
{
  "method": "wifi.profile.add",
  "params": {
    "ssid": "Office-5G",
    "password": "secret",
    "hidden": false
  }
}
```

语义：

- 同一 SSID 视为更新
- `password` **省略**：保留已存密码
- `password: ""`：显式保存空密码
- `hidden: true`：按 hidden network 处理

### `wifi.profile.remove`

请求：

```json
{
  "method": "wifi.profile.remove",
  "params": {
    "ssid": "Office-5G"
  }
}
```

## 还值得继续做的优化

### P1：缩短 net_manager 内部明文密码驻留时间

现在 `service_settings` 的队列内存已经会清零，但 `net_manager` 仍然把 profile 密码保存在运行时内存里，方便后续重连和候选切换。

这不是新漏洞，但如果后面想继续收紧暴露面，可以评估：

- 连接前按需从 NVS 读出密码
- 或者把活跃 profile 之外的密码从 RAM 中清掉

### P2：启用 NVS encryption

NVS 中的 profile 密码目前仍是明文。ESP-IDF 自带 NVS encryption，这块依然值得做，尤其是设备进入长期使用阶段以后。

### P3：清理剩余协议文档的历史叙述

当前这份文档已经对齐到现行实现，但其他设计文档如果仍在引用旧的 `config.set_many wifi.*` 心智，也该一起更新。

## 建议的验证方式

1. 通过 esp32dash 添加 2-3 个 profile，重启后检查是否连到最佳候选网络
2. 选择一个已存 profile，省略 `password` 更新一次，确认设备保留原密码
3. 对 open network 显式发送 `password: ""`，确认保存和连接行为正常
4. 删除当前活跃 profile，确认设备能重新协调连接状态
