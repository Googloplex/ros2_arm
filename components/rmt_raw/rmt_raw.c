/*
 * rmt_raw.c — реалізація драйвера RMT ESP32-S3 на «голих» регістрах.
 *
 * Канал: TX0. Джерело такту: APB 80 МГц. Символи пишемо через FIFO-регістр
 * RMT_CH0DATA_REG (кожен запис додає слово в пам'ять каналу з автоінкрементом
 * вказівника).
 *
 * ВАЖЛИВО про APB_FIFO_MASK. У TRM/soc це «маска» FIFO:
 *   APB_FIFO_MASK = 0  → доступ до пам'яті через FIFO (пишемо в CHnDATA);  ← нам це
 *   APB_FIFO_MASK = 1  → прямий доступ до RMT-RAM (FIFO вимкнено).
 * Значення після скидання = 0, тобто FIFO. Драйвер ESP-IDF навпаки ставить біт
 * у 1, бо працює напряму з RMTMEM. Ми ж свідомо тримаємо його у 0, щоб писати в
 * CHnDATA — тому в цьому місці ми ЧИСТИМО біт (а не ставимо його).
 */

#include "rmt_raw.h"

#include "soc/soc.h"             /* REG_READ/REG_WRITE/REG_SET_BIT/REG_SET_FIELD */
#include "soc/rmt_reg.h"         /* RMT_CH0*, RMT_SYS_CONF_REG, RMT_INT_*        */
#include "soc/gpio_reg.h"        /* GPIO_FUNCn_OUT_SEL_CFG_REG, GPIO_ENABLE_*    */
#include "soc/gpio_sig_map.h"    /* RMT_SIG_OUT0_IDX                             */
#include "soc/io_mux_reg.h"      /* PIN_FUNC_GPIO, PIN_FUNC_SELECT, ...          */
#include "soc/gpio_periph.h"     /* GPIO_PIN_MUX_REG[]                           */
#include "soc/system_reg.h"      /* SYSTEM_PERIP_CLK_EN0/RST, SYSTEM_RMT_*       */

/* -------- поточна конфігурація каналу -------- */
static int      s_gpio = -1;
static uint32_t s_res_hz = 1000000; /* частота тіка */

/* максимум ділянок, що влазять в один блок пам'яті (48 слів = 47 симв. + маркер) */
#define RMT_MAX_SEGMENTS 94
#define RMT_MAX_SYMBOLS  47

/* Упаковка одного 32-бітного слова RMT з двох ділянок. */
static inline uint32_t rmt_pack(uint16_t d0, uint8_t l0, uint16_t d1, uint8_t l1)
{
    return ((uint32_t)(d0 & 0x7FFF))
         | ((uint32_t)(l0 & 1u) << 15)
         | ((uint32_t)(d1 & 0x7FFF) << 16)
         | ((uint32_t)(l1 & 1u) << 31);
}

/* ---- крок 1: вивести вихід каналу 0 на пін через IO_MUX + GPIO matrix ---- */
static void rmt_route_pin(int gpio, uint32_t sig_idx)
{
    /* IO_MUX: обрати для піна функцію «проста GPIO», вимкнути вхід. */
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[gpio], PIN_FUNC_GPIO);
    PIN_INPUT_DISABLE(GPIO_PIN_MUX_REG[gpio]);

    /* GPIO matrix: під'єднати вихідний сигнал RMT до піна.
     * OEN_SEL=1 → дозвіл виходу береться з регістра GPIO_ENABLE (нижче). */
    REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + gpio * 4,
              (sig_idx & 0x1FF) | GPIO_FUNC0_OEN_SEL);

    /* Увімкнути драйвер виходу піна. */
    if (gpio < 32) {
        REG_WRITE(GPIO_ENABLE_W1TS_REG, 1u << gpio);
    } else {
        REG_WRITE(GPIO_ENABLE1_W1TS_REG, 1u << (gpio - 32));
    }
}

/*
 * Підбір двох дільників під бажане розрішення від джерела 80 МГц:
 *   f_тік = 80МГц / (SCLK_DIV_NUM + 1) / DIV_CNT
 * SCLK_DIV_NUM: 0..255 (глобальний), DIV_CNT: 1..255 (канальний).
 */
static void rmt_pick_dividers(uint32_t src_hz, uint32_t res_hz,
                              uint32_t *sclk_div_num, uint32_t *div_cnt)
{
    if (res_hz == 0) res_hz = 1000000;
    uint32_t total = src_hz / res_hz;
    if (total == 0) total = 1;

    uint32_t sd = 0;
    uint32_t dc = total;
    while (dc > 255 && sd < 255) {
        sd++;
        dc = total / (sd + 1);
    }
    if (dc > 255) dc = 255;
    if (dc == 0)  dc = 1;

    *sclk_div_num = sd;
    *div_cnt = dc;
}

