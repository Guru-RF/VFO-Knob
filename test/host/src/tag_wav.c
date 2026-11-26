/* Reads TailChaser's callsign tag in WAV files, as the knob would hear them:
 *
 *   tag_wav FILE...       one line each: the first reading, how soon, how sure
 *   tag_wav -v FILE...    and every reading, and the levels each second
 *   tag_wav -t S FILE...  and listen S seconds past each file's end (a quiet
 *                         room at -70 dBFS), as the knob listens on
 *
 * Any rate and channel count of 16-bit PCM; the first channel, resampled to
 * the decoder's 16 kHz. Exit 0 when every file gave a reading. */
#include "tag.h"
#include "wav_load.h"

#include <stdbool.h>

int main(int argc, char **argv)
{
    bool verbose = false;
    double tail = 0;
    int files = 0, read = 0;
    tag_dec_t *d = malloc(tag_dec_size());
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-v")) { verbose = true; continue; }
        if (!strcmp(argv[a], "-t") && a + 1 < argc) { tail = atof(argv[++a]); continue; }
        size_t n = 0;
        float *x = load_wav(argv[a], &n);
        if (x && tail > 0) {
            const size_t m = n + (size_t)(tail * TAG_FS);
            float *y = realloc(x, m * sizeof *y);
            if (y) {
                uint32_t r = 12345;
                for (size_t i = n; i < m; i++) {
                    r = r * 1664525u + 1013904223u;
                    y[i] = (float)((int32_t)r >> 8) / 8388608.0f * 0.0005f;
                }
                x = y;
                n = m;
            }
        }
        files++;
        if (!x) { printf("%s: not a 16-bit PCM WAV\n", argv[a]); continue; }
        tag_dec_init(d);
        tag_reading_t first = { 0 }, rd;
        int got = 0;
        for (size_t at = 0; at < n; at += 320) {
            tag_dec_feed_f(d, x + at, n - at < 320 ? n - at : 320);
            while (tag_dec_take(d, &rd)) {
                if (!got++) first = rd;
                if (verbose)
                    printf("  %6.2f s  %-9s %-7s sync %4.1f dB %.3f, %3u corrected, %u frame(s)\n", (double)rd.at / TAG_FS,
                           rd.call, rd.local ? "local" : "network", rd.sync_db, rd.sync_avg, rd.corrected, rd.frames);
            }
            if (verbose && (at / 320) % 50 == 49) {
                tag_stats_t st;
                tag_dec_stats(d, &st);
                printf("  %6.2f s  band %5.1f dBFS (floor %5.1f), all %5.1f dBFS, sync %4.1f dB, now %.3f, avg %.3f\n",
                       (double)(at + 320) / TAG_FS, st.band_dbfs, st.floor_dbfs, st.all_dbfs, st.sync_db, st.sync_now, st.sync_avg);
            }
        }
        tag_stats_t st;
        tag_dec_stats(d, &st);
        if (got) {
            read++;
            printf("%s: %s %s at %.2f s (sync %.1f dB %.3f, %u corrected, %u frame(s), clock %+d, start %+d); %d readings, %u tries, %u CRC bad\n",
                   argv[a], first.call, first.local ? "local" : "network", (double)first.at / TAG_FS,
                   first.sync_db, first.sync_avg, first.corrected, first.frames, first.drift, first.nudge, got, st.tries, st.crc_bad);
        } else {
            printf("%s: nothing; %u tries, %u CRC bad, sync at the end %.1f dB\n", argv[a], st.tries, st.crc_bad, st.sync_db);
        }
        free(x);
    }
    free(d);
    return files && read == files ? 0 : 1;
}
