# rmt_raw — драйвер RMT ESP32-S3 на «голих» регістрах (без HAL/драйвера)

Компонент ESP-IDF, що керує периферією **RMT** ESP32-S3 **тільки через записи в
регістри** з Technical Reference Manual. Жодного `driver/rmt_tx.h`,
`driver/rmt_encoder.h`, `rmt_new_*`, `rmt_transmit`, `rmt_enable` чи будь-якого
іншого RMT-драйвера/HAL. Дозволено лише:

- `REG_READ` / `REG_WRITE` / `REG_SET_BIT` / `REG_SET_FIELD` до регістрів RMT;
- регістри GPIO matrix / IO_MUX для виводу сигналу каналу на пін;
- опитування регістра `RMT_INT_RAW_REG` для очікування завершення.

Адреси й поля беруться з макросів `soc/rmt_reg.h`, `soc/gpio_reg.h`,
`soc/gpio_sig_map.h`, `soc/io_mux_reg.h`, `soc/system_reg.h` — так вони завжди
збігаються з TRM для цільового чипа. Використовується **TX-канал 0**
(вихідний сигнал `RMT_SIG_OUT0_IDX`).

## Структура

```
components/rmt_raw/
├── CMakeLists.txt          # REQUIRES soc   (жодного driver!)
├── include/rmt_raw.h       # публічний інтерфейс
└── rmt_raw.c               # реалізація на регістрах
main/
├── CMakeLists.txt
└── main.c                  # демо: сирі символи + імпульси + меандр
CMakeLists.txt              # кореневий проєкт
sdkconfig.defaults          # CONFIG_IDF_TARGET="esp32s3"
docs/                       # сюди — скріншоти логічного аналізатора
```

## Інтерфейс

```c
void rmt_raw_init(int gpio, uint32_t resolution_hz);
void rmt_raw_send_items(const rmt_item_t *items, size_t count);
void rmt_raw_send_pulses(const uint32_t *durations_us, size_t count, int start_level);
void rmt_raw_square(uint32_t freq_hz, uint8_t duty_pct, uint32_t periods);
void rmt_raw_wait(void);
```

`rmt_item_t` — один 32-бітний символ RMT (дві ділянки: рівень + тривалість у тіках):

```c
typedef struct {
    uint16_t dur0; uint8_t lvl0;   // 1-ша ділянка (15 біт тривалості, макс 32767)
    uint16_t dur1; uint8_t lvl1;   // 2-га ділянка
} rmt_item_t;
```

## Приклад використання

```c
#include "rmt_raw.h"

void app_main(void)
{
    // 1 тік = 1 мкс (джерело APB 80 МГц, дільники підбираються автоматично)
    rmt_raw_init(18, 1000000);

    // 1) готові символи
    const rmt_item_t seq[] = {
        { .dur0 = 10, .lvl0 = 1, .dur1 = 20, .lvl1 = 0 },  // ↑10мкс ↓20мкс
        { .dur0 = 30, .lvl0 = 1, .dur1 = 5,  .lvl1 = 0 },  // ↑30мкс ↓5мкс
    };
    rmt_raw_send_items(seq, 2);

    // 2) імпульси в мкс, рівень чергується від HIGH
    const uint32_t us[] = { 100, 50, 200, 50, 100 };
    rmt_raw_send_pulses(us, 5, 1);

    // 3) меандр 1 кГц, 50%, 20 періодів
    rmt_raw_square(1000, 50, 20);
}
```

## Тактування і розрахунок тіків

Дві сходинки ділення від джерела:

```
f_тік = f_джерела / (SCLK_DIV_NUM + 1) / DIV_CNT
```

- `f_джерела` = 80 МГц (APB, `SCLK_SEL = 1`);
- `SCLK_DIV_NUM` — глобальний дільник у `RMT_SYS_CONF_REG` (0..255);
- `DIV_CNT` — канальний дільник у `RMT_CH0CONF0_REG` (1..255).

Приклад для `resolution_hz = 1_000_000`: `SCLK_DIV_NUM = 0`, `DIV_CNT = 80`
→ 80 МГц / 1 / 80 = **1 МГц** → 1 тік = 1 мкс.

- `send_pulses`: `ticks = duration_us * f_тік / 1e6`.
- `square`: `period_ticks = f_тік / freq_hz`, `high = period_ticks * duty% / 100`,
  `low = period_ticks − high`.

