/*
 * captive_dns.c - tiny DNS "answer everything with our IP" server (RFC 1035).
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"

#include "app_config.h"
#include "captive_dns.h"

static const char *TAG = "captive_dns";

#define DNS_PORT        53
#define DNS_MAX_PKT     512
#define DNS_HDR_LEN     12
#define DNS_QTYPE_A     1
#define DNS_QCLASS_IN   1
#define DNS_FLAG_QR     0x8000
#define DNS_FLAG_OPCODE 0x7800
#define DNS_ANSWER_TTL  60

static TaskHandle_t   s_task = NULL;
static volatile bool  s_run = false;
static uint32_t       s_ip_be = 0;

static void serve_loop(int sock, uint8_t *pkt);

/* Walk the QNAME labels; return offset just past the question, or 0 on error. */
static int question_end(const uint8_t *pkt, int len)
{
    int pos = DNS_HDR_LEN;
    while (pos < len) {
        uint8_t label = pkt[pos];
        if (label == 0) {
            pos++;
            break;
        }
        if ((label & 0xC0) != 0) {
            return 0;       /* compression pointers are not valid in a query name */
        }
        pos += 1 + label;
    }
    if (pos + 4 > len) {
        return 0;
    }
    return pos + 4;         /* + QTYPE + QCLASS */
}

/* Open and bind the UDP/53 socket, -1 on failure. */
static int open_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        return -1;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed: errno %d", errno);
        close(sock);
        return -1;
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return sock;
}

/* The task lives for the whole uptime; the socket exists only while the
 * SoftAP is up. The AP is restarted several times during boot (band mode,
 * config), so start/stop are just flag flips and never race with task exit. */
static void dns_task(void *arg)
{
    (void)arg;
    static uint8_t pkt[DNS_MAX_PKT + 16];
    for (;;) {
        if (!s_run) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int sock = open_socket();
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "captive DNS listening on UDP %d", DNS_PORT);
        serve_loop(sock, pkt);
        close(sock);
        ESP_LOGI(TAG, "captive DNS stopped");
    }
}

static void serve_loop(int sock, uint8_t *pkt)
{
    while (s_run) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, pkt, DNS_MAX_PKT, 0, (struct sockaddr *)&from, &from_len);
        if (len < DNS_HDR_LEN) {
            continue;       /* timeout or runt packet */
        }
        uint16_t flags = (uint16_t)((pkt[2] << 8) | pkt[3]);
        uint16_t qdcount = (uint16_t)((pkt[4] << 8) | pkt[5]);
        if ((flags & DNS_FLAG_QR) || (flags & DNS_FLAG_OPCODE) || qdcount < 1) {
            continue;       /* not a standard query */
        }
        int qend = question_end(pkt, len);
        if (qend == 0) {
            continue;
        }
        uint16_t qtype = (uint16_t)((pkt[qend - 4] << 8) | pkt[qend - 3]);
        uint16_t qclass = (uint16_t)((pkt[qend - 2] << 8) | pkt[qend - 1]);

        /* Response: same header/question, QR=1, RD copied, RA=1, RCODE=0. */
        uint16_t rflags = 0x8080 | (flags & 0x0100);
        pkt[2] = (uint8_t)(rflags >> 8);
        pkt[3] = (uint8_t)(rflags & 0xFF);
        pkt[4] = 0; pkt[5] = 1;             /* QDCOUNT = 1 (answer the first question only) */
        pkt[8] = 0; pkt[9] = 0;             /* NSCOUNT */
        pkt[10] = 0; pkt[11] = 0;           /* ARCOUNT */
        int out_len = qend;
        if (qtype == DNS_QTYPE_A && qclass == DNS_QCLASS_IN) {
            pkt[6] = 0; pkt[7] = 1;         /* ANCOUNT = 1 */
            uint8_t *a = pkt + qend;
            a[0] = 0xC0; a[1] = DNS_HDR_LEN;            /* NAME: pointer to the question name */
            a[2] = 0; a[3] = DNS_QTYPE_A;               /* TYPE A */
            a[4] = 0; a[5] = DNS_QCLASS_IN;             /* CLASS IN */
            a[6] = 0; a[7] = 0; a[8] = 0; a[9] = DNS_ANSWER_TTL;
            a[10] = 0; a[11] = 4;                       /* RDLENGTH */
            memcpy(a + 12, &s_ip_be, 4);
            out_len += 16;
        } else {
            pkt[6] = 0; pkt[7] = 0;         /* no answer for AAAA/others: client falls back to A */
        }
        sendto(sock, pkt, out_len, 0, (struct sockaddr *)&from, from_len);
    }
}

esp_err_t captive_dns_start(uint32_t ip_be)
{
    s_ip_be = ip_be;
    s_run = true;
    if (s_task != NULL) {
        return ESP_OK;      /* task already exists: it (re)opens the socket itself */
    }
    if (xTaskCreate(dns_task, "captive_dns", APP_CAPTIVE_DNS_STACK, NULL, APP_CAPTIVE_DNS_PRIO, &s_task) != pdPASS) {
        s_run = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void captive_dns_stop(void)
{
    s_run = false;      /* socket is closed on the next receive timeout (<= 1 s) */
}
