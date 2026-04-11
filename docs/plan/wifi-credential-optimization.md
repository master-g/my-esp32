# WiFi 凭据管理分析与优化建议

## 当前架构

凭据管理分三层：

1. **Host 端** (`tools/esp32dash`)：通过 `config set_many` JSON-RPC 发送 `{wifi.ssid, wifi.password}`
2. **device_link**：解析 RPC，将凭据写入 NVS namespace `"wifi"`，然后调用 `net_manager_apply_credentials()`
3. **net_manager**：从 NVS 加载凭据，管理 WiFi STA 连接，指数退避重连

另外 Kconfig 提供了 `CONFIG_DASH_WIFI_SSID` / `CONFIG_DASH_WIFI_PASSWORD` 作为开发时的编译期 fallback，首次启动时写入 NVS。

## 现有设计的问题

### 1. build-time fallback 已经没有存在意义

既然 SSID/密码只能从 esp32dash config 输入，Kconfig 里的默认值在生产环境毫无用处——设备出厂时 NVS 为空，等 host agent 配置即可。编译期硬编码凭据反而有泄露风险（会留在 firmware binary 里）。

`seed_credentials_from_defaults()` 和 `load_credentials_from_nvs()` 里那段 fallback 逻辑（200-205 行）可以删掉。开发调试如果需要，直接用 esp32dash config 配就行。

### 2. RAM 里明文保存密码

`s_wifi_password[65]` 是一个 static buffer，密码在整个运行期间以明文驻留在 RAM 中。虽然 `net_snapshot_t` 对外只暴露 `has_credentials` 和 SSID，不暴露密码本身，但 password buffer 一直留在 RAM 里。

考虑到 ESP32 没有 memory protection 且调试口可读 RAM，这不算严重漏洞，但可以优化：`apply_wifi_config()` 把密码填进 `wifi_config_t` 之后，可以 `memset_s` 清零 `s_wifi_password`。下次需要时再从 NVS 读回来。这样密码在 RAM 中的暴露窗口缩小到初始化瞬间。

### 3. NVS 明文存储

密码在 NVS 中是明文存储的。ESP-IDF 支持 NVS encryption（基于 ESP32 的 eFuse key），可以开启分区加密。对于这种不频繁读写的场景，加密的额外开销可以忽略。

### 4. `apply_credentials()` 没有持久化

`net_manager_apply_credentials()` 更新了 RAM 中的凭据并调用 `apply_wifi_config()`，但写入 NVS 是在 `device_link` 层做的（`apply_pending_config → write_namespace_strings`）。这个职责分离其实还好——device_link 负责持久化，net_manager 负责运行时状态——但接口上有点不清晰。如果将来有别的调用方绕过 device_link 直接调 `net_manager_apply_credentials()`，凭据会只存在于 RAM 里，掉电丢失。

可以在 `net_manager_apply_credentials()` 内部加一步 NVS 写入，让它自己保证持久化。或者至少在头文件注释里明确标注"调用方负责持久化"。

### 5. WiFi 存储模式可以去掉 RAM 模式的特殊处理

当前 `esp_wifi_set_storage(WIFI_STORAGE_RAM)` 意味着每次重启都要重新配置。既然凭据已经在 NVS 里了，可以直接用 `WIFI_STORAGE_FLASH` 让 ESP WiFi 库自己管理。不过这个改动影响较大（会改变 WiFi 库的行为路径），需要评估重连逻辑是否兼容，优先级不高。

## 建议的优化（按优先级）

### P0：删除 Kconfig fallback 机制
- 删除 `Kconfig` 中的 `DASH_WIFI_SSID` 和 `DASH_WIFI_PASSWORD`
- 删除 `seed_credentials_from_defaults()` 函数
- 删除 `load_credentials_from_nvs()` 中 200-205 行的 fallback 逻辑
- 未配置凭据时设备安静等待，日志里一条 warning 就够了

### P1：使用后清零 RAM 中的密码
- 在 `apply_wifi_config()` 末尾 `mbedtls_platform_zeroize(s_wifi_password, sizeof(s_wifi_password))`
- `net_manager_apply_credentials()` 在调用 `apply_wifi_config()` 之前从 NVS 重读一次密码（或让调用方保证 RAM 中有密码时才调）

### P2：启用 NVS encryption
- 在 `partitions.csv` 中添加 NVS key partition
- 使用 `idf.py nvs-key-gen` 生成加密 key 并烧录到 eFuse
- 代码改动很小，主要是 partition table 和 provisioning 流程

### P3：明确 `apply_credentials` 的持久化契约
- 在 `net_manager.h` 的 `net_manager_apply_credentials` 注释中写明"不负责 NVS 持久化"
- 或者在函数内部自己写 NVS

## P4：多 profile 存储与自适应连接

### 背景

