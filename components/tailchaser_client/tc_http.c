/* One HTTP exchange with a TailChaser node. See tc_http.h. */
#include "tc_http.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "tc_ts.h"

#define CONNECT_MS      4000    /* on the LAN or the internet                */
#define CONNECT_TS_MS   9000    /* through the tunnel: a handshake first     */
#define ANSWER_MS       8000    /* the whole answer                          */
/* Begun, then nothing more for this long: given up, to be asked again on a
 * fresh connection. Through the tunnel a stream now and then stops dead
 * mid-answer and only picks up seconds later (seen 2026-11-20), long past
 * the face's 5 s; a new one answers at once. */
#define STALL_MS        1500

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* A connection: a socket, or the tunnel's. */
struct tc_conn {
    int           fd;
    tc_ts_conn_t *ts;
};
typedef struct tc_conn conn_t;

static bool plain_connect(conn_t *c, uint32_t ip, uint16_t port, char *why, size_t wn)
{
    c->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->fd < 0) {
        snprintf(why, wn, "no socket (%d)", errno);
        return false;
    }
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(ip) };
    const int fl = fcntl(c->fd, F_GETFL, 0);
    fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(c->fd, (struct sockaddr *)&sa, sizeof sa);
    if (rc != 0 && errno != EINPROGRESS) {
        snprintf(why, wn, "%s", errno == ECONNREFUSED ? "refused the connection" : "could not connect");
        goto fail;
    }
    if (rc != 0) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(c->fd, &w);
        struct timeval tv = { .tv_sec = CONNECT_MS / 1000, .tv_usec = (CONNECT_MS % 1000) * 1000 };
        rc = select(c->fd + 1, NULL, &w, NULL, &tv);
        if (rc <= 0) {
            snprintf(why, wn, "no answer");
            goto fail;
        }
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) {
            snprintf(why, wn, "%s", err == ECONNREFUSED ? "refused the connection"
                                    : err == EHOSTUNREACH ? "unreachable" : "could not connect");
            goto fail;
        }
    }
    fcntl(c->fd, F_SETFL, fl & ~O_NONBLOCK);
    struct timeval rt = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &rt, sizeof rt);
    return true;
fail:
    close(c->fd);
    c->fd = -1;
    return false;
}

static int conn_send(conn_t *c, const char *p, size_t n)
{
    if (c->ts) return tc_ts_send(c->ts, p, n);
    size_t done = 0;
    while (done < n) {
        const int k = (int)send(c->fd, p + done, n - done, 0);
        if (k <= 0) return -1;
        done += (size_t)k;
    }
    return (int)n;
}

/* Bytes; 0 when nothing came this second; -1 closed or failed. */
static int conn_recv(conn_t *c, char *p, size_t n)
{
    if (c->ts) return tc_ts_recv(c->ts, p, n, 1000);
    const int k = (int)recv(c->fd, p, n, 0);
    if (k > 0) return k;
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;
}

/* ...within timeout_ms, for a connection kept open. */
static int conn_recv_ms(conn_t *c, char *p, size_t n, uint32_t ms)
{
    if (c->ts) return tc_ts_recv(c->ts, p, n, ms ? ms : 1);
    fd_set r;
    FD_ZERO(&r);
    FD_SET(c->fd, &r);
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    const int s = select(c->fd + 1, &r, NULL, NULL, &tv);
    if (s == 0) return 0;
    if (s < 0) return errno == EINTR ? 0 : -1;
    const int k = (int)recv(c->fd, p, n, 0);
    if (k > 0) return k;
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;
}

static void conn_close(conn_t *c)
{
    if (c->ts) tc_ts_close(c->ts);
    if (c->fd >= 0) close(c->fd);
    c->ts = NULL;
    c->fd = -1;
}

/* The whole answer is in: its headers, and as much body as it said. */
static bool complete(const char *buf, size_t len)
{
    const char *end = NULL;
    for (size_t i = 0; i + 3 < len; i++)
        if (!memcmp(buf + i, "\r\n\r\n", 4)) {
            end = buf + i + 4;
            break;
        }
    if (!end) return false;
    for (const char *p = buf; p < end;) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) break;
        if ((size_t)(eol - p) > 15 && strncasecmp(p, "content-length:", 15) == 0)
            return len - (size_t)(end - buf) >= (size_t)strtoul(p + 15, NULL, 10);
        p = eol + 1;
    }
    return false;       /* no length: read until it closes */
}

/* Where the node is: an address as written, a peer on the tailnet, or a
 * name the DNS knows. While Tailscale is on its way, a name waits for it:
 * on0ora-tx is a tailnet's name, and the DNS has nothing to say of it. */
static uint32_t resolve(const char *host, bool *via_ts, bool *wait, char *why, size_t wn)
{
    *via_ts = false;
    *wait = false;
    struct in_addr ia;
    if (inet_aton(host, &ia)) return ntohl(ia.s_addr);
    const uint32_t ts = tc_ts_resolve(host);
    if (ts) {
        *via_ts = true;
        return ts;
    }
    const tc_ts_state_t now = tc_ts_state();
    if (now == TC_TS_STARTING || now == TC_TS_LOGIN) {
        snprintf(why, wn, "waiting for Tailscale (%s)", tc_ts_state_word(now));
        *wait = true;
        return 0;
    }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        const tc_ts_state_t s = tc_ts_state();
        if (s == TC_TS_OFF)      snprintf(why, wn, "no address for %.40s", host);
        else if (s == TC_TS_UP)  snprintf(why, wn, "%.30s is not on the tailnet, nor in the DNS", host);
        else                     snprintf(why, wn, "no address for %.30s (Tailscale %s)", host, tc_ts_state_word(s));
        return 0;
    }
    const uint32_t ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return ip;
}

