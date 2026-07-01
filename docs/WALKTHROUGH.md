# Хід роботи: як робив драйвер RMT по TRM (крок за кроком)

Це журнал виконання ДЗ: який розділ Technical Reference Manual ESP32-S3
відкривав, що він означає, і що з цього народилось у коді
(`components/rmt_raw/rmt_raw.c`). Мета — довести, що в драйвері немає магії: усе
це записи в регістри за адресами з мануала.

> Правило: **тільки регістри**. Жодного `driver/rmt_tx.h`, HAL, `rmt_new_*`,
> `rmt_transmit`, `rmt_enable`. Дозволено `REG_READ/REG_WRITE` до RMT, регістри
> GPIO matrix / IO_MUX і опитування прапорів.

Джерела, з якими звіряв кожен крок:
- TRM ESP32-S3, розділ **«RMT (Remote Control Transceiver)»** (Functional
  Description + Register Summary + описи бітів).
- Згенеровані з того ж TRM заголовки: `soc/rmt_reg.h`, `soc/rmt_struct.h`.

---

## Крок 0. «System Registers» → тактування периферії

**Що читав.** Розділ про системні регістри: кожна периферія має біт увімкнення
такту в `SYSTEM_PERIP_CLK_EN0_REG` і біт скидання в `SYSTEM_PERIP_RST_EN0_REG`.
Після ресету чипа багато периферій **клок-гейтнуті** (такт вимкнено) — доки не
увімкнеш такт, записи в її регістри навіть не «застрягають».

**Що означає.** Перш ніж чіпати будь-який регістр RMT, треба подати на блок такт
і зняти скидання. Це та сама дисципліна, що і з іншими периферіями.

**Що зробив.** На самому початку `rmt_raw_init` (перед усім RMT):

```c
REG_SET_BIT(SYSTEM_PERIP_CLK_EN0_REG, SYSTEM_RMT_CLK_EN); // подати такт на RMT
REG_CLR_BIT(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_RMT_RST);    // зняти скидання
```

---

## Крок 1. «RMT → Overview / Architecture» → як влаштований блок

**Що читав.** Огляд RMT: є кілька каналів (на S3 — 4 TX + 4 RX), кожен має свій
блок пам'яті. Дані — це масив 32-бітних «символів» у RAM каналу. Передавач читає
символи підряд і формує на виході рівні заданої тривалості **апаратно**, без
участі процесора → без джитера.

**Що означає.** Мій план: узяти **канал 0 (TX)**, налаштувати його тактування й
пам'ять, залити символи і натиснути «старт». Уся робота — навколо двох головних
регістрів каналу (`CHnDATA` для даних і `CHnCONF0` для керування) плюс
глобального `SYS_CONF`.

**Що зробив.** Зафіксував у коді константи каналу і межу пам'яті:

```c
#define RMT_MAX_SYMBOLS  47   // 48 слів на блок S3 − 1 нуль-маркер
#define RMT_MAX_SEGMENTS 94   // 47 символів × 2 ділянки
```

---

## Крок 2. «IO MUX and GPIO Matrix» → вивід сигналу каналу на пін

**Що читав.** Розділ IO MUX / GPIO Matrix: вихід периферії не з'являється на піні
сам. Треба (а) в **IO_MUX** обрати для піна функцію «проста GPIO», (б) в **GPIO
matrix** під'єднати до піна вихідний сигнал периферії через
`GPIO_FUNCn_OUT_SEL_CFG_REG` (поле `OUT_SEL` = індекс сигналу), (в) увімкнути
драйвер виходу піна. Індекс вихідного сигналу каналу 0 RMT — `RMT_SIG_OUT0_IDX`
(з `soc/gpio_sig_map.h`).

**Що означає.** Це та сама GPIO matrix, що ми вже вміємо — просто тепер за нею
стоїть RMT. `OEN_SEL = 1` означає «дозвіл виходу бери з регістра `GPIO_ENABLE`»,
тому додатково піднімаю відповідний біт `GPIO_ENABLE_W1TS`.

**Що зробив.** Функція `rmt_route_pin`:

```c
PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[gpio], PIN_FUNC_GPIO); // IO_MUX: функція GPIO
PIN_INPUT_DISABLE(GPIO_PIN_MUX_REG[gpio]);              // тільки вихід

REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + gpio * 4,        // GPIO matrix:
          (sig_idx & 0x1FF) | GPIO_FUNC0_OEN_SEL);      // сигнал RMT + OEN_SEL

if (gpio < 32) REG_WRITE(GPIO_ENABLE_W1TS_REG,  1u << gpio);        // увімкнути
else           REG_WRITE(GPIO_ENABLE1_W1TS_REG, 1u << (gpio - 32)); // вихід
```

