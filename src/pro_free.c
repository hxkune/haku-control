// SPDX-License-Identifier: GPL-3.0-only
// The open build has no haku Pro: what it has runs without a key (the paid build puts pro/license.c here instead).
#include "common.h"

void pro_start(void) {}
int  pro_active(void) { return 1; }
void pro_set_key(const char *key) { (void)key; }
void pro_buy(void) {}
int  pro_json(char *out, int cap) { return snprintf(out, cap, "null"); }
