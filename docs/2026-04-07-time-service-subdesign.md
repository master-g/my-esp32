# Time Service 子设计方案

## 1. 目的

本文档细化 `time_service`，目标是固定：

- 时间来源优先级
- RTC 与 NTP 的协作方式
- 对页面输出的快照格式
- 错误和退化行为

本文档是主设计中文件时间能力的补充说明。

## 2. 设计边界

### 2.1 覆盖范围

- RTC 读取与有效性判断
- NTP 校时后的系统时间更新
- 时间格式化输出

### 2.2 非覆盖范围

- 日历事件系统
- 用户自定义复杂日期格式

## 3. 数据来源优先级

固定优先级：

1. RTC 有效值
2. NTP 校时结果
3. 未同步占位态

原则：

- 系统启动后尽快显示 RTC 时间
- 联网后校时并回写 RTC
- 若 RTC 无效，则页面显示明确占位态

## 4. 快照模型

```c
typedef struct {
    bool rtc_valid;
    bool ntp_synced;
    uint32_t now_epoch_s;
    char hhmm[6];
    char date_text[24];
    char weekday_text[12];
} time_snapshot_t;
```

## 5. 对外接口

```c
esp_err_t time_service_init(void);
esp_err_t time_service_sync_ntp(void);
const time_snapshot_t *time_service_get_snapshot(void);
bool time_service_is_valid(void);
```

## 6. 退化与错误

- RTC 无效：返回占位态
- NTP 失败：保留 RTC 或旧值
- 无网络：继续使用 RTC

## 7. 验收用例

- 启动后 RTC 有效时立即可见时间
- NTP 成功后快照更新且 RTC 回写
- RTC 无效时返回 `--:--`

## 8. 假设

- v1 时区由系统统一配置
- 首页只需要分钟级刷新

---

*文档版本: 1.0*  
*创建日期: 2026-04-07*
