# Компонент rmt_raw — RMT на регістрах, без RMT-драйвера/HAL.
# У REQUIRES навмисно НЕМАЄ driver: потрібні лише заголовки soc (адреси/поля
# регістрів). esp_rom не потрібен — усе робиться прямими записами в регістри.
idf_component_register(
    SRCS "rmt_raw.c"
    INCLUDE_DIRS "include"
    REQUIRES soc
)
