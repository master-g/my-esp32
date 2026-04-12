# Satoshi Slot 子设计方案

## 1. 目的

本文档细化主设计文档中的 `Satoshi Slot` 页面，目标是把下面这些问题一次性定死：

- 目标集合到底比较什么
- 私钥生成、地址派生和匹配的计算链路如何组织
- `self-test` 如何验证命中保存和提醒链路
- 命中后的保存行为在有无安全存储时如何退化

本文档是 [dashboard-design.md](../dashboard-design.md) 中 `Satoshi Slot` 设计的补充说明。

## 2. 设计边界

### 2.1 覆盖范围

- `Satoshi Slot` 页面状态机
- `bitcoin_service` 在该页面下的子职责
- 目标指纹集合格式
- 私钥生成、派生、公钥哈希匹配、自检、保存和提醒链路

### 2.2 非覆盖范围

- 构造现实可行的“中本聪私钥恢复”方案
- 联网查询链上余额
- 导出完整钱包文件
- 与真实 BTC 节点、矿池或签名服务交互

## 3. 目标模型

### 3.1 核心定义

`Satoshi Slot` 的比较对象不是“钱包本身”，而是离线预置的一组目标指纹。

v1 固定采用：

- `P2PKH_HASH160_UNCOMPRESSED`
- `P2PKH_HASH160_COMPRESSED`

这意味着：

- 对每个候选私钥，设备会派生压缩和非压缩公钥
- 分别计算对应的 `HASH160`
- 与本地目标集合做匹配

v1 不直接比较：

- WIF 字符串
- Base58 地址字符串
- 原始私钥文本
- 任意联网返回结果

### 3.2 目标集合格式

目标集合由离线工具预生成并随固件只读打包。单条记录定义为：

```c
typedef enum {
    SLOT_FP_P2PKH_UNCOMPRESSED = 0,
    SLOT_FP_P2PKH_COMPRESSED = 1,
} slot_fingerprint_kind_t;

typedef struct {
    uint16_t label_id;
    uint8_t kind;
    uint8_t hash160[20];
} slot_target_entry_t;
```

字段约束：

- `label_id`：用于映射到紧凑标签，例如 `SATOSHI_SET_A`
- `kind`：指纹类型，避免压缩/非压缩混淆
- `hash160`：20 字节比较值

设计原因：

- 不在 UI 层硬编码字符串地址
- 比较值固定长度，适合 Flash 常量表
- 压缩和非压缩显式区分，避免推断分支

### 3.3 目标集合组织

v1 采用：

- 一个只读生产集合 `slot_target_set_prod`
- 一个只读自检集合 `slot_target_set_selftest`

当前代码线的生产集合起步只收录 `Genesis Coinbase` 指纹，用于先把目标集合、比对管线和命中保存链路跑通；后续再扩展更多候选地址。

生产集合和自检集合必须完全分离：

- 生产集合不包含任何“保证命中”的测试项
- 自检集合只用于验证链路，不参与正常扫描

## 4. 服务拆分

`Satoshi Slot` 仍然通过 `bitcoin_service` 暴露能力，但内部需要拆成几个稳定子模块：

| 子模块 | 职责 |
|------|------|
| `slot_rng` | 生成候选私钥 |
| `slot_derive` | 派生公钥、计算 `HASH160` |
| `slot_targetset` | 加载和查询目标集合 |
| `slot_runner` | 批处理循环、速率统计、暂停恢复 |
| `slot_persist` | 命中记录保存 |
| `slot_alert` | 屏幕高亮、页面状态切换、事件通知 |

关键原则：

- UI 不直接做任何加密运算
- 比较逻辑不散落在页面代码里
- 命中保存和告警走同一条提交路径，不能一半在 UI、一半在 service

## 5. 状态模型

### 5.1 页面模式

```c
typedef enum {
    SLOT_MODE_NORMAL = 0,
    SLOT_MODE_SELFTEST,
} slot_mode_t;
```

### 5.2 页面状态

```c
typedef enum {
    SLOT_STATE_IDLE = 0,
    SLOT_STATE_RUNNING,
    SLOT_STATE_PAUSED,
    SLOT_STATE_HIT,
    SLOT_STATE_STORAGE_UNAVAILABLE,
    SLOT_STATE_ERROR,
} slot_state_t;
```