当前只存一组 SSID/password，设备移动到不同环境需要手动切换。目标：存储多份凭据，启动时自动选信号最好的已知网络。

### NVS 存储结构

在 `"wifi"` namespace 下，从单一 key 改为带索引的 key scheme：

```
"active"    → u8, 当前活跃 profile 索引 (0-based), 0xFF 表示无
"ssid_0"    → string, profile 0 的 SSID
"pass_0"    → string, profile 0 的密码
"ssid_1"    → string, profile 1 的 SSID
"pass_1"    → string, profile 1 的密码
...
"ssid_N"    → string, profile N 的 SSID
"pass_N"    → string, profile N 的密码
```

profile 数量上限定义为编译时常量 `NET_PROFILE_MAX = 5`。5 个 profile 的存储开销约 5 × (33 + 65 + NVS overhead) ≈ 700 bytes，24KB NVS 完全够用。用 SSID 做 profile 的唯一标识，同一 SSID 更新密码时覆盖旧条目，不暴露索引给 host 端。

### Host 端协议扩展

`config.set_many` 新增 key：

```json
// 添加/更新 profile（ssid 相同时覆盖密码）
{ "items": [
    {"key": "wifi.add_ssid", "value": "Office-5G"},
    {"key": "wifi.add_password", "value": "secret"}
]}

// 删除指定 profile
{ "items": [
    {"key": "wifi.remove_ssid", "value": "Office-5G"}
]}

// 原有的 wifi.ssid + wifi.password 保持向后兼容，等同于 add
```

`config.export` 返回的 wifi 段改为：

```json
{
  "wifi": {
    "profiles": [
      {"ssid": "Home-5G", "has_password": true},
      {"ssid": "Office", "has_password": true}
    ],
    "active": "Home-5G"
  }
}
```

### 自适应连接策略

启动时选择流程：

```
net_manager_start()
  │
  ├─ load_profiles_from_nvs()      // 加载所有 profile 到 RAM
  │
  ├─ scan_access_points()          // 主动扫描一次
  │
  ├─ match_profiles(scan_results)  // 扫描结果与已知 profile 交叉匹配
  │   └─ 按信号强度排序，选 RSSI 最强的
  │
  ├─ connect(best_match)
  │
  └─ 如果连接失败（auth fail / timeout）
      └─ 尝试下一个匹配的 profile
      └─ 全部失败 → 指数退避重试
```

运行时断线重连改为：

1. auth failure → 当前 profile 标记为 failed，尝试下一个匹配的 profile
2. 其他原因（信号弱、AP 消失）→ 正常重连当前 profile
3. 重连 N 次失败后 → 触发 scan → 重新匹配
4. 所有 profile 都不可用 → 慢退避等待

### RAM 中的数据结构

```c
#define NET_PROFILE_MAX 5

typedef struct {
    char ssid[NET_MANAGER_SSID_MAX];
    char password[NET_MANAGER_PASSWORD_MAX];
} wifi_profile_t;

// net_manager 内部 static
static wifi_profile_t s_profiles[NET_PROFILE_MAX];
static uint8_t s_profile_count;
static uint8_t s_active_profile;
```

### 公共接口改动

`net_manager.h` 中：

```c
// 替代原来的 net_manager_apply_credentials
esp_err_t net_manager_add_profile(const char *ssid, const char *password);
esp_err_t net_manager_remove_profile(const char *ssid);
uint8_t net_manager_get_profile_count(void);
void net_manager_get_profiles(net_credentials_summary_t *summaries, size_t max, size_t *out_count);
```

### esp32dash config_ui 适配

WiFi 配置菜单改为：

```
WiFi Profiles:
  1. Home-5G  ✓ (connected)
  2. Office
  [A] Add profile
  [D] Delete profile
  [Q] Back
```

添加 profile 时可选先 scan 再选择，免去手动输入 SSID。

### 改动范围

| 文件 | 改动 |
|------|------|
| `src/components/net_manager/include/net_manager.h` | 新增 profile 相关类型和函数声明 |
| `src/components/net_manager/src/net_manager.c` | 多 profile 存储、scan-and-match、有序重连 |
| `src/components/net_manager/Kconfig` | 可选：`NET_PROFILE_MAX` 编译期配置 |
| `src/components/device_link/src/device_link.c` | config.set_many / config.export 适配 |
| `tools/esp32dash/src/config_ui.rs` | WiFi profile 管理菜单 |
| `tools/esp32dash/src/model.rs` | 协议结构体扩展 |

### 验证方式

1. 通过 esp32dash config 添加 2-3 个 profile，重启后检查是否选择信号最强的连接
2. 断开当前 AP，确认设备自动切换到下一个已知网络
3. 所有 AP 都不可达时，确认指数退避重试正常
4. 清空 NVS 后重启，确认无崩溃、日志显示 "no profiles configured"
