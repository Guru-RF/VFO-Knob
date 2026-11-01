/* EpiBeam's directions (components/vfo_core/src/beam.c): the arrays there
 * are; each direction's name and heading, on 4 and on 8; the dial's steps
 * round and round, both ways, a detent a direction; the direction a touch on
 * the ring lies in, each owning the half-step either side of it; and a beam
 * carried from one array to the other, on its heading or the one before. */
#include "tiny.h"
#include "beam.h"

#include <math.h>

static void test_arrays(void)
{
    CASE("the arrays: 4 directions or 8, nothing else");
    CHECK(beam_dirs_ok(4) && beam_dirs_ok(8));
    CHECK(!beam_dirs_ok(0) && !beam_dirs_ok(2) && !beam_dirs_ok(6) && !beam_dirs_ok(16) && !beam_dirs_ok(-4));
}

static void test_names(void)
{
    CASE("4: N, E, S, W at 0, 90, 180, 270 degrees");
    static const char *const N4[] = { "N", "E", "S", "W" };
    for (uint8_t i = 0; i < 4; i++) {
        CHECK(!strcmp(beam_name(4, i), N4[i]));
        CHECK_EQ(beam_deg(4, i), 90 * i);
    }
    CHECK(!strcmp(beam_name(4, 4), ""));

    CASE("8: the corners between, every 45 degrees");
    static const char *const N8[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    for (uint8_t i = 0; i < 8; i++) {
        CHECK(!strcmp(beam_name(8, i), N8[i]));
        CHECK_EQ(beam_deg(8, i), 45 * i);
    }
    CHECK(!strcmp(beam_name(8, 8), ""));

    CASE("anything but 8 directions is taken as 4");
    CHECK(!strcmp(beam_name(0, 1), "E") && !strcmp(beam_name(6, 3), "W"));
}

static void test_steps(void)
{
    CASE("a detent a direction, clockwise for clockwise");
    CHECK_EQ(beam_step(4, 0, 1), 1);
    CHECK_EQ(beam_step(8, 0, 1), 1);
    CHECK_EQ(beam_step(8, 2, 3), 5);

    CASE("round and round: past W to N, back past N to W, and many turns at once");
    CHECK_EQ(beam_step(4, 3, 1), 0);
    CHECK_EQ(beam_step(4, 0, -1), 3);
    CHECK_EQ(beam_step(8, 7, 1), 0);
    CHECK_EQ(beam_step(8, 0, -1), 7);
    CHECK_EQ(beam_step(4, 1, 4), 1);
    CHECK_EQ(beam_step(4, 1, -9), 0);
    CHECK_EQ(beam_step(8, 5, 8 * 1000 + 3), 0);
    CHECK_EQ(beam_step(8, 5, -8 * 1000 - 6), 7);

    CASE("every start, every step, always on the array");
    int off = 0;
    for (uint8_t n = 4; n <= 8; n += 4)
        for (uint8_t i = 0; i < n; i++)
            for (int32_t d = -40; d <= 40; d++) {
                const uint8_t r = beam_step(n, i, d);
                off += r >= n || r != (uint8_t)(((i + d) % n + n) % n);
            }
    CHECK_EQ(off, 0);
}

/* A point `r` px out at `deg` clockwise from north, on the glass (y down). */
static uint8_t at(uint8_t n, double deg, double r)
{
    const double a = deg * M_PI / 180.0;
    return beam_at(n, (int32_t)lround(r * sin(a)), (int32_t)lround(-r * cos(a)));
}

static void test_touch(void)
{
    CASE("a touch on the ring: the direction it lies in");
    CHECK_EQ(at(4, 0, 155), 0);
    CHECK_EQ(at(4, 90, 155), 1);
    CHECK_EQ(at(4, 180, 155), 2);
    CHECK_EQ(at(4, 270, 155), 3);
    CHECK_EQ(at(8, 45, 155), 1);
    CHECK_EQ(at(8, 315, 155), 7);

    CASE("...each owning the half-step either side: N from 315 to 45 on 4");
    CHECK_EQ(at(4, 316, 155), 0);
    CHECK_EQ(at(4, 44, 155), 0);
    CHECK_EQ(at(4, 46, 155), 1);
    CHECK_EQ(at(4, 314, 155), 3);
    CHECK_EQ(at(8, 22, 155), 0);
    CHECK_EQ(at(8, 23, 155), 1);
    CHECK_EQ(at(8, 338, 155), 0);
    CHECK_EQ(at(8, 337, 155), 7);

    CASE("every heading lands on the nearest direction, on both arrays");
    int wrong = 0;
    for (uint8_t n = 4; n <= 8; n += 4)
        for (int d = 0; d < 360; d++) {
            const double span = 360.0 / n, off = fmod(d + span / 2, span) - span / 2;
            if (fabs(fabs(off) - span / 2) < 1.5) continue;   /* on a boundary: either */
            wrong += at(n, d, 150) != (uint8_t)((int)lround(d / span) % n);
        }
    CHECK_EQ(wrong, 0);
}

static void test_convert(void)
{
    CASE("4 to 8: each on its own heading");
    for (uint8_t i = 0; i < 4; i++) CHECK_EQ(beam_convert(4, i, 8), 2 * i);

    CASE("8 to 4: N, E, S, W kept; a corner to the one before it");
    for (uint8_t i = 0; i < 8; i += 2) CHECK_EQ(beam_convert(8, i, 4), i / 2);
    CHECK_EQ(beam_convert(8, 1, 4), 0);                 /* NE: N */
    CHECK_EQ(beam_convert(8, 3, 4), 1);                 /* SE: E */
    CHECK_EQ(beam_convert(8, 7, 4), 3);                 /* NW: W */

    CASE("to the same array: unchanged");
    for (uint8_t i = 0; i < 8; i++) CHECK_EQ(beam_convert(8, i, 8), i);
}

int main(void)
{
    test_arrays();
    test_names();
    test_steps();
    test_touch();
    test_convert();
    printf("%d checks, %d failed\n", t_run, t_fail);
    return t_fail ? 1 : 0;
}
