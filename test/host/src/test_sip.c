/* The phone firmware's SIP (components/phone_client/sip.c), on the PC: a
 * "server" on a UDP socket of its own sends the knob's socket requests, and
 * sip_on_readable() takes them as the knob does. Anyone who reaches the port
 * can send anything: an INVITE without its Call-ID handed strlcpy() a NULL,
 * and the knob restarted. A message without any of RFC 3261's five
 * mandatory headers is dropped now; a whole INVITE rings as ever. */
#include "tiny.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sip.h"

static int s_srv;                 /* the server's socket */
static struct sockaddr_in s_knob; /* the knob's */
static int s_calls;
static sip_call_t s_state = SIP_CALL_IDLE;
static char s_peer[64];

static void on_call(sip_call_t state, const char *peer, const char *number, const char *why)
{
    s_calls++;
    s_state = state;
    snprintf(s_peer, sizeof s_peer, "%s", number);
}

static void on_media(const sip_media_t *m) {}

static const char *SDP =
    "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
    "m=audio 30000 RTP/AVP 8 0 101\r\na=rtpmap:8 PCMA/8000\r\na=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:101 telephone-event/8000\r\n";

/* An INVITE as a provider sends one, the header named `omit` left out. */
static size_t invite(char *b, size_t cap, const char *omit, const char *callid)
{
    const unsigned kp = ntohs(s_knob.sin_port);
    char h[8][160];
    snprintf(h[0], sizeof h[0], "Via: SIP/2.0/UDP 127.0.0.1;branch=z9hG4bK776asdhds");
    snprintf(h[1], sizeof h[1], "Max-Forwards: 70");
    snprintf(h[2], sizeof h[2], "From: \"Alice\" <sip:0470123456@127.0.0.1>;tag=1928301774");
    snprintf(h[3], sizeof h[3], "To: <sip:knob@127.0.0.1>");
    snprintf(h[4], sizeof h[4], "Call-ID: %s", callid);
    snprintf(h[5], sizeof h[5], "CSeq: 314159 INVITE");
    snprintf(h[6], sizeof h[6], "Contact: <sip:alice@127.0.0.1:5070>");
    snprintf(h[7], sizeof h[7], "Content-Type: application/sdp");
    size_t n = (size_t)snprintf(b, cap, "INVITE sip:knob@127.0.0.1:%u SIP/2.0\r\n", kp);
    for (int i = 0; i < 8; i++) {
        if (omit && !strncmp(h[i], omit, strlen(omit)) && h[i][strlen(omit)] == ':') continue;
        n += (size_t)snprintf(b + n, cap - n, "%s\r\n", h[i]);
    }
    n += (size_t)snprintf(b + n, cap - n, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(SDP), SDP);
    return n;
}

/* `b` to the knob's socket, taken as the knob takes it. */
static void deliver(const char *b, size_t n)
{
    sendto(s_srv, b, n, 0, (const struct sockaddr *)&s_knob, sizeof s_knob);
    sip_on_readable(1000);
}

/* The server's socket, and the knob's SIP started against it. */
static void start(void)
{
    s_srv = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(s_srv, (struct sockaddr *)&a, sizeof a);
    socklen_t al = sizeof a;
    getsockname(s_srv, (struct sockaddr *)&a, &al);

    sip_account_t acc = { .user = "knob", .pass = "secret", .domain = "127.0.0.1", .port = ntohs(a.sin_port) };
    const sip_events_t ev = { .on_call = on_call, .on_media = on_media };
    sip_start(&acc, &ev, 40000);
    socklen_t kl = sizeof s_knob;
    getsockname(sip_socket(), (struct sockaddr *)&s_knob, &kl);
    s_knob.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

/* Each case on a session of its own: a call taken by one would have the
 * next answered "busy", never reaching what it tries. */
static void stop(void)
{
    sip_stop();
    close(s_srv);
    s_calls = 0;
    s_state = SIP_CALL_IDLE;
}

static void test_mandatory(void)
{
    static char b[2048];
    const char *five[] = { "Via", "From", "To", "Call-ID", "CSeq" };
    for (int i = 0; i < 5; i++) {
        CASE(five[i]);
        start();
        deliver(b, invite(b, sizeof b, five[i], "a84b4c76e66710@pc33"));
        CHECK_EQ(s_calls, 0);
        CHECK_EQ(sip_call_state(), SIP_CALL_IDLE);
        stop();
    }

    CASE("a response without its CSeq");
    start();
    const char *r = "SIP/2.0 200 OK\r\nVia: SIP/2.0/UDP 127.0.0.1;branch=z9hG4bKx\r\n"
                    "From: <sip:knob@127.0.0.1>;tag=1\r\nTo: <sip:knob@127.0.0.1>\r\n"
                    "Call-ID: nothing@here\r\nContent-Length: 0\r\n\r\n";
    deliver(r, strlen(r));
    CHECK_EQ(s_calls, 0);
    stop();

    CASE("the whole INVITE rings");
    start();
    deliver(b, invite(b, sizeof b, NULL, "a84b4c76e66710@pc33"));
    CHECK_EQ(s_calls, 1);
    CHECK_EQ(s_state, SIP_CALL_IN);
    CHECK_EQ(sip_call_state(), SIP_CALL_IN);
    CHECK_STR(s_peer, "0470123456");
    stop();
}

T_MAIN(test_mandatory())
