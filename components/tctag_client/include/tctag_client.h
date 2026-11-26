/* The TailChaser Tag firmware's own, beside its radio.h (tctag_client.c): who
 * was heard and how the listening goes, for the face and the page. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "tag_view.h"

/* A copy of it all. From any task. */
void tctag_view(tag_view_t *out);

/* The list emptied, here and on the card (the page's). */
void tctag_clear(void);
