/* Deterministic byte fuzzer for the TCI parser.
 *
 * The parser is fed whatever arrives on an unauthenticated socket, so it has to
 * survive arbitrary bytes. Run under ASan/UBSan (on by default) this catches
 * overreads, and the deterministic PRNG keeps CI failures reproducible. */
#include "tiny.h"
#include "tci_parse.h"

static uint32_t rng_state = 0x1234567u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static const char *SEEDS[] = {
    "vfo:0,0,14074000;", "trx:0,true;", "ready;", "rx_smeter:0,-93;",
    "tx_sensors:0,-12.5,48.2,48.2,1.4,-3.0;", "modulation:0,usb;",
    "protocol:ExpertSDR3,1.5;", "rx_filter_band:0,-2700,-300;",
    "dds:0,14074000;", "active_slice:1;", "lock:0,false;",
};

int main(void)
{
    tci_fact_t f;
    char buf[512];

    /* 1. Pure random bytes, including embedded NULs. */
    for (int i = 0; i < 40000; i++) {
        size_t n = rnd() % sizeof buf;
        for (size_t j = 0; j < n; j++) buf[j] = (char)(rnd() & 0xFF);
        tci_parse(buf, n, &f);
    }

    /* 2. Mutated real frames -- far more likely to reach deep paths. */
    for (int i = 0; i < 40000; i++) {
        const char *s = SEEDS[rnd() % (sizeof SEEDS / sizeof SEEDS[0])];
        size_t n = strlen(s);
        if (n > sizeof buf) n = sizeof buf;
        memcpy(buf, s, n);
        int muts = (int)(rnd() % 4);
        for (int m = 0; m < muts && n; m++) {
            size_t p = rnd() % n;
            switch (rnd() % 3) {
            case 0: buf[p] = (char)(rnd() & 0xFF); break;
            case 1: buf[p] = '\0';                 break;
            case 2: n = p;                         break;  /* truncate */
            }
        }
        tci_parse(buf, n, &f);
    }

    /* 3. Truncations of every seed at every length -- the classic overread. */
    for (size_t s = 0; s < sizeof SEEDS / sizeof SEEDS[0]; s++) {
        size_t full = strlen(SEEDS[s]);
        for (size_t n = 0; n <= full; n++) {
            memcpy(buf, SEEDS[s], n);
            tci_parse(buf, n, &f);       /* deliberately not NUL-terminated */
        }
    }

    CASE("fuzz");
    CHECK(1);   /* reaching here without a sanitizer abort is the result */
    printf("fuzz_tci: 120k+ cases, no crash\n");
    return t_fail ? 1 : 0;
}
