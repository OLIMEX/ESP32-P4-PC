LAN Test component (ESP32-P4)
=============================

Minimal component with:
- lan_test_init(): init ETH (RMII + IP101), wait link + DHCP
- lan_test_wait_dhcp(): wait for DHCP (IP_EVENT_ETH_GOT_IP)
- lan_test_run(): ping 8.8.8.8
- lan_test_deinit(): stop and free everything

Notes for ESP32-P4
------------------
- RMII data pins are routed to EMAC signals; the IDF EMAC config does not expose per-pin rmii.* fields.
- Configure only SMI (MDC/MDIO) and RMII REF_CLK input (GPIO50 by default).
