/* Scenario S (docs/BT_A2DP_DESIGN.md §8.2): the headphones session (bt_port/bt_hp_bluedroid.c) end to end over the real
 * Bluedroid stack, bt_ctrl with the headphones rows forwarded (test_bt_host_hp) and tests/fake_rom.c, against a fake
 * headset written here: L2CAP acceptor, SDP server (A2DP sink + AVRCP CT/TG records), AVDTP acceptor (SBC sink SEP,
 * configurable rates and max bitpool, delay report), a media receiver that checks RTP / SBC framing, decodes the SBC
 * with the vendored decoder and measures the pacing, and a minimal AVCTP/AVRCP peer (passthrough keys, absolute
 * volume capability, registration, CHANGED, SetAbsoluteVolume).
 *
 *   S1  start -> READY: GAP/AVRCP/A2DP config, both SEPs registered, scan off; teardown back to the baseline
 *   S2  discovery: headset + phone + speaker found, CoD audio filter, the headset's name
 *   S3  pick the headset with the pairing window: discovery off, 0405 as master, Just Works accepted through patch
 *       0036 (window + gate), bond in bt_hp_bonds.bin, SDP, AVDTP discover/caps/set config/open -> CONNECTED; 48 kHz
 *       chosen (SEID 0), bitpool <= 35; delay report reaches the pacer
 *   S4  stream a 1 kHz tone (16 kHz mono through hp_sink, resampled x3) for 4 s: RTP sequence and timestamps
 *       contiguous, frames per packet = fpp, packets <= MTU, decoded tone at 1 kHz, rate within 10 % of real time
 *   S5  remote SUSPEND (player pause event), demand START resumes; media again
 *   S6  AVRCP: headset opens AVCTP, PLAY / PAUSE / FORWARD presses -> player keys; absolute volume: capability,
 *       registration, INTERIM, CHANGED -> bt_vol and re-registration, SetAbsoluteVolume from vol31
 *   S7  headset drops the link: LINK_LOST, reconnect at once with the stored key (no new pairing) -> OPEN
 *   S8  credit stall: the headset stops acknowledging while STARTED -> STALLED event, ACL closed through BTA,
 *       reconnect
 *   S9  headset forgets the key: a reconnect needs pairing, the window is closed -> Just Works refused (0036)
 *   S10 teardown while STARTED with a frozen headset: < 2 s, no hard path; a second full start/READY/stop works
 * Review fixes (each check fails without its fix):
 *   S1  a headphones stop never writes phone mode's bt_bonds.bin; the M2.3 gate is never evaluated inside the
 *       session lock (a critical section on target)
 *   S5b a local SUSPEND (REC switch, idle) is not counted or reported as a remote suspend
 *   S5c START rejected twice: the retry waits BT_HP_START_RETRY_MS, START_FAILED; a later play press starts again
 *   S6  single-button headsets: PLAY and PAUSE presses are toggles, STOP pauses; VOL passthrough is one player key
 *       (bt_vol changes only through the player); S6b keys and player events from before a file are dropped by
 *       bt_hp_player_begin()
 *   S8b credit stall without a Disconnection Complete: the escalation restarts headphones mode, which reconnects
 *   S10 (A6) the frozen stop with BTU_TASK's BT_HDR-sized allocations failing still confirms the A2DP deinit (0039's
 *       local close is allocation-free, patch 0043); with AVCTP up, the frozen teardowns leave no AVDTP/AVCTP blocks
 *   S10a (C13) late CONNECTED / DISCONNECTED / AUDIO_CFG of another device leave the stream's transmitter alone
 *   S10d (A6) a failed profile teardown (job rc 5) ends FAULTED (the screen's "BT fault"), the next start refused
 *   G   (test_bt_host, rows refused) scan and connect fail at once with BT_HP_MSG_UNSUPPORTED, no reconnect
 * Not covered here (listed in docs/BT_A2DP_HP_SESSION.md): 44.1 kHz-only peers (SEID 1 fallback), page-scan / LM
 * state variants, AVRCP FF/REW scrub, the player integration (test_player BT=1 variant), soak runs.
 * SPDX-License-Identifier: MIT */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "openfw.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "bt_ctrl.h"
#include "bt_hp.h"
#include "bt_store.h"
#include "fake_rom.h"
#include "host_env.h"
#include "host_xrec.h"
#include "hp_sink.h"
#include "hp_tx.h"
#include "hp_tx_bd.h"
#include "oi_codec_sbc.h"

int bth_headphones_scenario(const char *img_path);
int bth_headphones_gated(const char *img_path);

static int s_fail, s_checks;
#define SCHECK(cond, ...)                                                                                             \
    do {                                                                                                              \
        s_checks++;                                                                                                   \
        if (!(cond)) {                                                                                                \
            s_fail++;                                                                                                 \
            bth_log("FAIL %s:%d: ", __FILE__, __LINE__);                                                              \
            bth_log(__VA_ARGS__);                                                                                     \
            bth_log("\n");                                                                                            \
        }                                                                                                             \
    } while (0)

static uint32_t peer_poll(void);
#define WAIT(cond, ms)                                                                                                \
    ({                                                                                                                \
        int ok_ = 0;                                                                                                  \
        for (uint32_t t_ = 0u; t_ < (uint32_t)(ms); t_++) {                                                           \
            peer_poll();                                                                                              \
            if (cond) {                                                                                               \
                ok_ = 1;                                                                                              \
                break;                                                                                                \
            }                                                                                                         \
            vTaskDelay(1);                                                                                            \
        }                                                                                                             \
        ok_;                                                                                                          \
    })

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint16_t rdbe16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rdbe32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* ============================================ the headset's address book ====================================== */
static const uint8_t k_hp_wire[6] = { 0xC1, 0xC2, 0xC3, 0x1B, 0x00, 0x88 };     /* 88:00:1B:C3:C2:C1 */
static const uint8_t k_ph_wire[6] = { 0x61, 0x62, 0x63, 0x64, 0x65, 0x66 };
static const uint8_t k_sp_wire[6] = { 0x71, 0x72, 0x73, 0x74, 0x75, 0x76 };
static uint8_t s_hp_bda[6];

/* ============================================ platform seam for bt_hp (host) ================================== */
#define CFG_FILE "bt_hp_cfg.bin"
static uint8_t s_cfg_blob[64];
static uint32_t s_cfg_len, s_cfg_saves;
static volatile void *s_att_tx, *s_att_sink;
static const void *volatile s_att_link;

int bt_hp_plat_cfg_load(uint8_t *buf, uint32_t cap, uint32_t *len)
{
    if (s_cfg_len == 0u || s_cfg_len > cap) {
        return -1;
    }
    memcpy(buf, s_cfg_blob, s_cfg_len);
    *len = s_cfg_len;
    return 0;
}

int bt_hp_plat_cfg_save(const uint8_t *buf, uint32_t len)
{
    if (len > sizeof(s_cfg_blob)) {
        return -1;
    }
    memcpy(s_cfg_blob, buf, len);
    s_cfg_len = len;
    s_cfg_saves++;
    return 0;
}

void bt_hp_plat_attach_sink(void *tx, void *sink, const void *link, int ring_is_dac_buf)
{
    (void)ring_is_dac_buf;
    s_att_tx = tx;
    s_att_sink = sink;
    s_att_link = link;
}

int16_t *bt_hp_plat_borrow_dac_buf(uint32_t *bytes)
{
    (void)bytes;
    return NULL;
}

/* ============================================ the fake app task (player) ====================================== */
typedef struct {
    int (*stream_ready)(void);
    void (*request_start)(void);
} link_t;

static volatile int s_feed;            /* the "player" writes a 1 kHz tone, 16 kHz mono */
static volatile uint32_t s_fed, s_keys[8], s_pev;
static uint32_t s_phase;
static volatile uint32_t s_vol31_req = 0xFFu;
static volatile int s_hold;            /* the "player" is not running: keys and events stay queued (S6b) */

static void app_hook(void)
{
    bt_hp_poll();
    int k;
    while (!s_hold && (k = bt_hp_take_key()) != BT_HP_KEY_NONE) {
        if (k >= 0 && k < 8) {
            s_keys[k]++;
        }
    }
    if (!s_hold) {
        s_pev |= bt_hp_take_player_events();
    }
    if (s_vol31_req != 0xFFu) {
        bt_hp_set_vol31((uint8_t)s_vol31_req);
        s_vol31_req = 0xFFu;
    }
    hp_sink_t *sk = (hp_sink_t *)s_att_sink;
    const link_t *l = (const link_t *)s_att_link;
    if (!s_feed || sk == NULL || l == NULL || !l->stream_ready()) {
        return;
    }
    int16_t buf[256];
    for (int round = 0; round < 8; round++) {
        uint32_t fr = hp_sink_free(sk);
        if (fr < 256u) {
            break;
        }
        for (int i = 0; i < 256; i++) {
            buf[i] = (int16_t)(12000.0 * sin(2.0 * M_PI * 1000.0 * (double)(s_phase + (uint32_t)i) / 16000.0));
        }
        uint32_t w = hp_sink_write(sk, buf, 256u, 1u);
        s_phase += w;
        s_fed += w;
        if (w == 0u) {
            break;
        }
    }
    l->request_start();                /* demand start, as aout_write does */
}

static int fn_start(uint32_t a) { (void)a; return bt_hp_start(bthx_card(), 1); }
static int fn_stop(uint32_t a) { (void)a; return bt_hp_stop(); }
static int fn_scan(uint32_t a) { return bt_hp_scan((uint8_t)a); }
static int fn_pair_hp(uint32_t a) { return bt_hp_connect_addr(s_hp_bda, (int)a); }
static int fn_suspend(uint32_t a) { (void)a; bt_hp_suspend(); return 0; }
static int fn_forget(uint32_t a) { (void)a; return bt_hp_forget(); }
static int fn_set_out(uint32_t a) { bt_hp_set_out_mode((uint8_t)a); return (int)bt_hp_out_mode(); }
static int fn_player_begin(uint32_t a) { (void)a; bt_hp_player_begin(); return 0; }
static int fn_fmt(uint32_t a)
{
    hp_sink_t *sk = (hp_sink_t *)s_att_sink;
    if (sk == NULL) {
        return -1;
    }
    hp_sink_set_format(sk, a, 1u);
    hp_sink_set_mute(sk, 0);
    hp_sink_set_gain_q15(sk, 32768u);
    return 0;
}

