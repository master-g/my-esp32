#include "bootstrap.h"

#include "bsp_display.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BOOT_FAIL_RESTART_LIMIT 3

static const char *TAG = "main";

// 跨重启保留的启动失败计数。RTC_NOINIT 段在软复位后保留、冷启动后是垃圾值,
// 故下面用复位原因归一化。
static RTC_NOINIT_ATTR uint32_t s_boot_fail_count;

void app_main(void)
{
    esp_err_t err = bootstrap_start();
    if (err == ESP_OK) {
        s_boot_fail_count = 0;
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // 仅当上次复位是本函数主动触发的软复位时才累加;其他原因(上电/panic/外部)
    // 视为本轮第一次失败,把垃圾计数归一化。
    if (esp_reset_reason() != ESP_RST_SW) {
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
