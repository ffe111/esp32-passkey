/* Capacitive touch user-presence input. SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include <stdbool.h>

void touch_presence_init(void);
/* Debounced: true while a finger is on the touch pad. */
bool touch_presence_read(void);