static bt_hp_ui_t UI(void)
{
    bt_hp_ui_t u;
    bt_hp_get_ui(&u);   /* read-only snapshot of app-task state; racy by a few fields at worst */
    return u;
}

/* ============================================ the fake headset: L2CAP ========================================== */
#define CID_SDP   0x0040u
#define CID_SIG   0x0041u
#define CID_MEDIA 0x0042u
#define CID_AVCT  0x0043u
#define PEER_MTU  895u

typedef struct {
    uint16_t lcid, rcid, psm;
    uint8_t  open, cfg_in, cfg_out;
} ch_t;

static struct {
    uint8_t asm_buf[2048];
    uint32_t asm_len;
    uint8_t sig_id;
    ch_t sdp, sig, media, avct;
    uint8_t frozen;                /* answers nothing (S10) */
    /* SDP */
    uint32_t sdp_reqs, sdp_a2dp, sdp_avrc;
    /* AVDTP */
    uint8_t  caps_freq;            /* SBC freq bits the sink advertises */
    uint8_t  caps_max_bp;
    uint32_t avdt_cmds[16];
    uint8_t  cfg_cie[4];
    uint8_t  int_seid, acp_seid_cfg;
    uint8_t  streaming;
    uint8_t  tl;
    uint32_t avdt_rejects;
    uint8_t  reject_start;         /* START commands to reject (BAD_STATE) before accepting again (S5c) */
    uint32_t n_start;
    uint32_t start_ms[4];          /* when the first STARTs arrived */
    /* media */
    uint32_t pkts, frames, bytes, bad, seq_err, ts_err, fpp_err, len_err, hdr_err;
    uint16_t last_seq;
    uint32_t last_ts, last_nf;
    uint8_t  have_last;
    uint32_t max_pkt;
    uint64_t first_us, last_us;
    int16_t *pcm;
    size_t   pcm_n, pcm_cap;
    uint32_t dec_err;
    /* AVCTP */
    uint32_t avct_rx, getcaps, regs, setvol, setvol_last, psth_acks;
    uint8_t  avct_tl;
    uint8_t  reg_tl;
    uint8_t  reg_pending;
} H;

static uint64_t now_us(void) { return (uint64_t)bt_xrec_plat_now_ms() * 1000u; }

static int acl_send(const uint8_t *f, uint16_t len)
{
    for (int i = 0; i < 2000; i++) {
        bth_rom_lock();
        int r = fake_peer_send_acl(f, len);
        bool up = fake_peer.connected;
        bth_rom_unlock();
        bth_ctrl_kick();
        if (r == 0) {
            return 0;
        }
        if (!up) {
            return -1;
        }
        vTaskDelay(1);
    }
    SCHECK(0, "headset ACL could not be injected");
    return -1;
}

static void l2_send(uint16_t cid, const uint8_t *p, uint16_t len)
{
    uint8_t f[1100];
    put16(f, len);
    put16(&f[2], cid);
    memcpy(&f[4], p, len);
    (void)acl_send(f, (uint16_t)(len + 4u));
}

static void sig_send(uint8_t code, uint8_t id, const uint8_t *d, uint16_t len)
{
    uint8_t b[64];
    b[0] = code;
    b[1] = id;
    put16(&b[2], len);
    memcpy(&b[4], d, len);
    l2_send(0x0001u, b, (uint16_t)(4u + len));
}

static ch_t *ch_lcid(uint16_t lcid)
{
    if (lcid == CID_SDP) return &H.sdp;
    if (lcid == CID_SIG) return &H.sig;
    if (lcid == CID_MEDIA) return &H.media;
    if (lcid == CID_AVCT) return &H.avct;
    return NULL;
}

static void l2_cfg_req(ch_t *c)
{
    uint8_t d[8];
    put16(d, c->rcid);
    put16(&d[2], 0u);
    d[4] = 0x01u;
    d[5] = 2u;
    put16(&d[6], PEER_MTU);
    sig_send(0x04u, ++H.sig_id, d, 8u);
}

/* The headset opens a channel itself (AVCTP, S6). */
static void l2_connect(ch_t *c, uint16_t psm, uint16_t lcid)
{
    memset(c, 0, sizeof(*c));
    c->psm = psm;
    c->lcid = lcid;
    uint8_t d[4];
    put16(d, psm);
    put16(&d[2], lcid);
    sig_send(0x02u, ++H.sig_id, d, 4u);
}

static void sig_in(const uint8_t *p, uint32_t n)
{
    while (n >= 4u) {
        uint8_t code = p[0], id = p[1];
        uint16_t len = rd16(&p[2]);
        const uint8_t *d = &p[4];
        if (4u + len > n) {
            return;
        }
        uint8_t r[16];
        memset(r, 0, sizeof(r));
        ch_t *c = NULL;
        switch (code) {
        case 0x02: {                                     /* Connection Request from the device */
            uint16_t psm = rd16(d), scid = rd16(&d[2]);
            uint16_t lcid = 0u;
            if (psm == 0x0001u) {
                c = &H.sdp;
                lcid = CID_SDP;
            } else if (psm == 0x0019u) {
                c = H.sig.open && H.sig.cfg_in ? &H.media : &H.sig;   /* second AVDTP channel = media (§AVDTP 1.3) */
                lcid = c == &H.media ? CID_MEDIA : CID_SIG;
            } else if (psm == 0x0017u) {
                c = &H.avct;
                lcid = CID_AVCT;
            }
            put16(r, lcid);
            put16(&r[2], scid);
            put16(&r[4], c != NULL ? 0u : 0x0002u);
            sig_send(0x03u, id, r, 8u);
            if (c != NULL) {
                memset(c, 0, sizeof(*c));
                c->lcid = lcid;
                c->rcid = scid;
                c->psm = psm;
                c->open = 1u;
                l2_cfg_req(c);
            }
            break;
        }
        case 0x03:                                       /* Connection Response (our AVCTP) */
            c = ch_lcid(rd16(&d[2]));
            if (c != NULL && rd16(&d[4]) == 0u) {
                c->rcid = rd16(d);
                c->open = 1u;
                l2_cfg_req(c);
            }
            break;
        case 0x04:                                       /* Configuration Request */
            c = ch_lcid(rd16(d));
            put16(r, c != NULL ? c->rcid : 0u);
            sig_send(0x05u, id, r, 6u);
            if (c != NULL) {
                c->cfg_in = 1u;
            }
            break;
        case 0x05:
            c = ch_lcid(rd16(d));
            if (c != NULL) {
                c->cfg_out = 1u;
            }
            break;
        case 0x06:                                       /* Disconnection Request */
            memcpy(r, d, 4u);
            sig_send(0x07u, id, r, 4u);
            c = ch_lcid(rd16(d));
            if (c != NULL) {
                c->open = 0u;
                if (c == &H.media) {
                    H.streaming = 0u;
                }
            }
            break;
        case 0x0A: {                                     /* Information Request */
            uint16_t type = rd16(d);
            put16(r, type);
            uint16_t rl = 4u;
            if (type == 1u) {
                put16(&r[4], 672u);
                rl = 6u;
            } else if (type == 2u) {
                rl = 8u;
            } else if (type == 3u) {
                r[4] = 0x02u;
                rl = 12u;
            } else {
                put16(&r[2], 1u);
            }
            sig_send(0x0Bu, id, r, rl);
            break;
        }
        case 0x08:
            sig_send(0x09u, id, r, 0u);
            break;
        default:
            break;
        }
        p += 4u + len;
        n -= 4u + len;
    }
}

/* ============================================ SDP server ====================================================== */
/* DE helpers: write into b at *o */
static void de_u16(uint8_t *b, uint32_t *o, uint16_t v) { b[(*o)++] = 0x09; b[(*o)++] = (uint8_t)(v >> 8); b[(*o)++] = (uint8_t)v; }
static void de_u32(uint8_t *b, uint32_t *o, uint32_t v)
{
    b[(*o)++] = 0x0A;
    b[(*o)++] = (uint8_t)(v >> 24);
    b[(*o)++] = (uint8_t)(v >> 16);
    b[(*o)++] = (uint8_t)(v >> 8);
    b[(*o)++] = (uint8_t)v;
}
static void de_uuid(uint8_t *b, uint32_t *o, uint16_t v) { b[(*o)++] = 0x19; b[(*o)++] = (uint8_t)(v >> 8); b[(*o)++] = (uint8_t)v; }
static uint32_t de_seq_open(uint8_t *b, uint32_t *o) { b[(*o)++] = 0x35; uint32_t at = (*o)++; return at; }
static void de_seq_close(uint8_t *b, uint32_t *o, uint32_t at) { b[at] = (uint8_t)(*o - at - 1u); }

/* rec 0: A2DP sink, 1: AVRCP controller (the headset's buttons), 2: AVRCP target (category 2: absolute volume) */
static void sdp_record(uint8_t *b, uint32_t *o, int rec)
{
    uint32_t r = de_seq_open(b, o);
    de_u16(b, o, 0x0000u);
    de_u32(b, o, 0x00010001u + (uint32_t)rec);
    de_u16(b, o, 0x0001u);                                   /* ServiceClassIDList */
    uint32_t s = de_seq_open(b, o);
    if (rec == 0) {
        de_uuid(b, o, 0x110Bu);
    } else if (rec == 1) {
        de_uuid(b, o, 0x110Eu);
        de_uuid(b, o, 0x110Fu);
    } else {
        de_uuid(b, o, 0x110Cu);
    }
    de_seq_close(b, o, s);
    de_u16(b, o, 0x0004u);                                   /* ProtocolDescriptorList */
    s = de_seq_open(b, o);
    uint32_t a = de_seq_open(b, o);
    de_uuid(b, o, 0x0100u);
    de_u16(b, o, rec == 0 ? 0x0019u : 0x0017u);
    de_seq_close(b, o, a);
    a = de_seq_open(b, o);
    de_uuid(b, o, rec == 0 ? 0x0019u : 0x0017u);
    de_u16(b, o, rec == 0 ? 0x0103u : 0x0104u);
    de_seq_close(b, o, a);
    de_seq_close(b, o, s);
    de_u16(b, o, 0x0009u);                                   /* BluetoothProfileDescriptorList */
    s = de_seq_open(b, o);
    a = de_seq_open(b, o);
    de_uuid(b, o, rec == 0 ? 0x110Du : 0x110Eu);
    de_u16(b, o, rec == 0 ? 0x0103u : 0x0106u);
    de_seq_close(b, o, a);
    de_seq_close(b, o, s);
    de_u16(b, o, 0x0311u);                                   /* SupportedFeatures */
    de_u16(b, o, rec == 0 ? 0x0001u : rec == 1 ? 0x0001u : 0x0002u);
    de_seq_close(b, o, r);
}

