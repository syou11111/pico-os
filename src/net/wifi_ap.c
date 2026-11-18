#include "wifi_ap.h"
#include "../kernel/kernel.h"
#include "pico/stdlib.h"
#include "pico/stdio/driver.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Unified Input Ring Buffer ---------- */
#define RX_RING_SIZE 256
static uint8_t  rx_ring[RX_RING_SIZE];
static uint16_t rx_head = 0;
static uint16_t rx_tail = 0;

/* ---------- Output Mirror Ring Buffer ---------- */
#define TX_RING_SIZE 1024
static char     tx_ring[TX_RING_SIZE];
static uint32_t tx_total = 0;

/* ---------- Wi-Fi AP State ---------- */
static volatile bool ap_ready = false;
static volatile bool wifi_hw_ok = false;
static volatile int8_t pending_led = -1;

void os_input_push(uint8_t ch) {
    uint32_t primask = os_enter_critical();
    uint16_t next = (uint16_t)((rx_head + 1U) % RX_RING_SIZE);
    if (next != rx_tail) {
        rx_ring[rx_head] = ch;
        rx_head = next;
    }
    os_exit_critical(primask);
}

void os_input_push_str(const char *s) {
    if (!s) return;
    while (*s) {
        os_input_push((uint8_t)*s++);
    }
}

static int pop_rx_byte(void) {
    int res = -1;
    uint32_t primask = os_enter_critical();
    if (rx_head != rx_tail) {
        res = (int)rx_ring[rx_tail];
        rx_tail = (uint16_t)((rx_tail + 1U) % RX_RING_SIZE);
    }
    os_exit_critical(primask);
    return res;
}

int os_getchar_timeout_us(uint32_t timeout_us) {
    int ch = pop_rx_byte();
    if (ch >= 0) return ch;

    ch = getchar_timeout_us(0);
    if (ch != PICO_ERROR_TIMEOUT) return ch;

    if (timeout_us == 0) return PICO_ERROR_TIMEOUT;

    uint32_t start = time_us_32();
    while ((uint32_t)(time_us_32() - start) < timeout_us) {
        if (timeout_us >= 2000U) {
            os_sleep_ms(1);
        } else {
            tight_loop_contents();
        }
        ch = pop_rx_byte();
        if (ch >= 0) return ch;
        ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) return ch;
    }
    return PICO_ERROR_TIMEOUT;
}

void wifi_log_output(const char *s, size_t len) {
    if (!s || len == 0) return;
    uint32_t primask = os_enter_critical();
    for (size_t i = 0; i < len; i++) {
        tx_ring[tx_total % TX_RING_SIZE] = s[i];
        tx_total++;
    }
    os_exit_critical(primask);
}

static void wifi_stdio_out_chars(const char *buf, int len) {
    if (len > 0) {
        wifi_log_output(buf, (size_t)len);
    }
}

static stdio_driver_t wifi_stdio_driver = {
    .out_chars = wifi_stdio_out_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = false
#endif
};

/* ---------- Minimal DHCP Server (UDP 67 -> 68) ---------- */
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAX_LEASES  16

typedef struct {
    bool    used;
    uint8_t mac[6];
} dhcp_lease_t;

static dhcp_lease_t dhcp_leases[DHCP_MAX_LEASES];
static struct udp_pcb *dhcp_pcb = NULL;

