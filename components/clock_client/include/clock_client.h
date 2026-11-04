/* The Clock firmware's own, beside its radio.h (clock_client.c): its alarms
 * on the configuration page (clock_web.c), and their beeps -- on a Bluetooth
 * headset or speaker while one is connected, on the jack while none is. */
#pragma once

#include <stdbool.h>

/* The alarms saved on the page since last asked: for app_main to hand the
 * face. Once. */
bool clock_client_take_alarms(void);

/* From app_main's loop, every pass: while `ringing`, the alarm's beeps
 * topped up ahead of the playback, the Bluetooth device's audio opened for
 * them and the jack silent once it is; once it stops, what is left of them
 * dropped, the device's audio closed and the jack back. Their loudness is
 * the knob's VOLUME, which app_main keeps at the page's (ui_set_levels). */
void clock_client_sound(bool ringing);
