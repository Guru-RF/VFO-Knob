/* One HTTP exchange with a TailChaser node: through the tailnet when the
 * node is on it (tc_ts.h), else over the LAN or the internet as it is. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tc_proto.h"

typedef struct {
    int         code;           /* the HTTP status; 0 = no answer            */
    const char *body;           /* inside the caller's buffer                */
    size_t      body_len;
    bool        tailnet;        /* it went through Tailscale                 */
    bool        wait_ts;        /* nothing tried: Tailscale is not up yet    */
    char        why[80];        /* what went wrong, when nothing came back   */
    char        addr[20];       /* the address it went to, "100.101.102.103" */
    uint32_t    ip;             /* the same, host order                      */
} tc_http_t;

/* method "GET" or "POST"; form NULL, or a POST's form ("tx=1"). The answer
 * lands in buf (cap bytes, the largest kept). The key goes in X-API-Key and
 * nowhere else -- never logged. True when an HTTP answer came, whatever its
 * status. */
bool tc_http(const tc_addr_t *a, const char *key, const char *method, const char *path, const char *form,
             char *buf, size_t cap, tc_http_t *r);

/* A connection kept open -- the audio's websocket -- made as tc_http makes
 * its own: through the tailnet or not. NULL when it could not be, why (or
 * wait_ts) in r; r's tailnet and addr say how it went. */
typedef struct tc_conn tc_conn_t;
tc_conn_t *tc_conn_open(const tc_addr_t *a, tc_http_t *r);
/* n, or -1. */
int  tc_conn_send(tc_conn_t *c, const void *p, size_t n);
/* Bytes; 0 when nothing came within timeout_ms; -1 closed or failed. */
int  tc_conn_recv(tc_conn_t *c, void *p, size_t n, uint32_t timeout_ms);
void tc_conn_close(tc_conn_t *c);
