#pragma once
#include "lvgl.h"

/* Show a transient overlay near the top of the active screen.
 * If a toast is already visible it is replaced. Auto-dismisses. */
void toast_show(const char *text);
