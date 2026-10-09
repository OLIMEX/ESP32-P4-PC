# Ethernet production-test component

This component initializes the ESP32-P4 EMAC and the board's RMII PHY, waits
for DHCP, and optionally tests Internet reachability by pinging the IPv4
address selected by `CONFIG_LAN_TEST_PING_TARGET` (default `8.8.8.8`).

## API

- `lan_test_eth_start()` initializes Ethernet and waits for link.
- `lan_test_wait_dhcp()` waits for `IP_EVENT_ETH_GOT_IP` and returns the address.
- `lan_test_run_stats()` sends a configurable number of pings and reports loss
  and average round-trip time.
- `lan_test_run()` is the simpler ping-only entry point.
- `lan_test_deinit()` stops Ethernet and releases its resources.

RMII data pins use the ESP32-P4's fixed EMAC signals; ESP-IDF does not expose
individual data-pin fields. Only SMI (MDC/MDIO) and the RMII reference-clock
input (GPIO50 by default) are configured explicitly.