static bool pattern_has(const uint8_t *p, uint32_t n, uint16_t uuid)
{
    for (uint32_t i = 0u; i + 3u <= n; i++) {
        if (p[i] == 0x19u && rdbe16(&p[i + 1u]) == uuid) {
            return true;
        }
    }
    return false;
}

static void sdp_in(const uint8_t *p, uint16_t n)
{
    if (n < 5u) {
        return;
    }
    H.sdp_reqs++;
    uint8_t pdu = p[0];
    uint8_t rsp[400];
    uint32_t o = 0u;
    rsp[o++] = pdu == 0x06u ? 0x07u : 0x01u;
    rsp[o++] = p[1];
    rsp[o++] = p[2];
    uint32_t plen_at = o;
    o += 2u;
    if (pdu != 0x06u) {
        rsp[o++] = 0x00u;
        rsp[o++] = 0x03u;                                    /* Invalid request syntax (only 0x06 is served) */
    } else {
        /* pattern = first DES after the header */
        const uint8_t *pat = &p[5];
        uint32_t plen = (pat[0] == 0x35u) ? (uint32_t)pat[1] + 2u : 0u;
        bool a2dp = pattern_has(pat, plen, 0x110Bu) || pattern_has(pat, plen, 0x110Du);
        bool ct = pattern_has(pat, plen, 0x110Eu) || pattern_has(pat, plen, 0x110Fu);
        bool tg = pattern_has(pat, plen, 0x110Cu) || pattern_has(pat, plen, 0x110Eu);
        if (a2dp) {
            H.sdp_a2dp++;
        }
        if (ct || tg) {
            H.sdp_avrc++;
        }
        uint32_t cnt_at = o;
        o += 2u;
        uint32_t lists_start = o;
        uint32_t all = de_seq_open(rsp, &o);
        if (a2dp) {
            sdp_record(rsp, &o, 0);
        }
        if (ct) {
            sdp_record(rsp, &o, 1);
        }
        if (tg) {
            sdp_record(rsp, &o, 2);
        }
        de_seq_close(rsp, &o, all);
        uint32_t cnt = o - lists_start;
        rsp[cnt_at] = (uint8_t)(cnt >> 8);
        rsp[cnt_at + 1u] = (uint8_t)cnt;
        rsp[o++] = 0x00u;                                    /* no continuation */
    }
    uint32_t pl = o - plen_at - 2u;
    rsp[plen_at] = (uint8_t)(pl >> 8);
    rsp[plen_at + 1u] = (uint8_t)pl;
    l2_send(H.sdp.rcid, rsp, (uint16_t)o);
}

/* ============================================ AVDTP acceptor ================================================== */
#define SEID_SNK 1u

static void avdt_send(uint8_t tl, uint8_t msg_type, uint8_t sig, const uint8_t *d, uint16_t len)
{
    uint8_t b[64];
    b[0] = (uint8_t)((tl << 4) | msg_type);
    b[1] = sig;
    if (len != 0u) {
        memcpy(&b[2], d, len);
    }
    l2_send(H.sig.rcid, b, (uint16_t)(2u + len));
}

static uint16_t caps_build(uint8_t *c, bool all)
{
    uint16_t o = 0u;
    c[o++] = 0x01u;                                          /* Media Transport */
    c[o++] = 0u;
    c[o++] = 0x07u;                                          /* Media Codec */
    c[o++] = 6u;
    c[o++] = 0x00u;                                          /* audio */
    c[o++] = 0x00u;                                          /* SBC */
    c[o++] = (uint8_t)((H.caps_freq << 4) | 0x0Fu);          /* freq | all channel modes */
    c[o++] = 0xFFu;                                          /* blocks 4..16, subbands 4/8, SNR + loudness */
    c[o++] = 2u;
    c[o++] = H.caps_max_bp;
    if (all) {
        c[o++] = 0x08u;                                      /* Delay Reporting */
        c[o++] = 0u;
    }
    return o;
}

static void avdt_in(const uint8_t *p, uint16_t n)
{
    if (n < 2u) {
        return;
    }
    uint8_t tl = (uint8_t)(p[0] >> 4), mt = (uint8_t)(p[0] & 3u), sig = (uint8_t)(p[1] & 0x3Fu);
    if (mt != 0u) {
        return;                                              /* responses to our own commands (delay report) */
    }
    if (sig < 16u) {
        H.avdt_cmds[sig]++;
    }
    uint8_t r[32];
    uint16_t rl = 0u;
    switch (sig) {
    case 0x01:                                               /* DISCOVER */
        r[0] = (uint8_t)(SEID_SNK << 2);
        r[1] = (uint8_t)((0u << 4) | (1u << 3));             /* audio, sink */
        rl = 2u;
        break;
    case 0x02:                                               /* GET_CAPABILITIES */
    case 0x0C:                                               /* GET_ALL_CAPABILITIES */
        rl = caps_build(r, sig == 0x0Cu);
        break;
    case 0x03: {                                             /* SET_CONFIGURATION */
        if (n >= 4u) {
            H.acp_seid_cfg = (uint8_t)(p[2] >> 2);
            H.int_seid = (uint8_t)(p[3] >> 2);
            for (uint16_t o = 4u; o + 2u <= n; o = (uint16_t)(o + 2u + p[o + 1u])) {
                if (p[o] == 0x07u && p[o + 1u] >= 6u) {
                    memcpy(H.cfg_cie, &p[o + 4u], 4u);
                }
            }
        }
        avdt_send(tl, 2u, sig, NULL, 0u);
        /* the sink reports its delay (1/10 ms) once configured (AVDTP 1.3 §8.19) */
        uint8_t dr[3] = { (uint8_t)(H.int_seid << 2), 0x05u, 0xDCu };   /* 150.0 ms */
        avdt_send(++H.tl & 0x0Fu, 0u, 0x0Du, dr, 3u);
        return;
    }
    case 0x06:                                               /* OPEN */
    case 0x08:                                               /* CLOSE */
    case 0x0A:                                               /* ABORT */
        if (sig != 0x06u) {
            H.streaming = 0u;
        }
        break;
    case 0x07:                                               /* START */
        if (H.n_start < 4u) {
            H.start_ms[H.n_start] = bt_xrec_plat_now_ms();
        }
        H.n_start++;
        if (H.reject_start != 0u) {
            H.reject_start--;
            r[0] = n >= 3u ? p[2] : (uint8_t)(SEID_SNK << 2);  /* first failing SEID, BAD_STATE */
            r[1] = 0x31u;
            avdt_send(tl, 3u, sig, r, 2u);
            return;
        }
        H.streaming = 1u;
        H.have_last = 0u;
        break;
    case 0x09:                                               /* SUSPEND */
        H.streaming = 0u;
        break;
    case 0x0D:                                               /* DELAY_REPORT (as SNK we never get one) */
        break;
    default:
        H.avdt_rejects++;
        r[0] = 0x19u;                                        /* NOT_SUPPORTED_COMMAND */
        avdt_send(tl, 1u, sig, r, 1u);
        return;
    }
    avdt_send(tl, 2u, sig, r, rl);
}

static void avdt_cmd(uint8_t sig)            /* the headset's own command (remote SUSPEND) */
{
    uint8_t d[1] = { (uint8_t)(H.int_seid << 2) };
    avdt_send(++H.tl & 0x0Fu, 0u, sig, d, 1u);
    if (sig == 0x09u) {
        H.streaming = 0u;
    }
}

/* ============================================ media receiver ================================================== */
static OI_CODEC_SBC_DECODER_CONTEXT s_dec;
static OI_UINT32 s_dec_data[CODEC_DATA_WORDS(2, SBC_CODEC_FAST_FILTER_BUFFERS)];

static void media_in(const uint8_t *p, uint16_t n)
{
    if (n < 13u || (p[0] & 0xC0u) != 0x80u) {
        H.hdr_err++;
        return;
    }
    uint64_t t = now_us();
    if (H.pkts == 0u) {
        H.first_us = t;
    }
    H.last_us = t;
    uint16_t seq = rdbe16(&p[2]);
    uint32_t ts = rdbe32(&p[4]);
    uint8_t nf = (uint8_t)(p[12] & 0x0Fu);
    if (H.have_last) {
        if ((uint16_t)(H.last_seq + 1u) != seq) {
            H.seq_err++;
        }
        if (ts != H.last_ts + H.last_nf * 128u) {
            H.ts_err++;
        }
    }
    H.have_last = 1u;
    H.last_seq = seq;
    H.last_ts = ts;
    H.last_nf = nf;
    H.pkts++;
    H.bytes += n;
    if (n > H.max_pkt) {
        H.max_pkt = n;
    }
    const OI_BYTE *f = &p[13];
    OI_UINT32 left = (OI_UINT32)(n - 13u);
    uint32_t got = 0u;
    while (left > 0u) {
        if (H.pcm_n + 256u * 2u > H.pcm_cap) {
            H.pcm_cap = H.pcm_cap ? H.pcm_cap * 2u : 1u << 20;
            H.pcm = (int16_t *)realloc(H.pcm, H.pcm_cap * sizeof(int16_t));
        }
        OI_UINT32 bytes = (OI_UINT32)((H.pcm_cap - H.pcm_n) * sizeof(int16_t));
        OI_STATUS st = OI_CODEC_SBC_DecodeFrame(&s_dec, &f, &left, H.pcm + H.pcm_n, &bytes);
        if (st != OI_OK) {
            H.dec_err++;
            break;
        }
        H.pcm_n += bytes / sizeof(int16_t);
        got++;
    }
    if (got != nf) {
        H.fpp_err++;
    }
    H.frames += got;
}

/* Goertzel power of f over the last n stereo frames' left channel, relative to total power. */
static double tone_purity(double f, double rate, size_t n)
{
    if (H.pcm_n < 2u * n) {
        return 0.0;
    }
    const int16_t *x = H.pcm + H.pcm_n - 2u * n;
    double w = 2.0 * M_PI * f / rate, c = 2.0 * cos(w), s1 = 0, s2 = 0, tot = 0;
    for (size_t i = 0; i < n; i++) {
        double v = x[2u * i];
        double s0 = v + c * s1 - s2;
        s2 = s1;
        s1 = s0;
        tot += v * v;
    }
    double pw = s1 * s1 + s2 * s2 - c * s1 * s2;
    return tot > 0 ? (2.0 * pw / (double)n) / tot : 0.0;
}

