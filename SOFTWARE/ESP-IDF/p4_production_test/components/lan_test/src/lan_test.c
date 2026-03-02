#include "lan_test.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"

#include "lwip/inet.h"
#include "ping/ping_sock.h"

static const char *TAG = "lan_test";

/* ---- Kconfig ---- */
#define PHY_ADDR         CONFIG_LAN_TEST_PHY_ADDR
#define PHY_RST_GPIO     CONFIG_LAN_TEST_PHY_RST_GPIO
#define MDC_GPIO         CONFIG_LAN_TEST_MDC_GPIO
#define MDIO_GPIO        CONFIG_LAN_TEST_MDIO_GPIO
#define RMII_CLK_GPIO    CONFIG_LAN_TEST_RMII_CLK_IN_GPIO

/* ---- Events ---- */
#define BIT_LINK_UP  (1U << 0)
#define BIT_GOT_IP   (1U << 1)

static EventGroupHandle_t s_evt;
static esp_netif_t *s_netif;
static esp_eth_handle_t s_eth;
static esp_eth_netif_glue_handle_t s_glue;
static esp_eth_mac_t *s_mac;
static esp_eth_phy_t *s_phy;
static bool s_started;

/* ---------- ETH/IP events ---------- */
static void eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;

    if (id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "ETH link UP");
        xEventGroupSetBits(s_evt, BIT_LINK_UP);
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "ETH link DOWN");
        xEventGroupClearBits(s_evt, BIT_LINK_UP | BIT_GOT_IP);
    }
}

static void got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    const esp_netif_ip_info_t *ip = &e->ip_info;

    ESP_LOGI(TAG, "GOT IP: " IPSTR " MASK: " IPSTR " GW: " IPSTR,
             IP2STR(&ip->ip), IP2STR(&ip->netmask), IP2STR(&ip->gw));

    xEventGroupSetBits(s_evt, BIT_GOT_IP);
}

/* ---------- Start ETH + wait LINK ---------- */
esp_err_t lan_test_eth_start(esp_netif_t **out_netif)
{
    if (s_started) {
        if (out_netif) *out_netif = s_netif;
        return ESP_OK;
    }

    if (!s_evt) {
        s_evt = xEventGroupCreate();
        if (!s_evt) return ESP_ERR_NO_MEM;
    }
    xEventGroupClearBits(s_evt, BIT_LINK_UP | BIT_GOT_IP);

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&cfg);
    if (!s_netif) return ESP_ERR_NO_MEM;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = PHY_ADDR;
    phy_cfg.reset_gpio_num = PHY_RST_GPIO;

    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    // ESP32-P4 naming: mdc_num/mdio_num
    emac_cfg.smi_gpio.mdc_num  = MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = MDIO_GPIO;

    emac_cfg.interface = EMAC_DATA_INTERFACE_RMII;

    // External 50 MHz REF_CLK input
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = RMII_CLK_GPIO;

    s_mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    if (!s_mac) { lan_test_deinit(); return ESP_ERR_NO_MEM; }

    s_phy = esp_eth_phy_new_ip101(&phy_cfg);
    if (!s_phy) { lan_test_deinit(); return ESP_ERR_NO_MEM; }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(s_mac, s_phy);
    err = esp_eth_driver_install(&eth_cfg, &s_eth);
    if (err != ESP_OK) { lan_test_deinit(); return err; }

    s_glue = esp_eth_new_netif_glue(s_eth);
    if (!s_glue) { lan_test_deinit(); return ESP_ERR_NO_MEM; }

    err = esp_netif_attach(s_netif, s_glue);
    if (err != ESP_OK) { lan_test_deinit(); return err; }

    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event, NULL);
    if (err != ESP_OK) { lan_test_deinit(); return err; }

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip, NULL);
    if (err != ESP_OK) { lan_test_deinit(); return err; }

    err = esp_eth_start(s_eth);
    if (err != ESP_OK) { lan_test_deinit(); return err; }

    EventBits_t b = xEventGroupWaitBits(
        s_evt, BIT_LINK_UP, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000)
    );
    if (!(b & BIT_LINK_UP)) {
        lan_test_deinit();
        return ESP_ERR_TIMEOUT;
    }

    s_started = true;
    if (out_netif) *out_netif = s_netif;
    return ESP_OK;
}

/* ---------- DHCP wait ---------- */
esp_err_t lan_test_wait_dhcp(esp_netif_t *netif, uint32_t timeout_ms, esp_netif_ip_info_t *ip_info)
{
    if (!netif) return ESP_ERR_INVALID_ARG;

    EventBits_t b = xEventGroupWaitBits(
        s_evt, BIT_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)
    );
    if (!(b & BIT_GOT_IP)) return ESP_ERR_TIMEOUT;

    if (ip_info) {
        return esp_netif_get_ip_info(netif, ip_info);
    }
    return ESP_OK;
}

