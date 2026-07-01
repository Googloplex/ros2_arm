/*
 * Демо для компонента rmt_raw (RMT ESP32-S3 на регістрах, без драйвера/HAL).
 *
 * Що показуємо на логічному аналізаторі (підключати до OUT_GPIO, спільна земля):
 *   1) задану послідовність «сирих» символів rmt_item_t;
 *   2) послідовність імпульсів у мікросекундах (send_pulses);
 *   3) меандр відомої частоти й скважності (square).
 *
 * Розрішення 1 МГц → 1 тік = 1 мкс, тож усі тіки читаються як мікросекунди.
 */

#include "rmt_raw.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define OUT_GPIO 18   /* пін виходу RMT-каналу 0 */

static const char *TAG = "rmt_raw_demo";

void app_main(void)
{
    rmt_raw_init(OUT_GPIO, 1000000); /* 1 тік = 1 мкс */
    ESP_LOGI(TAG, "RMT raw init on GPIO %d, 1 MHz tick", OUT_GPIO);

    /* Відома послідовність сирих символів:
     *   high 10мкс / low 20мкс, high 30мкс / low 5мкс, high 15мкс / low 15мкс */
    const rmt_item_t seq[] = {
        { .dur0 = 10, .lvl0 = 1, .dur1 = 20, .lvl1 = 0 },
        { .dur0 = 30, .lvl0 = 1, .dur1 = 5,  .lvl1 = 0 },
        { .dur0 = 15, .lvl0 = 1, .dur1 = 15, .lvl1 = 0 },
    };

    /* Імпульси в мкс, рівень чергується від HIGH: 100↑ 50↓ 200↑ 50↓ 100↑ */
    const uint32_t pulses_us[] = { 100, 50, 200, 50, 100 };

    while (1) {
        rmt_raw_send_items(seq, sizeof(seq) / sizeof(seq[0]));
        vTaskDelay(pdMS_TO_TICKS(50));

        rmt_raw_send_pulses(pulses_us, sizeof(pulses_us) / sizeof(pulses_us[0]), 1);
        vTaskDelay(pdMS_TO_TICKS(50));

        /* Меандр 1 кГц, 50%, 20 періодів — легко рахувати поділки. */
        rmt_raw_square(1000, 50, 20);
        vTaskDelay(pdMS_TO_TICKS(50));

        /* Меандр 2 кГц, 25% (для перевірки скважності), 20 періодів. */
        rmt_raw_square(2000, 25, 20);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
