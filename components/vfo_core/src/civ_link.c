/* CI-V as a byte stream: see civ_link.h. */
#include "civ_link.h"

#include <ctype.h>
#include <string.h>

void civ_rx_init(civ_rx_t *r) { memset(r, 0, sizeof *r); }

size_t civ_rx_byte(civ_rx_t *r, uint8_t b)
{
    if (r->len == 0) {                       /* looking for the first FE */
        if (b == 0xFE) r->buf[r->len++] = b;
        else r->junk++;
        return 0;
    }
    if (r->len == 1) {                       /* ...and the second */
        if (b == 0xFE) r->buf[r->len++] = b;
        else {
            r->len = 0;
            r->junk += 2;
        }
        return 0;
    }
    /* A third FE or more: the preamble is longer, as CI-V allows. */
    if (r->len == 2 && b == 0xFE) return 0;
    if (b == 0xFC) {                         /* a collision on the bus: the frame is void */
        r->len = 0;
        r->dropped++;
        return 0;
    }
    if (r->len >= CIV_FRAME_MAX) {           /* too long to be ours: start again */
        r->len = 0;
        r->dropped++;
        if (b == 0xFE) r->buf[r->len++] = b;
        return 0;
    }
    r->buf[r->len++] = b;
    if (b != 0xFD) return 0;
    const size_t n = r->len;
    r->len = 0;
    /* FE FE to from cmd FD at the least. */
    if (n < 6) {
        r->dropped++;
        return 0;
    }
    r->frames++;
    return n;
}

size_t civ_frame_build(uint8_t *out, size_t cap, uint8_t to, uint8_t from, const uint8_t *body, size_t n)
{
    if (!out || !body || n == 0 || n + 5 > cap) return 0;
    if (memchr(body, 0xFD, n)) return 0;
    out[0] = 0xFE;
    out[1] = 0xFE;
    out[2] = to;
    out[3] = from;
    memcpy(out + 4, body, n);
    out[4 + n] = 0xFD;
    return n + 5;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool civ_bda_parse(const char *s, uint8_t out[6])
{
    if (!s || strlen(s) != 17) return false;
    uint8_t a[6];
    for (int i = 0; i < 6; i++) {
        const char *p = s + i * 3;
        const int   h = hexval(p[0]), l = hexval(p[1]);
        if (h < 0 || l < 0) return false;
        if (i < 5 && p[2] != ':' && p[2] != '-') return false;
        a[i] = (uint8_t)(h << 4 | l);
    }
    memcpy(out, a, 6);
    return true;
}

bool civ_is_icom_bt_name(const char *name)
{
    return name && !strncmp(name, "ICOM BT", 7);
}

uint8_t civ_level_from(unsigned v, unsigned full)
{
    if (!full) return 0;
    if (v > full) v = full;
    return (uint8_t)((v * 255u + full / 2) / full);
}

unsigned civ_level_to(uint8_t level, unsigned full)
{
    return (level * full + 127u) / 255u;
}

uint32_t civ_redial_ms(uint32_t fails)
{
    if (fails > 5) return 30000;
    const uint32_t ms = 1000u << fails;
    return ms > 30000 ? 30000 : ms;
}