---

## Крок 3. «RMT → Clock» + `RMT_SYS_CONF_REG` → глобальне тактування

**Що читав.** Функціональний опис тактування і опис `RMT_SYS_CONF_REG`. Частота
тіка формується двома дільниками:

```
f_тік = f_джерела / (SCLK_DIV_NUM + 1) / DIV_CNT
```

Поля `SYS_CONF` (звірено з `rmt_struct.h`):
- `SCLK_SEL` [25:24], **default 1 = APB 80 МГц** (3 = XTAL 40 МГц);
- `SCLK_DIV_NUM` [11:4] — глобальний дільник;
- `SCLK_ACTIVE` (26), `CLK_EN` (31), `MEM_CLK_FORCE_ON` (1) — вмикання тактів;
- `APB_FIFO_MASK` (0) — режим доступу до пам'яті (див. крок 5).

**Що означає.** Беру джерело APB (80 МГц) і підбираю два дільники під бажане
розрішення (`resolution_hz`). Для 1 МГц: `SCLK_DIV_NUM=0`, `DIV_CNT=80` → 1 тік =
1 мкс — зручно рахувати на аналізаторі.

**Що зробив.** Підбір дільників + запис `SYS_CONF`:

```c
static void rmt_pick_dividers(uint32_t src_hz, uint32_t res_hz,
                              uint32_t *sclk_div_num, uint32_t *div_cnt) {
    uint32_t total = src_hz / res_hz;          // повний коефіцієнт ділення
    uint32_t sd = 0, dc = total;
    while (dc > 255 && sd < 255) { sd++; dc = total / (sd + 1); } // розкид по двох
    *sclk_div_num = sd; *div_cnt = (dc ? dc : 1);
}
```
```c
REG_SET_BIT (RMT_SYS_CONF_REG, RMT_CLK_EN);
REG_SET_BIT (RMT_SYS_CONF_REG, RMT_MEM_CLK_FORCE_ON);
REG_CLR_BIT (RMT_SYS_CONF_REG, RMT_MEM_FORCE_PD);
REG_SET_FIELD(RMT_SYS_CONF_REG, RMT_SCLK_SEL, 1);          // 1 = APB 80 МГц
REG_SET_FIELD(RMT_SYS_CONF_REG, RMT_SCLK_DIV_NUM, sclk_div_num);
REG_SET_BIT (RMT_SYS_CONF_REG, RMT_SCLK_ACTIVE);
```

---

## Крок 4. `RMT_CHnCONF0_REG` → головний регістр каналу

**Що читав.** Опис `RMT_CH0CONF0_REG` (звірено з `rmt_reg.h`, біти збіглися):

| Поле | Біт | Призначення |
|---|---|---|
| `TX_START` | 0 | старт передачі |
| `MEM_RD_RST` | 1 | скинути вказівник **читання** |
| `APB_MEM_RST` | 2 | скинути вказівник **запису** |
| `TX_CONTI_MODE` | 3 | циклічний режим (нам 0) |
| `IDLE_OUT_LV` / `IDLE_OUT_EN` | 5 / 6 | рівень і утримання спокою |
| `DIV_CNT` | 8 | канальний дільник такту |
| `MEM_SIZE` | 16 | скільки блоків пам'яті у каналу |
| `CONF_UPDATE` | 24 | **защіпка**: застосувати зміни CONF0 |

**Що означає — головна пастка.** `CONF0` — з **защіпкою**: поки не виставиш
`CONF_UPDATE`, залізо НЕ застосує нові значення полів. Це найчастіша причина
«нічого не працює».

**Що зробив.** Кінець `rmt_raw_init`:

```c
REG_SET_FIELD(RMT_CH0CONF0_REG, RMT_DIV_CNT_CH0, div_cnt); // канальний дільник
REG_SET_FIELD(RMT_CH0CONF0_REG, RMT_MEM_SIZE_CH0, 1);      // один блок пам'яті
REG_CLR_BIT (RMT_CH0CONF0_REG, RMT_TX_CONTI_MODE_CH0);     // одноразово
REG_CLR_BIT (RMT_CH0CONF0_REG, RMT_IDLE_OUT_LV_CH0);       // спокій = 0
REG_SET_BIT (RMT_CH0CONF0_REG, RMT_IDLE_OUT_EN_CH0);       // тримати спокій
REG_SET_BIT (RMT_CH0CONF0_REG, RMT_APB_MEM_RST_CH0);       // скинути запис-вказівник
REG_CLR_BIT (RMT_CH0CONF0_REG, RMT_APB_MEM_RST_CH0);
REG_SET_BIT (RMT_CH0CONF0_REG, RMT_CONF_UPDATE_CH0);       // ← защіпка!
```

