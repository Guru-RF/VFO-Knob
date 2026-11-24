/* SVXConnect's SvxLink protocol 2 (GitHub #27): the login's messages as
 * components/svx_client/svx/common/proto.c builds and reads them, laid out
 * byte by byte as SvxLink's ReflectorMsg.h packs them; the HMAC-SHA1 of
 * svx_auth.c against RFC 2202 and a digest Python's hmac computes; the
 * password's rules; protocol 2's datagrams, both ways; and svx_udp.c's
 * plain channel over the PC's loopback: what it sends, and which datagrams
 * it believes -- the reflector's own, in order. */
#include "tiny.h"
#include "common/proto.h"
#include "svx_auth.h"
#include "svx_udp.h"

#include <arpa/inet.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void hex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
    out[2 * n] = 0;
}

static void test_hmac(void)
{
    CASE("HMAC-SHA1, RFC 2202");
    uint8_t d[20];
    char h[41];
    uint8_t k1[20];
    memset(k1, 0x0b, sizeof k1);
    CHECK_EQ(svx_hmac_sha1(k1, 20, (const uint8_t *)"Hi There", 8, d), 0);
    hex(d, 20, h);
    CHECK_STR(h, "b617318655057264e28bc0b6fb378c8ef146be00");
    CHECK_EQ(svx_hmac_sha1((const uint8_t *)"Jefe", 4,
                           (const uint8_t *)"what do ya want for nothing?", 28, d), 0);
    hex(d, 20, h);
    CHECK_STR(h, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    uint8_t k6[80];
    memset(k6, 0xaa, sizeof k6);                 /* longer than SHA-1's block */
    const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    CHECK_EQ(svx_hmac_sha1(k6, 80, (const uint8_t *)m6, strlen(m6), d), 0);
    hex(d, 20, h);
    CHECK_STR(h, "aa4ae5e15272d00e95705637ce8a3b55ed402112");

    CASE("the login's digest");
    /* python3 -c "import hmac,hashlib; print(hmac.new(b'test-pass-1234',
     *             bytes(range(20)), hashlib.sha1).hexdigest())" */
    uint8_t ch[20];
    for (int i = 0; i < 20; i++) ch[i] = (uint8_t)i;
    CHECK_EQ(svx_auth_digest("test-pass-1234", ch, d), 0);
    hex(d, 20, h);
    CHECK_STR(h, "66ab9519bab737b77763aa80cb75ecc430afd54f");
    CHECK_EQ(svx_auth_digest("", ch, d), -1);
    CHECK_EQ(svx_auth_digest(NULL, ch, d), -1);
}

static void test_pass(void)
{
    CASE("a password a reflector can have");
    CHECK(svx_pass_ok("test-pass-1234"));
    CHECK(svx_pass_ok("a"));
    CHECK(svx_pass_ok("with a space inside"));
    CHECK(svx_pass_ok("\"quotes\" & 100%"));
    char p64[65], p65[66];
    memset(p64, 'x', 64); p64[64] = 0;
    memset(p65, 'x', 65); p65[65] = 0;
    CHECK(svx_pass_ok(p64));
    CHECK(!svx_pass_ok(p65));
    CHECK(!svx_pass_ok(""));
    CHECK(!svx_pass_ok(NULL));
    CHECK(!svx_pass_ok(" leading"));
    CHECK(!svx_pass_ok("trailing "));
    CHECK(!svx_pass_ok("tab\there"));
    CHECK(!svx_pass_ok("new\nline"));
    CHECK(!svx_pass_ok("caf\xc3\xa9"));         /* printable ASCII only */
}

static void test_tcp(void)
{
    uint8_t b[256];
    size_t n = 0;

    CASE("MsgProtoVer 2.0");
    CHECK_EQ(proto_build_proto_ver_at(b, sizeof b, &n, PROTO_V2_MAJOR, PROTO_V2_MINOR), 0);
    static const uint8_t pv[] = { 0, 0, 0, 6, 0, 5, 0, 2, 0, 0 };
    CHECK_EQ(n, sizeof pv);
    CHECK(memcmp(b, pv, sizeof pv) == 0);
    CHECK_EQ(proto_build_proto_ver_at(b, 9, &n, 2, 0), -1);   /* no room */

    CASE("MsgAuthChallenge");
    uint8_t m[4 + 20], c[20];
    m[0] = 0; m[1] = 10; m[2] = 0; m[3] = 20;
    for (int i = 0; i < 20; i++) m[4 + i] = (uint8_t)(0xa0 + i);
    CHECK_EQ(proto_parse_auth_challenge(m, sizeof m, c), 0);
    CHECK(memcmp(c, m + 4, 20) == 0);
    CHECK_EQ(proto_parse_auth_challenge(m, sizeof m - 1, c), -1);  /* cut short */
    CHECK_EQ(proto_parse_auth_challenge(m, 3, c), -1);
    m[3] = 16;                                                     /* not 20 */
    CHECK_EQ(proto_parse_auth_challenge(m, sizeof m, c), -1);
    m[3] = 0;                                                      /* v3's empty one */
    CHECK_EQ(proto_parse_auth_challenge(m, 4, c), -1);

    CASE("MsgAuthResponse, protocol 2");
    uint8_t dg[20];
    for (int i = 0; i < 20; i++) dg[i] = (uint8_t)(0x10 + i);
    CHECK_EQ(proto_build_auth_response_v2(b, sizeof b, &n, "N0CALL-1", dg), 0);
    /* [u32 2+2+8+2+20][u16 11][u16 8]"N0CALL-1"[u16 20][digest] */
    CHECK_EQ(n, 4 + 2 + 2 + 8 + 2 + 20);
    CHECK_EQ(b[3], 2 + 2 + 8 + 2 + 20);
    CHECK_EQ(b[0] | b[1] | b[2], 0);
    CHECK_EQ(b[4] << 8 | b[5], MSG_AUTH_RESPONSE);
    CHECK_EQ(b[6] << 8 | b[7], 8);
    CHECK(memcmp(b + 8, "N0CALL-1", 8) == 0);
    CHECK_EQ(b[16] << 8 | b[17], 20);                 /* the vector's count */
    CHECK(memcmp(b + 18, dg, 20) == 0);
    CHECK_EQ(proto_build_auth_response_v2(b, 30, &n, "N0CALL-1", dg), -1);

    CASE("MsgNodeInfo, protocol 2");
    const char *j = "{\"sysop\":\"N0CALL-1\"}";
    CHECK_EQ(proto_build_node_info_v2(b, sizeof b, &n, j, strlen(j)), 0);
    CHECK_EQ(n, 4 + 2 + 2 + strlen(j));
    CHECK_EQ(b[4] << 8 | b[5], MSG_NODE_INFO);
    CHECK_EQ(b[6] << 8 | b[7], strlen(j));
    CHECK(memcmp(b + 8, j, strlen(j)) == 0);

    CASE("MsgServerInfo, as protocol 2's reflector sends it");
    /* type, reserved, client id 0x1234, nodes ["N0CALL-2"], codecs ["OPUS"] */
    static const uint8_t si[] = { 0, 100, 0, 0, 0x12, 0x34,
                                  0, 1, 0, 8, 'N', '0', 'C', 'A', 'L', 'L', '-', '2',
                                  0, 1, 0, 4, 'O', 'P', 'U', 'S' };
    uint16_t id = 0;
    int nodes = 0, opus = 0;
    CHECK_EQ(proto_parse_server_info(si, sizeof si, &id, &nodes, &opus), 0);
    CHECK_EQ(id, 0x1234);
    CHECK_EQ(nodes, 1);
    CHECK_EQ(opus, 1);
}

static void test_udp(void)
{
    uint8_t b[256];
    size_t n = 0;
    uint16_t t, id, seq;
    const uint8_t *body;
    size_t bl;

    CASE("protocol 2's heartbeat");
    CHECK_EQ(proto_build_udp_v2(b, sizeof b, &n, UDP_MSG_HEARTBEAT, 0x0102, 0, NULL, 0), 0);
    static const uint8_t hb[] = { 0, 1, 1, 2, 0, 0 };
    CHECK_EQ(n, sizeof hb);
    CHECK(memcmp(b, hb, sizeof hb) == 0);
    CHECK_EQ(proto_parse_udp_v2(b, n, &t, &id, &seq, &body, &bl), 0);
    CHECK_EQ(t, UDP_MSG_HEARTBEAT);
    CHECK_EQ(id, 0x0102);
    CHECK_EQ(seq, 0);
    CHECK_EQ(bl, 0);

    CASE("protocol 2's audio");
    const uint8_t opus[] = { 0x78, 0x01, 0x02, 0x03, 0x04 };
    CHECK_EQ(proto_build_udp_v2(b, sizeof b, &n, UDP_MSG_AUDIO, 7, 0xfffe, opus, sizeof opus), 0);
    static const uint8_t au[] = { 0, 101, 0, 7, 0xff, 0xfe, 0, 5, 0x78, 1, 2, 3, 4 };
    CHECK_EQ(n, sizeof au);
    CHECK(memcmp(b, au, sizeof au) == 0);
    CHECK_EQ(proto_parse_udp_v2(b, n, &t, &id, &seq, &body, &bl), 0);
    CHECK_EQ(t, UDP_MSG_AUDIO);
    CHECK_EQ(seq, 0xfffe);
    CHECK_EQ(bl, sizeof opus);
    CHECK(memcmp(body, opus, sizeof opus) == 0);
    CHECK_EQ(proto_parse_udp_v2(b, n - 1, &t, &id, &seq, &body, &bl), -1);  /* cut short */
    CHECK_EQ(proto_parse_udp_v2(b, 7, &t, &id, &seq, &body, &bl), -1);
    CHECK_EQ(proto_parse_udp_v2(b, 5, &t, &id, &seq, &body, &bl), -1);
    CHECK_EQ(proto_build_udp_v2(b, 12, &n, UDP_MSG_AUDIO, 7, 0, opus, sizeof opus), -1);

    CASE("protocol 2's flush, and what the reflector says back");
    CHECK_EQ(proto_build_udp_v2(b, sizeof b, &n, UDP_MSG_FLUSH_SAMPLES, 7, 3, NULL, 0), 0);
    CHECK_EQ(n, 6);
    static const uint8_t fl[] = { 0, 103, 0, 7, 0, 9 };    /* AllSamplesFlushed */
    CHECK_EQ(proto_parse_udp_v2(fl, sizeof fl, &t, &id, &seq, &body, &bl), 0);
    CHECK_EQ(t, UDP_MSG_ALL_SAMPLES_FLUSHED);
    CHECK_EQ(seq, 9);
    CHECK_EQ(bl, 0);
}

/* A socket on 127.0.0.1, any port: its fd, and the port in *port. */
static int udp_local(uint16_t *port)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &al) != 0) return -1;
    *port = ntohs(a.sin_port);
    return fd;
}

