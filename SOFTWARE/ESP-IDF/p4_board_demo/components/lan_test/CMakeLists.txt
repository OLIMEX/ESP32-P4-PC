idf_component_register(
    SRCS "src/lan_test.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_event esp_eth lwip
)