/* The node reached, through the tailnet or not; false and why in r. */
static bool open_to(const tc_addr_t *a, tc_http_t *r, conn_t *c)
{
    memset(r, 0, sizeof *r);
    c->fd = -1;
    c->ts = NULL;
    bool via_ts;
    const uint32_t ip = resolve(a->host, &via_ts, &r->wait_ts, r->why, sizeof r->why);
    if (!ip) return false;
    struct in_addr ia = { .s_addr = htonl(ip) };
    snprintf(r->addr, sizeof r->addr, "%s", inet_ntoa(ia));
    r->ip = ip;
    if (tc_ts_is_tailnet(ip)) {
        const tc_ts_state_t s = tc_ts_state();
        if (s == TC_TS_UP) {
            via_ts = true;
        } else if (s != TC_TS_OFF) {
            snprintf(r->why, sizeof r->why, "on the tailnet, and Tailscale is %s", tc_ts_state_word(s));
            r->wait_ts = s == TC_TS_STARTING || s == TC_TS_LOGIN;
            return false;
        }
        /* Off: the network the knob is on may reach it (a subnet router). */
    }
    r->tailnet = via_ts;
    if (via_ts) {
        c->ts = tc_ts_connect(ip, a->port, CONNECT_TS_MS);
        if (!c->ts) {
            snprintf(r->why, sizeof r->why, "no answer through the tailnet");
            return false;
        }
        return true;
    }
    return plain_connect(c, ip, a->port, r->why, sizeof r->why);
}

tc_conn_t *tc_conn_open(const tc_addr_t *a, tc_http_t *r)
{
    conn_t c;
    if (!open_to(a, r, &c)) return NULL;
    conn_t *p = malloc(sizeof *p);
    if (!p) {
        conn_close(&c);
        snprintf(r->why, sizeof r->why, "no memory");
        return NULL;
    }
    *p = c;
    return p;
}

int tc_conn_send(tc_conn_t *c, const void *p, size_t n) { return c ? conn_send(c, p, n) : -1; }

int tc_conn_recv(tc_conn_t *c, void *p, size_t n, uint32_t timeout_ms)
{
    return c ? conn_recv_ms(c, p, n, timeout_ms) : -1;
}

void tc_conn_close(tc_conn_t *c)
{
    if (!c) return;
    conn_close(c);
    free(c);
}

bool tc_http(const tc_addr_t *a, const char *key, const char *method, const char *path, const char *form,
             char *buf, size_t cap, tc_http_t *r)
{
    conn_t c;
    if (!open_to(a, r, &c)) return false;

    /* The request, the key in it: never logged. */
    char *req = buf;
    int n;
    if (form)
        n = snprintf(req, cap, "%s %s HTTP/1.0\r\nHost: %s\r\nX-API-Key: %s\r\nUser-Agent: vfo-knob\r\n"
                               "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\n"
                               "Connection: close\r\n\r\n%s",
                     method, path, a->host, key, (unsigned)strlen(form), form);
    else
        n = snprintf(req, cap, "%s %s HTTP/1.0\r\nHost: %s\r\nX-API-Key: %s\r\nUser-Agent: vfo-knob\r\n"
                               "Connection: close\r\n\r\n",
                     method, path, a->host, key);
    if (n <= 0 || (size_t)n >= cap) {
        snprintf(r->why, sizeof r->why, "request too long");
        conn_close(&c);
        return false;
    }
    const bool sent = conn_send(&c, req, (size_t)n) == n;
    memset(req, 0, (size_t)n);          /* the key gone from the buffer */
    if (!sent) {
        snprintf(r->why, sizeof r->why, "could not send");
        conn_close(&c);
        return false;
    }

    size_t len = 0;
    const int64_t t_sent = now_ms(), until = t_sent + ANSWER_MS;
    int64_t t_last = t_sent;
    bool closed = false;
    for (;;) {
        if (len + 1 >= cap) break;                  /* as much as fits */
        const int k = conn_recv(&c, buf + len, cap - 1 - len);
        if (k < 0) {
            closed = true;
            break;
        }
        len += (size_t)k;
        if (k > 0) t_last = now_ms();
        if (k > 0 && complete(buf, len)) break;
        if (now_ms() >= until) break;
        if (len && now_ms() - t_last >= STALL_MS) break;
    }
    conn_close(&c);
    buf[len] = 0;
    if (!len) {
        snprintf(r->why, sizeof r->why, "no answer");
        return false;
    }
    if (!tc_http_answer(buf, len, &r->code, &r->body, &r->body_len)) {
        if (len + 1 >= cap) snprintf(r->why, sizeof r->why, "answer too large");
        else snprintf(r->why, sizeof r->why, "answer cut short: %u bytes, %s %u ms after the last", (unsigned)len,
                      closed ? "closed" : "nothing more", (unsigned)(now_ms() - t_last));
        return false;
    }
    return true;
}
