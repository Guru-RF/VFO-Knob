#include "beam.h"

#include <math.h>

static const char *const NAMES[BEAM_MAX] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };

static uint8_t dirs(uint8_t n) { return n == 8 ? 8 : 4; }

bool beam_dirs_ok(long n) { return n == 4 || n == 8; }

const char *beam_name(uint8_t n_dirs, uint8_t i)
{
    const uint8_t n = dirs(n_dirs);
    return i < n ? NAMES[i * (BEAM_MAX / n)] : "";
}

uint16_t beam_deg(uint8_t n_dirs, uint8_t i)
{
    const uint8_t n = dirs(n_dirs);
    return (uint16_t)(i % n * (360 / n));
}

uint8_t beam_step(uint8_t n_dirs, uint8_t i, int32_t detents)
{
    const int32_t n = dirs(n_dirs);
    int32_t r = (i % n + detents % n) % n;
    if (r < 0) r += n;
    return (uint8_t)r;
}

uint8_t beam_at(uint8_t n_dirs, int32_t dx, int32_t dy)
{
    const uint8_t n = dirs(n_dirs);
    /* Clockwise from north: east over north, the glass's y running down. */
    float deg = atan2f((float)dx, (float)-dy) * (180.0f / 3.14159265f);
    if (deg < 0) deg += 360.0f;
    const float span = 360.0f / n;
    return (uint8_t)((int)((deg + span / 2) / span) % n);
}

uint8_t beam_convert(uint8_t from_n, uint8_t i, uint8_t to_n)
{
    return (uint8_t)(beam_deg(from_n, i) * dirs(to_n) / 360);
}
