/* The tailnet on the PC, in place of components/tailchaser_client/tc_ts.c and
 * MicroLink: for the tailchaser harness, whose test steers it with
 *
 *   TC_TS_END    what Tailscale comes to after starting: up (the default),
 *                login, refused or failed
 *   TC_TS_AFTER  how long it is starting, ms (300)
 *   TC_TS_PEERS  the tailnet's devices: "tcnode=100.100.1.2,other=100.100.1.3"
 *   TC_BUILD     the firmware build's id (esp_app_get_elf_sha256), "hostbld0" if unset
 *   TC_TS_CRASH  set: starting it takes the whole knob down, as MicroLink's
 *                flash read from a PSRAM stack did on the first knob
 *
 * Every address on it is this PC: a connection to 100.x.y.z:port lands on
 * 127.0.0.1:port. The auth key is never written anywhere, only whether there
 * is one. */
#include "tc_ts.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_timer.h"
#include "shim.h"

static pthread_mutex_t s_m = PTHREAD_MUTEX_INITIALIZER;
static int64_t         s_t0 = -1;       /* when it was started, us */
static int             s_starts;

static tc_ts_state_t end_state(void)
{
    const char *e = getenv("TC_TS_END");
    if (!e || !strcmp(e, "up")) return TC_TS_UP;
    if (!strcmp(e, "login")) return TC_TS_LOGIN;
    if (!strcmp(e, "refused")) return TC_TS_REFUSED;
    return TC_TS_FAILED;
}

void tc_ts_start(const char *auth_key, const char *device_name)
{
    pthread_mutex_lock(&s_m);
    const bool first = s_t0 < 0;
    if (first) s_t0 = esp_timer_get_time();
    s_starts++;
    pthread_mutex_unlock(&s_m);
    if (first) shim_say("@TSSTART name=%s auth=%s", device_name, auth_key && auth_key[0] ? "set" : "none");
    if (first && getenv("TC_TS_CRASH")) {
        shim_say("@CRASH");
        abort();
    }
}

tc_ts_state_t tc_ts_state(void)
{
    pthread_mutex_lock(&s_m);
    const int64_t t0 = s_t0;
    pthread_mutex_unlock(&s_m);
    if (t0 < 0) return TC_TS_OFF;
    const char *a = getenv("TC_TS_AFTER");
    const int64_t after = (a ? atoll(a) : 300) * 1000;
    return esp_timer_get_time() - t0 < after ? TC_TS_STARTING : end_state();
}

const char *tc_ts_state_word(tc_ts_state_t s)
{
    switch (s) {
    case TC_TS_OFF:      return "off";
    case TC_TS_STARTING: return "starting";
    case TC_TS_LOGIN:    return "waiting for a login";
    case TC_TS_UP:       return "connected";
    case TC_TS_REFUSED:  return "refused";
    default:             return "failed";
    }
}

bool tc_ts_login_url(char *out, size_t cap)
{
    if (tc_ts_state() != TC_TS_LOGIN) {
        if (cap) out[0] = 0;
        return false;
    }
    snprintf(out, cap, "https://login.tailscale.com/a/host0test");
    return true;
}

uint32_t tc_ts_ip(void) { return tc_ts_state() == TC_TS_UP ? 0x64656667u : 0; }   /* 100.101.102.103 */

int tc_ts_peers(void)
{
    if (tc_ts_state() != TC_TS_UP) return 0;
    const char *p = getenv("TC_TS_PEERS");
    int n = p && *p ? 1 : 0;
    for (; p && *p; p++) n += *p == ',';
    return n;
}

uint32_t tc_ts_resolve(const char *host)
{
    if (tc_ts_state() != TC_TS_UP) return 0;
    const char *p = getenv("TC_TS_PEERS");
    const size_t hl = strlen(host);
    while (p && *p) {
        const char *eq = strchr(p, '='), *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        if (eq && eq < end && (size_t)(eq - p) == hl && !strncmp(p, host, hl)) {
            char ip[20];
            snprintf(ip, sizeof ip, "%.*s", (int)(end - eq - 1), eq + 1);
            struct in_addr a;
            if (inet_aton(ip, &a)) return ntohl(a.s_addr);
        }
        p = *end ? end + 1 : end;
    }
    return 0;
}

int esp_app_get_elf_sha256(char *dst, size_t size)
{
    const char *b = getenv("TC_BUILD");
    if (!size) return 0;
    snprintf(dst, size, "%s", b && b[0] ? b : "hostbld0");
    return (int)strlen(dst);
}

void tc_ts_forget(void) { shim_say("@TSFORGET"); }

struct tc_ts_conn { int fd; };

tc_ts_conn_t *tc_ts_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (tc_ts_state() != TC_TS_UP || !tc_ts_is_tailnet(ip)) return NULL;
    shim_say("@TSCONN %u.%u.%u.%u:%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16 & 255), (unsigned)(ip >> 8 & 255),
             (unsigned)(ip & 255), (unsigned)port);
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(0x7f000001) };
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        if (fd >= 0) close(fd);
        return NULL;
    }
    tc_ts_conn_t *c = calloc(1, sizeof *c);
    c->fd = fd;
    return c;
}

int tc_ts_send(tc_ts_conn_t *c, const void *p, size_t n)
{
    size_t done = 0;
    while (done < n) {
        const ssize_t k = send(c->fd, (const char *)p + done, n - done, MSG_NOSIGNAL);
        if (k <= 0) return -1;
        done += (size_t)k;
    }
    return (int)n;
}

int tc_ts_recv(tc_ts_conn_t *c, void *p, size_t n, uint32_t timeout_ms)
{
    struct pollfd pf = { .fd = c->fd, .events = POLLIN };
    const int r = poll(&pf, 1, (int)timeout_ms);
    if (r == 0) return 0;
    if (r < 0) return errno == EINTR ? 0 : -1;
    const ssize_t k = recv(c->fd, p, n, 0);
    return k > 0 ? (int)k : -1;
}

int tc_ts_path(uint32_t ip) { (void)ip; return tc_ts_state() == TC_TS_UP ? 1 : 0; }

void tc_ts_close(tc_ts_conn_t *c)
{
    if (!c) return;
    close(c->fd);
    free(c);
}