---

## Крок 5. Формат символу + `APB_FIFO_MASK` → куди і як писати дані

**Що читав.** Формат 32-бітного символу з TRM:

```
[14:0]  duration0   (15 біт, макс 32767 тіків)
[15]    level0
[30:16] duration1
[31]    level1
```
`duration0 = 0` → **маркер кінця** передачі.

І — найважливіше уточнення цього ДЗ — дослівний опис поля `APB_FIFO_MASK` з
`rmt_struct.h` (згенеровано з TRM v1.8):

```c
/** apb_fifo_mask : R/W; bitpos: [0]; default: 0;
 *  1'h1: access memory directly.   1'h0: access memory by FIFO.
 */
```

**Що означає.** Щоб додавати символи простим записом у `RMT_CHnDATA_REG` (FIFO з
автоінкрементом вказівника), потрібен **FIFO-режим**, тобто `APB_FIFO_MASK = 0`
(і це значення за замовчуванням). `= 1` навпаки вимикає FIFO і дає прямий доступ
до RAM. У тексті ДЗ це місце сформульовано інвертовано («біт вмикає FIFO») — я
виправив за мануалом і в коді **чищу** біт:

```c
REG_CLR_BIT(RMT_SYS_CONF_REG, RMT_APB_FIFO_MASK); // 0 → доступ через FIFO (CHnDATA)
```

**Що зробив.** Упаковка символу один-в-один за розкладкою бітів:

```c
static inline uint32_t rmt_pack(uint16_t d0, uint8_t l0, uint16_t d1, uint8_t l1) {
    return ((uint32_t)(d0 & 0x7FFF))
         | ((uint32_t)(l0 & 1u) << 15)
         | ((uint32_t)(d1 & 0x7FFF) << 16)
         | ((uint32_t)(l1 & 1u) << 31);
}
```

---

## Крок 6. Запис партії символів + нуль-маркер

**Що читав.** Порядок роботи з пам'яттю: перед новою партією скинути вказівник
запису (`APB_MEM_RST`), інакше писатиметься поверх старого сміття; наприкінці —
обов'язковий нуль-символ (`duration0 = 0`).

**Що означає.** `send_items` просто ллє готові символи у `CHnDATA` й додає нуль.

**Що зробив.**

```c
REG_SET_BIT(RMT_CH0CONF0_REG, RMT_APB_MEM_RST_CH0);  // скинути запис-вказівник
REG_CLR_BIT(RMT_CH0CONF0_REG, RMT_APB_MEM_RST_CH0);
for (size_t i = 0; i < count; i++)
    REG_WRITE(RMT_CH0DATA_REG, rmt_pack(items[i].dur0, items[i].lvl0,
                                        items[i].dur1, items[i].lvl1));
REG_WRITE(RMT_CH0DATA_REG, 0);                        // нуль-маркер кінця
```

---

## Крок 7. Старт передачі

**Що читав.** Опис бітів старту: перед пуском скинути вказівник **читання**
(`MEM_RD_RST`), потім `TX_START`, і застосувати через `CONF_UPDATE`.

**Що зробив.** `rmt_start_tx`:

```c
REG_WRITE  (RMT_INT_CLR_REG, RMT_CH0_TX_END_INT_CLR); // стерти старий прапор
REG_SET_BIT(RMT_CH0CONF0_REG, RMT_MEM_RD_RST_CH0);    // вказівник читання → 0
REG_CLR_BIT(RMT_CH0CONF0_REG, RMT_MEM_RD_RST_CH0);
REG_SET_BIT(RMT_CH0CONF0_REG, RMT_TX_START_CH0);      // старт
REG_SET_BIT(RMT_CH0CONF0_REG, RMT_CONF_UPDATE_CH0);   // защіпка
```

---

## Крок 8. «RMT → Interrupts» → очікування завершення

**Що читав.** `RMT_INT_RAW_REG` містить сирі прапори; біт `CHn_TX_END`
піднімається, коли передачу завершено. Скидається записом у `RMT_INT_CLR_REG`.
Для каналу 0 обидва біти — нульові (звірено).

**Що означає.** Без переривань — просто опитую прапор у циклі (обов'язкова
частина; завершення по перериванню — на зірочку).

**Що зробив.** `rmt_raw_wait`:

