/* The tailchaser firmware's client (components/tailchaser_client) on the PC,
 * against stand-in TailChaser nodes (tools/mock_tailchaser.py), for
 * test_tailchaser_client.py.
 *
 * The client runs as on the knob, on its own task (a thread), its HTTP over
 * the PC's sockets, the tailnet shim_tc/tc_ts_host.c's. Its repeaters are set
 * through its own web endpoints (tc_web.c) on the shim's server, at
 * SHIM_HTTP_PORT, as the configuration page sets them.
 *
 * The driver talks to it a line at a time on stdin; each answer is a line
 * starting with '@', among the log's lines on stdout:
 *
 *   status        @STATUS {radio_get_status and the page's state}
 *   dial <n>      n detents
 *   lock <0|1>    the menu's RX: open, or block it (the face's lock)
 *   mute <0|1>    the menu's TX: open, or block it (the face's mute)
 *   amute <0|1>   the face's speaker: every repeater's audio on, or muted
 *   quit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "radio.h"
#include "shim.h"
#include "tailchaser_client.h"

size_t radio_web_endpoints(const httpd_uri_t **out);

static int s_haptics;
void haptic_hook(uint8_t effect, uint8_t prio)
{
    (void)effect; (void)prio;
    s_haptics++;
}

static void jstr(char *o, size_t cap, const char *s)
{
    size_t k = 0;
    o[k++] = '"';
    for (; *s && k + 8 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = (char)c; }
        else if (c < 0x20)         k += (size_t)snprintf(o + k, cap - k, "\\u%04x", c);
        else                       o[k++] = (char)c;
    }
    o[k++] = '"';
    o[k] = 0;
}

static void status(void)
{
    radio_status_t st;
    radio_get_status(&st);
    static char state[3072];
    tc_state_json(state, sizeof state);
    char name[80], talker[40], info[80], last[40], server[100], note[40], close_[120];
    jstr(name, sizeof name, st.tg_name);
    jstr(talker, sizeof talker, st.talker);
    jstr(info, sizeof info, st.talker_info);
    jstr(last, sizeof last, st.last_talker);
    jstr(server, sizeof server, st.server);
    char where[60];
    jstr(where, sizeof where, st.where);
    jstr(note, sizeof note, st.note);
    jstr(close_, sizeof close_, st.last_close);
    shim_say("@STATUS {\"link\":%d,\"ready\":%s,\"tg\":%lu,\"tg_name\":%s,\"talker\":%s,\"talker_info\":%s,"
             "\"last_talker\":%s,\"server\":%s,\"note\":%s,\"note_seq\":%lu,\"tx\":%s,\"on_air\":%s,"
             "\"rx_locked\":%s,\"tx_blocked\":%s,\"rx_db\":%.1f,\"connects\":%lu,\"closes\":%lu,"
             "\"last_close\":%s,\"reflector\":%s,\"rx_only\":%s,\"where\":%s,\"haptics\":%d,"
             "\"smeter_dbm\":%.1f,\"spk_muted\":%s,\"samples\":%llu,\"flushes\":%lu,\"talker_ms\":%lu,"
             "\"page\":%s}",
             (int)st.link, radio_is_ready() ? "true" : "false", (unsigned long)st.tg, name, talker, info, last,
             server, note, (unsigned long)st.note_seq, st.tx ? "true" : "false", radio_on_air() ? "true" : "false",
             st.tg_locked ? "true" : "false", st.muted ? "true" : "false", st.rx_level_db,
             (unsigned long)st.connects, (unsigned long)st.closes, close_, st.reflector ? "true" : "false",
             st.rx_only ? "true" : "false", where, s_haptics, st.smeter_dbm, st.spk_muted ? "true" : "false",
             (unsigned long long)shim_samples, (unsigned long)shim_flushes, (unsigned long)st.talker_ms, state);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    httpd_handle_t h;
    const httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&h, &cfg) != ESP_OK) return 2;
    const httpd_uri_t *u;
    const size_t n = radio_web_endpoints(&u);
    for (size_t i = 0; i < n; i++) httpd_register_uri_handler(h, &u[i]);
    if (radio_start(NULL, 0, NULL, NULL) != ESP_OK) return 2;
    shim_say("@READY");

    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        char cmd[32] = "";
        long arg = 0;
        sscanf(line, "%31s %ld", cmd, &arg);
        if (!strcmp(cmd, "status")) {
            status();
            continue;
        }
        if (!strcmp(cmd, "quit")) break;
        if (!strcmp(cmd, "dial"))      radio_tune_by((int32_t)arg, 1, 0);
        else if (!strcmp(cmd, "lock")) radio_tg_lock(arg != 0);
        else if (!strcmp(cmd, "mute")) radio_mute(arg != 0);
        else if (!strcmp(cmd, "amute")) tc_set_audio_mute(arg != 0);
        else {
            shim_say("@ERR %s", line);
            continue;
        }
        shim_say("@OK %s", cmd);
    }
    shim_say("@BYE");
    return 0;
}
