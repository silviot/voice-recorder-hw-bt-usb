/* Link-only traps for the host harness. SMP BR/EDR pairing completion (smp_utils.c smp_proc_pairing_cmpl) and two
 * smp_api.c entry points reference BLE-only functions that smp_keys.c compiles out when BLE_INCLUDED is FALSE. In
 * the firmware (clang -Os, lld --gc-sections) the callers are dead: smp_sm_action[] is all NULL without BLE, so the
 * references vanish. The host build (-O1, GNU ld) keeps them; these traps let it link and abort loudly if one is
 * ever reached. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>

static void unreachable(const char *f)
{
    fprintf(stderr, "bt_host: %s reached (BLE-only SMP code, not built in the Classic-only configuration)\n", f);
    abort();
}

void smp_clear_local_oob_data(void) { unreachable(__func__); }
void *smp_get_local_oob_data(void) { unreachable(__func__); return NULL; }
unsigned char smp_encrypt_data(unsigned char *key, unsigned char key_len, unsigned char *pt, unsigned char pt_len,
                               void *out)
{
    (void)key; (void)key_len; (void)pt; (void)pt_len; (void)out;
    unreachable(__func__);
    return 0;
}
