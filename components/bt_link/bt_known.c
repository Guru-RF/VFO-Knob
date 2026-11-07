/* The Bluetooth devices this knob has connected. See bt_known.h. */
#include "bt_known.h"

#include <string.h>

static int find(const bt_known_t *t, const uint8_t *bda)
{
    for (int i = 0; i < t->n; i++)
        if (!memcmp(t->rec[i].bda, bda, 6)) return i;
    return -1;
}

static bool no_address(const uint8_t *bda)
{
    static const uint8_t none[6];
    return !memcmp(bda, none, 6);
}

void bt_known_load(bt_known_t *t, const void *blob, size_t len)
{
    memset(t, 0, sizeof *t);
    const uint8_t *p = blob;
    for (size_t o = 0; p && o + sizeof(bt_known_rec_t) <= len && t->n < BT_KNOWN_DEVICES; o += sizeof(bt_known_rec_t)) {
        bt_known_rec_t r;
        memcpy(&r, p + o, sizeof r);
        r.name[sizeof r.name - 1] = 0;
        if (!no_address(r.bda) && find(t, r.bda) < 0) t->rec[t->n++] = r;
    }
}

bool bt_known_seen(bt_known_t *t, const uint8_t bda[6], const char *name, uint8_t kind)
{
    if (no_address(bda)) return false;
    int i = find(t, bda);
    bt_known_rec_t r;
    memset(&r, 0, sizeof r);
    memcpy(r.bda, bda, 6);
    r.kind = kind;
    const char *from = name && name[0] ? name : i >= 0 ? t->rec[i].name : "";
    const char *end  = memchr(from, 0, sizeof r.name - 1);
    memcpy(r.name, from, end ? (size_t)(end - from) : sizeof r.name - 1);
    if (i >= 0) r.spare = t->rec[i].spare;
    if (i == 0 && !memcmp(&t->rec[0], &r, sizeof r)) return false;
    if (i < 0) i = t->n < BT_KNOWN_DEVICES ? t->n++ : BT_KNOWN_DEVICES - 1;     /* all taken: the oldest goes */
    memmove(&t->rec[1], &t->rec[0], (size_t)i * sizeof t->rec[0]);
    t->rec[0] = r;
    return true;
}

const char *bt_known_name(const bt_known_t *t, const uint8_t bda[6])
{
    const int i = find(t, bda);
    return i >= 0 && t->rec[i].name[0] ? t->rec[i].name : NULL;
}

bool bt_known_forget(bt_known_t *t, const uint8_t bda[6])
{
    const int i = find(t, bda);
    if (i < 0) return false;
    memmove(&t->rec[i], &t->rec[i + 1], (size_t)(t->n - i - 1) * sizeof t->rec[0]);
    t->n--;
    memset(&t->rec[t->n], 0, sizeof t->rec[0]);
    return true;
}