状态语义：

- `IDLE`：尚未开始
- `RUNNING`：正在扫描
- `PAUSED`：用户暂停或因页面离开前台而暂停
- `HIT`：检测到匹配并已冻结当前链路
- `STORAGE_UNAVAILABLE`：安全存储不满足要求，正常模式不可启动
- `ERROR`：派生、存储或内部断言失败

### 5.3 运行快照

页面只消费一个聚合快照：

```c
typedef struct {
    slot_state_t state;
    slot_mode_t mode;
    bool secure_storage_ready;
    uint64_t attempts;
    uint32_t keys_per_sec;
    uint32_t batch_size;
    uint32_t updated_at_epoch_s;
    uint8_t last_fp_kind;
    uint8_t last_hash160_prefix[4];
    uint16_t matched_label_id;
    bool hit_persisted;
} slot_snapshot_t;
```

说明：

- `last_hash160_prefix` 只用于 UI 预览，不暴露完整指纹
- `matched_label_id` 仅在命中后有效
- `hit_persisted` 用于区分“已命中但保存失败”和“已命中且保存成功”

## 6. 计算管线

### 6.1 正常模式

每个候选私钥都必须走同一条确定性链路：

1. 从 `esp_random()` 或等效 CSPRNG 收集 32 字节随机数
2. 若候选值不在 secp256k1 有效范围 `[1, n-1]` 内，则丢弃重试
3. 派生非压缩公钥和压缩公钥
4. 对两种公钥分别做 `SHA256 -> RIPEMD160`
5. 在生产目标集合中查询：
   - `SLOT_FP_P2PKH_UNCOMPRESSED`
   - `SLOT_FP_P2PKH_COMPRESSED`
6. 任一匹配则生成 hit record，停止扫描

### 6.2 查询策略

目标集合在 Flash 中保持按 `(kind, hash160)` 排序。

v1 查询方式固定为二分查找：

- 每个候选最多查两次
- 不引入 Bloom filter
- 不做多层缓存

这样做的原因：

- 目标集合预计较小，二分查找复杂度足够低
- 算法简单，便于验证正确性
- 避免额外 RAM 结构和误判路径

### 6.3 批处理和调度

扫描由独立 worker task 执行，不在 UI 线程中运行。

v1 运行参数固定为：

- 批大小：`256` keys / batch
- UI 更新频率：每 `500 ms` 聚合一次
- 每完成一个 batch 后主动 `yield`

约束：

- 页面离开前台时必须暂停
- 进入 `DIM` / `SLEEP` 时必须暂停
- `self-test` 和 `normal` 共用同一条派生与比较代码

## 7. Self-Test 设计

### 7.1 设计目标

`self-test` 必须验证真实链路，而不是单独写一条“伪命中”逻辑。

### 7.2 运行方式

v1 固定采用一个内置测试向量：

- `slot_selftest_privkey[32]`
- 由该私钥派生出的目标指纹进入 `slot_target_set_selftest`

进入 `self-test` 后：

1. 切换目标集合到 `slot_target_set_selftest`
2. 候选源从随机数切换为固定测试向量
3. 走完全相同的派生、比较、保存、告警流程
4. 结果被标记为 `self_test_hit`

当前代码线已经按这个方向接入真实派生与比较，不再使用伪命中分支。

### 7.3 约束

- 自检命中记录不得覆盖真实命中记录
- UI 必须显式标注 `SELF-TEST HIT`
- 自检完成后回到 `IDLE`

## 8. 命中后的保存与告警

### 8.1 命中记录

命中记录定义为：

```c
typedef struct {
    bool is_self_test;
    uint32_t created_at_epoch_s;
    uint16_t label_id;
    uint8_t kind;
    uint8_t hash160[20];
    uint8_t private_key[32];
} slot_hit_record_t;
```

### 8.2 安全存储策略

v1 采用强约束：

- 若未检测到安全存储就绪，则 `SLOT_MODE_NORMAL` 不可启动
- 此时页面进入 `STORAGE_UNAVAILABLE`
- 只有 `self-test` 允许继续运行

`secure_storage_ready` 的定义：

- NVS 可用
- NVS 加密已启用
- 若平台已启用 Flash encryption，则视为满足更高安全等级

当前代码线默认采用 ESP32-S3 的 HMAC-based NVS encryption 来满足这条门槛：

