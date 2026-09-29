/* Talkgroup names, from the reflector's portal.
 *
 * An enhanced reflector publishes https://portal.<reflector>/talkgroups.json,
 * {"9990": "Parrot, test your audio here", ...} -- be.svx.link's portal
 * serves portal.be.svx.link. It is decoration: a plain reflector has none,
 * and every talkgroup without a name is shown by its number. The names are
 * kept in NVS, so a knob shows yesterday's at once and the portal is asked
 * once a day.
 *
 * The reflector task's alone: no locking.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* What NVS holds for this reflector, if anything. */
void svx_portal_load_cache(const char *reflector);

/* Time to ask the portal: never asked, or a day since the names came. */
bool svx_portal_due(void);

/* Ask it; blocks for a few seconds at most. True if the names changed. */
bool svx_portal_fetch(const char *reflector);

/* "Parrot, test your audio here", or NULL. */
const char *svx_portal_name(uint32_t tg);

/* The named talkgroups, in numeric order: the dial's list on a knob that
 * has not been given one. */
int svx_portal_ids(uint32_t *ids, int max);
