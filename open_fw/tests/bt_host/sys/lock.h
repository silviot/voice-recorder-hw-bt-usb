/* Host harness stand-in for newlib/picolibc <sys/lock.h>: btc_spp.c, btc_l2cap.c and btc_opp_vfs.c include it but
 * use none of its declarations (they lock with osi_mutex). Firmware builds get the real picolibc header.
 * SPDX-License-Identifier: MIT */
#pragma once
