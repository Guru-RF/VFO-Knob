/* SPDX-License-Identifier: MIT
 * SVXConnect-CLI — Copyright (c) 2026 Joeri Van Dooren
 *
 * SvxLink reflector protocol v3.
 *
 * Wire invariants — these are load-bearing. Changing any of them silently
 * breaks interoperability in ways that look like "audio sometimes stops":
 *
 *   PROTO 3.0, default port 5300 (TCP control AND UDP audio, same host:port)
 *
 *   TCP frame  = [u32 BE body_len][u16 BE type][body]   body_len EXCLUDES itself
 *   TCP string = [u16 BE len][raw bytes]                no NUL terminator
 *
 *   UDP wire   = [AAD][TAG(8, truncated from GCM's 16)][CIPHERTEXT]
 *     TX IV  = tx_iv_rand[6] || client_id BE16 || counter BE32          (12 B)
 *     RX IV  = rx_iv_rand[4] || 0x00 0x00     || counter BE32           (12 B)
 *     TX AAD = counter BE32 (4 B); FIRST packet only: counter BE32 ||
 *              client_id BE16 (6 B) with counter == 0
 *     RX AAD = counter BE32 (always 4 B)
 *
 *   UDP plaintext: type 101 (audio) => [u16 type][u16 len][opus]
 *                  ALL OTHER types  => [u16 type][raw body]
 *
 *   MsgAuthResponse digest is 20 ZERO bytes — mTLS is the real authentication.
 *   MsgStartUDPEncryption with an EMPTY body means "reuse OUR TX key for RX".
 *   MsgSelectTG RESETS the server-side monitor list, so MsgTgMonitor must
 *     ALWAYS be re-sent immediately after it.
 *   MsgTgMonitor is a FULL REPLACEMENT SET: u16 count + N x u32, sorted asc.
 *   MsgTalkerStop must be matched on CALLSIGN ONLY — the tg the server reports
 *     on stop can differ from the one it reported on start.
 *   The first UDP datagram must be sent immediately after MsgStartUDPEncryption
 *     (it opens the NAT pinhole and advertises our client_id).
 *   Heartbeats: TCP <= 5000 ms, UDP <= 10000 ms, and MIRROR every inbound
 *     MsgHeartbeat.
 */
#ifndef SVX_PROTO_H
#define SVX_PROTO_H

#include <stdint.h>
#include <stddef.h>

#define PROTO_MAJOR 3
#define PROTO_MINOR 0

/* Protocol 2, the one before certificates: a reflector's [USERS] and
 * [PASSWORDS], everything in the clear (SvxLink's ReflectorMsg.h, and
 * SVXConnect for Android's CallPassword profiles).
 *
 *   TCP: MsgProtoVer 2.0 -> MsgAuthChallenge [u16 20][20 random bytes]
 *        -> MsgAuthResponse [string callsign][u16 20][HMAC-SHA1 digest]
 *        -> MsgAuthOk, MsgServerInfo -> MsgNodeInfo with the JSON alone.
 *        The digest is keyed with the password's bytes, over the challenge:
 *        the password itself never goes over the network.
 *   UDP: [u16 type][u16 client_id][u16 seq], then the body -- audio's
 *        [u16 len][opus] as in v3 -- no encryption. The reflector knows the
 *        client by the id and the address of its TCP connection; seq counts
 *        each side's datagrams, wrapping at 16 bits.
 */
#define PROTO_V2_MAJOR 2
#define PROTO_V2_MINOR 0
#define SVX_CHALLENGE_LEN 20
#define SVX_UDP_V2_HDR     6

#define SVX_DEFAULT_PORT 5300

/* Timers, in milliseconds. */
#define SVX_TCP_HEARTBEAT_MS   5000
#define SVX_UDP_HEARTBEAT_MS  10000

/* TCP control-channel message types. */
enum {
    MSG_HEARTBEAT             = 1,
    MSG_PROTO_VER             = 5,
    MSG_PROTO_VER_DOWNGRADE   = 6,
    MSG_AUTH_CHALLENGE        = 10,
    MSG_AUTH_RESPONSE         = 11,
    MSG_AUTH_OK               = 12,
    MSG_ERROR                 = 13,
    MSG_START_ENC_REQUEST     = 14,
    MSG_START_ENCRYPTION      = 15,
    MSG_CLIENT_CSR_REQUEST    = 16,
    MSG_CLIENT_CSR            = 17,
    MSG_CLIENT_CERT           = 18,
    MSG_CA_INFO               = 19,
    MSG_CA_BUNDLE_REQUEST     = 20,
    MSG_CA_BUNDLE_RESPONSE    = 21,
    MSG_SERVER_INFO           = 100,
    MSG_NODE_JOINED           = 102,
    MSG_NODE_LEFT             = 103,
    MSG_TALKER_START          = 104,
    MSG_TALKER_STOP           = 105,
    MSG_SELECT_TG             = 106,
    MSG_TG_MONITOR            = 107,
    MSG_NODE_INFO             = 111,
    MSG_START_UDP_ENCRYPTION  = 114
};

/* UDP audio-channel message types. */
enum {
    UDP_MSG_HEARTBEAT           = 1,
    UDP_MSG_AUDIO               = 101,
    UDP_MSG_FLUSH_SAMPLES       = 102,
    UDP_MSG_ALL_SAMPLES_FLUSHED = 103
};

const char *proto_msg_name(uint16_t type);

/* ---- builders ----
 * All write a complete frame INCLUDING the 4-byte length prefix, so the result
 * can be handed straight to send()/SSL_write(). Return 0 on success, -1 if the
 * buffer is too small; *out_len receives the total byte count. */

