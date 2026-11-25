/* bt_rom_rng: what the RW-BT mask ROM's RNG hook (RAM slot 0x00803C24, thunk ROM 0x3DE98) returns, decided per call
 * site from the caller's return address (docs/BT_ROM_INTERFACE.md §2.3, plan M2.1 "covered hooks get DRBG output").
 *
 * The ROM reaches the hook only through `bl 0x3DE98` (thunk: ldr r3,=0x00803C1C; ldr r3,[r3,#8]; bx r3), so LR at
 * hook entry is the ROM call site + 4. Static disassembly of all 11 call sites (no other reference to the slot or to
 * the thunk exists in the 384 KiB ROM):
 *
 *   return addr  ROM caller   what the ROM does with r0                       here
 *   0x000312FE   0x312F4      lo + r0 % (hi - lo), << 7 (timer, 0x424D4)      LEGACY
 *   0x00031400   0x313F4      strb: one byte of a 16-byte LMP random number    DRBG byte
 *                             (11 LMP callers: AU_RAND / EN_RAND / IN_RAND...)
 *   0x00031D76   0x31D6C      strb into the SSP nonce hash block (1st of 4)    DRBG byte + fill the 28-byte block
 *   0x00031D80/88/90  0x31D6C strb into the SSP nonce hash block (2nd..4th)    DRBG byte
 *   0x00031EE8   0x31EE2      r0 % 1000000: SSP passkey (display)              DRBG word
 *   0x00035780   0x3577C      (r0 + k) & 0x3FF -> slot/backoff field           LEGACY
 *   0x00039C30   0x39C2C      r0 & 0x7F, folded to 0..78: AFH channel pick     LEGACY
 *   0x00042F78   0x42F74      r0 & 0xF, (r0 & 0xFFFFFF) >> 16 -> PDU fields    LEGACY
 *   0x00044152   0x4414E      ((r0 & 0xF) + 1) & 0x1E: even slot offset        LEGACY
 *
 * Every caller masks, reduces or byte-stores r0, so no ROM path depends on the legacy 0..79 range for memory safety.
 * LEGACY (and any unknown return address) keeps the legacy xorshift % 80 exactly, so radio timing behaves as tested
 * on silicon. Security sites take bytes from a pool the app task fills from bt_entropy (DRBG); the hook runs in ROM
 * context (bt_ctrl task, possibly under the ROM's PRIMASK lock), so it never blocks and never calls bt_entropy.
 *
 * SSP nonce (0x31D6C): the ROM hashes a 28-byte block at 0x0080340C with SHA-256 and keeps 16 bytes as the per-link
 * SSP nonce (linkenv + 0x17C; also the OOB "R" at 0x008037F8). It writes only BD_ADDR (+0..5), one CLKN byte (+8) and
 * the four hook bytes (+0x0C/0x10/0x14/0x18); the other 17 bytes are residue of the previous hash. At the FIRST of
 * the four hook calls (return address 0x31D76) the whole block is overwritten with pool bytes before the ROM writes
 * its own fields, so the hashed input carries 17 x 8 + 4 x 8 = 168 DRBG bits: the nonce is a full 128-bit value.
 * The block is a scratch buffer: every user (f1 0x31A00/0x31F6C, g 0x32102, f2 0x321F4, f3 0x323B8) builds its
 * message and hashes it within one call, and the ROM kernel is single-threaded, so nothing reads the residue.
 *
 * Pool: single producer (app task: bt_rom_rng_feed), single consumer (the hook, bt_ctrl task). Free-running indices,
 * release/acquire fences; consumed bytes are wiped. When a security site finds the pool short, the hook returns the
 * legacy value (it cannot fail) and counts it in `starved`: the platform refuses pairing for the session then.
 * SPDX-License-Identifier: MIT */
#ifndef BT_ROM_RNG_H
#define BT_ROM_RNG_H

#include <stddef.h>
#include <stdint.h>

#define BT_ROM_RNG_POOL_BYTES     512u    /* power of two */
#define BT_ROM_NONCE_BLOCK_ADDR   0x0080340Cu
#define BT_ROM_NONCE_BLOCK_BYTES  28u

enum {
    BT_ROM_RNG_SITE_UNKNOWN = 0,
    BT_ROM_RNG_SITE_LEGACY,
    BT_ROM_RNG_SITE_BYTE,
    BT_ROM_RNG_SITE_NONCE_FIRST,
    BT_ROM_RNG_SITE_WORD,
};

typedef struct {
    uint32_t drbg_calls;     /* security sites served from the pool */
    uint32_t nonce_fills;    /* SSP nonce blocks filled (one per nonce) */
    uint32_t legacy_calls;   /* LEGACY sites */
    uint32_t unknown_calls;  /* return address not in the table (legacy value; investigate if nonzero) */
    uint32_t starved;        /* security site served the legacy value because the pool was short */
    uint32_t fed_bytes;      /* bytes accepted by bt_rom_rng_feed since the last reset */
    uint32_t last_unknown;   /* last unknown return address */
} bt_rom_rng_stats_t;

/* Site class of a hook return address (thumb bit ignored). */
int bt_rom_rng_site(uint32_t ret_addr);

/* Empty the pool (wiped) and zero the stats. Only while the ROM cannot call the hook (before start / after stop). */
void bt_rom_rng_reset(void);

/* Producer (one task). Bytes the pool can take now; feed copies at most that many and returns the count. */
size_t bt_rom_rng_space(void);
size_t bt_rom_rng_feed(const uint8_t *p, size_t n);

/* Consumer: the hook body. ret_addr = LR at hook entry, nonce_block = the ROM's 28-byte hash block, legacy = the
 * legacy generator (xorshift % 80). Never blocks. */
uint32_t bt_rom_rng_hook(uint32_t ret_addr, volatile uint8_t *nonce_block, uint32_t (*legacy)(void));

void bt_rom_rng_stats(bt_rom_rng_stats_t *out);

/* The `starved` count alone, lock-free from any task (one aligned 32-bit load; the hook is its only writer). Nonzero
 * latches until bt_rom_rng_reset() (the next session): the pairing decisions that run between two app-task polls (the
 * patch 0036 Just Works hook in BTC, the numeric-comparison and PIN replies) refuse while it is set (STEP10 C3). */
uint32_t bt_rom_rng_starved(void);

#endif /* BT_ROM_RNG_H */