void rmt_raw_init(int gpio, uint32_t resolution_hz)
{
    s_gpio = gpio;
    s_res_hz = resolution_hz ? resolution_hz : 1000000;

    /* 0. Увімкнути такт периферії RMT і зняти скидання (регістри SYSTEM). */
    REG_SET_BIT(SYSTEM_PERIP_CLK_EN0_REG, SYSTEM_RMT_CLK_EN);
    REG_CLR_BIT(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_RMT_RST);

    /* 1. Маршрутизація піна. */
    rmt_route_pin(gpio, RMT_SIG_OUT0_IDX);

    /* 2. Глобальне тактування (RMT_SYS_CONF_REG). */
    uint32_t sclk_div_num, div_cnt;
    rmt_pick_dividers(80000000u, s_res_hz, &sclk_div_num, &div_cnt);

    REG_SET_BIT(RMT_SYS_CONF_REG, RMT_CLK_EN);            /* такт регістрів    */
    REG_SET_BIT(RMT_SYS_CONF_REG, RMT_MEM_CLK_FORCE_ON);  /* такт пам'яті      */
    REG_CLR_BIT(RMT_SYS_CONF_REG, RMT_MEM_FORCE_PD);      /* пам'ять під живл. */
    REG_SET_FIELD(RMT_SYS_CONF_REG, RMT_SCLK_SEL, 1);     /* 1 = APB 80 МГц    */
    REG_SET_FIELD(RMT_SYS_CONF_REG, RMT_SCLK_DIV_NUM, sclk_div_num);
    REG_SET_FIELD(RMT_SYS_CONF_REG, RMT_SCLK_DIV_A, 0);
    REG_SET_FIELD(RMT_SYS_CONF_REG, RMT_SCLK_DIV_B, 0);
    REG_SET_BIT(RMT_SYS_CONF_REG, RMT_SCLK_ACTIVE);       /* активувати такт   */
    REG_CLR_BIT(RMT_SYS_CONF_REG, RMT_APB_FIFO_MASK);     /* 0 → FIFO (CHnDATA) */

    /* 3. Конфіг каналу 0 (RMT_CH0CONF0_REG). */
    REG_SET_FIELD(RMT_CH0CONF0_REG, RMT_DIV_CNT_CH0, div_cnt);
    REG_SET_FIELD(RMT_CH0CONF0_REG, RMT_MEM_SIZE_CH0, 1); /* один блок пам'яті */
    REG_CLR_BIT(RMT_CH0CONF0_REG, RMT_TX_CONTI_MODE_CH0); /* одноразово        */
    REG_CLR_BIT(RMT_CH0CONF0_REG, RMT_IDLE_OUT_LV_CH0);   /* рівень спокою = 0 */
    REG_SET_BIT(RMT_CH0CONF0_REG, RMT_IDLE_OUT_EN_CH0);   /* тримати спокій    */

    /* Скинути вказівник запису пам'яті. */
    REG_SET_BIT(RMT_CH0CONF0_REG, RMT_APB_MEM_RST_CH0);
    REG_CLR_BIT(RMT_CH0CONF0_REG, RMT_APB_MEM_RST_CH0);

    /* Застосувати зміни CONF0 (защіпка!). */
    REG_SET_BIT(RMT_CH0CONF0_REG, RMT_CONF_UPDATE_CH0);
}

/* Старт передачі: скинути прапор, скинути вказівник читання, TX_START, latch. */
static void rmt_start_tx(void)
{
    REG_WRITE(RMT_INT_CLR_REG, RMT_CH0_TX_END_INT_CLR);   /* стерти старий TX_END */

    REG_SET_BIT(RMT_CH0CONF0_REG, RMT_MEM_RD_RST_CH0);    /* вказівник читання→0  */
    REG_CLR_BIT(RMT_CH0CONF0_REG, RMT_MEM_RD_RST_CH0);

    REG_SET_BIT(RMT_CH0CONF0_REG, RMT_TX_START_CH0);       /* старт               */
    REG_SET_BIT(RMT_CH0CONF0_REG, RMT_CONF_UPDATE_CH0);    /* защіпка             */
}

void rmt_raw_wait(void)
{
    while (!(REG_READ(RMT_INT_RAW_REG) & RMT_CH0_TX_END_INT_RAW)) {
        /* активне опитування прапора завершення передачі */
    }
    REG_WRITE(RMT_INT_CLR_REG, RMT_CH0_TX_END_INT_CLR);
}