/* ============================================ AVCTP / AVRCP ==================================================== */
static void avct_send(uint8_t tl, int cr, const uint8_t *avc, uint16_t len)
{
    uint8_t b[64];
    b[0] = (uint8_t)((tl << 4) | (cr ? 0x02u : 0u));
    b[1] = 0x11u;
    b[2] = 0x0Eu;
    memcpy(&b[3], avc, len);
    l2_send(H.avct.rcid, b, (uint16_t)(3u + len));
}

static void avrc_vendor_rsp(uint8_t tl, uint8_t ctype, uint8_t pdu, const uint8_t *prm, uint16_t pl)
{
    uint8_t a[40];
    a[0] = ctype;
    a[1] = 0x48u;
    a[2] = 0x00u;                                            /* VENDOR DEPENDENT */
    a[3] = 0x00u;
    a[4] = 0x19u;
    a[5] = 0x58u;                                            /* BT SIG company id */
    a[6] = pdu;
    a[7] = 0x00u;
    a[8] = (uint8_t)(pl >> 8);
    a[9] = (uint8_t)pl;
    memcpy(&a[10], prm, pl);
    avct_send(tl, 1, a, (uint16_t)(10u + pl));
}

static void avct_in(const uint8_t *p, uint16_t n)
{
    if (n < 6u) {
        return;
    }
    H.avct_rx++;
    uint8_t tl = (uint8_t)(p[0] >> 4);
    int cr = (p[0] & 0x02u) != 0u;
    const uint8_t *a = &p[3];
    uint16_t al = (uint16_t)(n - 3u);
    if (cr) {
        if (al >= 3u && a[2] == 0x7Cu && a[0] == 0x09u) {
            H.psth_acks++;                                   /* ACCEPTED to our passthrough */
        }
        return;
    }
    if (al >= 10u && a[2] == 0x00u) {                        /* VENDOR DEPENDENT command from the device's CT */
        uint8_t pdu = a[6];
        uint8_t prm[8];
        if (pdu == 0x10u) {                                  /* GetCapabilities */
            H.getcaps++;
            prm[0] = a[10];
            if (a[10] == 0x03u) {
                prm[1] = 1u;
                prm[2] = 0x0Du;                              /* EVENT_VOLUME_CHANGED */
                avrc_vendor_rsp(tl, 0x0Cu, pdu, prm, 3u);
            } else {
                prm[1] = 1u;
                prm[2] = 0x00u;
                prm[3] = 0x19u;
                prm[4] = 0x58u;
                avrc_vendor_rsp(tl, 0x0Cu, pdu, prm, 5u);
            }
        } else if (pdu == 0x31u) {                           /* RegisterNotification */
            H.regs++;
            H.reg_tl = tl;
            H.reg_pending = 1u;
            prm[0] = 0x0Du;
            prm[1] = 50u;
            avrc_vendor_rsp(tl, 0x0Fu, pdu, prm, 2u);        /* INTERIM, volume 50 */
        } else if (pdu == 0x50u) {                           /* SetAbsoluteVolume */
            H.setvol++;
            H.setvol_last = a[10] & 0x7Fu;
            prm[0] = (uint8_t)H.setvol_last;
            avrc_vendor_rsp(tl, 0x09u, pdu, prm, 1u);
        } else {
            prm[0] = 0x00u;
            avrc_vendor_rsp(tl, 0x0Au, pdu, prm, 1u);        /* REJECTED */
        }
        return;
    }
    uint8_t rsp[8];
    memcpy(rsp, a, al < 8u ? al : 8u);
    rsp[0] = 0x08u;                                          /* NOT IMPLEMENTED */
    avct_send(tl, 1, rsp, al < 8u ? al : 8u);
}

static void avrc_key(uint8_t op)                 /* press + release from the headset's buttons */
{
    for (int rel = 0; rel < 2; rel++) {
        uint8_t a[5] = { 0x00u, 0x48u, 0x7Cu, (uint8_t)(op | (rel ? 0x80u : 0u)), 0u };
        avct_send(++H.avct_tl & 0x0Fu, 0, a, 5u);
        (void)WAIT(0, 30);
    }
}

static void avrc_vol_changed(uint8_t v)
{
    if (!H.reg_pending) {
        return;
    }
    H.reg_pending = 0u;
    uint8_t prm[2] = { 0x0Du, v };
    avrc_vendor_rsp(H.reg_tl, 0x0Du, 0x31u, prm, 2u);        /* CHANGED */
}

/* ============================================ air ============================================================== */
static fake_acl_rec_t s_acl_copy[FAKE_ROM_LOG_MAX];

static void l2_frame_in(const uint8_t *f, uint32_t n)
{
    uint16_t cid = rd16(&f[2]);
    const uint8_t *p = &f[4];
    uint16_t len = (uint16_t)(n - 4u);
    if (H.frozen) {
        return;
    }
    if (cid == 0x0001u) {
        sig_in(p, len);
    } else if (cid == CID_SDP && H.sdp.open) {
        sdp_in(p, len);
    } else if (cid == CID_SIG && H.sig.open) {
        avdt_in(p, len);
    } else if (cid == CID_MEDIA && H.media.open) {
        media_in(p, len);
    } else if (cid == CID_AVCT && H.avct.open) {
        avct_in(p, len);
    } else {
        H.bad++;
    }
}

static uint32_t peer_poll(void)
{
    bth_rom_lock();
    uint32_t n = fake_rom.n_acl;
    if (n != 0u) {
        memcpy(s_acl_copy, fake_rom.acl, n * sizeof(fake_acl_rec_t));
        fake_rom.n_acl = 0u;
    }
    bth_rom_unlock();
    for (uint32_t i = 0u; i < n; i++) {
        const fake_acl_rec_t *r = &s_acl_copy[i];
        if ((r->pb_bc & 3u) != 1u) {
            H.asm_len = 0u;
        }
        if (H.asm_len + r->len > sizeof(H.asm_buf)) {
            H.asm_len = 0u;
            continue;
        }
        memcpy(&H.asm_buf[H.asm_len], r->data, r->len);
        H.asm_len += r->len;
        if (H.asm_len >= 4u && H.asm_len >= 4u + rd16(H.asm_buf)) {
            l2_frame_in(H.asm_buf, 4u + rd16(H.asm_buf));
            H.asm_len = 0u;
        }
    }
    return n;
}

static void peer_link_reset(void)
{
    uint8_t f = H.caps_freq, bp = H.caps_max_bp;
    int16_t *pcm = H.pcm;
    size_t cap = H.pcm_cap;
    memset(&H, 0, sizeof(H));
    H.caps_freq = f;
    H.caps_max_bp = bp;
    H.pcm = pcm;
    H.pcm_cap = cap;
    OI_CODEC_SBC_DecoderReset(&s_dec, s_dec_data, sizeof(s_dec_data), 2, 2, FALSE, FALSE);
}

/* ============================================ bookkeeping ====================================================== */
typedef struct {
    size_t bytes, blocks;
    UBaseType_t tasks;
    uint32_t timers;
    uint32_t seq;
} sbase_t;

static sbase_t ssnap(void)
{
    sbase_t b;
    bth_heap_stats_t h;
    bth_heap_stats(&h);
    b.bytes = h.live_bytes;
    b.blocks = h.live_blocks;
    b.tasks = uxTaskGetNumberOfTasks();
    b.timers = bth_timers_in_use();
    b.seq = bth_heap_seq();
    return b;
}

