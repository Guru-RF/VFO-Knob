/* The FlexRadios on the LAN, by their discovery broadcast: see discovery.h.
 *
 * One lwIP pcb on UDP 4992 hears them, in the TCP/IP task itself: no task of
 * its own, no socket and no queue of buffers behind it. Each packet is read
 * as it arrives, into a buffer in PSRAM, and the WiFi driver's buffer goes
 * straight back; what the radio said is kept in a short list, by its serial
 * number, for as long as it keeps saying it. A radio broadcasts once a
 * second, so the list costs the knob a few microseconds a second per radio. */
#include "discovery.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"
#include "net_prov.h"

static const char *TAG = "flex-found";

#define DISC_PORT     4992
#define DISC_MAX      4             /* radios kept: more than a LAN has */
#define DISC_KEEP_MS  30000         /* not heard for this long: gone */
#define DISC_NAME     16            /* the dial's names: 15 characters */

typedef struct {
    flex_disc_t r;
    uint32_t    t_seen;
    bool        used;
    bool        said;               /* its arrival logged */
} entry_t;

/* In PSRAM, as the flex client's state is: internal RAM is the WiFi's. The
 * lock stays internal: a spinlock's atomic does not work on PSRAM. */
EXT_RAM_BSS_ATTR static entry_t s_list[DISC_MAX];
static portMUX_TYPE  s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_listening, s_asked, s_said;
static volatile uint32_t s_retry_at;     /* the port refused: not before then */

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static bool fresh(const entry_t *e, uint32_t t) { return e->used && t - e->t_seen < DISC_KEEP_MS; }

/* What the dial calls it: its nickname, else its model. */
static void name_of(const flex_disc_t *r, char *out, size_t cap)
{
    strlcpy(out, r->nickname[0] ? r->nickname : r->model[0] ? r->model : r->serial, cap);
}

/* --- hearing them: the TCP/IP task's ----------------------------------------- */

static void on_packet(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *from, u16_t port)
{
    (void)arg;
    (void)pcb;
    (void)port;
    /* This task's alone, so no lock for them. */
    EXT_RAM_BSS_ATTR static uint8_t b[1472];
    EXT_RAM_BSS_ATTR static flex_disc_t r;
    const u16_t n = pbuf_copy_partial(p, b, sizeof b, 0);
    pbuf_free(p);
    if (!flex_disc_parse(b, n, &r)) return;
    /* No address of its own in it: the one it came from. */
    if (!r.ip[0] && from && IP_IS_V4(from)) ip4addr_ntoa_r(ip_2_ip4(from), r.ip, sizeof r.ip);
    if (!r.ip[0]) return;
    const uint32_t t = now_ms();
    taskENTER_CRITICAL(&s_lock);
    /* Its own entry; else a free or a stale one; else the longest silent. */
    int at = -1;
    for (int i = 0; i < DISC_MAX && at < 0; i++)
        if (s_list[i].used && !strcmp(s_list[i].r.serial, r.serial)) at = i;
    const bool known = at >= 0;
    for (int i = 0; i < DISC_MAX && at < 0; i++)
        if (!fresh(&s_list[i], t)) at = i;
    if (at < 0) {
        at = 0;
        for (int i = 1; i < DISC_MAX; i++)
            if (t - s_list[i].t_seen > t - s_list[at].t_seen) at = i;
    }
    s_list[at].r      = r;
    s_list[at].t_seen = t;
    s_list[at].used   = true;
    if (!known) s_list[at].said = false;
    taskEXIT_CRITICAL(&s_lock);
}

static void open_pcb(void *arg)
{
    (void)arg;
    struct udp_pcb *pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb && udp_bind(pcb, IP4_ADDR_ANY, DISC_PORT) == ERR_OK) {
        udp_recv(pcb, on_packet, NULL);
        s_listening = true;
        return;
    }
    if (pcb) udp_remove(pcb);
    s_retry_at = now_ms() + 5000;
    s_asked = false;                          /* asked again, in a while */
}

