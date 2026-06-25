#include "bootstrap.h"

#include "bsp_display.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BOOT_FAIL_RESTART_LIMIT 3
#define BOOT_FAIL_MAGIC 0xB00710ADu

static const char *TAG = "main";

// 跨重启保留的启动失败计数。RTC_NOINIT 段在复位后保留、冷启动后是垃圾值。
// 用 magic word 检测其有效性,而不依赖枚举复位原因(panic/WDT/brownout 易漏判)。
static RTC_NOINIT_ATTR uint32_t s_boot_fail_count;
static RTC_NOINIT_ATTR uint32_t s_boot_fail_magic;

void app_main(void)
{
    esp_err_t err = bootstrap_start();
    if (err == ESP_OK) {
        s_boot_fail_count = 0;
        s_boot_fail_magic = BOOT_FAIL_MAGIC;
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // 冷启动垃圾值在此归一化为本轮第一次失败;此后无论复位原因(主动 esp_restart、
    // panic、WDT)失败都累加,直到上限——保证有界,不会被 panic/WDT 误清零而无限重启。
    if (s_boot_fail_magic != BOOT_FAIL_MAGIC) {
        s_boot_fail_magic = BOOT_FAIL_MAGIC;
        s_boot_fail_count = 0;
    }
    s_boot_fail_count++;

    ESP_LOGE(TAG, "Bootstrap failed (attempt %lu): %s", (unsigned long)s_boot_fail_count,
             esp_err_to_name(err));

    // 显示就绪则画错误屏(醒目区别于黑屏);错误码已在上面打到串口。
    if (!bsp_display_show_fatal_screen()) {
        ESP_LOGE(TAG, "display not ready; error reported on serial only");
    }

    if (s_boot_fail_count < BOOT_FAIL_RESTART_LIMIT) {
        ESP_LOGW(TAG, "restarting to retry (%lu/%d)", (unsigned long)s_boot_fail_count,
                 BOOT_FAIL_RESTART_LIMIT);
        vTaskDelay(pdMS_TO_TICKS(3000)); // 让错误屏与日志可见
        esp_restart();
    }

    // 达到上限:停在错误屏/串口,不再无限重启(硬故障可读、不空耗)。
    ESP_LOGE(TAG, "restart limit reached; halting on error state");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
