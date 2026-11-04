/* The Weather firmware's client (VFO_RADIO=weather): no radio, but
 * Open-Meteo's forecast for the page's place, asked every few minutes (the
 * page's Refresh). Its own task, one request at a time; app_main hands what
 * comes to the face (ui.c WEATHER_FACE). radio.h answers as the setup
 * firmware's does, but for the link: READY while there is a forecast. */
#ifndef WEATHER_CLIENT_H
#define WEATHER_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "weather.h"

/* The newest forecast, if its gen is not *gen: into out, *gen moved on;
 * false when there is nothing new. */
bool weather_client_take(wx_t *out, uint32_t *gen);

/* Ask now, not at the next refresh: the place moved. */
void weather_client_poke(void);

/* The forecast's state in a few words, for the face's foot: "" while it is
 * fresh, else what went wrong ("no answer", "HTTP 429", "no WiFi"). */
void weather_client_state(char *out, size_t n);

/* Seconds since the last forecast came, for the page; -1 before the first. */
int32_t weather_client_age_s(void);

/* The page saved the settings (weather_web.c): Open-Meteo asked anew, and
 * once, true from take_settings -- for app_main to hand them to the face. */
void weather_client_settings_saved(void);
bool weather_client_take_settings(void);

#endif /* WEATHER_CLIENT_H */
