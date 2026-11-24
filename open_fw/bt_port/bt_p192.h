/* NIST P-192 key generation for the RW-BT mask ROM's SSP key slot (plan docs/BLUETOOTH_BLUEDROID_PLAN.md M2.1,
 * docs/BT_ROM_INTERFACE.md §2.3).
 *
 * Why: the ROM's rom_lm_sec_p192_init (0x31DE4) copies a FIXED P-192 key pair from ROM (private key at 0xD7EC, public
 * key at 0xD84C) into RAM (0x00803798 / 0x0080334C) and never generates another one; every SSP pairing of every SL6801
 * would use the same, publicly readable private key. bt_rom_real installs a fresh pair made here instead.
 *
 * Byte order is the ROM's: scalars and coordinates are 24-byte LITTLE-endian integers; a public key is X || Y (48 B).
 * Pure C, no allocation, no globals; the scalar multiplication is a fixed 192-step double-and-add-always ladder with
 * constant-time selects (timing does not depend on the scalar's bits). Verified against the ROM's own pair, the
 * Bluetooth P-192 debug pair and independently computed vectors (tests/test_bt_p192.c).
 * SPDX-License-Identifier: MIT */
#ifndef BT_P192_H
#define BT_P192_H

#include <stddef.h>
#include <stdint.h>

#define BT_P192_BYTES 24u

/* pub = d * G. d must be in [1, n-1]. Returns 0, or -1 for an out-of-range scalar / an internal self-check failure
 * (result not on the curve). pub is zeroed on failure. */
int bt_p192_pubkey(const uint8_t d_le[24], uint8_t pub_le[48]);

/* 1 if (X, Y) is a point of P-192 (coordinates < p and y^2 = x^3 - 3x + b), else 0. */
int bt_p192_on_curve(const uint8_t pub_le[48]);

/* FIPS 186-4 B.4.2 (testing candidates): d = candidate from rng() if 1 <= candidate <= n-1, else retry (at most 8
 * draws; a rejection happens with probability ~2^-64 per draw). rng returns 0 on success. Returns 0 with d and pub
 * filled, -1 when rng failed (fail closed: no fallback) or no candidate was accepted; d/pub zeroed on failure. */
int bt_p192_keygen(int (*rng)(void *ctx, void *out, size_t n), void *ctx, uint8_t d_le[24], uint8_t pub_le[48]);

#endif /* BT_P192_H */
