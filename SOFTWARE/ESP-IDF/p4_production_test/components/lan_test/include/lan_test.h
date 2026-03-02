#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef struct {
    uint32_t transmitted;
    uint32_t received;
    uint32_t dropped;
    uint32_t avg_time_ms;   // average RTT of received replies
} lan_ping_stats_t;

/**
 * Ping 8.8.8.8 with N packets and return stats.
 */
esp_err_t lan_test_run_stats(uint32_t ping_timeout_ms,
                             uint32_t ping_count,
                             lan_ping_stats_t *out_stats,
                             char *reason,
                             size_t reason_len);

/* Start ETH + wait LINK UP */
esp_err_t lan_test_eth_start(esp_netif_t **out_netif);

/* Wait DHCP and return IP */
esp_err_t lan_test_wait_dhcp(esp_netif_t *netif,
                             uint32_t timeout_ms,
                             esp_netif_ip_info_t *ip_info);

/* Ping 8.8.8.8 */
esp_err_t lan_test_run(uint32_t ping_timeout_ms,
                       char *reason,
                       size_t reason_len);

/* Stop + deinit LAN */
esp_err_t lan_test_deinit(void);

#ifdef __cplusplus
}
#endif