void disc_start(void)
{
    if (s_listening || s_asked || (s_retry_at && (int32_t)(now_ms() - s_retry_at) < 0)) return;
    /* Not before the station is on its network: there is nothing to hear,
     * and lwIP's TCP/IP task -- where the pcb lives -- is surely up. (A flag:
     * asking esp_netif instead waits on that very task, or asserts before
     * it is there.) */
    if (!net_prov_is_connected()) return;
    taskENTER_CRITICAL(&s_lock);
    const bool go = !s_asked;
    s_asked = true;
    taskEXIT_CRITICAL(&s_lock);
    /* Never waiting on the TCP/IP task: with its queue full, next time. */
    if (go && tcpip_try_callback(open_pcb, NULL) != ERR_OK) s_asked = false;
}

/* --- what the dial may offer ------------------------------------------------ */

/* Room in the configured list for one more: four at most. (A new knob's one
 * entry with no address is room too: only a list of four is full, and the
 * page saves none without an address.) */
static bool room(void)
{
    return net_prov_radio_count() < NET_PROV_RADIOS;
}

/* The i-th radio heard lately and not configured: its slot, and its address
 * in ip. -1 when there is none. */
static int lan_slot(int i, char *ip, size_t cap)
{
    const uint32_t t = now_ms();
    for (int k = 0; k < DISC_MAX; k++) {
        bool ok;
        taskENTER_CRITICAL(&s_lock);
        ok = fresh(&s_list[k], t);
        if (ok) strlcpy(ip, s_list[k].r.ip, cap);
        taskEXIT_CRITICAL(&s_lock);
        if (ok && !net_prov_radio_known(ip) && i-- == 0) return k;
    }
    return -1;
}

int disc_lan_count(void)
{
    if (!room()) return 0;
    char ip[16];
    int n = 0;
    while (n < DISC_MAX && lan_slot(n, ip, sizeof ip) >= 0) n++;
    return n;
}

bool disc_lan_get(int i, flex_disc_t *out)
{
    char ip[16];
    const int k = i >= 0 && room() ? lan_slot(i, ip, sizeof ip) : -1;
    if (k < 0) return false;
    taskENTER_CRITICAL(&s_lock);
    *out = s_list[k].r;
    taskEXIT_CRITICAL(&s_lock);
    return true;
}

bool disc_lan_name(int i, char *name, size_t cap)
{
    char ip[16];
    const int k = i >= 0 && room() ? lan_slot(i, ip, sizeof ip) : -1;
    if (cap) name[0] = 0;
    if (k < 0) return false;
    taskENTER_CRITICAL(&s_lock);
    name_of(&s_list[k].r, name, cap < DISC_NAME ? cap : DISC_NAME);
    taskEXIT_CRITICAL(&s_lock);
    return true;
}

esp_err_t disc_lan_use(int i)
{
    flex_disc_t *r = heap_caps_malloc(sizeof *r, MALLOC_CAP_SPIRAM);
    net_radio_t *list = heap_caps_calloc(NET_PROV_RADIOS, sizeof *list, MALLOC_CAP_SPIRAM);
    esp_err_t e = ESP_ERR_NO_MEM;
    if (r && list) {
        e = ESP_ERR_NOT_FOUND;
        if (disc_lan_get(i, r)) {
            /* The list as it is, less a new knob's entry with no address. */
            int n = 0;
            for (int k = 0; k < net_prov_radio_count() && n < NET_PROV_RADIOS; k++)
                if (net_prov_radio_get(k, &list[n]) && list[n].host[0]) n++;
            if (n < NET_PROV_RADIOS) {
                net_radio_t *x = &list[n];
                memset(x, 0, sizeof *x);
                name_of(r, x->name, DISC_NAME);
                strlcpy(x->host, r->ip, sizeof x->host);
                x->port = r->port;
                e = net_prov_radios_save(list, n + 1, n);
                if (e == ESP_OK)
                    ESP_LOGI(TAG, "%s at %s:%u joins the radios, in use", x->name, x->host,
                             (unsigned)x->port);
            } else {
                e = ESP_ERR_NO_MEM;           /* four already: room() said none */
            }
        }
    }
    free(r);
    free(list);
    return e;
}

/* --- the log ----------------------------------------------------------------- */