static int find_or_alloc_lease(const uint8_t *mac) {
    for (int i = 0; i < DHCP_MAX_LEASES; i++) {
        if (dhcp_leases[i].used && memcmp(dhcp_leases[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    for (int i = 0; i < DHCP_MAX_LEASES; i++) {
        if (!dhcp_leases[i].used) {
            dhcp_leases[i].used = true;
            memcpy(dhcp_leases[i].mac, mac, 6);
            return i;
        }
    }
    memcpy(dhcp_leases[0].mac, mac, 6);
    return 0;
}

static void dhcp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr, u16_t port) {
    (void)arg;
    (void)addr;
    (void)port;
    if (!p) return;

    if (p->tot_len < 240) {
        pbuf_free(p);
        return;
    }

    uint8_t req[548];
    uint16_t copy_len = p->tot_len > sizeof(req) ? sizeof(req) : p->tot_len;
    pbuf_copy_partial(p, req, copy_len, 0);
    pbuf_free(p);

    if (req[0] != 1) return; /* Must be BOOTREQUEST */
    if (req[236] != 0x63 || req[237] != 0x82 || req[238] != 0x53 || req[239] != 0x63) {
        return; /* Bad DHCP magic cookie */
    }

    uint8_t msg_type = 0;
    uint16_t idx = 240;
    while (idx < copy_len) {
        uint8_t opt = req[idx++];
        if (opt == 255) break;
        if (opt == 0) continue;
        if (idx >= copy_len) break;
        uint8_t olen = req[idx++];
        if (idx + olen > copy_len) break;
        if (opt == 53 && olen == 1) {
            msg_type = req[idx];
        }
        idx += olen;
    }

    if (msg_type != 1 && msg_type != 3) return; /* Only DISCOVER (1) or REQUEST (3) */

    int lease_idx = find_or_alloc_lease(&req[28]);
    uint8_t client_ip_last = (uint8_t)(2 + lease_idx);

    uint8_t resp[300];
    memset(resp, 0, sizeof(resp));
    resp[0] = 2; /* BOOTREPLY */
    resp[1] = req[1];
    resp[2] = req[2];
    resp[3] = 0;
    memcpy(&resp[4], &req[4], 4);   /* xid */
    resp[10] = req[10];             /* flags */
    resp[11] = req[11];
    /* yiaddr = 192.168.4.(2 + lease_idx) */
    resp[16] = 192; resp[17] = 168; resp[18] = 4; resp[19] = client_ip_last;
    /* siaddr = 192.168.4.1 */
    resp[20] = 192; resp[21] = 168; resp[22] = 4; resp[23] = 1;
    memcpy(&resp[28], &req[28], 16); /* chaddr */

    /* Magic cookie */
    resp[236] = 0x63; resp[237] = 0x82; resp[238] = 0x53; resp[239] = 0x63;

    uint16_t o = 240;
    /* Option 53: DHCP Message Type (2=OFFER, 5=ACK) */
    resp[o++] = 53; resp[o++] = 1; resp[o++] = (msg_type == 1) ? 2 : 5;
    /* Option 54: Server Identifier 192.168.4.1 */
    resp[o++] = 54; resp[o++] = 4; resp[o++] = 192; resp[o++] = 168; resp[o++] = 4; resp[o++] = 1;
    /* Option 51: Lease Time = 3600s */
    resp[o++] = 51; resp[o++] = 4; resp[o++] = 0x00; resp[o++] = 0x00; resp[o++] = 0x0E; resp[o++] = 0x10;
    /* Option 1: Subnet Mask 255.255.255.0 */
    resp[o++] = 1;  resp[o++] = 4; resp[o++] = 255; resp[o++] = 255; resp[o++] = 255; resp[o++] = 0;
    /* Option 3: Router 192.168.4.1 */
    resp[o++] = 3;  resp[o++] = 4; resp[o++] = 192; resp[o++] = 168; resp[o++] = 4; resp[o++] = 1;
    /* Option 6: DNS Server 192.168.4.1 */
    resp[o++] = 6;  resp[o++] = 4; resp[o++] = 192; resp[o++] = 168; resp[o++] = 4; resp[o++] = 1;
    /* End */
    resp[o++] = 255;

    struct pbuf *out_p = pbuf_alloc(PBUF_TRANSPORT, o, PBUF_RAM);
    if (!out_p) return;
    memcpy(out_p->payload, resp, o);

    ip_addr_t bcast = IPADDR4_INIT_BYTES(255, 255, 255, 255);
    udp_sendto_if(pcb, out_p, &bcast, DHCP_CLIENT_PORT, &cyw43_state.netif[CYW43_ITF_AP]);
    pbuf_free(out_p);
}

static void start_dhcp_server(void) {
    if (dhcp_pcb) return;
    dhcp_pcb = udp_new();
    if (!dhcp_pcb) return;
    if (udp_bind(dhcp_pcb, IP_ANY_TYPE, DHCP_SERVER_PORT) == ERR_OK) {
        udp_recv(dhcp_pcb, dhcp_recv_cb, NULL);
    }
}

/* ---------- HTTP Server (Port 80) ---------- */
static const char INDEX_HTML[] =
"HTTP/1.1 200 OK\r\n"
"Content-Type: text/html; charset=utf-8\r\n"
"Connection: close\r\n\r\n"
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
"<title>PicoOS Wireless Keyboard</title>"
"<style>"
"*{box-sizing:border-box}body{margin:0;padding:10px;background:#0d1117;color:#e6edf3;font-family:system-ui,sans-serif}"
"h3{margin:0 0 8px;color:#38bdf8;font-size:16px}"
"#out{background:#05080f;border:1px solid #30363d;border-radius:8px;padding:8px;height:190px;overflow-y:auto;"
"font-family:monospace;font-size:12px;white-space:pre-wrap;word-break:break-all;margin:0 0 8px}"
".row{display:flex;gap:6px;margin-bottom:8px;flex-wrap:wrap}"
"input{flex:1;min-width:120px;background:#161b22;color:#fff;border:1px solid #30363d;border-radius:6px;padding:8px;font-size:15px}"
"button{background:#21262d;color:#fff;border:1px solid #30363d;border-radius:6px;padding:8px 10px;font-size:13px;font-weight:600}"
"button:active{background:#0284c7}.p{background:#0284c7}.d{background:#991b1b}.g{background:#15803d}"
".pad{display:grid;grid-template-columns:repeat(3,54px);gap:4px;justify-content:center;margin:6px 0}"
"</style></head><body>"
"<h3>PicoOS Wi-Fi Keyboard</h3>"
"<pre id=out></pre>"
"<div class=row><input id=live placeholder='ライブ入力 (1文字ずつ即送信)' autocomplete=off autocorrect=off autocapitalize=off></div>"
"<div class=row><input id=cmd placeholder='コマンド送信 (Enterで実行)' autocomplete=off autocapitalize=off><button class=p onclick=sendCmd()>送信</button></div>"
"<div class=row>"
"<button onclick=\"hx('1B')\">ESC</button>"
"<button onclick=\"hx('09')\">TAB</button>"
"<button onclick=\"hx('7F')\">BS</button>"
"<button class=p onclick=\"hx('0D')\">ENTER</button>"
"<button class=d onclick=\"hx('03')\">Ctrl+C</button>"
"</div>"
"<div class=pad>"
"<div></div><button onclick=\"hx('1B5B41')\">▲</button><div></div>"
"<button onclick=\"hx('1B5B44')\">◀</button><button onclick=\"hx('1B5B42')\">▼</button><button onclick=\"hx('1B5B43')\">▶</button>"
"</div>"
"<div class=row>"
"<button onclick=\"st(':w\\r')\">:w</button>"
"<button onclick=\"st(':wq\\r')\">:wq</button>"
"<button onclick=\"st(':q!\\r')\">:q!</button>"
"<button onclick=\"st('ps\\r')\">ps</button>"
"<button onclick=\"st('ls\\r')\">ls</button>"
"<button onclick=\"st('pinout\\r')\">pinout</button>"
"<button onclick=\"st('disp\\r')\">disp</button>"
"<button onclick=\"st('clear\\r')\">clear</button>"
"<button class=g onclick=\"st('cc -r sensor.c\\r')\">sensor.c</button>"
"<button class=g onclick=\"st('cc -r game.c\\r')\">game.c</button>"
"</div>"
"<script>"
"let seq=0,q='',busy=false;"
"const out=document.getElementById('out'),live=document.getElementById('live'),cmd=document.getElementById('cmd');"
"function toHex(s){return Array.from(new TextEncoder().encode(s)).map(b=>b.toString(16).padStart(2,'0')).join('')}"
"function hx(h){q+=h;pump()}"
"function st(s){hx(toHex(s))}"
"function sendCmd(){if(cmd.value){st(cmd.value+'\\r');cmd.value=''}else hx('0D')}"
"cmd.addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();sendCmd()}});"
"live.addEventListener('keydown',e=>{"
"if(e.key==='Enter'){e.preventDefault();hx('0D')}"
"else if(e.key==='Backspace'){e.preventDefault();hx('7F')}"
"else if(e.key==='Escape'){e.preventDefault();hx('1B')}"
"else if(e.key==='Tab'){e.preventDefault();hx('09')}"
"else if(e.key==='ArrowUp'){e.preventDefault();hx('1B5B41')}"
"else if(e.key==='ArrowDown'){e.preventDefault();hx('1B5B42')}"
"else if(e.key==='ArrowRight'){e.preventDefault();hx('1B5B43')}"
"else if(e.key==='ArrowLeft'){e.preventDefault();hx('1B5B44')}"
"});"
"live.addEventListener('input',()=>{if(live.value){st(live.value);live.value=''}});"
"async function pump(){"
"if(busy)return;busy=true;"
"try{const d=q;q='';const r=await fetch('/k?s='+seq+'&d='+d);"
"const t=await r.text();const nl=t.indexOf('\\n');"
"if(nl>=0){seq=parseInt(t.slice(0,nl),10)||seq;const body=t.slice(nl+1).replace(/\\x1b\\[[0-9;?]*[A-Za-z]/g,'');"
"if(body){out.textContent=(out.textContent+body).slice(-2500);out.scrollTop=out.scrollHeight}}"
"}catch(e){}finally{busy=false;if(q)pump()}"
"}"
"setInterval(pump,220);pump();"
"</script></body></html>";

typedef struct {
    const char *send_ptr;
    size_t send_left;
    bool free_buf;
    char *dyn_buf;
} http_conn_t;

static struct tcp_pcb *http_listen_pcb = NULL;

static void http_close_conn(struct tcp_pcb *pcb, http_conn_t *hc) {
    tcp_arg(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    if (hc) {
        if (hc->free_buf && hc->dyn_buf) {
            free(hc->dyn_buf);
        }
        free(hc);
    }
    tcp_close(pcb);
}

static void http_send_more(struct tcp_pcb *pcb, http_conn_t *hc) {
    while (hc->send_left > 0) {
        u16_t avail = tcp_sndbuf(pcb);
        if (avail == 0) break;
        u16_t chunk = (hc->send_left > avail) ? avail : (u16_t)hc->send_left;
        err_t err = tcp_write(pcb, hc->send_ptr, chunk, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) break;
        hc->send_ptr += chunk;
        hc->send_left -= chunk;
    }
    tcp_output(pcb);
    if (hc->send_left == 0) {
        http_close_conn(pcb, hc);
    }
}

static err_t http_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)len;
    http_conn_t *hc = (http_conn_t *)arg;
    if (hc) {
        http_send_more(pcb, hc);
    }
    return ERR_OK;
}

static err_t http_poll_cb(void *arg, struct tcp_pcb *pcb) {
    http_conn_t *hc = (http_conn_t *)arg;
    http_close_conn(pcb, hc);
    return ERR_OK;
}

static void http_err_cb(void *arg, err_t err) {
    (void)err;
    http_conn_t *hc = (http_conn_t *)arg;
    if (hc) {
        if (hc->free_buf && hc->dyn_buf) free(hc->dyn_buf);
        free(hc);
    }
}

static int hex_nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void process_k_query(const char *path, uint32_t *client_seq) {
    const char *sp = strstr(path, "s=");
    if (sp) {
        *client_seq = (uint32_t)strtoul(sp + 2, NULL, 10);
    }
    const char *dp = strstr(path, "d=");
    if (dp) {
        dp += 2;
        while (dp[0] && dp[1] && dp[0] != '&' && dp[0] != ' ') {
            int hi = hex_nib(dp[0]);
            int lo = hex_nib(dp[1]);
            if (hi < 0 || lo < 0) break;
            os_input_push((uint8_t)((hi << 4) | lo));
            dp += 2;
        }
    }
}

static err_t http_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    http_conn_t *hc = (http_conn_t *)arg;
    if (!p || err != ERR_OK) {
        if (p) pbuf_free(p);
        http_close_conn(pcb, hc);
        return ERR_OK;
    }

    tcp_recved(pcb, p->tot_len);

    char req[256];
    uint16_t clen = p->tot_len > sizeof(req) - 1 ? sizeof(req) - 1 : p->tot_len;
    pbuf_copy_partial(p, req, clen, 0);
    req[clen] = '\0';
    pbuf_free(p);

    if (hc->send_ptr != NULL) {
        return ERR_OK;
    }

    if (strncmp(req, "GET /k", 6) == 0) {
        uint32_t client_seq = 0;
        process_k_query(req + 6, &client_seq);

        char delta[TX_RING_SIZE + 1];
        uint32_t cur_total;
        size_t dlen = 0;

        uint32_t primask = os_enter_critical();
        cur_total = tx_total;
        if (client_seq < cur_total) {
            uint32_t avail = cur_total - client_seq;
            if (avail > TX_RING_SIZE) {
                avail = TX_RING_SIZE;
                client_seq = cur_total - TX_RING_SIZE;
            }
            for (uint32_t i = 0; i < avail; i++) {
                delta[dlen++] = tx_ring[(client_seq + i) % TX_RING_SIZE];
            }
        }
        os_exit_critical(primask);
        delta[dlen] = '\0';

        char *resp = (char *)malloc(128 + dlen);
        if (!resp) {
            http_close_conn(pcb, hc);
            return ERR_OK;
        }
        int hlen = snprintf(resp, 128,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n\r\n"
            "%lu\n",
            (unsigned long)cur_total);
        if (dlen > 0) {
            memcpy(resp + hlen, delta, dlen);
        }
        hc->dyn_buf = resp;
        hc->free_buf = true;
        hc->send_ptr = resp;
        hc->send_left = (size_t)hlen + dlen;
        http_send_more(pcb, hc);
        return ERR_OK;
    }

    /* Serve index HTML for GET / and any other route */
    hc->dyn_buf = NULL;
    hc->free_buf = false;
    hc->send_ptr = INDEX_HTML;
    hc->send_left = sizeof(INDEX_HTML) - 1;
    http_send_more(pcb, hc);
    return ERR_OK;
}

static err_t http_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !newpcb) return ERR_VAL;

    http_conn_t *hc = (http_conn_t *)calloc(1, sizeof(http_conn_t));
    if (!hc) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }
    tcp_arg(newpcb, hc);
    tcp_recv(newpcb, http_recv_cb);
    tcp_sent(newpcb, http_sent_cb);
    tcp_err(newpcb, http_err_cb);
    tcp_poll(newpcb, http_poll_cb, 8);
    return ERR_OK;
}

