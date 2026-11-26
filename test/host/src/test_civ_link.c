/* CI-V as a byte stream (components/vfo_core/src/civ_link.c): the icombt
 * firmware's frames off the IC-705's Bluetooth serial port, whole however
 * the bytes come; the frames it sends; the radio's address as the page and
 * the scan give it; which scanned devices are Icom radios; the redial's
 * waits. */
#include "tiny.h"
#include "civ_link.h"

#include <string.h>

/* Feeds n bytes; the frames found, their lengths in lens (up to 8). */
static int feed(civ_rx_t *r, const uint8_t *p, size_t n, size_t *lens, uint8_t frames[][CIV_FRAME_MAX])
{
    int k = 0;
    for (size_t i = 0; i < n; i++) {
        const size_t f = civ_rx_byte(r, p[i]);
        if (f && k < 8) {
            lens[k] = f;
            memcpy(frames[k], r->buf, f);
            k++;
        }
    }
    return k;
}

int main(void)
{
    civ_rx_t r;
    size_t   lens[8];
    uint8_t  fr[8][CIV_FRAME_MAX];

    CASE("a frequency report, whole");
    {
        const uint8_t in[] = { 0xFE, 0xFE, 0xE0, 0xA4, 0x03, 0x00, 0x40, 0x07, 0x14, 0x00, 0xFD };
        civ_rx_init(&r);
        CHECK_EQ(feed(&r, in, sizeof in, lens, fr), 1);
        CHECK_EQ(lens[0], sizeof in);
        CHECK(!memcmp(fr[0], in, sizeof in));
        CHECK_EQ(r.frames, 1u);
    }

    CASE("two frames in one read, then one split across reads");
    {
        const uint8_t a[] = { 0xFE, 0xFE, 0xE0, 0xA4, 0xFB, 0xFD, 0xFE, 0xFE, 0xE0, 0xA4, 0x15, 0x02, 0x01, 0x20, 0xFD,
                              0xFE, 0xFE, 0xE0 };
        const uint8_t b[] = { 0xA4, 0x1C, 0x00, 0x00, 0xFD };
        civ_rx_init(&r);
        CHECK_EQ(feed(&r, a, sizeof a, lens, fr), 2);
        CHECK_EQ(lens[0], 6u);
        CHECK_EQ(fr[0][4], 0xFB);
        CHECK_EQ(lens[1], 9u);
        CHECK_EQ(fr[1][4], 0x15);
        CHECK_EQ(feed(&r, b, sizeof b, lens, fr), 1);
        CHECK_EQ(lens[0], 8u);
        CHECK_EQ(fr[0][4], 0x1C);
    }

    CASE("junk before the preamble, a lone FE, a long preamble");
    {
        const uint8_t in[] = { 0x00, 0x13, 0xFD, 0xFE, 0x42, 0xFE, 0xFE, 0xFE, 0xE0, 0xA4, 0x04, 0x01, 0x02, 0xFD };
        civ_rx_init(&r);
        CHECK_EQ(feed(&r, in, sizeof in, lens, fr), 1);
        CHECK_EQ(lens[0], 8u);                       /* the third FE not kept */
        CHECK_EQ(fr[0][2], 0xE0);
        CHECK_EQ(fr[0][4], 0x04);
        CHECK(r.junk >= 4u);
    }

    CASE("a collision voids the frame; the next is whole");
    {
        const uint8_t in[] = { 0xFE, 0xFE, 0xE0, 0xA4, 0x03, 0xFC, 0xFE, 0xFE, 0xE0, 0xA4, 0xFB, 0xFD };
        civ_rx_init(&r);
        CHECK_EQ(feed(&r, in, sizeof in, lens, fr), 1);
        CHECK_EQ(fr[0][4], 0xFB);
        CHECK_EQ(r.dropped, 1u);
    }

    CASE("too short to be a frame: dropped");
    {
        const uint8_t in[] = { 0xFE, 0xFE, 0xE0, 0xFD };
        civ_rx_init(&r);
        CHECK_EQ(feed(&r, in, sizeof in, lens, fr), 0);
        CHECK_EQ(r.dropped, 1u);
    }

    CASE("a runaway frame: dropped at the limit, the stream recovers");
    {
        uint8_t in[CIV_FRAME_MAX + 20];
        in[0] = in[1] = 0xFE;
        for (size_t i = 2; i < sizeof in; i++) in[i] = 0x11;
        civ_rx_init(&r);
        CHECK_EQ(feed(&r, in, sizeof in, lens, fr), 0);
        CHECK(r.dropped >= 1u);
        const uint8_t ok[] = { 0xFE, 0xFE, 0xE0, 0xA4, 0xFB, 0xFD };
        CHECK_EQ(feed(&r, ok, sizeof ok, lens, fr), 1);
        CHECK_EQ(lens[0], 6u);
        /* ...and a frame exactly at the limit is kept. */
        uint8_t big[CIV_FRAME_MAX];
        big[0] = big[1] = 0xFE;
        big[2] = 0xE0;
        big[3] = 0xA4;
        for (size_t i = 4; i < sizeof big - 1; i++) big[i] = 0x22;
        big[sizeof big - 1] = 0xFD;
        civ_rx_init(&r);
        CHECK_EQ(feed(&r, big, sizeof big, lens, fr), 1);
        CHECK_EQ(lens[0], (size_t)CIV_FRAME_MAX);
    }

    CASE("frames built");
    {
        uint8_t       out[16];
        const uint8_t rd[] = { 0x03 };
        CHECK_EQ(civ_frame_build(out, sizeof out, 0xA4, 0xE0, rd, 1), 6u);
        const uint8_t want[] = { 0xFE, 0xFE, 0xA4, 0xE0, 0x03, 0xFD };
        CHECK(!memcmp(out, want, 6));
        const uint8_t set[] = { 0x05, 0x00, 0x40, 0x07, 0x14, 0x00 };
        CHECK_EQ(civ_frame_build(out, sizeof out, 0xA4, 0xE0, set, sizeof set), 11u);
        CHECK_EQ(out[10], 0xFD);
        /* No room, nothing to say, or an FD inside: none. */
        CHECK_EQ(civ_frame_build(out, 6, 0xA4, 0xE0, set, sizeof set), 0u);
        CHECK_EQ(civ_frame_build(out, sizeof out, 0xA4, 0xE0, set, 0), 0u);
        const uint8_t bad[] = { 0x14, 0xFD };
        CHECK_EQ(civ_frame_build(out, sizeof out, 0xA4, 0xE0, bad, 2), 0u);
        /* What is built reads back as one frame. */
        civ_rx_init(&r);
        const size_t n = civ_frame_build(out, sizeof out, 0xE0, 0xA4, set, sizeof set);
        CHECK_EQ(feed(&r, out, n, lens, fr), 1);
        CHECK_EQ(lens[0], n);
    }

    CASE("Bluetooth addresses");
    {
        uint8_t a[6] = { 0 };
        CHECK(civ_bda_parse("a4:c1:38:0b:5e:ff", a));
        const uint8_t want[] = { 0xA4, 0xC1, 0x38, 0x0B, 0x5E, 0xFF };
        CHECK(!memcmp(a, want, 6));
        CHECK(civ_bda_parse("A4-C1-38-0B-5E-FF", a));
        CHECK(!memcmp(a, want, 6));
        uint8_t keep[6] = { 1, 2, 3, 4, 5, 6 };
        CHECK(!civ_bda_parse("IC-705.local", keep));
        CHECK(!civ_bda_parse("192.168.1.10", keep));
        CHECK(!civ_bda_parse("a4:c1:38:0b:5e", keep));
        CHECK(!civ_bda_parse("a4:c1:38:0b:5e:fg", keep));
        CHECK(!civ_bda_parse("a4:c1:38:0b:5e:ff:", keep));
        CHECK(!civ_bda_parse("a4c1:380b:5eff:00", keep));
        CHECK(!civ_bda_parse("", keep));
        CHECK(!civ_bda_parse(NULL, keep));
        const uint8_t same[] = { 1, 2, 3, 4, 5, 6 };
        CHECK(!memcmp(keep, same, 6));
    }

    CASE("Icom radios among the devices found");
    {
        CHECK(civ_is_icom_bt_name("ICOM BT(IC-705)"));
        CHECK(civ_is_icom_bt_name("ICOM BT(IC-705)A"));
        CHECK(!civ_is_icom_bt_name("icom bt(ic-705)"));
        CHECK(!civ_is_icom_bt_name("X6200"));
        CHECK(!civ_is_icom_bt_name("ICOM"));
        CHECK(!civ_is_icom_bt_name(""));
        CHECK(!civ_is_icom_bt_name(NULL));
    }

    CASE("the redial's waits");
    {
        CHECK_EQ(civ_redial_ms(0), 1000u);
        CHECK_EQ(civ_redial_ms(1), 2000u);
        CHECK_EQ(civ_redial_ms(4), 16000u);
        CHECK_EQ(civ_redial_ms(5), 30000u);
        CHECK_EQ(civ_redial_ms(6), 30000u);
        CHECK_EQ(civ_redial_ms(1000), 30000u);
    }

    CASE("levels: the knob's to CI-V's and back");
    {
        CHECK_EQ(civ_level_from(0, 100), 0u);
        CHECK_EQ(civ_level_from(100, 100), 255u);
        CHECK_EQ(civ_level_from(250, 100), 255u);
        CHECK_EQ(civ_level_from(100, 200), 128u);       /* unity: the radio's 50 % */
        CHECK_EQ(civ_level_from(200, 200), 255u);
        CHECK_EQ(civ_level_from(5, 0), 0u);
        CHECK_EQ(civ_level_to(255, 100), 100u);
        CHECK_EQ(civ_level_to(0, 200), 0u);
        CHECK_EQ(civ_level_to(128, 200), 100u);
        int bad = 0;
        for (unsigned v = 0; v <= 100; v++) bad += civ_level_to(civ_level_from(v, 100), 100) != v;
        for (unsigned v = 0; v <= 200; v++) {
            /* 201 steps onto 256: each reads back within one */
            const unsigned b = civ_level_to(civ_level_from(v, 200), 200);
            bad += b + 1 < v || b > v + 1;
        }
        CHECK_EQ(bad, 0);
    }

    CASE("noise: whatever comes, a frame is FE FE ... FD and fits");
    {
        /* The radio's bytes are the air's: a million random ones, FE and FD
         * made common, under the host build's sanitizers. */
        uint32_t x = 0x12345678u;
        uint32_t frames = 0;
        civ_rx_init(&r);
        for (int i = 0; i < 1000000; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            const uint8_t b = (x & 7) == 0 ? 0xFE : (x & 15) == 1 ? 0xFD : (uint8_t)(x >> 8);
            const size_t  f = civ_rx_byte(&r, b);
            if (!f) continue;
            frames++;
            if (f < 6 || f > CIV_FRAME_MAX || r.buf[0] != 0xFE || r.buf[1] != 0xFE || r.buf[f - 1] != 0xFD) {
                CHECK(0);
                break;
            }
        }
        CHECK(frames > 0u);
        CHECK_EQ(r.frames, frames);
    }

    CASE("the SPP UUID, as the second chip takes it");
    {
        const uint8_t u[16] = CIV_SPP_UUID;
        CHECK_EQ(u[2], 0x11);
        CHECK_EQ(u[3], 0x01);
        CHECK_EQ(u[15], 0xFB);
    }

    printf("%s: %d checks, %d failed\n", __FILE__, t_run, t_fail);
    return t_fail ? 1 : 0;
}
