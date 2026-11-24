/* SvxLink protocol 2's login: callsign and password, in the clear.
 *
 *   -> MsgProtoVer 2.0
 *   <- MsgAuthChallenge, 20 random bytes
 *   -> MsgAuthResponse: the callsign, HMAC-SHA1(password, challenge)
 *   <- MsgAuthOk, MsgServerInfo (our client id)
 *   -> MsgNodeInfo, the JSON alone; a UDP heartbeat, from which the
 *      reflector learns the audio channel's port
 *
 * A wrong callsign, group or password is the reflector's MsgError "Access
 * denied". Nothing is encrypted after the login, the audio included.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "svx_net.h"
#include "svx_udp.h"

typedef struct {
    uint16_t client_id;
    int      nodes;             /* online, from ServerInfo */
    bool     opus;              /* the reflector offers Opus */
    bool     denied;            /* "Access denied": the callsign or the password */
    char     why[96];           /* what went wrong, when it did */
} svx_v2_result_t;

/* On a connection open in the clear: 0 when logged in -- the link then reads
 * as it comes (svx_link_pump), the audio channel is open in the clear and has
 * sent its first datagram -- or -1, with r->why. `json` is the NodeInfo,
 * `frame` scratch for what is sent, `step_ms` the wait for each answer. The
 * password is used for the digest only: never logged, never sent. */
int svx_v2_login(svx_link_t *l, svx_udp_t *u, const char *call, const char *pass,
                 const char *json, uint8_t *frame, size_t cap, int step_ms,
                 svx_v2_result_t *r);
