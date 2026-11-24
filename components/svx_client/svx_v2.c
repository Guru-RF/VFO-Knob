/* SvxLink protocol 2's login. See svx_v2.h. */
#include "svx_v2.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"

#include "common/proto.h"
#include "svx_auth.h"

static const char *TAG = "svx-v2";

#define SEND_MS 3000

static int fail(svx_v2_result_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int fail(svx_v2_result_t *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->why, sizeof r->why, fmt, ap);
    va_end(ap);
    return -1;
}

int svx_v2_login(svx_link_t *l, svx_udp_t *u, const char *call, const char *pass,
                 const char *json, uint8_t *frame, size_t cap, int step_ms,
                 svx_v2_result_t *r)
{
    memset(r, 0, sizeof *r);
    uint16_t type;
    uint8_t *p;
    size_t   len, n;

    if (proto_build_proto_ver_at(frame, cap, &n, PROTO_V2_MAJOR, PROTO_V2_MINOR) != 0 ||
        svx_link_send(l, frame, n, SEND_MS) != 0)
        return fail(r, "could not greet the reflector");
    for (;;) {
        const int k = svx_link_frame(l, &type, &p, &len, step_ms);
        if (k <= 0) return fail(r, k ? "the reflector closed the connection" : "no answer from the reflector");
        switch (type) {
        case MSG_HEARTBEAT:
            if (proto_build_heartbeat(frame, cap, &n) == 0) svx_link_send(l, frame, n, SEND_MS);
            break;
        case MSG_AUTH_CHALLENGE: {
            uint8_t challenge[SVX_CHALLENGE_LEN], digest[20];
            if (proto_parse_auth_challenge(p, len, challenge) != 0)
                return fail(r, "the reflector's challenge does not parse");
            const bool ok = svx_auth_digest(pass, challenge, digest) == 0 &&
                            proto_build_auth_response_v2(frame, cap, &n, call, digest) == 0 &&
                            svx_link_send(l, frame, n, SEND_MS) == 0;
            memset(digest, 0, sizeof digest);
            memset(frame, 0, n <= cap ? n : cap);
            if (!ok) return fail(r, "could not answer the reflector's challenge");
            break;
        }
        case MSG_AUTH_OK:
            ESP_LOGI(TAG, "authenticated as %s, with a password", call);
            break;
        case MSG_SERVER_INFO: {
            int nodes = 0, opus = 0;
            if (proto_parse_server_info(p, len, &r->client_id, &nodes, &opus) != 0)
                return fail(r, "the reflector's ServerInfo does not parse");
            r->nodes = nodes;
            r->opus  = opus != 0;
            if (svx_udp_open(u, l->addr, l->port) != 0) return fail(r, "no socket for the audio channel");
            svx_udp_plain(u, r->client_id);
            if (proto_build_node_info_v2(frame, cap, &n, json, strlen(json)) != 0 ||
                svx_link_send(l, frame, n, SEND_MS) != 0)
                return fail(r, "could not send NodeInfo");
            /* The first datagram: the reflector learns our port from it. */
            svx_udp_send(u, UDP_MSG_HEARTBEAT, NULL, 0);
            l->plain = true;                 /* from now on read as it comes */
            return 0;
        }
        case MSG_ERROR: {
            char e[80] = "";
            proto_parse_error(p, len, e, sizeof e);
            /* "Access denied" is any of three: the callsign is not one of the
             * reflector's [USERS], its group has no password, or the password
             * is not that one -- or the callsign is logged in already. */
            if (!strcasecmp(e, "Access denied")) {
                r->denied = true;
                return fail(r, "reflector: access denied -- check the callsign and the password");
            }
            return fail(r, "reflector: %s", e);
        }
        case MSG_PROTO_VER_DOWNGRADE: {
            uint16_t ma = 0, mi = 0;
            proto_parse_proto_ver_downgrade(p, len, &ma, &mi);
            return fail(r, "the reflector speaks protocol %u.%u, not 2.0", ma, mi);
        }
        case MSG_CA_INFO:
            return fail(r, "the reflector asks for a certificate: choose protocol 3");
        default:
            break;
        }
    }
}