int proto_build_proto_ver     (uint8_t *buf, size_t cap, size_t *out_len);
/* MsgProtoVer at another version: protocol 2's login. */
int proto_build_proto_ver_at  (uint8_t *buf, size_t cap, size_t *out_len,
                               uint16_t major, uint16_t minor);
int proto_build_ca_bundle_req (uint8_t *buf, size_t cap, size_t *out_len);
int proto_build_start_enc_req (uint8_t *buf, size_t cap, size_t *out_len);
int proto_build_heartbeat     (uint8_t *buf, size_t cap, size_t *out_len);
int proto_build_csr_request   (uint8_t *buf, size_t cap, size_t *out_len);
int proto_build_client_csr    (uint8_t *buf, size_t cap, size_t *out_len,
                               const char *csr_pem, size_t csr_pem_len);
int proto_build_auth_response (uint8_t *buf, size_t cap, size_t *out_len,
                               const char *callsign, const uint8_t digest[20]);
/* Protocol 2's: the digest as a vector, its u16 length ahead of it, the way
 * SvxLink's Async::Msg packs one. */
int proto_build_auth_response_v2(uint8_t *buf, size_t cap, size_t *out_len,
                                 const char *callsign, const uint8_t digest[20]);
/* Protocol 2's MsgNodeInfo: the JSON alone, no UDP key. */
int proto_build_node_info_v2  (uint8_t *buf, size_t cap, size_t *out_len,
                               const char *json, size_t json_len);
int proto_build_node_info     (uint8_t *buf, size_t cap, size_t *out_len,
                               const uint8_t *iv_rand, size_t iv_len,
                               const uint8_t *key,     size_t key_len,
                               const char *json,       size_t json_len);
int proto_build_select_tg     (uint8_t *buf, size_t cap, size_t *out_len,
                               uint32_t tg_id);
/* tg_ids must be sorted ascending; this is a full replacement set. */
int proto_build_tg_monitor    (uint8_t *buf, size_t cap, size_t *out_len,
                               const uint32_t *tg_ids, size_t n);

/* ---- parsers ----
 * All operate on the message BODY, i.e. the bytes after the 4-byte length
 * prefix, so payload[0..1] is the big-endian type. */

/* MsgServerInfo. Any out pointer may be NULL.
 * Layout: type(2) reserved(2) clientID(2) then a string list of node
 * callsigns and a string list of supported audio codecs. We need the client
 * id (for the UDP IV) and the node count (for the status bar); the codec list
 * is parsed only to confirm OPUS is offered. */
int proto_parse_server_info(const uint8_t *payload, size_t len,
                            uint16_t *out_client_id,
                            int      *out_node_count,
                            int      *out_have_opus);

/* MsgAuthChallenge: [u16 20][20 bytes]. 0 with the challenge, -1 if it is
 * not 20 bytes. */
int proto_parse_auth_challenge(const uint8_t *payload, size_t len,
                               uint8_t out[SVX_CHALLENGE_LEN]);

/* MsgProtoVerDowngrade — the server refuses 3.0 and names what it wants. */
int proto_parse_proto_ver_downgrade(const uint8_t *payload, size_t len,
                                    uint16_t *out_major, uint16_t *out_minor);

/* MsgStartUDPEncryption.
 * Returns 0 and fills iv_rand/key when the server supplied them,
 *         1 when the body is empty (reuse our own TX key for RX),
 *        -1 on a malformed message. */
int proto_parse_start_udp_encryption(const uint8_t *payload, size_t len,
                                     uint8_t out_iv_rand[4],
                                     uint8_t out_key[16]);

/* MsgTalkerStart (104) / MsgTalkerStop (105) share a layout. */
int proto_parse_talker(const uint8_t *payload, size_t len,
                       uint32_t *out_tg, char *out_callsign, size_t cs_cap);

/* MsgNodeJoined (102) / MsgNodeLeft (103): a single callsign string. */
int proto_parse_node_event(const uint8_t *payload, size_t len,
                           char *out_callsign, size_t cs_cap);

/* MsgError: a single message string. */
int proto_parse_error(const uint8_t *payload, size_t len,
                      char *out_msg, size_t msg_cap);

/* Scan a payload for PEM blocks and concatenate them, NUL-terminated. Used for
 * both the CA bundle and the signed client certificate, because the server
 * wraps one in a string field and appends the other raw. */
int proto_parse_pem_blob(const uint8_t *payload, size_t len,
                         char *out, size_t out_cap);

/* ---- UDP plaintext ----
 * Build the plaintext of a UDP message (before encryption). Note the audio
 * type carries an extra u16 length that other types do not. */
int proto_build_udp_plaintext(uint8_t *buf, size_t cap, size_t *out_len,
                              uint16_t type,
                              const uint8_t *body, size_t body_len);

/* Split a decrypted UDP plaintext into its type and body. For type 101 the
 * body is the Opus frame with the inner u16 length already removed. */
int proto_parse_udp_plaintext(const uint8_t *pt, size_t len,
                              uint16_t *out_type,
                              const uint8_t **out_body, size_t *out_body_len);

/* ---- UDP, protocol 2 ----
 * The whole datagram: [u16 type][u16 client_id][u16 seq] and the body as
 * proto_build_udp_plaintext() has it after its type. */
int proto_build_udp_v2(uint8_t *buf, size_t cap, size_t *out_len,
                       uint16_t type, uint16_t client_id, uint16_t seq,
                       const uint8_t *body, size_t body_len);
/* A received datagram: its type, client id and seq, and the body as
 * proto_parse_udp_plaintext() gives it. */
int proto_parse_udp_v2(const uint8_t *dg, size_t len,
                       uint16_t *out_type, uint16_t *out_client_id, uint16_t *out_seq,
                       const uint8_t **out_body, size_t *out_body_len);

#endif