/* From `fd` to the knob's channel: a datagram as the reflector builds one. */
static void to_knob(int fd, uint16_t knob_port, uint16_t type, uint16_t seq,
                    const uint8_t *body, size_t len)
{
    uint8_t b[64];
    size_t n = 0;
    proto_build_udp_v2(b, sizeof b, &n, type, 0x55, seq, body, len);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(knob_port),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    sendto(fd, b, n, 0, (struct sockaddr *)&to, sizeof to);
}

/* What the channel believes next: 1 with its type, gap and first byte, or 0. */
static int from_refl(svx_udp_t *u, uint16_t *type, int *gap, int *first)
{
    struct pollfd p = { .fd = u->fd, .events = POLLIN };
    poll(&p, 1, 200);
    const uint8_t *body;
    size_t len;
    const int k = svx_udp_recv(u, type, &body, &len, gap);
    if (k > 0) *first = len ? body[0] : -1;
    return k;
}

static void test_udp_plain(void)
{
    CASE("svx_udp.c, protocol 2: what goes to the reflector");
    uint16_t rport = 0, oport = 0;
    const int refl = udp_local(&rport), other = udp_local(&oport);
    CHECK(refl >= 0 && other >= 0);
    svx_udp_t u;
    CHECK_EQ(svx_udp_open(&u, htonl(INADDR_LOOPBACK), rport), 0);
    svx_udp_plain(&u, 0x55);
    CHECK(u.keyed_tx && u.keyed_rx);              /* ready at once: PTT may key */
    CHECK_EQ(svx_udp_send(&u, UDP_MSG_HEARTBEAT, NULL, 0), 0);
    const uint8_t op[] = { 0xf8, 0xff, 0xfe };
    CHECK_EQ(svx_udp_send(&u, UDP_MSG_AUDIO, op, sizeof op), 0);
    CHECK_EQ(svx_udp_send(&u, UDP_MSG_FLUSH_SAMPLES, NULL, 0), 0);
    CHECK_EQ(u.tx_seq, 3);
    CHECK_EQ(u.n_tx, 3);
    uint8_t b[64];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    static const uint8_t hb[] = { 0, 1, 0, 0x55, 0, 0 };
    static const uint8_t au[] = { 0, 101, 0, 0x55, 0, 1, 0, 3, 0xf8, 0xff, 0xfe };
    static const uint8_t fs[] = { 0, 102, 0, 0x55, 0, 2 };
    ssize_t k = recvfrom(refl, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
    CHECK(k == sizeof hb && memcmp(b, hb, sizeof hb) == 0);
    k = recv(refl, b, sizeof b, 0);
    CHECK(k == sizeof au && memcmp(b, au, sizeof au) == 0);
    k = recv(refl, b, sizeof b, 0);
    CHECK(k == sizeof fs && memcmp(b, fs, sizeof fs) == 0);
    const uint16_t kport = ntohs(from.sin_port);

    CASE("svx_udp.c, protocol 2: what is believed");
    uint16_t t;
    int gap, first;
    const uint8_t a1[] = { 1 }, a2[] = { 2 }, a3[] = { 3 }, a4[] = { 4 };
    to_knob(refl, kport, UDP_MSG_HEARTBEAT, 0, NULL, 0);
    CHECK_EQ(from_refl(&u, &t, &gap, &first), 1);
    CHECK_EQ(t, UDP_MSG_HEARTBEAT);
    CHECK_EQ(gap, 0);
    to_knob(refl, kport, UDP_MSG_AUDIO, 1, a1, 1);
    CHECK_EQ(from_refl(&u, &t, &gap, &first), 1);
    CHECK_EQ(t, UDP_MSG_AUDIO);
    CHECK_EQ(first, 1);
    CHECK_EQ(gap, 0);
    to_knob(refl, kport, UDP_MSG_AUDIO, 3, a3, 1);       /* 2 went missing */
    CHECK_EQ(from_refl(&u, &t, &gap, &first), 1);
    CHECK_EQ(first, 3);
    CHECK_EQ(gap, 1);
    to_knob(refl, kport, UDP_MSG_AUDIO, 2, a2, 1);       /* ...and comes late */
    to_knob(other, kport, UDP_MSG_AUDIO, 4, a4, 1);      /* not the reflector */
    CHECK_EQ(from_refl(&u, &t, &gap, &first), 0);
    CHECK_EQ(u.n_replayed, 1);
    CHECK_EQ(u.n_auth_fail, 1);
    CHECK_EQ(u.n_lost, 1);
    CHECK_EQ(u.n_rx, 3);
    to_knob(refl, kport, UDP_MSG_AUDIO, 4, a4, 1);       /* the next is fine */
    CHECK_EQ(from_refl(&u, &t, &gap, &first), 1);
    CHECK_EQ(first, 4);
    CHECK_EQ(gap, 0);

    CASE("svx_udp.c, protocol 2: seq wraps at 16 bits");
    u.rx_high = 0xfffe;
    to_knob(refl, kport, UDP_MSG_HEARTBEAT, 0xffff, NULL, 0);
    CHECK_EQ(from_refl(&u, &t, &gap, &first), 1);
    CHECK_EQ(gap, 0);
    to_knob(refl, kport, UDP_MSG_HEARTBEAT, 0, NULL, 0);
    CHECK_EQ(from_refl(&u, &t, &gap, &first), 1);
    CHECK_EQ(gap, 0);
    u.tx_seq = 0xffff;
    CHECK_EQ(svx_udp_send(&u, UDP_MSG_HEARTBEAT, NULL, 0), 0);
    CHECK_EQ(u.tx_seq, 0);                                /* and keeps sending */
    CHECK_EQ(svx_udp_send(&u, UDP_MSG_HEARTBEAT, NULL, 0), 0);
    k = recv(refl, b, sizeof b, 0);
    CHECK(k == 6 && b[4] == 0xff && b[5] == 0xff);
    k = recv(refl, b, sizeof b, 0);
    CHECK(k == 6 && b[4] == 0 && b[5] == 0);

    svx_udp_close(&u);
    close(refl);
    close(other);
}

T_MAIN(test_hmac(); test_pass(); test_tcp(); test_udp(); test_udp_plain())
