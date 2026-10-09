/* Host shim: ESP-IDF's esp_http_server, as much as webcfg.c uses of it, on a
 * plain socket -- so the knob's own configuration and radio pages, and the
 * API behind them, run on the PC against a mock receiver. One request at a
 * time on one thread, as the knob's server takes them; each connection
 * closes after its answer (HTTP/1.1, Connection: close). It listens on
 * 127.0.0.1 only, on SHIM_HTTP_PORT (8080 unless it says), whatever port the
 * config asks for. */
#include "esp_http_server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "shim.h"

#define MAX_URIS  64
#define HEAD_MAX  16384
#define HDRS_MAX  8

typedef struct {
    httpd_uri_t              uris[MAX_URIS];
    int                      n;
    httpd_err_handler_func_t on404;
    int                      fd;
} server_t;

/* A request in flight: its connection, its head, the body read with it,
 * and the answer being made. */
typedef struct {
    int    fd;
    char   head[HEAD_MAX + 1];
    size_t head_len;
    char  *body;                    /* what came after the head in the same reads */
    size_t body_have, body_used;
    char   status[48];
    char   type[64];
    char   hdrs[HDRS_MAX][2][128];
    int    n_hdrs;
    bool   started;                 /* a chunked answer begun */
    bool   sent;
} conn_t;

static conn_t *C(httpd_req_t *r) { return (conn_t *)r->aux; }

static bool write_all(int fd, const char *p, size_t n)
{
    while (n) {
        const ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static void head_out(conn_t *c, long len)
{
    char h[2048];
    int o = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nConnection: close\r\n",
                     c->status[0] ? c->status : "200 OK", c->type[0] ? c->type : "text/html");
    for (int i = 0; i < c->n_hdrs; i++)
        o += snprintf(h + o, sizeof h - o, "%s: %s\r\n", c->hdrs[i][0], c->hdrs[i][1]);
    if (len >= 0) o += snprintf(h + o, sizeof h - o, "Content-Length: %ld\r\n\r\n", len);
    else          o += snprintf(h + o, sizeof h - o, "Transfer-Encoding: chunked\r\n\r\n");
    write_all(c->fd, h, (size_t)o);
}

/* ------------------------------------------------------------- the answer */

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    snprintf(C(r)->status, sizeof C(r)->status, "%s", status);
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{
    snprintf(C(r)->type, sizeof C(r)->type, "%s", type);
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    conn_t *c = C(r);
    if (c->n_hdrs >= HDRS_MAX) return ESP_FAIL;
    snprintf(c->hdrs[c->n_hdrs][0], sizeof c->hdrs[0][0], "%s", field);
    snprintf(c->hdrs[c->n_hdrs][1], sizeof c->hdrs[0][1], "%s", value);
    c->n_hdrs++;
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len)
{
    conn_t *c = C(r);
    if (c->sent || c->started) return ESP_FAIL;
    if (len == HTTPD_RESP_USE_STRLEN) len = buf ? (ssize_t)strlen(buf) : 0;
    head_out(c, (long)len);
    if (len > 0 && !write_all(c->fd, buf, (size_t)len)) return ESP_FAIL;
    c->sent = true;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len)
{
    conn_t *c = C(r);
    if (c->sent) return ESP_FAIL;
    if (!c->started) {
        head_out(c, -1);
        c->started = true;
    }
    if (len == HTTPD_RESP_USE_STRLEN) len = buf ? (ssize_t)strlen(buf) : 0;
    char sz[24];
    if (!buf || len <= 0) {
        write_all(c->fd, "0\r\n\r\n", 5);
        c->sent = true;
        return ESP_OK;
    }
    const int n = snprintf(sz, sizeof sz, "%zx\r\n", (size_t)len);
    if (!write_all(c->fd, sz, (size_t)n) || !write_all(c->fd, buf, (size_t)len) || !write_all(c->fd, "\r\n", 2))
        return ESP_FAIL;
    return ESP_OK;
}

esp_err_t httpd_resp_send_custom_err(httpd_req_t *r, const char *status, const char *msg)
{
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, msg ? msg : status, HTTPD_RESP_USE_STRLEN);
}

esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    const char *st = error == HTTPD_400_BAD_REQUEST ? "400 Bad Request"
                   : error == HTTPD_401_UNAUTHORIZED ? "401 Unauthorized"
                   : error == HTTPD_403_FORBIDDEN ? "403 Forbidden"
                   : error == HTTPD_404_NOT_FOUND ? "404 Not Found"
                   : error == HTTPD_405_METHOD_NOT_ALLOWED ? "405 Method Not Allowed"
                   : error == HTTPD_408_REQ_TIMEOUT ? "408 Request Timeout"
                   : "500 Internal Server Error";
    return httpd_resp_send_custom_err(r, st, msg);
}

/* ------------------------------------------------------------- the request */

int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
{
    conn_t *c = C(r);
    if (c->body_used < c->body_have) {
        size_t n = c->body_have - c->body_used;
        if (n > len) n = len;
        memcpy(buf, c->body + c->body_used, n);
        c->body_used += n;
        return (int)n;
    }
    const ssize_t k = recv(c->fd, buf, len, 0);
    return k > 0 ? (int)k : k == 0 ? HTTPD_SOCK_ERR_FAIL : HTTPD_SOCK_ERR_TIMEOUT;
}

int httpd_req_to_sockfd(httpd_req_t *r) { return C(r)->fd; }

esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len)
{
    const char *q = strchr(r->uri, '?');
    if (!q) return ESP_ERR_NOT_FOUND;
    if (strlen(q + 1) >= len) {
        snprintf(buf, len, "%s", q + 1);
        return ESP_ERR_HTTPD_RESULT_TRUNC;
    }
    strcpy(buf, q + 1);
    return ESP_OK;
}

esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t size)
{
    const size_t kl = strlen(key);
    for (const char *p = qry; p && *p;) {
        const char *amp = strchr(p, '&');
        const size_t n = amp ? (size_t)(amp - p) : strlen(p);
        if (n > kl && p[kl] == '=' && !strncmp(p, key, kl)) {
            const size_t vl = n - kl - 1;
            if (!size) return ESP_ERR_HTTPD_RESULT_TRUNC;
            const size_t c = vl < size - 1 ? vl : size - 1;
            memcpy(val, p + kl + 1, c);
            val[c] = 0;
            return vl < size ? ESP_OK : ESP_ERR_HTTPD_RESULT_TRUNC;
        }
        p = amp ? amp + 1 : NULL;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t size)
{
    const conn_t *c = C(r);
    const size_t fl = strlen(field);
    for (const char *l = strstr(c->head, "\r\n"); l && l[2]; l = strstr(l + 2, "\r\n")) {
        const char *s = l + 2;
        if (!strncasecmp(s, field, fl) && s[fl] == ':') {
            s += fl + 1;
            while (*s == ' ') s++;
            const char *e = strstr(s, "\r\n");
            const size_t n = e ? (size_t)(e - s) : strlen(s);
            const size_t k = n < size - 1 ? n : size - 1;
            memcpy(val, s, k);
            val[k] = 0;
            return n < size ? ESP_OK : ESP_ERR_HTTPD_RESULT_TRUNC;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------- the server */

esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u)
{
    server_t *s = h;
    if (s->n >= MAX_URIS) return ESP_FAIL;
    s->uris[s->n++] = *u;
    return ESP_OK;
}

esp_err_t httpd_register_err_handler(httpd_handle_t h, httpd_err_code_t error, httpd_err_handler_func_t fn)
{
    server_t *s = h;
    if (error == HTTPD_404_NOT_FOUND) s->on404 = fn;
    return ESP_OK;
}

static void serve_one(server_t *s, int fd)
{
    static conn_t c;
    static httpd_req_t r;
    memset(&c, 0, sizeof c);
    memset(&r, 0, sizeof r);
    c.fd = fd;
    const struct timeval tv = { .tv_sec = 5 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char *end = NULL;
    while (!end && c.head_len < HEAD_MAX) {
        const ssize_t k = recv(fd, c.head + c.head_len, HEAD_MAX - c.head_len, 0);
        if (k <= 0) return;
        c.head_len += (size_t)k;
        c.head[c.head_len] = 0;
        end = strstr(c.head, "\r\n\r\n");
    }
    if (!end) return;
    c.body = end + 4;
    c.body_have = c.head_len - (size_t)(c.body - c.head);
    end[2] = 0;                                     /* the head ends at its last line's \r\n */
    char m[8] = "";
    if (sscanf(c.head, "%7s %511s", m, r.uri) != 2) return;
    r.method = !strcmp(m, "POST") ? HTTP_POST : !strcmp(m, "GET") ? HTTP_GET : !strcmp(m, "PUT") ? HTTP_PUT
             : !strcmp(m, "DELETE") ? HTTP_DELETE : HTTP_HEAD;
    r.aux = &c;
    r.handle = s;
    char cl[24];
    if (httpd_req_get_hdr_value_str(&r, "Content-Length", cl, sizeof cl) == ESP_OK) r.content_len = strtoul(cl, 0, 10);
    const size_t pl = strcspn(r.uri, "?");
    const httpd_uri_t *u = NULL;
    for (int i = 0; i < s->n && !u; i++)
        if ((int)s->uris[i].method == r.method && strlen(s->uris[i].uri) == pl && !strncmp(s->uris[i].uri, r.uri, pl))
            u = &s->uris[i];
    if (u) {
        r.user_ctx = u->user_ctx;
        u->handler(&r);
    } else if (s->on404) {
        s->on404(&r, HTTPD_404_NOT_FOUND);
    } else {
        httpd_resp_send_err(&r, HTTPD_404_NOT_FOUND, "Nothing matches the given URI");
    }
    if (c.started && !c.sent) httpd_resp_send_chunk(&r, NULL, 0);
}

static void *accept_loop(void *arg)
{
    server_t *s = arg;
    for (;;) {
        const int fd = accept(s->fd, NULL, NULL);
        if (fd < 0) continue;
        serve_one(s, fd);
        shutdown(fd, SHUT_WR);
        /* What the browser still sends -- a body not read -- before it closes. */
        char sink[512];
        const struct timeval tv = { .tv_usec = 200000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        while (recv(fd, sink, sizeof sink, 0) > 0) {}
        close(fd);
    }
    return NULL;
}

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    (void)config;
    server_t *s = calloc(1, sizeof *s);
    if (!s) return ESP_ERR_NO_MEM;
    const char *p = getenv("SHIM_HTTP_PORT");
    const int port = p ? atoi(p) : 8080;
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s->fd, (struct sockaddr *)&a, sizeof a) || listen(s->fd, 16)) {
        perror("shim_httpd: bind");
        close(s->fd);
        free(s);
        return ESP_FAIL;
    }
    pthread_t t;
    pthread_create(&t, NULL, accept_loop, s);
    pthread_detach(t);
    *handle = s;
    shim_say("@WEB 127.0.0.1:%d", port);
    return ESP_OK;
}
