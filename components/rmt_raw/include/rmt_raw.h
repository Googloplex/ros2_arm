#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * rmt_raw — керування периферією RMT ESP32-S3 НАПРЯМУ через регістри.
 *
 * Жодного driver/rmt_*.h, жодного HAL, жодних rmt_new_* / rmt_transmit /
 * rmt_enable. Усе — тільки записи в регістри RMT + GPIO matrix / IO_MUX.
 *
 * Адреси й поля беруться з макросів soc/rmt_reg.h, soc/gpio_sig_map.h,
 * soc/gpio_reg.h, soc/io_mux_reg.h, soc/system_reg.h (звірено з TRM ESP32-S3,
 * розділ «RMT»). Використовується канал TX 0 (сигнал RMT_SIG_OUT0_IDX).
 */

/* Один RMT-символ: дві ділянки сигналу підряд (рівень + тривалість у тіках). */
typedef struct {
    uint16_t dur0;  /* тривалість 1-ї ділянки, тіки (15 біт, макс 32767) */
    uint8_t  lvl0;  /* рівень 1-ї ділянки (0/1)                           */
    uint16_t dur1;  /* тривалість 2-ї ділянки, тіки                       */
    uint8_t  lvl1;  /* рівень 2-ї ділянки (0/1)                           */
} rmt_item_t;

/*
 * Налаштувати канал 0 і вивести його на пін `gpio`.
 * `resolution_hz` — бажана частота тіка (напр. 1000000 → 1 тік = 1 мкс).
 * Дільники (глобальний SCLK_DIV_NUM + канальний DIV_CNT) підбираються
 * автоматично від джерела APB 80 МГц.
 */
void rmt_raw_init(int gpio, uint32_t resolution_hz);

/*
 * Записати готові символи у пам'ять каналу (через FIFO-регістр CHnDATA),
 * додати нуль-маркер кінця й запустити передачу. Блокує до завершення.
 */
void rmt_raw_send_items(const rmt_item_t *items, size_t count);

/*
 * Зручна обгортка: тривалості задаються в МІКРОСЕКУНДАХ, рівень чергується
 * починаючи від `start_level`. Мкс переводяться в тіки за розрішенням init.
 */
void rmt_raw_send_pulses(const uint32_t *durations_us, size_t count, int start_level);

/*
 * Меандр: частота (Гц), скважність (duty, %), кількість періодів.
 * Один період = один символ (high, потім low).
 */
void rmt_raw_square(uint32_t freq_hz, uint8_t duty_pct, uint32_t periods);

/* Дочекатися завершення поточної передачі опитуванням прапора TX_END. */
void rmt_raw_wait(void);

#ifdef __cplusplus
}
#endif