```c
while (!(REG_READ(RMT_INT_RAW_REG) & RMT_CH0_TX_END_INT_RAW)) { }
REG_WRITE(RMT_INT_CLR_REG, RMT_CH0_TX_END_INT_CLR);
```

---

## Крок 9. Обгортки: `send_pulses` (мкс→тіки) і `square` (частота/скважність)

**Що читав.** Ту саму формулу тіка з кроку 3 і межу 15 біт (32767) на одну
ділянку з кроку 5.

**Що означає.**
- `send_pulses`: `ticks = duration_us × f_тік / 1e6`; рівень чергується від
  `start_level`; тривалості довші за 32767 тіків розбиваю на кілька ділянок того
  ж рівня.
- `square`: `period_ticks = f_тік / freq_hz`, `high = period_ticks × duty% /
  100`, `low = period_ticks − high`; один період = один символ `{high,1, low,0}`;
  довгі серії шлю партіями по блоку пам'яті.

**Що зробив (ядро `send_pulses`).**

```c
uint64_t ticks = (uint64_t)durations_us[i] * s_res_hz / 1000000ULL; // мкс → тіки
while (ticks > 0 && ns < RMT_MAX_SEGMENTS) {
    uint16_t chunk = (ticks > 0x7FFF) ? 0x7FFF : (uint16_t)ticks;   // 15-біт межа
    seg_dur[ns] = chunk; seg_lvl[ns] = level; ns++;
    ticks -= chunk;
}
level ^= 1;                                                          // чергування
```

---

## Крок 10. Перевірка логічним аналізатором

**Що читав.** Підказки з ДЗ: частота дискретизації з запасом; **спільна земля**
аналізатора з платою; починати з низької частоти (1 кГц), щоб легко рахувати.

**Що робив.** Прошив демо (`main/main.c`), щуп на **GPIO 18**, земля спільна.
Очікувані форми:

| Що надсилаю | Що має бути на екрані |
|---|---|
| `seq` (сирі символи) | ↑10мкс ↓20, ↑30 ↓5, ↑15 ↓15 |
| `pulses_us={100,50,200,50,100}`, start=1 | ↑100 ↓50 ↑200 ↓50 ↑100 (мкс) |
| `square(1000, 50, 20)` | період 1000 мкс, duty 50%, 20 періодів |
| `square(2000, 25, 20)` | період 500 мкс, duty 25% |

Скріншоти з вимірами — у цій же теці `docs/` (`01_raw_items.png` …).

---

## Підсумкова таблиця «регістр → навіщо → де в коді»

| Регістр / поле | Навіщо | Функція в коді |
|---|---|---|
| `SYSTEM_PERIP_CLK_EN0` / `RST_EN0` | подати такт RMT, зняти скидання | `rmt_raw_init` |
| `GPIO_FUNCn_OUT_SEL_CFG` + IO_MUX + `GPIO_ENABLE` | вивести канал на пін | `rmt_route_pin` |
| `RMT_SYS_CONF` (`SCLK_*`, `CLK_EN`, `APB_FIFO_MASK`) | джерело/дільник/FIFO | `rmt_raw_init` |
| `RMT_CH0CONF0` (`DIV_CNT`,`MEM_SIZE`,`IDLE_*`,`CONF_UPDATE`) | конфіг каналу | `rmt_raw_init` |
| `RMT_CH0DATA` | заливати символи (FIFO) | `send_items` / `send_segments` |
| `RMT_CH0CONF0` (`MEM_RD_RST`,`TX_START`,`CONF_UPDATE`) | старт | `rmt_start_tx` |
| `RMT_INT_RAW` / `RMT_INT_CLR` (`CH0_TX_END`) | дочекатись кінця | `rmt_raw_wait` |

## Головні висновки (граблі, на яких можна було застрягти)

1. **`CONF_UPDATE`** — без нього зміни `CONF0` не застосовуються. Найчастіша
   причина «код правильний, а нічого не міняється».
2. **Полярність `APB_FIFO_MASK`** — для запису через `CHnDATA` біт має бути **0**
   (FIFO), а не 1. Перевірив по дослівному опису поля в мануалі.
3. **Нуль-маркер** (`duration0 = 0`) — без нього RMT не знає, де кінець.
4. **Два скидання пам'яті** — `APB_MEM_RST` перед записом, `MEM_RD_RST` перед
   стартом.
5. **Два дільники** — розрішення задають `SCLK_DIV_NUM` і `DIV_CNT` разом.
6. **Такт периферії** — спершу увімкнути RMT у `SYSTEM_PERIP_CLK_EN0`, інакше
   регістри «мертві».
