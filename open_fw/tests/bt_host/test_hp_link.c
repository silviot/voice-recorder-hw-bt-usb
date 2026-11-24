/* Headphones-path smoke over the real Bluedroid stack (docs/BT_A2DP_DESIGN.md milestone H2; `test_bt_host_hp hp`):
 * bt_ctrl built with BT_CTRL_HP_GATE_LIST (0401 0402 0405 0408 forwarded) over tests/fake_rom.c, whose scripted peer
 * plays a discoverable headset (CoD 0x240418, "HP-Fake") next to a phone and a speaker that does not answer names.
 *
 *   H1  enable, SPP (client side only, to make Bluedroid page), page scan off (0C1A 0x00) before the inquiry
 *   H2  GAP discovery: 0401 -> three results; BTA's name pass (0419 one at a time, after Inquiry Complete) -> the
 *       headset's name and CoD reach the GAP callback; no RNR before Inquiry Complete
 *   H3  discovery cancelled while the inquiry runs: 0402 (zero-length E001), deferred CC, no Inquiry Complete
 *   H4  connect: esp_spp_start_discovery(headset) -> 0405 -> ACL as master (link view), Bluedroid's set-up commands,
 *       the headset's L2CAP refuses SDP, Bluedroid drops the idle link (0406) -> ACL disconnected
 *   H5  teardown back to the heap / task / esp_timer baseline; no unknown opcode, deadline, fault or HCI timeout
 *
 * The A2DP/AVDTP peer comes with milestone H5 (Bumble or fake_hp.c); this only proves the controller rows and the
 * fake ROM models with the stack that will drive them.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "bt_ctrl.h"
#include "fake_rom.h"
#include "host_env.h"

int bth_hp_scenario(void);

static int h_fail, h_checks;
#define HCHECK(cond, ...)                                                                                             \
    do {                                                                                                              \
        h_checks++;                                                                                                   \
        if (!(cond)) {                                                                                                \
            h_fail++;                                                                                                 \
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

static const uint8_t k_hp_wire[6] = { 0xC1, 0xC2, 0xC3, 0x1B, 0x00, 0x88 };     /* 88:00:1B:C3:C2:C1 */
static const uint8_t k_ph_wire[6] = { 0x61, 0x62, 0x63, 0x64, 0x65, 0x66 };
static const uint8_t k_sp_wire[6] = { 0x71, 0x72, 0x73, 0x74, 0x75, 0x76 };

static void to_bda(const uint8_t wire[6], esp_bd_addr_t out)
{
    for (int i = 0; i < 6; i++) {
        out[i] = wire[5 - i];
    }
}

/* ---- callbacks (BTC task; plain counters, read by the test task) ---- */
#define MAX_FOUND 8
typedef struct {
    esp_bd_addr_t bda;
    uint32_t cod;
    int has_cod;
    char name[32];
} found_t;

static found_t s_found[MAX_FOUND];
static volatile int s_found_n, s_disc_started, s_disc_stopped, s_spp_init, s_spp_uninit, s_spp_disc, s_spp_disc_status;
static volatile int s_acl_up, s_acl_down, s_acl_up_stat, s_acl_down_reason;
static volatile uint16_t s_acl_handle;

static found_t *found_slot(const esp_bd_addr_t bda)
{
    for (int i = 0; i < s_found_n; i++) {
        if (memcmp(s_found[i].bda, bda, 6) == 0) {
            return &s_found[i];
        }
    }
    if (s_found_n >= MAX_FOUND) {
        return NULL;
    }
    found_t *f = &s_found[s_found_n];
    memset(f, 0, sizeof(*f));
    memcpy(f->bda, bda, 6);
    s_found_n++;
    return f;
}

