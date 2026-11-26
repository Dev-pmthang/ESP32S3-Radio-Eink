/*
 * captive_dns.h - minimal DNS responder for the SoftAP captive portal.
 *
 * Every A query from a SoftAP client is answered with the AP's own IP so
 * phones and laptops open http://192.168.4.1/ automatically ("sign in to
 * network"). Runs as a small UDP task only while the SoftAP is active.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

/* ip_be: IPv4 address in network byte order (esp_netif_ip_info_t.ip.addr). */
esp_err_t captive_dns_start(uint32_t ip_be);
void      captive_dns_stop(void);