void rmt_raw_send_items(const rmt_item_t *items, size_t count)
{
    if (!items || count == 0) return;
    if (count > RMT_MAX_SYMBOLS) count = RMT_MAX_SYMBOLS;

    /* Скинути вказівник запису перед новою партією. */
    REG_SET_BIT(RMT_CH0CONF0_REG, RMT_APB_MEM_RST_CH0);
    REG_CLR_BIT(RMT_CH0CONF0_REG, RMT_APB_MEM_RST_CH0);

    for (size_t i = 0; i < count; i++) {
        REG_WRITE(RMT_CH0DATA_REG,
                  rmt_pack(items[i].dur0, items[i].lvl0,
                           items[i].dur1, items[i].lvl1));
    }
    /* Нуль-маркер кінця (duration0 = 0). */
    REG_WRITE(RMT_CH0DATA_REG, 0);

    rmt_start_tx();
    rmt_raw_wait();
}

/* Внутрішнє: список ділянок (рівень+тіки) → пари в символи → CHnDATA. */
static void rmt_send_segments(const uint16_t *dur, const uint8_t *lvl, size_t n)
{
    REG_SET_BIT(RMT_CH0CONF0_REG, RMT_APB_MEM_RST_CH0);
    REG_CLR_BIT(RMT_CH0CONF0_REG, RMT_APB_MEM_RST_CH0);

    for (size_t i = 0; i < n; i += 2) {
        uint16_t d0 = dur[i];  uint8_t l0 = lvl[i];
        uint16_t d1 = 0;       uint8_t l1 = 0;
        if (i + 1 < n) { d1 = dur[i + 1]; l1 = lvl[i + 1]; }
        REG_WRITE(RMT_CH0DATA_REG, rmt_pack(d0, l0, d1, l1));
    }
    REG_WRITE(RMT_CH0DATA_REG, 0);  /* маркер кінця */

    rmt_start_tx();
    rmt_raw_wait();
}

void rmt_raw_send_pulses(const uint32_t *durations_us, size_t count, int start_level)
{
    if (!durations_us || count == 0) return;

    static uint16_t seg_dur[RMT_MAX_SEGMENTS];
    static uint8_t  seg_lvl[RMT_MAX_SEGMENTS];
    size_t ns = 0;
    int level = start_level ? 1 : 0;

    for (size_t i = 0; i < count && ns < RMT_MAX_SEGMENTS; i++) {
        /* мкс → тіки: ticks = us * f_тік / 1e6 */
        uint64_t ticks = (uint64_t)durations_us[i] * s_res_hz / 1000000ULL;
        if (ticks == 0) ticks = 1;

        /* довші за 15 біт розбиваємо на кілька ділянок того ж рівня */
        while (ticks > 0 && ns < RMT_MAX_SEGMENTS) {
            uint16_t chunk = (ticks > 0x7FFF) ? 0x7FFF : (uint16_t)ticks;
            seg_dur[ns] = chunk;
            seg_lvl[ns] = (uint8_t)level;
            ns++;
            ticks -= chunk;
        }
        level ^= 1;
    }

    rmt_send_segments(seg_dur, seg_lvl, ns);
}

void rmt_raw_square(uint32_t freq_hz, uint8_t duty_pct, uint32_t periods)
{
    if (freq_hz == 0 || periods == 0) return;
    if (duty_pct > 100) duty_pct = 100;

    /* період у тіках */
    uint64_t period_ticks = (uint64_t)s_res_hz / freq_hz;
    if (period_ticks < 2) period_ticks = 2;

    uint32_t high = (uint32_t)(period_ticks * duty_pct / 100);
    uint32_t low  = (uint32_t)(period_ticks - high);
    if (high == 0) high = 1;
    if (low  == 0) low  = 1;
    if (high > 0x7FFF) high = 0x7FFF;
    if (low  > 0x7FFF) low  = 0x7FFF;

    /* один період = один символ; відправляємо партіями по блоку пам'яті */
    while (periods > 0) {
        size_t batch = (periods > RMT_MAX_SYMBOLS) ? RMT_MAX_SYMBOLS : periods;
        rmt_item_t items[RMT_MAX_SYMBOLS];
        for (size_t i = 0; i < batch; i++) {
            items[i].dur0 = (uint16_t)high; items[i].lvl0 = 1;
            items[i].dur1 = (uint16_t)low;  items[i].lvl1 = 0;
        }
        rmt_raw_send_items(items, batch);
        periods -= batch;
    }
}