/* BTC hands callbacks a 4-aligned copy (fine on Cortex-M4; see tests/bt_host/Makefile). */
__attribute__((no_sanitize("alignment")))
static void gap_cb(esp_bt_gap_cb_event_t e, esp_bt_gap_cb_param_t *p)
{
    switch (e) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        found_t *f = found_slot(p->disc_res.bda);
        for (int i = 0; f != NULL && i < p->disc_res.num_prop; i++) {
            const esp_bt_gap_dev_prop_t *pr = &p->disc_res.prop[i];
            if (pr->type == ESP_BT_GAP_DEV_PROP_COD && pr->len >= 4) {
                memcpy(&f->cod, pr->val, 4);
                f->has_cod = 1;
            } else if (pr->type == ESP_BT_GAP_DEV_PROP_BDNAME && pr->len > 0) {
                int n = pr->len < (int)sizeof(f->name) - 1 ? pr->len : (int)sizeof(f->name) - 1;
                memcpy(f->name, pr->val, (size_t)n);
                f->name[n] = '\0';
            }
        }
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            s_disc_started++;
        } else {
            s_disc_stopped++;
        }
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        s_acl_up_stat = p->acl_conn_cmpl_stat.stat;
        s_acl_handle = p->acl_conn_cmpl_stat.handle;
        s_acl_up++;
        break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        s_acl_down_reason = p->acl_disconn_cmpl_stat.reason;
        s_acl_down++;
        break;
    default:
        break;
    }
}

__attribute__((no_sanitize("alignment")))
static void spp_cb(esp_spp_cb_event_t e, esp_spp_cb_param_t *p)
{
    switch (e) {
    case ESP_SPP_INIT_EVT:
        s_spp_init++;
        break;
    case ESP_SPP_UNINIT_EVT:
        s_spp_uninit++;
        break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
        s_spp_disc_status = p->disc_comp.status;
        s_spp_disc++;
        break;
    default:
        break;
    }
}

/* ---- the headset's L2CAP signalling: refuses every channel (no AVDTP/SDP peer before milestone H5) ---- */
static fake_acl_rec_t s_acl_copy[FAKE_ROM_LOG_MAX];
static uint8_t s_asm[2048];
static uint32_t s_asm_len, s_l2_conn_reqs, s_l2_info_reqs, s_acl_frames;
static uint16_t s_l2_psm_seen;

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void sig_send(uint8_t code, uint8_t id, const uint8_t *d, uint16_t len)
{
    uint8_t f[64];
    put16(f, (uint16_t)(4u + len));
    put16(&f[2], 0x0001u);
    f[4] = code;
    f[5] = id;
    put16(&f[6], len);
    memcpy(&f[8], d, len);
    bth_rom_lock();
    int r = fake_peer_send_acl(f, (uint16_t)(8u + len));
    bth_rom_unlock();
    bth_ctrl_kick();
    HCHECK(r == 0, "headset signalling frame not injected");
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
        if (code == 0x02u && len >= 4u) {                    /* Connection Request: PSM not supported */
            s_l2_conn_reqs++;
            s_l2_psm_seen = rd16(d);
            put16(&r[2], rd16(&d[2]));
            put16(&r[4], 0x0002u);
            sig_send(0x03u, id, r, 8u);
        } else if (code == 0x0Au && len >= 2u) {             /* Information Request */
            s_l2_info_reqs++;
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
        } else if (code == 0x08u) {                          /* Echo Request */
            sig_send(0x09u, id, r, 0u);
        }
        p += 4u + len;
        n -= 4u + len;
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
            s_asm_len = 0u;
        }
        if (s_asm_len + r->len > sizeof(s_asm)) {
            s_asm_len = 0u;
            continue;
        }
        memcpy(&s_asm[s_asm_len], r->data, r->len);
        s_asm_len += r->len;
        if (s_asm_len >= 4u && s_asm_len >= 4u + rd16(s_asm)) {
            s_acl_frames++;
            if (rd16(&s_asm[2]) == 0x0001u) {
                sig_in(&s_asm[4], rd16(s_asm));
            }
            s_asm_len = 0u;
        }
    }
    return n;
}

/* ---- bookkeeping ---- */
typedef struct {
    size_t bytes, blocks;
    UBaseType_t tasks;
    uint32_t timers;
} hsnap_t;

static hsnap_t hsnap(void)
{
    hsnap_t s;
    bth_heap_stats_t h;
    bth_heap_stats(&h);
    s.bytes = h.live_bytes;
    s.blocks = h.live_blocks;
    s.tasks = uxTaskGetNumberOfTasks();
    s.timers = bth_timers_in_use();
    return s;
}