- `normal mode` 依赖默认 `nvs` 分区成功以加密模式初始化
- 不再把 Flash encryption 当作启动前提
- 首次启动时，若配置的 HMAC eFuse key slot 为空，ESP-IDF 可能会自动写入该 slot

### 8.3 保存策略

为避免真实命中被测试数据覆盖，存储分两类：

- `slot_real_hit_latest`
- `slot_selftest_hit_latest`

正常命中时：

1. 暂停扫描
2. 先写 `slot_real_hit_latest`
3. 写成功后再把 `hit_persisted=true`
4. 再触发 UI 告警和事件广播

如果保存失败：

- 状态仍进入 `HIT`
- UI 必须明确显示 `MATCHED BUT NOT SAVED`

### 8.4 告警链路

v1 命中后必须触发：

- 屏幕切到高亮态
- `Satoshi Slot` 页面进入全屏命中视图
- 事件总线发出 `APP_EVENT_DATA_BITCOIN`

可选扩展：

- 若未来存在通用主机提醒桥接，可追加外发提醒

v1 不要求把命中事件接到 `claude_bridge`。

## 9. 页面细化

### 9.1 主视图

页面主视图显示：

- `Satoshi Slot` 主标题
- 右上角小号状态标签，不再在标题下单独放一行大号状态字
- 当前状态
- 是否允许正常模式运行
- 已尝试次数
- 当前速度 `keys/s`
- 最近指纹前缀
- 目标集合版本或标签

主视图布局约束：

- 所有内容都必须落在 `640x172` 的逻辑 landscape 视口内
- 页面采用 `header / content / footer` 三段式布局
- 右侧命中/告警 panel 和底部 action row 必须完整可见，不得依赖裁切显示

### 9.2 交互

按钮固定为：

- `Start`
- `Pause`
- `Self-test`
- `Reset`
- `Confirm`（仅命中后显示）

规则：

- `Start` 只在 `secure_storage_ready=true` 且 `state in {IDLE, PAUSED}` 时可用
- `Self-test` 始终可用
- `Reset` 只清除计数器和页面快照，不清除已保存命中记录

### 9.3 命中视图

命中视图必须显示：

- `REAL HIT` 或 `SELF-TEST HIT`
- 命中标签
- 保存是否成功
- 手动确认按钮

确认后：

- 若是 `self-test`，回到 `IDLE`
- 若是真实命中，保持 `PAUSED`，不自动恢复扫描

## 10. 对外接口

`bitcoin_service` 至少提供：

```c
esp_err_t bitcoin_service_init(void);
const slot_snapshot_t *bitcoin_service_get_slot_snapshot(void);
esp_err_t bitcoin_service_start_slot(slot_mode_t mode);
void bitcoin_service_pause_slot(void);
void bitcoin_service_reset_slot_counters(void);
bool bitcoin_service_slot_has_real_hit(void);
```

接口原则：

- UI 总是拉快照，不直接拿内部 worker 状态
- `start_slot(NORMAL)` 在安全存储不可用时返回显式错误
- `self-test` 和 `normal` 共享同一接口，只由 mode 区分

## 11. 验收用例

### 11.1 正常链路

- 安全存储就绪时，点击 `Start` 能进入 `RUNNING`
- 页面前台连续运行时，`attempts` 和 `keys/s` 持续更新
- 页面切走或进入低功耗后自动暂停

### 11.2 安全退化

- 安全存储不可用时，`Start` 被禁止或返回显式错误
- 页面显示 `STORAGE_UNAVAILABLE`
- `Self-test` 仍然可执行

### 11.3 自检链路

- `Self-test` 必须通过真实派生和比较管线命中
- 生成 `self_test_hit` 记录
- UI 显示 `SELF-TEST HIT`

### 11.4 命中保存

- 正常模式命中后，扫描停止
- `slot_real_hit_latest` 写入成功
- UI 标记 `hit_persisted=true`

## 12. 假设

- v1 只比较预置的目标指纹集合，不联网下载目标列表
- v1 只支持基于 `HASH160` 的目标指纹匹配
- v1 不在页面中展示或导出完整私钥文本
- 命中概率现实中近乎为零，因此 `self-test` 是必需能力，不是调试附属品

---

*文档版本: 1.0*
*创建日期: 2026-04-07*