void disc_news(void)
{
    if (s_listening && !s_said) {
        s_said = true;
        ESP_LOGI(TAG, "listening for FlexRadios on the LAN (UDP %d)", DISC_PORT);
    }
    const uint32_t t = now_ms();
    for (int k = 0; k < DISC_MAX; k++) {
        char name[24], model[16], ip[16], status[16], who[32];
        int news = 0;                         /* 1 heard, 2 gone quiet */
        taskENTER_CRITICAL(&s_lock);
        entry_t *e = &s_list[k];
        if (e->used && !fresh(e, t)) {
            e->used = false;
            news = 2;
        } else if (e->used && !e->said) {
            e->said = true;
            news = 1;
        }
        if (news) {
            strlcpy(name, e->r.nickname[0] ? e->r.nickname : e->r.serial, sizeof name);
            strlcpy(model, e->r.model, sizeof model);
            strlcpy(ip, e->r.ip, sizeof ip);
            strlcpy(status, e->r.status, sizeof status);
            strlcpy(who, e->r.who, sizeof who);
        }
        taskEXIT_CRITICAL(&s_lock);
        if (news == 1)
            ESP_LOGI(TAG, "%s, a %s, at %s: %s%s%s", name, model[0] ? model : "FlexRadio", ip,
                     status[0] ? status : "?", who[0] ? ", in use by " : "", who);
        else if (news == 2)
            ESP_LOGI(TAG, "%s (%s) is no longer heard", name, ip);
    }
}

/* --- the configuration page -------------------------------------------------- */

/* For a JSON string: quotes and backslashes escaped, control characters out. */
static void esc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 2 < cap; in++) {
        if ((unsigned char)*in < 0x20) continue;
        if (*in == '"' || *in == '\\') out[o++] = '\\';
        out[o++] = *in;
    }
    out[o] = 0;
}

/* GET /api/flexfound: {"listening":true,"room":true,"radios":[{"serial",
 * "model","nickname","callsign","ip","port","version","status","who","age",
 * "known"}, ...]} -- every radio heard lately; known: already configured. */
static esp_err_t web_found(httpd_req_t *req)
{
    disc_start();
    enum { CAP = 2048 };
    char *j = heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    flex_disc_t *r = heap_caps_malloc(sizeof *r, MALLOC_CAP_SPIRAM);
    if (!j || !r) {
        free(j);
        free(r);
        return httpd_resp_send_500(req);
    }
    int o = snprintf(j, CAP, "{\"listening\":%s,\"room\":%s,\"radios\":[",
                     s_listening ? "true" : "false", room() ? "true" : "false");
    const uint32_t t = now_ms();
    int n = 0;
    for (int k = 0; k < DISC_MAX && o < CAP - 400; k++) {
        uint32_t age = 0;
        bool ok;
        taskENTER_CRITICAL(&s_lock);
        ok = fresh(&s_list[k], t);
        if (ok) {
            *r  = s_list[k].r;
            age = t - s_list[k].t_seen;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (!ok) continue;
        char nick[52], model[36], call[36], ver[52], st[36], who[68], ser[52];
        esc(r->nickname, nick, sizeof nick);
        esc(r->model, model, sizeof model);
        esc(r->callsign, call, sizeof call);
        esc(r->version, ver, sizeof ver);
        esc(r->status, st, sizeof st);
        esc(r->who, who, sizeof who);
        esc(r->serial, ser, sizeof ser);
        o += snprintf(j + o, CAP - o, "%s{\"serial\":\"%s\",\"model\":\"%s\",\"nickname\":\"%s\","
                      "\"callsign\":\"%s\",\"ip\":\"%s\",\"port\":%u,\"version\":\"%s\","
                      "\"status\":\"%s\",\"who\":\"%s\",\"age\":%lu,\"known\":%s}",
                      n++ ? "," : "", ser, model, nick, call, r->ip, (unsigned)r->port, ver, st, who,
                      (unsigned long)(age / 1000), net_prov_radio_known(r->ip) ? "true" : "false");
    }
    if (o < CAP) snprintf(j + o, CAP - o, "]}");
    free(r);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t e = httpd_resp_sendstr(req, j);
    free(j);
    return e;
}

size_t disc_web_endpoints(const httpd_uri_t **out)
{
    static const httpd_uri_t U[] = {
        { .uri = "/api/flexfound", .method = HTTP_GET, .handler = web_found },
    };
    *out = U;
    return sizeof U / sizeof U[0];
}