/* ---------- Ping stats ---------- */
#define BIT_PING_DONE (1U << 0)

static EventGroupHandle_t s_ping_evt;

typedef struct {
    uint32_t rtt_sum_ms;
} ping_acc_t;

static ping_acc_t s_ping_acc;

static void ping_on_success(esp_ping_handle_t hdl, void *args)
{
    (void)args;

    uint32_t time_ms = 0;
    // RTT in ms
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &time_ms, sizeof(time_ms));
    s_ping_acc.rtt_sum_ms += time_ms;
}

static void ping_on_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    (void)args;
}

static void ping_on_end(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    xEventGroupSetBits(s_ping_evt, BIT_PING_DONE);
    esp_ping_stop(hdl);
}

esp_err_t lan_test_run_stats(uint32_t ping_timeout_ms,
                             uint32_t ping_count,
                             lan_ping_stats_t *out_stats,
                             char *reason,
                             size_t reason_len)
{
    if (reason && reason_len) reason[0] = 0;
    if (out_stats) memset(out_stats, 0, sizeof(*out_stats));

    if (!s_started) {
        if (reason && reason_len) snprintf(reason, reason_len, "LAN not started");
        return ESP_ERR_INVALID_STATE;
    }

    if (ping_count < 10) ping_count = 10;

    if (!s_ping_evt) {
        s_ping_evt = xEventGroupCreate();
        if (!s_ping_evt) return ESP_ERR_NO_MEM;
    }
    xEventGroupClearBits(s_ping_evt, BIT_PING_DONE);
    memset(&s_ping_acc, 0, sizeof(s_ping_acc));

    ip_addr_t target_addr;
    if (!ipaddr_aton("8.8.8.8", &target_addr)) {
        if (reason && reason_len) snprintf(reason, reason_len, "Invalid target address");
        return ESP_ERR_INVALID_ARG;
    }

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr  = target_addr;
    cfg.count        = ping_count;
    cfg.interval_ms  = 250;
    cfg.timeout_ms   = 1000;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_on_success,
        .on_ping_timeout = ping_on_timeout,
        .on_ping_end     = ping_on_end,
        .cb_args         = NULL,
    };

    esp_ping_handle_t ping = NULL;
    esp_err_t err = esp_ping_new_session(&cfg, &cbs, &ping);
    if (err != ESP_OK) {
        if (reason && reason_len) snprintf(reason, reason_len, "esp_ping_new_session: %s", esp_err_to_name(err));
        return err;
    }

    esp_ping_start(ping);

    EventBits_t b = xEventGroupWaitBits(
        s_ping_evt, BIT_PING_DONE, pdTRUE, pdTRUE, pdMS_TO_TICKS(ping_timeout_ms)
    );

    uint32_t tx = 0, rx = 0;
    esp_ping_get_profile(ping, ESP_PING_PROF_REQUEST, &tx, sizeof(tx));
    esp_ping_get_profile(ping, ESP_PING_PROF_REPLY,   &rx, sizeof(rx));

    esp_ping_delete_session(ping);

    if (out_stats) {
        out_stats->transmitted = tx;
        out_stats->received    = rx;
        out_stats->dropped     = (tx >= rx) ? (tx - rx) : 0;
        out_stats->avg_time_ms = (rx > 0) ? (s_ping_acc.rtt_sum_ms / rx) : 0;
    }

    if (!(b & BIT_PING_DONE)) {
        if (reason && reason_len) snprintf(reason, reason_len, "Ping timeout (tx=%u rx=%u)", (unsigned)tx, (unsigned)rx);
        return ESP_ERR_TIMEOUT;
    }

    if (rx == 0) {
        if (reason && reason_len) snprintf(reason, reason_len, "Ping failed (tx=%u rx=0)", (unsigned)tx);
        return ESP_FAIL;
    }

    if (reason && reason_len) snprintf(reason, reason_len, "OK");
    return ESP_OK;
}

/* ---------- Deinit ---------- */
esp_err_t lan_test_deinit(void)
{
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip);
    esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event);

    if (s_eth) {
        esp_eth_stop(s_eth);
    }

    if (s_netif) {
        esp_netif_destroy(s_netif);
        s_netif = NULL;
    }

    if (s_glue) {
        esp_eth_del_netif_glue(s_glue);
        s_glue = NULL;
    }

    if (s_eth) {
        esp_eth_driver_uninstall(s_eth);
        s_eth = NULL;
    }

    if (s_mac) {
        s_mac->del(s_mac);
        s_mac = NULL;
    }
    if (s_phy) {
        s_phy->del(s_phy);
        s_phy = NULL;
    }

    if (s_evt) {
        xEventGroupClearBits(s_evt, BIT_LINK_UP | BIT_GOT_IP);
    }

    s_started = false;
    return ESP_OK;
}