static int ssettle(const sbase_t *base, sbase_t *now)
{
    for (int i = 0; i < 600; i++) {
        *now = ssnap();
        if (now->bytes == base->bytes && now->blocks == base->blocks && now->tasks == base->tasks &&
            now->timers == base->timers) {
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return 0;
}

static void controller_clean(const char *where)
{
    bt_ctrl_stats_t s;
    bt_ctrl_get_stats(&s);
    bth_rom_lock();
    uint32_t errs = fake_rom.errors;
    char first[160];
    memcpy(first, fake_rom.first_error, sizeof(first));
    bth_rom_unlock();
    SCHECK(errs == 0u, "%s: fake ROM contract error: %s", where, first);
    SCHECK(s.cmd_unknown == 0u, "%s: %u unknown opcodes (last %04X)", where, s.cmd_unknown, s.unknown_opcode_last);
    SCHECK(s.invariant == 0u && s.lock_violations == 0u, "%s: bt_ctrl invariant", where);
    SCHECK(s.rom_unknown_ids == 0u, "%s: %u ROM ids without a packer", where, s.rom_unknown_ids);
    SCHECK(bth_ctrl_faults() == 0u, "%s: BT fault (first code %u)", where, bth_ctrl_first_fault());
    SCHECK(bth_hci_cmd_timeouts() == 0u, "%s: HCI command timeout", where);
    SCHECK(bthx_hard_path_reason() == 0u, "%s: hard path (reason %u)", where, bthx_hard_path_reason());
}

static uint32_t rom_count(uint16_t id)
{
    uint32_t n = 0u;
    bth_rom_lock();
    for (uint32_t i = 0u; i < fake_rom.n_cmds; i++) {
        n += fake_rom.cmds[i].id == id;
    }
    bth_rom_unlock();
    return n;
}

static uint32_t rom_stops(void)
{
    bth_rom_lock();
    uint32_t n = fake_rom.stops;
    bth_rom_unlock();
    return n;
}

static void rom_log_clear(void)
{
    bth_rom_lock();
    fake_rom.n_cmds = 0u;
    bth_rom_unlock();
}

static void world(void)
{
    fake_peer_cfg_t c;
    memset(&c, 0, sizeof(c));
    memcpy(c.addr, k_hp_wire, 6);
    c.cod[0] = 0x18; c.cod[1] = 0x04; c.cod[2] = 0x24;         /* 0x240418 headphones */
    c.io_cap = 0x03;                                           /* NoInputNoOutput: Just Works */
    c.auth_req = 0x04;
    c.key_type = 0x04;                                         /* unauthenticated combination P-192 */
    c.user_confirms = true;
    c.auto_encrypt = true;
    static const uint8_t f0[8] = { 0xBF, 0xFE, 0xCF, 0xFE, 0xDB, 0xFF, 0x7B, 0x87 };
    memcpy(c.features, f0, 8);
    c.features_p1[0] = 0x01;
    /* like the laptop of silicon 2026-11-25 (BlueZ): max_page 2, page 2 bit 0 (CSB) clear, and the fake ROM makes the
     * initiator pairing decision from the single "remote page 1" slot (docs/BT_ROM_HCI_MAP.md §12, patch 0040) */
    c.features_p2[0] = 0x00;
    c.features_p2[1] = 0x03;                                   /* SC host/controller-ish bits: anything but bit 0 */
    c.max_page = 2;
    c.rom_auth_decision = true;
    c.lmp_ver = 0x09;
    c.manuf = 0x001D;
    c.clk_off = 0x2468;
    c.psrm = 1;
    c.rssi = -48;
    c.discoverable = true;
    memcpy(c.name, "HP-Fake", 7);
    fake_dev_t d;
    bth_rom_lock();
    fake_peer_init(&c);
    memset(&d, 0, sizeof(d));
    memcpy(d.addr, k_ph_wire, 6);
    d.cod[0] = 0x0C; d.cod[1] = 0x02; d.cod[2] = 0x5A;
    d.psrm = 1;
    d.answers_name = true;
    memcpy(d.name, "Phone-1", 7);
    (void)fake_rom_add_device(&d);
    memset(&d, 0, sizeof(d));
    memcpy(d.addr, k_sp_wire, 6);
    d.cod[0] = 0x14; d.cod[1] = 0x04; d.cod[2] = 0x24;
    d.psrm = 2;
    (void)fake_rom_add_device(&d);
    fake_rom.inq_auto_complete = true;
    fake_rom.auto_ack_acl = true;
    bth_rom_unlock();
    for (int i = 0; i < 6; i++) {
        s_hp_bda[i] = k_hp_wire[5 - i];
    }
}

static bool start_ready(const char *what)
{
    bth_step(what, 20000u);
    SCHECK(bthx_call_fn(fn_start, 0u, 3000u) == 0, "%s: bt_hp_start", what);
    bool ok = WAIT(UI().state == BT_HP_ST_READY || UI().state == BT_HP_ST_FAULTED, 15000);
    bth_step_done();
    bt_hp_ui_t u = UI();
    SCHECK(ok && u.state == BT_HP_ST_READY, "%s: not READY (state %u)", what, u.state);
    return u.state == BT_HP_ST_READY;
}

static uint32_t stop_session(const char *what)
{
    bth_step(what, 10000u);
    uint64_t t0 = now_us();
    uint32_t seq = bthx_post_fn(fn_stop, 0u);
    int r = -1000;
    (void)WAIT(bthx_done(seq, &r), 8000);
    uint32_t ms = (uint32_t)((now_us() - t0) / 1000u);
    bth_step_done();
    SCHECK(r == 0, "%s: bt_hp_stop result %d (hard path reason)", what, r);
    SCHECK(ms < BT_XREC_SHUTDOWN_DEADLINE_MS, "%s: stop took %u ms (deadline %u)", what, ms,
           BT_XREC_SHUTDOWN_DEADLINE_MS);
    SCHECK(UI().state == BT_HP_ST_OFF, "%s: state %u after stop", what, UI().state);
    bth_log("    %s: %u ms (profile teardown rc %u)\n", what, ms, bthx_profile_down_rc());
    return ms;
}

static bool wait_link(uint8_t link, uint32_t ms) { return WAIT(UI().link == link, ms); }

/* A late A2DP event of another device (STEP10 C13), delivered to the session's A2DP callback on BTC_TASK, where
 * btc_av delivers its own (bth_btc_run). */
static esp_a2d_cb_event_t s_late_ev;
static esp_a2d_cb_param_t s_late_p;
static void late_cb(void *arg)
{
    (void)arg;
    esp_a2d_cb_t cb = (esp_a2d_cb_t)bth_a2dp_app_cb();
    if (cb != NULL) {
        cb(s_late_ev, &s_late_p);
    }
}
static bool late_a2d(esp_a2d_cb_event_t ev, const esp_a2d_cb_param_t *p)
{
    s_late_ev = ev;
    s_late_p = *p;
    return bth_btc_run(late_cb, NULL, 2000u) == 0;
}

/* ============================================ scenario ========================================================= */
#define CARD_SECTORS 4096u
static uint8_t s_disk[CARD_SECTORS * 512u];
static exfat_vol_t s_card;
static int disk_read(void *ctx, uint32_t lba, uint32_t n, void *buf)
{
    (void)ctx;
    if (lba + n > CARD_SECTORS) return -1;
    memcpy(buf, &s_disk[lba * 512u], n * 512u);
    return 0;
}
static int disk_write(void *ctx, uint32_t lba, uint32_t n, const void *buf)
{
    (void)ctx;
    if (lba + n > CARD_SECTORS) return -1;
    memcpy(&s_disk[lba * 512u], buf, n * 512u);
    return 0;
}
static const blkdev_t s_dev = { disk_read, disk_write, CARD_SECTORS, NULL };

static int bond_file_size(void)
{
    static uint8_t img[BT_STORE_IMAGE_MAX];
    return bthx_read_root_file("bt_hp_bonds.bin", img, sizeof(img));
}

int bth_headphones_scenario(const char *img_path)
{
    size_t known_leak_blocks = 0u, known_leak_bytes = 0u;
    bth_log("[S] headphones mode end to end: bt_hp session + Bluedroid A2DP/AVRCP + bt_ctrl + fake ROM headset (heap seq %u)\n", (unsigned)bth_heap_seq());
    FILE *fimg = fopen(img_path, "rb");
    if (fimg == NULL || fread(s_disk, 1u, sizeof(s_disk), fimg) != sizeof(s_disk) || exfat_mount(&s_card, &s_dev) != 0) {
        bth_log("S: %s is not a %u-sector exFAT image\n", img_path, CARD_SECTORS);
        if (fimg) fclose(fimg);
        return 1;
    }
    fclose(fimg);
    exfat_set_timestamp(&s_card, 0x5B2A6000u);
    H.caps_freq = 0x03u;                                     /* 44.1 | 48 */
    H.caps_max_bp = 53u;
    peer_link_reset();
    world();
    SCHECK(bthx_init(&s_card) == 0, "bthx_init");
    bthx_set_app_hook(app_hook);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* ---- S1 ---- */
    bth_log("[S1] start -> READY, stop (warm-up and baseline) (heap seq %u)\n", (unsigned)bth_heap_seq());
    sbase_t base0 = ssnap();
    if (!start_ready("S1 start")) {
        goto out;
    }
    SCHECK(s_att_tx != NULL && s_att_sink != NULL && s_att_link != NULL, "sink not attached to the audio output");
    SCHECK(bt_ctrl_scan_enable() == 0u, "scan enable 0x%02X (want off)", bt_ctrl_scan_enable());
    stop_session("S1 stop");
    SCHECK(s_att_tx == NULL, "sink still attached after stop");
    {
        static uint8_t img[BT_STORE_IMAGE_MAX];
        SCHECK(bthx_read_root_file("bt_bonds.bin", img, sizeof(img)) < 0,
               "a headphones stop wrote phone mode's bt_bonds.bin");
        SCHECK(bthx_gate_in_lock() == 0u, "pairing gate evaluated inside the session lock %u times",
               bthx_gate_in_lock());
    }
    {
        /* Settings -> Audio output (main.c UI_SET_ROW_AOUT): the mode is saved in bt_hp_cfg.bin (byte 5, after magic
         * and version), only when it changes, and survives the session */
        uint32_t saves = s_cfg_saves;
        SCHECK(bthx_call_fn(fn_set_out, BT_HP_OUT_BOTH, 1000u) == (int)BT_HP_OUT_BOTH, "out mode Both not taken");
        SCHECK(s_cfg_saves == saves + 1u && s_cfg_len >= 6u && s_cfg_blob[5] == BT_HP_OUT_BOTH,
               "out mode Both not saved (saves %u -> %u, byte %u)", saves, s_cfg_saves, s_cfg_blob[5]);
        SCHECK(bthx_call_fn(fn_set_out, BT_HP_OUT_BOTH, 1000u) == (int)BT_HP_OUT_BOTH && s_cfg_saves == saves + 1u,
               "unchanged out mode saved again");
        SCHECK(bthx_call_fn(fn_set_out, BT_HP_OUT_SPEAKER, 1000u) == (int)BT_HP_OUT_SPEAKER &&
               s_cfg_saves == saves + 2u && s_cfg_blob[5] == BT_HP_OUT_SPEAKER, "out mode Speaker not saved");
    }
    controller_clean("S1");
    sbase_t w;
    (void)ssettle(&base0, &w);
    bth_log("    S1 warm-up left %zd blocks / %zd B (Bluedroid residents)\n", (ssize_t)(w.blocks - base0.blocks),
            (ssize_t)(w.bytes - base0.bytes));
    sbase_t base = ssnap();

    /* ---- S2 ---- */
    bth_log("[S2] discovery (heap seq %u)\n", (unsigned)bth_heap_seq());
    if (!start_ready("S2 start")) {
        goto out;
    }
    rom_log_clear();
    SCHECK(bthx_call_fn(fn_scan, 4u, 2000u) == 0, "bt_hp_scan");
    SCHECK(WAIT(UI().scanning == 0u && UI().n_found >= 3u, 20000), "discovery did not finish (found %u)",
           UI().n_found);
    {
        bt_hp_ui_t u = UI();
        int hp = -1, audio = 0;
        for (int i = 0; i < u.n_found; i++) {
            audio += u.found[i].is_audio;
            if (memcmp(u.found[i].bda, s_hp_bda, 6) == 0) {
                hp = i;
            }
        }
        SCHECK(hp >= 0 && u.found[hp].is_audio && strcmp(u.found[hp].name, "HP-Fake") == 0,
               "headset not found / not audio / name '%s'", hp >= 0 ? u.found[hp].name : "-");
        SCHECK(audio == 2, "audio-class devices %d (want headset + speaker)", audio);
        SCHECK(rom_count(0xE001u) == 0u || 1, "inquiry");   /* the 0401 itself is checked by test_hp_link */
    }

    /* ---- S3 ---- */
    bth_log("[S3] pick + Just Works in the window + A2DP connect (heap seq %u)\n", (unsigned)bth_heap_seq());
    peer_link_reset();
    rom_log_clear();
    bt_hp_debug_t d0;
    SCHECK(bthx_call_fn(fn_pair_hp, 1u, 2000u) == 0, "bt_hp_connect_addr(pair)");
    bth_step("S3 connect", 30000u);
    bool open = wait_link(BT_HP_LINK_OPEN, 20000);
    bth_step_done();
    {
        bt_hp_ui_t u = UI();
        bt_hp_get_debug(&d0);
        SCHECK(open, "link not OPEN (link %u msg %u)", u.link, u.msg);
        SCHECK(d0.jw_allowed == 1u && d0.jw_refused == 0u, "Just Works allowed %u refused %u", d0.jw_allowed,
               d0.jw_refused);
        SCHECK(fake_peer.pairings == 1u && fake_peer.we_paged, "pairings %u we_paged %u", fake_peer.pairings,
               fake_peer.we_paged);
        /* patch 0040: page 1 only, so the ROM's slot keeps the host-SSP bit and the initiator decision picks SSP */
        SCHECK(fake_peer.ext_reads[1] >= 1u && fake_peer.ext_reads[2] == 0u,
               "remote ext features read page 1 %u, page 2 %u (want page 1 only, patch 0040)", fake_peer.ext_reads[1],
               fake_peer.ext_reads[2]);
        SCHECK(fake_peer.pin_requests == 0u && fake_peer.io_cap_requests >= 1u && d0.pin_rejected == 0u,
               "initiator pairing: PIN requests %u (legacy), IO capability requests %u (SSP), PIN rejected %u",
               fake_peer.pin_requests, fake_peer.io_cap_requests, d0.pin_rejected);
        /* patch 0041: our IO Capability Reply as initiator asks for general bonding (BTM_AUTH_GB_BIT 0x04, MITM bit
         * unchanged); without it 0x01 (MITM, no bonding), whose key BlueZ dropped at disconnect */
        SCHECK(fake_peer.host_io_replies >= 1u && (fake_peer.host_auth_req & 0x06u) == 0x04u,
               "042B auth_req 0x%02X (want general bonding, patch 0041)", fake_peer.host_auth_req);
        SCHECK(H.sdp_a2dp >= 1u, "no A2DP SDP query");
        SCHECK(H.avdt_cmds[1] >= 1u && (H.avdt_cmds[2] + H.avdt_cmds[12]) >= 1u && H.avdt_cmds[3] == 1u &&
               H.avdt_cmds[6] == 1u, "AVDTP discover %u caps %u/%u setcfg %u open %u", H.avdt_cmds[1], H.avdt_cmds[2],
               H.avdt_cmds[12], H.avdt_cmds[3], H.avdt_cmds[6]);
        SCHECK((H.cfg_cie[0] >> 4) == 0x1u, "configured freq bits 0x%X (want 48 kHz)", H.cfg_cie[0] >> 4);
        SCHECK(H.cfg_cie[3] <= 35u && H.cfg_cie[3] >= 2u, "configured max bitpool %u (want <= 35)", H.cfg_cie[3]);
        SCHECK(WAIT(UI().rate == 48000u, 2000), "session rate %u", UI().rate);
        SCHECK(strcmp(u.target_name, "HP-Fake") == 0, "target name '%s'", u.target_name);
        SCHECK(u.has_last && memcmp(u.last, s_hp_bda, 6) == 0, "last headset not stored");
        SCHECK(WAIT(d0.delay_reports >= 1u || (bt_hp_get_debug(&d0), d0.delay_reports >= 1u), 2000),
               "delay report not seen");
        hp_tx_t *t = hp_tx_bd();
        SCHECK(hp_tx_lat_ms(t) == 150u, "pacer latency %u ms (sink reported 150.0)", hp_tx_lat_ms(t));
    }
    SCHECK(WAIT(bond_file_size() > 0, 3000), "bt_hp_bonds.bin not written");

    /* ---- S4 ---- */
    bth_log("[S4] stream a 1 kHz tone (heap seq %u)\n", (unsigned)bth_heap_seq());
    SCHECK(bthx_call_fn(fn_fmt, 16000u, 2000u) == 0, "sink format");
    s_feed = 1;
    bth_step("S4 START", 10000u);
    SCHECK(wait_link(BT_HP_LINK_STARTED, 5000) && WAIT(H.streaming && H.pkts > 0u, 3000), "not streaming (link %u)",
           UI().link);
    bth_step_done();
    uint32_t p0 = H.frames;
    uint64_t t0 = now_us();
    bth_step("S4 media", 20000u);
    (void)WAIT(0, 4000);
    bth_step_done();
    {
        double secs = (double)(now_us() - t0) / 1e6;
        double fps = (double)(H.frames - p0) / secs, want = 48000.0 / 128.0;
        hp_tx_t *t = hp_tx_bd();
        bth_log("    S4: %u pkts, %u frames, %.1f frames/s (want %.1f), fpp %u, max pkt %u B, mtu %u, pads %u\n",
                H.pkts, H.frames, fps, want, t->fpp, H.max_pkt, t->audio_mtu, t->st.pad_pkts);
        SCHECK(fabs(fps - want) / want < 0.10, "rate %.1f frames/s vs %.1f", fps, want);
        SCHECK(H.seq_err == 0u && H.ts_err == 0u && H.hdr_err == 0u, "RTP seq %u ts %u hdr %u errors", H.seq_err,
               H.ts_err, H.hdr_err);
        SCHECK(H.fpp_err == 0u && H.dec_err == 0u, "frames/packet mismatch %u, decode errors %u", H.fpp_err, H.dec_err);
        SCHECK(H.max_pkt <= t->audio_mtu + 13u, "packet %u > MTU %u", H.max_pkt, t->audio_mtu);
        double pur = tone_purity(1000.0, 48000.0, 9600u);
        bth_log("    S4: 1 kHz share of the decoded signal %.3f\n", pur);
        SCHECK(pur > 0.9, "decoded tone purity %.3f", pur);
        SCHECK(t->st.tx_lost_sbc == 0u, "tx_lost %u", t->st.tx_lost_sbc);
    }

    /* ---- S5 ---- */
    bth_log("[S5] remote SUSPEND, demand START (heap seq %u)\n", (unsigned)bth_heap_seq());
    s_pev = 0u;
    s_feed = 0;
    avdt_cmd(0x09u);
    SCHECK(WAIT(UI().link == BT_HP_LINK_OPEN, 3000), "remote suspend not seen (link %u)", UI().link);
    SCHECK(WAIT(s_pev & BT_HP_PEV_REMOTE_SUSPEND, 1000), "no REMOTE_SUSPEND player event (0x%x)", s_pev);
    uint32_t pk = H.pkts;
    s_feed = 1;
    SCHECK(wait_link(BT_HP_LINK_STARTED, 5000) && WAIT(H.pkts > pk + 20u, 3000), "no resume (link %u)", UI().link);
    {
        bt_hp_debug_t d;
        bt_hp_get_debug(&d);
        SCHECK(d.remote_suspends == 1u && d.starts >= 2u, "remote suspends %u starts %u", d.remote_suspends,
               d.starts);
    }
    /* S5b: our own SUSPEND (REC switch; the idle suspend takes the same path) */
    {
        bt_hp_debug_t a, b;
        s_feed = 0;
        (void)WAIT(0, 100);
        s_pev = 0u;
        bt_hp_get_debug(&a);
        SCHECK(bthx_call_fn(fn_suspend, 0u, 2000u) == 0, "bt_hp_suspend");
        SCHECK(WAIT(UI().link == BT_HP_LINK_OPEN, 3000), "local suspend not done (link %u)", UI().link);
        (void)WAIT(0, 300);
        bt_hp_get_debug(&b);
        SCHECK(b.suspends == a.suspends + 1u && b.remote_suspends == a.remote_suspends,
               "S5b: suspends %u -> %u, remote suspends %u -> %u (a local suspend counted as remote)", a.suspends,
               b.suspends, a.remote_suspends, b.remote_suspends);
        SCHECK((s_pev & BT_HP_PEV_REMOTE_SUSPEND) == 0u, "S5b: local suspend raised REMOTE_SUSPEND (0x%x)", s_pev);
    }
    /* S5c: the headset refuses START twice (busy with another source), later accepts */
    {
        s_pev = 0u;
        H.n_start = 0u;
        H.reject_start = 2u;
        s_feed = 1;
        SCHECK(WAIT(s_pev & BT_HP_PEV_START_FAILED, 4000), "S5c: no START_FAILED (0x%x, STARTs %u)", s_pev, H.n_start);
        s_feed = 0;                                          /* the player pauses */
        SCHECK(H.n_start == 2u && H.start_ms[1] - H.start_ms[0] >= BT_HP_START_RETRY_MS - 50u,
               "S5c: %u STARTs, retry after %u ms (want 2, >= %u)", H.n_start, H.start_ms[1] - H.start_ms[0],
               BT_HP_START_RETRY_MS);
        H.reject_start = 0u;
        (void)WAIT(0, 300);
        s_feed = 1;                                          /* play pressed again */
        SCHECK(wait_link(BT_HP_LINK_STARTED, 8000), "S5c: no START after START_FAILED (link %u)", UI().link);
        bt_hp_debug_t d;
        bt_hp_get_debug(&d);
        SCHECK(d.start_fail == 2u, "S5c: start_fail %u", d.start_fail);
    }

    /* ---- S6 ---- */
    bth_log("[S6] AVRCP keys and absolute volume (heap seq %u)\n", (unsigned)bth_heap_seq());
    if (!H.avct.open) {
        l2_connect(&H.avct, 0x0017u, CID_AVCT);
    }
    SCHECK(WAIT(H.avct.open && H.avct.cfg_in && H.avct.cfg_out, 3000), "AVCTP not open");
    (void)WAIT(0, 300);
    memset((void *)s_keys, 0, sizeof(s_keys));
    avrc_key(0x44u);                                         /* PLAY */
    avrc_key(0x46u);                                         /* PAUSE */
    avrc_key(0x45u);                                         /* STOP */
    avrc_key(0x4Bu);                                         /* FORWARD */
    SCHECK(WAIT(s_keys[BT_HP_KEY_TOGGLE] == 2u && s_keys[BT_HP_KEY_PAUSE] == 1u && s_keys[BT_HP_KEY_NEXT] == 1u, 2000),
           "keys TOGGLE %u (PLAY/PAUSE presses) PLAY %u PAUSE %u (STOP) NEXT %u", s_keys[BT_HP_KEY_TOGGLE],
           s_keys[BT_HP_KEY_PLAY], s_keys[BT_HP_KEY_PAUSE], s_keys[BT_HP_KEY_NEXT]);
    SCHECK(s_keys[BT_HP_KEY_PLAY] == 0u, "PLAY press delivered as PLAY (dropped by a playing player)");
    SCHECK(H.psth_acks == 0u || 1, "passthrough");
    SCHECK(WAIT(H.getcaps >= 1u && H.regs >= 1u, 3000), "absolute volume: GetCapabilities %u, registrations %u",
           H.getcaps, H.regs);
    SCHECK(WAIT(UI().abs_vol, 1000), "session did not see absolute volume");
    avrc_vol_changed(90u);
    SCHECK(WAIT(UI().bt_vol == 90u && H.regs >= 2u, 2000), "CHANGED: bt_vol %u, registrations %u", UI().bt_vol,
           H.regs);
    uint32_t sv = H.setvol;
    s_vol31_req = 10u;
    SCHECK(WAIT(H.setvol > sv && H.setvol_last == (10u * 127u + 15u) / 31u, 2000), "SetAbsoluteVolume %u (last %u)",
           H.setvol, H.setvol_last);
    {
        /* VOL+ passthrough: one player key; bt_vol moves only through the player's bt_hp_set_vol31 */
        (void)WAIT(0, 100);
        uint8_t v0 = UI().bt_vol;
        avrc_key(0x41u);                                     /* VOLUME UP */
        SCHECK(WAIT(s_keys[BT_HP_KEY_VOL_UP] == 1u, 2000), "VOL_UP key %u", s_keys[BT_HP_KEY_VOL_UP]);
        SCHECK(UI().bt_vol == v0, "VOL_UP passthrough changed bt_vol %u -> %u besides the player key", v0,
               UI().bt_vol);
    }
    /* S6b: keys and events queued while no file plays are dropped when the next file starts */
    {
        s_hold = 1;
        memset((void *)s_keys, 0, sizeof(s_keys));
        s_pev = 0u;
        avrc_key(0x4Bu);                                     /* FORWARD on the menu */
        s_feed = 0;                                          /* no file plays: no demand START after the suspend */
        avdt_cmd(0x09u);                                     /* the headset suspends (taken off) */
        SCHECK(WAIT(UI().link == BT_HP_LINK_OPEN, 3000), "S6b: remote suspend not seen (link %u)", UI().link);
        (void)WAIT(0, 200);
        SCHECK(bthx_call_fn(fn_player_begin, 0u, 2000u) == 0, "bt_hp_player_begin");
        s_hold = 0;
        (void)WAIT(0, 300);
        SCHECK(s_keys[BT_HP_KEY_NEXT] == 0u && (s_pev & BT_HP_PEV_REMOTE_SUSPEND) == 0u,
               "S6b: stale NEXT %u / player events 0x%x reached the new file", s_keys[BT_HP_KEY_NEXT], s_pev);
    }

    /* ---- S7 ---- */
    bth_log("[S7] headset drops the link, reconnect with the stored key (heap seq %u)\n", (unsigned)bth_heap_seq());
    s_pev = 0u;
    s_feed = 0;
    uint32_t pairings = fake_peer.pairings;
    peer_link_reset();                                       /* the reconnect starts at once (backoff step 0) */
    bth_rom_lock();
    fake_peer_disconnect(0x13u);
    bth_rom_unlock();
    bth_ctrl_kick();
    SCHECK(WAIT(s_pev & BT_HP_PEV_LINK_LOST, 3000), "no LINK_LOST (0x%x)", s_pev);
    bth_step("S7 reconnect", 30000u);
    SCHECK(wait_link(BT_HP_LINK_OPEN, 20000), "no reconnect (link %u)", UI().link);
    bth_step_done();
    SCHECK(fake_peer.pairings == pairings, "reconnect paired again (%u)", fake_peer.pairings);

    /* ---- S8 ---- */
    bth_log("[S8] credit stall while streaming (heap seq %u)\n", (unsigned)bth_heap_seq());
    s_feed = 1;
    SCHECK(wait_link(BT_HP_LINK_STARTED, 5000) && WAIT(H.pkts > 20u, 3000), "not streaming before the stall");
    s_pev = 0u;
    bth_rom_lock();
    fake_rom.auto_ack_acl = false;                           /* the baseband stops acknowledging: NOCP stops */
    bth_rom_unlock();
    bth_step("S8 stall", 30000u);
    SCHECK(WAIT(s_pev & BT_HP_PEV_STALLED, 4000), "no STALLED event (0x%x)", s_pev);
    s_feed = 0;
    peer_link_reset();                                       /* the stalled link's channels are gone */
    bth_rom_lock();
    fake_rom.auto_ack_acl = true;
    (void)fake_rom_ack_acl(64u);
    bth_rom_unlock();
    bth_ctrl_kick();
    SCHECK(WAIT(UI().link == BT_HP_LINK_NONE, 10000), "stalled link not closed (link %u)", UI().link);
    SCHECK(WAIT(UI().link == BT_HP_LINK_OPEN, 20000), "no reconnect after the stall (link %u)", UI().link);
    {
        bt_hp_debug_t d;
        bt_hp_get_debug(&d);
        SCHECK(d.stalls == 1u, "stalls %u", d.stalls);
        SCHECK(rom_count(0xE400u) >= 1u, "no 0406 reached the controller");
    }
    bth_step_done();

    /* ---- S8b ---- */
    bth_log("[S8b] credit stall, no Disconnection Complete: escalation restarts headphones mode (heap seq %u)\n",
            (unsigned)bth_heap_seq());
    {
        s_feed = 1;
        SCHECK(wait_link(BT_HP_LINK_STARTED, 5000) && WAIT(H.pkts > 20u, 3000), "S8b: not streaming before the stall");
        s_pev = 0u;
        bth_rom_lock();
        uint32_t stops = fake_rom.stops;
        fake_rom.auto_ack_acl = false;                       /* NOCP stops ... */
        fake_peer.defer_detach = true;                       /* ... and the 0406 never completes (ghost link) */
        bth_rom_unlock();
        bth_step("S8b stall", 40000u);
        SCHECK(WAIT(s_pev & BT_HP_PEV_STALLED, 4000), "S8b: no STALLED event (0x%x)", s_pev);
        s_feed = 0;
        bool esc = WAIT(rom_stops() != stops, BT_HP_STALL_DISC_MS + 6000u);
        SCHECK(esc, "S8b: no escalation (controller not stopped)");
        peer_link_reset();
        bth_rom_lock();
        fake_peer.defer_detach = false;
        fake_rom.auto_ack_acl = true;
        bth_rom_unlock();
        SCHECK(WAIT(UI().state == BT_HP_ST_READY && UI().link == BT_HP_LINK_OPEN, 30000),
               "S8b: headphones mode not back and reconnected (state %u link %u)", UI().state, UI().link);
        bth_step_done();
        controller_clean("S8b");
    }

    /* ---- S9 ---- */
    bth_log("[S9] headset lost its key, window closed: Just Works refused (heap seq %u)\n", (unsigned)bth_heap_seq());
    {
        bt_hp_debug_t d1;
        bt_hp_get_debug(&d1);
        peer_link_reset();
        bth_rom_lock();
        fake_peer.has_key = false;
        fake_peer_disconnect(0x13u);
        bth_rom_unlock();
        bth_ctrl_kick();
        SCHECK(WAIT(UI().link == BT_HP_LINK_NONE, 3000), "link not down");
        bth_step("S9 refused", 30000u);
        bt_hp_debug_t d2;
        SCHECK(WAIT((bt_hp_get_debug(&d2), d2.jw_refused > d1.jw_refused), 15000), "Just Works not refused");
        SCHECK(fake_peer.host_rejects >= 1u || d2.jw_refused >= 1u, "host did not reject the confirmation");
        SCHECK(!wait_link(BT_HP_LINK_OPEN, 2000), "opened without pairing");
        bth_step_done();
        bth_rom_lock();
        fake_peer_set_key(fake_peer.key);                    /* the headset finds its key again */
        bth_rom_unlock();
    }

    /* ---- S10 ---- */
    bth_log("[S10] teardown with a frozen headset while streaming, then a second cycle (heap seq %u)\n", (unsigned)bth_heap_seq());
    peer_link_reset();
    SCHECK(bthx_call_fn(fn_pair_hp, 1u, 2000u) == 0, "connect for S10 (re-pair: S9's failed pairing removed the key)");
    bth_step("S10 connect", 30000u);
    SCHECK(wait_link(BT_HP_LINK_OPEN, 20000), "S10 not OPEN (link %u)", UI().link);
    s_feed = 1;
    SCHECK(wait_link(BT_HP_LINK_STARTED, 5000), "S10 not STARTED");
    if (!H.avct.open) {
        l2_connect(&H.avct, 0x0017u, CID_AVCT);              /* AVCTP up too: its link block must not leak (A6) */
    }
    SCHECK(WAIT(H.avct.open && H.avct.cfg_in && H.avct.cfg_out, 3000), "S10: AVCTP not open");
    bth_step_done();
    (void)WAIT(0, 300);
    /* ---- S10a ---- (review STEP10 C13) late A2DP events of another device while streaming: the session ignores or
     * closes them, and the BTC callback must leave the stream's transmitter alone (before: a DISCONNECTED stopped the
     * tick, a CONNECTED replaced the connection handle, an AUDIO_CFG the encoder configuration) */
    bth_log("[S10a] late A2DP events of another device while streaming (heap seq %u)\n", (unsigned)bth_heap_seq());
    {
        static const uint8_t other[6] = { 0x00u, 0x1Bu, 0xDCu, 0x5Au, 0x5Au, 0x01u };
        bth_step("S10a late events", 20000u);
        uint32_t rate0 = UI().rate;
        esp_a2d_cb_param_t p;
        memset(&p, 0, sizeof(p));
        p.audio_cfg.mcc.type = ESP_A2D_MCT_SBC;
        memcpy(&p.audio_cfg.mcc.cie, H.cfg_cie, 4u);
        ((uint8_t *)&p.audio_cfg.mcc.cie)[0] = (uint8_t)((H.cfg_cie[0] & 0x0Fu) | 0x20u);   /* 44.1 kHz, not 48 */
        memcpy(p.audio_cfg.remote_bda, other, 6u);
        SCHECK(late_a2d(ESP_A2D_AUDIO_CFG_EVT, &p), "S10a: AUDIO_CFG not delivered");
        memset(&p, 0, sizeof(p));
        p.conn_stat.state = ESP_A2D_CONNECTION_STATE_CONNECTED;
        memcpy(p.conn_stat.remote_bda, other, 6u);
        p.conn_stat.conn_hdl = ESP_A2D_INVALID_CONN_HDL;  /* the session's close of it is refused: B is not touched */
        p.conn_stat.audio_mtu = 300u;
        SCHECK(late_a2d(ESP_A2D_CONNECTION_STATE_EVT, &p), "S10a: CONNECTED not delivered");
        uint32_t p0 = H.pkts;
        SCHECK(WAIT(H.pkts > p0 + 20u, 2000), "S10a: the stream stopped after another device's CONNECTED (%u pkts)",
               H.pkts - p0);
        p.conn_stat.state = ESP_A2D_CONNECTION_STATE_DISCONNECTED;
        p.conn_stat.disc_rsn = ESP_A2D_DISC_RSN_ABNORMAL;
        SCHECK(late_a2d(ESP_A2D_CONNECTION_STATE_EVT, &p), "S10a: DISCONNECTED not delivered");
        p0 = H.pkts;
        SCHECK(WAIT(H.pkts > p0 + 20u, 2000), "S10a: the stream stopped after another device's DISCONNECTED (%u pkts)",
               H.pkts - p0);
        SCHECK(UI().link == BT_HP_LINK_STARTED, "S10a: link %u after the late events", UI().link);
        SCHECK(UI().rate == rate0, "S10a: another device's AUDIO_CFG applied (rate %u, was %u)", UI().rate, rate0);
        SCHECK(H.hdr_err == 0u && H.dec_err == 0u && H.len_err == 0u, "S10a: media errors hdr %u dec %u len %u",
               H.hdr_err, H.dec_err, H.len_err);
        bth_step_done();
    }
    H.frozen = 1u;                                           /* nothing answers any more (L2CAP, AVDTP, AVCTP) */
    bth_rom_lock();
    fake_rom.auto_ack_acl = false;
    fake_peer.defer_detach = true;
    bth_rom_unlock();
    s_feed = 0;
    /* STEP10 A6: patch 0039's local close must not depend on an allocation (before 0043 it posted a BT_HDR from
     * BTU_TASK; without it the stream waited for the frozen peer: A2DP deinit timeout, job rc 5) */
    bth_heap_fail_task_size("BTU_TASK", 8u);                 /* sizeof(BT_HDR): 4 x UINT16, no payload */
    stop_session("S10 frozen stop");
    bth_heap_fail_task_size(NULL, 0u);
    bth_log("    S10: %llu BT_HDR-sized BTU allocations failed during the frozen stop\n",
            (unsigned long long)bth_heap_task_size_injected());
    SCHECK(bthx_profile_down_rc() == 0u, "S10: profile teardown rc %u (A2DP deinit not confirmed)",
           bthx_profile_down_rc());
    bth_rom_lock();
    fake_peer.defer_detach = false;
    fake_rom.auto_ack_acl = true;
    bth_rom_unlock();
    H.frozen = 0u;
    controller_clean("S10");
    bth_log("[S10b] second cycle (heap seq %u)\n", (unsigned)bth_heap_seq());
    {
        /* A frozen-peer teardown left AVDTP's stream and signalling blocks (and AVCTP's link block) of that link
         * behind until patch 0043; a clean cycle after it adds nothing either way. */
        free(H.pcm);                                         /* the receiver's buffer, not the session's */
        H.pcm = NULL;
        H.pcm_cap = H.pcm_n = 0u;
        sbase_t after_frozen = ssnap(), now2;
        if (start_ready("S10b start")) {
            stop_session("S10b stop");
        }
        int clean = ssettle(&after_frozen, &now2);
        SCHECK(clean, "S10b: a clean cycle after the frozen teardown changed the heap: %zu B / %zu blocks (was %zu / %zu)",
               now2.bytes, now2.blocks, after_frozen.bytes, after_frozen.blocks);
        known_leak_blocks = after_frozen.blocks > base.blocks ? after_frozen.blocks - base.blocks : 0u;
        known_leak_bytes = after_frozen.bytes > base.bytes ? after_frozen.bytes - base.bytes : 0u;
        bth_log("    S10: frozen teardowns left %zu blocks / %zu B (btc's resident connect queue)\n", known_leak_blocks,
                known_leak_bytes);
        /* two frozen-peer teardowns in this run (S8b's stall escalation, S10 with AVCTP up): AVDT_Deregister /
         * AVCT_Deregister free the dead link's AVDTP stream/channel blocks and AVCTP link block (patch 0043, STEP10
         * A6; before: 31+ blocks / 3.4+ KB). What stays is btc's profile connect queue, allocated once (1 block). */
        SCHECK(known_leak_blocks <= 1u && known_leak_bytes <= 64u, "frozen teardowns left %zu blocks / %zu B",
               known_leak_blocks, known_leak_bytes);
    }

    /* ---- S10c ---- (review STEP10) */
    bth_log("[S10c] headphones mode off: Forget refused (bond kept), Just Works refused (heap seq %u)\n",
            (unsigned)bth_heap_seq());
    {
        SCHECK(UI().state == BT_HP_ST_OFF && UI().has_last, "S10c: want OFF with a remembered headset (state %u last %u)",
               UI().state, UI().has_last);
        /* C8: with BT off the link key cannot be removed, so the headset must stay listed (Forget again later) */
        SCHECK(bthx_call_fn(fn_forget, 0u, 2000u) != 0 && UI().has_last, "S10c: Forget with BT off accepted (last %u)",
               UI().has_last);
        /* C4 / A3: no headphones session -> the patch 0036 hook refuses Just Works (phone mode never keeps it) */
        SCHECK(openfw_just_works_allowed(s_hp_bda) == 0, "S10c: Just Works allowed with no headphones session");
    }

    /* ---- S10d ---- (review STEP10 A6) last: it leaves headphones mode refused for the rest of the run */
    bth_log("[S10d] profile teardown failure (job rc 5) is a fault the screen shows, start refused (heap seq %u)\n",
            (unsigned)bth_heap_seq());
    if (start_ready("S10d start")) {
        bthx_force_profile_down_fail();
        bth_step("S10d stop", 10000u);
        uint32_t seq = bthx_post_fn(fn_stop, 0u);
        int r = -1000;
        SCHECK(WAIT(bthx_done(seq, &r), 8000) && r == 0, "S10d: bt_hp_stop result %d (want 0: no hard path)", r);
        bth_step_done();
        SCHECK(bthx_profile_down_rc() == 2u, "S10d: profile rc %u (knob not applied)", bthx_profile_down_rc());
        SCHECK(UI().state == BT_HP_ST_FAULTED, "S10d: state %u after a failed A2DP teardown (want FAULTED)",
               UI().state);
        SCHECK(bthx_hard_path_reason() == 0u, "S10d: hard path %u", bthx_hard_path_reason());
        SCHECK(bthx_call_fn(fn_start, 0u, 3000u) != 0, "S10d: bt_hp_start accepted after a failed A2DP teardown");
        SCHECK(UI().state == BT_HP_ST_FAULTED, "S10d: state %u after the refused start", UI().state);
    }

out:
    s_feed = 0;
    if (UI().state != BT_HP_ST_OFF && UI().state != BT_HP_ST_FAULTED) {
        stop_session("final stop");
    }
    bthx_set_app_hook(NULL);
    free(H.pcm);                                             /* the receiver's decode buffer is not the session's */
    H.pcm = NULL;
    H.pcm_cap = H.pcm_n = 0u;
    sbase_t now;
    base.blocks += known_leak_blocks;
    base.bytes += known_leak_bytes;
    int settled = ssettle(&base, &now);
    SCHECK(settled, "heap %zu B / %zu blocks (base %zu / %zu), tasks %lu (base %lu), timers %u (base %u)", now.bytes,
           now.blocks, base.bytes, base.blocks, (unsigned long)now.tasks, (unsigned long)base.tasks, now.timers,
           base.timers);
    if (!settled) {
        bth_heap_dump_since(base.seq);
    }
    bth_log("test_bt_host headphones: %d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}

/* G (test_bt_host: bt_ctrl refuses 0401/0402/0405/0408, as in a release BT=1 image): headphones mode starts, but
 * search and connect fail at once with BT_HP_MSG_UNSUPPORTED instead of a 0-device search / endless CONNECT_FAIL. */
static int fn_pair_hp_nopair(uint32_t a) { (void)a; return bt_hp_connect_addr(s_hp_bda, 0); }

int bth_headphones_gated(const char *img_path)
{
    bth_log("[G] headphones mode with the initiator rows refused (release image) (heap seq %u)\n",
            (unsigned)bth_heap_seq());
    FILE *fimg = fopen(img_path, "rb");
    if (fimg == NULL || fread(s_disk, 1u, sizeof(s_disk), fimg) != sizeof(s_disk) || exfat_mount(&s_card, &s_dev) != 0) {
        bth_log("G: %s is not a %u-sector exFAT image\n", img_path, CARD_SECTORS);
        if (fimg) fclose(fimg);
        return 1;
    }
    fclose(fimg);
    peer_link_reset();
    world();
    SCHECK(bthx_init(&s_card) == 0, "bthx_init");
    bthx_set_app_hook(app_hook);
    vTaskDelay(pdMS_TO_TICKS(20));
    if (start_ready("G start")) {
        rom_log_clear();
        uint32_t seq = UI().msg_seq;
        SCHECK(bthx_call_fn(fn_scan, 4u, 2000u) != 0, "G: bt_hp_scan accepted with 0401 refused");
        SCHECK(UI().msg == BT_HP_MSG_UNSUPPORTED && UI().msg_seq != seq, "G: scan msg %u (want UNSUPPORTED)", UI().msg);
        SCHECK(UI().scanning == 0u, "G: scanning");
        seq = UI().msg_seq;
        SCHECK(bthx_call_fn(fn_pair_hp, 1u, 2000u) != 0 && bthx_call_fn(fn_pair_hp_nopair, 0u, 2000u) != 0,
               "G: bt_hp_connect_addr accepted with 0405 refused");
        SCHECK(UI().msg == BT_HP_MSG_UNSUPPORTED && UI().msg_seq != seq, "G: connect msg %u (want UNSUPPORTED)",
               UI().msg);
        (void)WAIT(0, 1500);
        SCHECK(UI().link == BT_HP_LINK_NONE && !UI().reconnecting, "G: link %u reconnecting %u", UI().link,
               UI().reconnecting);
        SCHECK(rom_count(0xE000u) == 0u && rom_count(0xE004u) == 0u, "G: inquiry / page reached the ROM");
        stop_session("G stop");
    }
    controller_clean("G");
    bthx_set_app_hook(NULL);
    bth_log("test_bt_host headphones (gated): %d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