static void start_http_server(void) {
    if (http_listen_pcb) return;
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) return;
    if (tcp_bind(pcb, IP_ANY_TYPE, 80) != ERR_OK) {
        tcp_close(pcb);
        return;
    }
    http_listen_pcb = tcp_listen_with_backlog(pcb, 4);
    if (!http_listen_pcb) {
        tcp_close(pcb);
        return;
    }
    tcp_accept(http_listen_pcb, http_accept_cb);
}

void wifi_ap_init_and_start(void) {
    stdio_set_driver_enabled(&wifi_stdio_driver, true);
    if (cyw43_arch_init() == 0) {
        wifi_hw_ok = true;
    }
}

bool wifi_ap_is_ready(void) {
    return ap_ready;
}

void wifi_ap_print_status(void) {
    if (!wifi_hw_ok) {
        printf("Wi-Fi hardware not initialized.\r\n");
        return;
    }
    printf("Wi-Fi AP : %s\r\n", ap_ready ? "ACTIVE" : "STARTING");
    printf("SSID     : %s\r\n", WIFI_AP_SSID);
    printf("Password : %s\r\n", WIFI_AP_PASS);
    printf("URL      : http://%s\r\n", WIFI_AP_IP);
}

void wifi_ap_set_led(bool on) {
    pending_led = on ? 1 : 0;
}

void wifi_ap_task(void *arg) {
    (void)arg;
    if (!wifi_hw_ok) {
        while (1) os_sleep_ms(1000);
    }

    cyw43_arch_enable_ap_mode(WIFI_AP_SSID, WIFI_AP_PASS, CYW43_AUTH_WPA2_AES_PSK);
    start_dhcp_server();
    start_http_server();
    ap_ready = true;

    while (1) {
        int8_t l = pending_led;
        if (l >= 0) {
            pending_led = -1;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, l);
        }
        cyw43_arch_poll();
        os_sleep_ms(5);
    }
}