Тривалість однієї ділянки — 15 біт (макс **32767** тіків). Довші інтервали в
`send_pulses` автоматично розбиваються на кілька ділянок того самого рівня.

## Послідовність роботи з регістрами

1. **SYSTEM**: увімкнути такт RMT (`SYSTEM_RMT_CLK_EN`), зняти скидання
   (`SYSTEM_RMT_RST`).
2. **Пін**: IO_MUX → функція GPIO; GPIO matrix → під'єднати `RMT_SIG_OUT0_IDX`
   до піна (`GPIO_FUNCn_OUT_SEL_CFG_REG`), увімкнути вихід (`GPIO_ENABLE`).
3. **`RMT_SYS_CONF_REG`**: джерело/дільник/такти + режим доступу до пам'яті.
4. **`RMT_CH0CONF0_REG`**: `DIV_CNT`, `MEM_SIZE=1`, рівень спокою, `APB_MEM_RST`,
   і обов'язково `CONF_UPDATE` (защіпка).
5. **Символи**: запис у `RMT_CH0DATA_REG` + нуль-маркер (`duration0 = 0`).
6. **Старт**: `MEM_RD_RST` → `TX_START` → `CONF_UPDATE`.
7. **Очікування**: опитування біта `CH0_TX_END` у `RMT_INT_RAW_REG`, потім
   скидання через `RMT_INT_CLR_REG`.

## Важлива примітка про `APB_FIFO_MASK`

Символи пишемо через FIFO-регістр `RMT_CH0DATA_REG` (кожен запис додає слово з
автоінкрементом вказівника). Для цього режиму біт `RMT_APB_FIFO_MASK` у
`RMT_SYS_CONF_REG` має бути **0**:

```
APB_FIFO_MASK = 0  → доступ до пам'яті через FIFO (пишемо в CHnDATA)   ← ми так
APB_FIFO_MASK = 1  → прямий доступ до RMT-RAM, FIFO вимкнено
```

Значення після скидання = 0 (тобто FIFO). Драйвер ESP-IDF, навпаки, ставить цей
біт у 1, бо працює напряму з `RMTMEM`. Тому в коді ми свідомо **чистимо** біт
(`REG_CLR_BIT`), а не ставимо — інакше записи в `CHnDATA` не потраплятимуть у
пам'ять каналу. Полярність звірено з `soc/rmt_reg.h` та `hal/rmt_ll.h`
(`rmt_ll_enable_mem_access_nonfifo`).

## Типові граблі (вже враховані в коді)

- **`CONF_UPDATE`** після зміни `CONF0` — без нього залізо не застосує значення.
- **Нуль-маркер** (`duration0 = 0`) наприкінці — інакше RMT не знає, де кінець.
- **`APB_MEM_RST`** перед записом партії; **`MEM_RD_RST`** перед стартом.
- **Обидва дільники** (`SCLK_DIV_NUM` + `DIV_CNT`) задають розрішення разом.

## Збірка і прошивка

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Перевірка логічним аналізатором

Підключіть щуп до **GPIO 18** і **спільну землю** аналізатора з платою.
Частота дискретизації — з великим запасом над частотою сигналу; почніть з
меандру 1 кГц, щоб легко рахувати поділки.

Очікувано:

| Що надсилаємо | Що має бути на аналізаторі |
|---|---|
| `seq` (сирі символи) | ↑10мкс ↓20мкс, ↑30мкс ↓5мкс, ↑15мкс ↓15мкс |
| `pulses_us = {100,50,200,50,100}`, start=1 | ↑100 ↓50 ↑200 ↓50 ↑100 (мкс) |
| `square(1000, 50, 20)` | період 1000 мкс, duty 50%, 20 періодів |
| `square(2000, 25, 20)` | період 500 мкс, duty 25%, 20 періодів |

Скріншоти з вимірами покладіть у `docs/` і додайте сюди:

```
docs/01_raw_items.png     — збіг сирих символів
docs/02_send_pulses.png   — тривалості імпульсів
docs/03_square_1k.png     — період і скважність меандру
```

## Ідеї на додаткові бали (зірочка)

- Завершення по **перериванню** `TX_END` через `esp_intr_alloc` замість опитування.
- Послідовність **довша за блок** (~48 символів): wrap-режим (`MEM_TX_WRAP_EN`)
  з дозаписом по порогу.
- **«Морзянка»**: крапки/тире/паузи як довільний патерн поверх `send_pulses`.
- Виміряти реальні тривалості й пояснити відхилення (округлення тіків, дільники).
