/* The knob on a tailnet: Tailscale, through MicroLink (components/microlink),
 * so a TailChaser node on the owner's tailnet (on0ora-tx, 100.x.y.z) is
 * reached from wherever the knob is, with nothing changed at the repeater.
 *
 * Started once, after the WiFi has an address; it stays up until the knob
 * restarts. The node's keys are the knob's own (the settings store), so it
 * stays the same node on the tailnet across restarts. With no auth key it
 * asks for a login: the page and the face show the address to open. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TC_TS_OFF = 0,          /* not asked for                                */
    TC_TS_STARTING,         /* talking to the control server                */
    TC_TS_LOGIN,            /* waiting for someone to open the login address */
    TC_TS_UP,               /* on the tailnet                               */
    TC_TS_REFUSED,          /* the auth key refused, or the node not approved */
    TC_TS_FAILED,           /* could not start                              */
} tc_ts_state_t;

/* Once; auth_key "" for a login in the browser. */
void tc_ts_start(const char *auth_key, const char *device_name);
tc_ts_state_t tc_ts_state(void);
const char   *tc_ts_state_word(tc_ts_state_t s);
/* The login address while TC_TS_LOGIN; false otherwise. */
bool     tc_ts_login_url(char *out, size_t cap);
uint32_t tc_ts_ip(void);                    /* this knob's, host order; 0 = none */
int      tc_ts_peers(void);
/* A peer by its name on the tailnet, "on0ora-tx" or its full MagicDNS name;
 * 0 when the tailnet has none by that name. Host order. */
uint32_t tc_ts_resolve(const char *host);
/* Forget this knob's keys: a new node on the next start. */
void     tc_ts_forget(void);

/* A TCP connection through the tunnel. recv: bytes, 0 when nothing came
 * within timeout_ms, -1 when closed or failed. */
typedef struct tc_ts_conn tc_ts_conn_t;
tc_ts_conn_t *tc_ts_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms);
/* How a peer (host order) is reached: 1 direct UDP, 2 through a DERP relay,
 * 0 not known. */
int  tc_ts_path(uint32_t ip);
int  tc_ts_send(tc_ts_conn_t *c, const void *p, size_t n);
int  tc_ts_recv(tc_ts_conn_t *c, void *p, size_t n, uint32_t timeout_ms);
void tc_ts_close(tc_ts_conn_t *c);

/* 100.64.0.0/10: Tailscale's addresses. */
static inline bool tc_ts_is_tailnet(uint32_t ip)
{
    return (ip & 0xFFC00000u) == 0x64400000u;
}
