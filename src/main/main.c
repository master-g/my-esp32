#include "bootstrap.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = bootstrap_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bootstrap failed: %s", esp_err_to_name(err));
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
