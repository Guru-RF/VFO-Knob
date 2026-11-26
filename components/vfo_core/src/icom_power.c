/* An Icom radio's power over its network: see icom_power.h. */
#include "icom_power.h"

void icom_wake_reset(icom_wake_t *w)
{
    w->sent   = false;
    w->t_sent = 0;
}

icom_wake_do_t icom_wake_step(icom_wake_t *w, uint32_t t, bool civ_open, uint32_t t_civ_open,
                              bool have_freq, icom_wake_may_t may)
{
    if (have_freq || !civ_open) return ICOM_WAKE_WAIT;
    if (!w->sent) {
        if (may != ICOM_WAKE_ASKED && t - t_civ_open < ICOM_WAKE_AFTER_MS) return ICOM_WAKE_WAIT;
        if (may == ICOM_WAKE_NEVER) return ICOM_WAKE_ASLEEP;
        w->sent   = true;
        w->t_sent = t;
        return ICOM_WAKE_SEND;
    }
    return t - w->t_sent >= ICOM_WAKE_BOOT_MS ? ICOM_WAKE_GIVE_UP : ICOM_WAKE_WAIT;
}

bool icom_wake_pending(const icom_wake_t *w, bool have_freq)
{
    return w->sent && !have_freq;
}

icom_off_t icom_off_setting(uint8_t v)
{
    return v == 0x00 ? ICOM_OFF_SHUTDOWN : v == 0x01 ? ICOM_OFF_STANDBY : ICOM_OFF_UNKNOWN;
}

bool icom_standby_allowed(icom_off_t setting, bool ready, bool on_air)
{
    return setting == ICOM_OFF_STANDBY && ready && !on_air;
}