static int hsettle(const hsnap_t *b, hsnap_t *now)
{
    for (int i = 0; i < 600; i++) {
        *now = hsnap();
        if (now->bytes == b->bytes && now->blocks == b->blocks && now->tasks == b->tasks && now->timers == b->timers) {
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return 0;
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

static void rom_log_clear(void)
{
    bth_rom_lock();
    fake_rom.n_cmds = 0u;
    bth_rom_unlock();
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
    HCHECK(errs == 0u, "%s: fake ROM contract error: %s", where, first);
    HCHECK(s.cmd_unknown == 0u, "%s: %u unknown opcodes (last %04X)", where, s.cmd_unknown, s.unknown_opcode_last);
    HCHECK(s.mandatory_fwd_fail == 0u && s.invariant == 0u && s.lock_violations == 0u, "%s: bt_ctrl invariant", where);
    HCHECK(s.fwd_deadline == 0u && s.rom_unknown_ids == 0u && s.rom_short == 0u, "%s: deadline %u unknown id %u short "
           "%u", where, s.fwd_deadline, s.rom_unknown_ids, s.rom_short);
    HCHECK(bth_ctrl_faults() == 0u, "%s: BT fault (first code %u)", where, bth_ctrl_first_fault());
    HCHECK(bth_hci_cmd_timeouts() == 0u, "%s: HCI command timeout", where);
}

static void hp_peer_cfg(fake_peer_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    memcpy(c->addr, k_hp_wire, 6);
    c->cod[0] = 0x18; c->cod[1] = 0x04; c->cod[2] = 0x24;   /* 0x240418 headphones */
    c->io_cap = 0x03;
    c->key_type = 0x04;
    static const uint8_t f0[8] = { 0xBF, 0xFE, 0xCF, 0xFE, 0xDB, 0xFF, 0x7B, 0x87 };
    memcpy(c->features, f0, 8);
    c->features_p1[0] = 0x01;
    c->max_page = 1;
    c->lmp_ver = 0x09;
    c->manuf = 0x001D;
    c->clk_off = 0x2468;
    c->psrm = 1;
    c->rssi = -48;
    c->discoverable = true;
    memcpy(c->name, "HP-Fake", 7);
}

static void hp_world(void)
{
    fake_peer_cfg_t c;
    hp_peer_cfg(&c);
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
    bth_rom_unlock();
}

static int stack_up(void)
{
    HCHECK(bth_ctrl_open() == 0, "bt_ctrl open");
    bth_step("hp: init + enable", 20000u);
    int ok = esp_bluedroid_init() == ESP_OK && esp_bluedroid_enable() == ESP_OK;
    bth_step_done();
    HCHECK(ok, "esp_bluedroid_init/enable");
    esp_spp_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = ESP_SPP_MODE_CB;
    cfg.tx_buffer_size = 990;
    int i0 = s_spp_init;
    HCHECK(esp_bt_gap_register_callback(gap_cb) == ESP_OK && esp_spp_register_callback(spp_cb) == ESP_OK &&
           esp_spp_enhanced_init(&cfg) == ESP_OK, "GAP/SPP registration");
    bth_step("hp: SPP init", 5000u);
    HCHECK(WAIT(s_spp_init > i0, 4000), "ESP_SPP_INIT_EVT");
    bth_step_done();
    return ok;
}

static void stack_down(void)
{
    int u0 = s_spp_uninit;
    bth_step("hp: teardown", 30000u);
    HCHECK(esp_spp_deinit() == ESP_OK, "esp_spp_deinit");
    HCHECK(WAIT(s_spp_uninit > u0, 4000), "ESP_SPP_UNINIT_EVT");
    HCHECK(esp_bluedroid_disable() == ESP_OK, "disable");
    HCHECK(bth_ctrl_close(4000u) == BT_CTRL_STOP_OK, "bt_ctrl close");
    HCHECK(esp_bluedroid_deinit() == ESP_OK, "deinit");
    bth_step_done();
}

int bth_hp_scenario(void)
{
    bth_log("[H0] warm-up cycle (btc_spp keeps resident objects after its first init)\n");
    if (stack_up()) {
        stack_down();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    hsnap_t base = hsnap();

    bth_log("[H1] enable, SPP client, headset world, page scan off\n");
    hp_world();
    stack_up();
    rom_log_clear();
    HCHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE) == ESP_OK, "scan mode connectable");
    HCHECK(WAIT(bt_ctrl_scan_enable() == 0x02u, 2000), "page scan on (0C1A 0x02) %02X", bt_ctrl_scan_enable());
    HCHECK(esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE) == ESP_OK, "scan mode off");
    HCHECK(WAIT(bt_ctrl_scan_enable() == 0x00u, 2000), "page scan off before 0401/0405 (design §6.2)");
    controller_clean("H1");

    bth_log("[H2] discovery: inquiry, results, BTA name pass\n");
    rom_log_clear();
    int st0 = s_disc_started, sp0 = s_disc_stopped;
    bth_step("hp: discovery", 20000u);
    HCHECK(esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 2, 0) == ESP_OK, "start discovery");
    HCHECK(WAIT(s_disc_started > st0, 3000), "DISC_STATE STARTED");
    HCHECK(WAIT(s_disc_stopped > sp0, 15000), "DISC_STATE STOPPED");
    bth_step_done();
    esp_bd_addr_t hp, ph, sp;
    to_bda(k_hp_wire, hp);
    to_bda(k_ph_wire, ph);
    to_bda(k_sp_wire, sp);
    const found_t *fh = NULL, *fp = NULL, *fs = NULL;
    for (int i = 0; i < s_found_n; i++) {
        fh = memcmp(s_found[i].bda, hp, 6) == 0 ? &s_found[i] : fh;
        fp = memcmp(s_found[i].bda, ph, 6) == 0 ? &s_found[i] : fp;
        fs = memcmp(s_found[i].bda, sp, 6) == 0 ? &s_found[i] : fs;
    }
    HCHECK(fh && fp && fs && s_found_n == 3, "3 devices found (%d)", s_found_n);
    HCHECK(fh && fh->has_cod && fh->cod == 0x240418u, "headset CoD %06X", fh ? fh->cod : 0u);
    HCHECK(fh && strcmp(fh->name, "HP-Fake") == 0, "headset name '%s'", fh ? fh->name : "");
    HCHECK(fp && strcmp(fp->name, "Phone-1") == 0, "phone name '%s'", fp ? fp->name : "");
    HCHECK(fs && fs->name[0] == '\0', "the speaker has no name (page timeout)");
    bth_rom_lock();
    uint32_t inq = fake_rom.inquiries, comp = fake_rom.inq_completes, saved = fake_rom.saved;
    int first_rnr = -1, first_inq = -1;
    for (uint32_t i = 0u; i < fake_rom.n_cmds; i++) {
        if (fake_rom.cmds[i].id == 0xE000u && first_inq < 0) first_inq = (int)i;
        if (fake_rom.cmds[i].id == 0xE00Cu && first_rnr < 0) first_rnr = (int)i;
    }
    bth_rom_unlock();
    HCHECK(inq == 1u && comp == 1u, "one inquiry, one Inquiry Complete (%u/%u)", inq, comp);
    HCHECK(rom_count(0xE00Cu) == 3u, "BTA's name pass: 3 RNR (%u)", rom_count(0xE00Cu));
    HCHECK(first_inq >= 0 && first_rnr > first_inq && saved == 0u, "no RNR before Inquiry Complete (none SAVED)");
    HCHECK(bt_ctrl_inquiry_active() == 0, "inquiry over");
    controller_clean("H2");

    bth_log("[H3] discovery cancelled during the inquiry: 0402\n");
    bth_rom_lock();
    fake_rom.inq_auto_complete = false;
    uint32_t inq0 = fake_rom.inquiries, comp0 = fake_rom.inq_completes;
    bth_rom_unlock();
    rom_log_clear();
    st0 = s_disc_started;
    sp0 = s_disc_stopped;
    bth_step("hp: cancel", 20000u);
    HCHECK(esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 8, 0) == ESP_OK, "start discovery (10.24 s)");
    HCHECK(WAIT(s_disc_started > st0 && bt_ctrl_inquiry_active(), 3000), "inquiry running");
    HCHECK(esp_bt_gap_cancel_discovery() == ESP_OK, "cancel discovery");
    HCHECK(WAIT(s_disc_stopped > sp0, 5000), "DISC_STATE STOPPED after the cancel");
    bth_step_done();
    bth_rom_lock();
    uint32_t inq1 = fake_rom.inquiries, comp1 = fake_rom.inq_completes, canc = fake_rom.inq_cancels;
    int e001_len = -1;
    for (uint32_t i = 0u; i < fake_rom.n_cmds; i++) {
        if (fake_rom.cmds[i].id == 0xE001u) e001_len = fake_rom.cmds[i].len;
    }
    bth_rom_unlock();
    HCHECK(inq1 == inq0 + 1u && canc == 1u && comp1 == comp0, "cancelled: no Inquiry Complete (%u %u %u)", inq1, canc,
           comp1);
    HCHECK(e001_len == 0, "0402 reached the ROM as E001 with no payload (%d)", e001_len);
    HCHECK(bt_ctrl_inquiry_active() == 0, "inquiry over after the cancel CC");
    controller_clean("H3");

    bth_log("[H4] connect to the headset (SPP SDP client pages it), refused SDP, idle link dropped\n");
    rom_log_clear();
    int up0 = s_acl_up, dn0 = s_acl_down, sd0 = s_spp_disc;
    bth_step("hp: connect", 30000u);
    HCHECK(esp_spp_start_discovery(hp) == ESP_OK, "esp_spp_start_discovery");
    HCHECK(WAIT(s_acl_up > up0, 5000), "ACL_CONN_CMPL_STAT");
    HCHECK(s_acl_up_stat == ESP_BT_STATUS_SUCCESS && s_acl_handle == 0x0080u, "ACL up: stat %d h %04X",
           s_acl_up_stat, s_acl_handle);
    bt_ctrl_link_info_t li;
    HCHECK(bt_ctrl_link_info(0x80, &li) == 1 && li.up && li.outgoing && li.role == BT_CTRL_ROLE_MASTER &&
           memcmp(li.bda, hp, 6) == 0, "link view: outgoing, master");
    bth_rom_lock();
    uint8_t allow_rs = fake_peer.host_allow_rs, psrm = fake_peer.host_psrm;
    uint16_t pkt = fake_peer.host_pkt_type;
    bool paged = fake_peer.we_paged;
    bth_rom_unlock();
    HCHECK(paged && allow_rs == 0u && psrm == 1u && pkt != 0u, "0405: allow_rs %u psrm %u pkt %04X", allow_rs, psrm,
           pkt);
    HCHECK(WAIT(s_spp_disc > sd0, 8000), "ESP_SPP_DISCOVERY_COMP_EVT");
    HCHECK(s_spp_disc_status != ESP_SPP_SUCCESS && s_l2_conn_reqs >= 1u && s_l2_psm_seen == 0x0001u,
           "SDP refused by the headset (status %d, PSM %04X)", s_spp_disc_status, s_l2_psm_seen);
    HCHECK(WAIT(s_acl_down > dn0, 12000), "ACL_DISCONN_CMPL_STAT (idle link dropped)");
    bth_step_done();
    HCHECK(bt_ctrl_link_info(0x80, &li) == 1 && !li.up, "link view: down");
    HCHECK(rom_count(0xE004u) == 1u && rom_count(0xE400u) == 1u, "one 0405, one 0406 (%u, %u)", rom_count(0xE004u),
           rom_count(0xE400u));
    bt_ctrl_stats_t s;
    bt_ctrl_get_stats(&s);
    bth_rom_lock();
    char ids[160];
    int at = 0;
    for (uint32_t i = 0u; i < fake_rom.n_cmds && at < (int)sizeof(ids) - 6; i++) {
        at += snprintf(&ids[at], sizeof(ids) - (size_t)at, " %04X", fake_rom.cmds[i].id);
    }
    bth_rom_unlock();
    bth_log("    ROM commands on the connect path:%s; refused locally so far: %u\n", ids, s.cmd_rej);
    controller_clean("H4");

    bth_log("[H5] teardown\n");
    stack_down();
    controller_clean("H5");
    hsnap_t now;
    HCHECK(hsettle(&base, &now), "baseline: heap %zu B / %zu blocks (base %zu / %zu), tasks %lu (%lu), timers %u (%u)",
           now.bytes, now.blocks, base.bytes, base.blocks, (unsigned long)now.tasks, (unsigned long)base.tasks,
           now.timers, base.timers);
    bth_log("test_bt_host hp: %d checks, %d failures\n", h_checks, h_fail);
    return h_fail == 0 ? 0 : 1;
}
