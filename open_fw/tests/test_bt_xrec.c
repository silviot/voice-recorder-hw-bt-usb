/* Host unit tests for bt_port/bt_xrec_bluedroid.c (plan M4 XREC session): the real session code and the real
 * xrec_proto against mocked esp_spp / esp_bt_gap APIs and a mocked platform (tests drive the BTC callbacks directly,
 * single-threaded). Scenarios: bring-up/config, data path, congestion, partial writes and the in-flight window, write
 * errors, disconnect mid-transfer, pairing-window policy (numeric comparison, Just Works, legacy PIN, passkey), the local
 * numeric-comparison prompt (accept, reject, timeout, window closing, peer gone, second request, bench autoconfirm),
 * bonded reconnect, stop during transfer, deferred reboot behind congestion, entropy fail-closed, event-queue
 * overflow, platform faults, bring-up/config failures and timeouts, the shutdown deadline / latch hard paths.
 * Built by make test-bt-xrec (bt/host_tests.mk) with ASan/UBSan.
 * SPDX-License-Identifier: MIT */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openfw.h"
#include "openfw_crypto.h"
#include "esp_err.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "bt_xrec_bluedroid.h"

void bt_xrec_test_reset(void);

static int g_checks, g_fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(c)) {                                                                                                    \
            g_fails++;                                                                                                 \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                                              \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static const uint8_t PEER[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
static const uint8_t PEER2[6] = { 0x21, 0x22, 0x23, 0x24, 0x25, 0x26 };
#define H1 0x81u
#define H2 0x82u

/* =============================================== mocks: platform =============================================== */
static uint32_t m_now = 1000u;
static int m_lock_depth, m_lock_max, m_notifies, m_allocs, m_frees, m_alloc_fail, m_start_allowed = 1;
static int m_entropy_fail;
static uint32_t m_entropy_ctr = 0x00123456u;
static int m_up_calls, m_down_calls, m_hold_up, m_hold_down, m_up_result, m_down_result, m_job_kind, m_job_pending;
static int m_dl_latched, m_hard_reason, m_service, m_session_end, m_reboot_mode, m_saved_key, m_poll_fault;
static uint16_t m_peer_mtu = 660u;
static int m_verbose;

uint32_t bt_xrec_plat_now_ms(void) { return m_now; }
void bt_xrec_plat_lock(void)
{
    m_lock_depth++;
    if (m_lock_depth > m_lock_max) {
        m_lock_max = m_lock_depth;
    }
}
void bt_xrec_plat_unlock(void) { m_lock_depth--; }
void bt_xrec_plat_notify_app(void) { m_notifies++; }
static char m_last_log[160];
static uint32_t m_cfm_logged = 0xFFFFFFFFu;   /* value of the last "numeric comparison accepted" line */
void bt_xrec_plat_log(const char *msg)
{
    snprintf(m_last_log, sizeof(m_last_log), "%s", msg);
    if (m_verbose) {
        printf("    log: %s\n", msg);
    }
}
void bt_xrec_plat_log_u32(const char *msg, uint32_t v)
{
    snprintf(m_last_log, sizeof(m_last_log), "%s %u", msg, (unsigned)v);
    if (strstr(msg, "numeric comparison accepted") != NULL) {
        m_cfm_logged = v;
    }
    if (m_verbose) {
        printf("    log: %s %u\n", msg, (unsigned)v);
    }
}
/* Controller link notes (bt_ctrl_note_pop + bt_ctrl_note_format on target): a scripted queue. */
static struct { int kind; uint8_t arg, status; uint16_t handle; char line[64]; } m_notes[24];
static int m_notes_n;
int bt_xrec_plat_ctrl_note(char *line, uint16_t cap, bt_xrec_note_t *info)
{
    if (m_notes_n == 0) {
        return 0;
    }
    int kind = m_notes[0].kind;
    info->arg = m_notes[0].arg;
    info->status = m_notes[0].status;
    info->handle = m_notes[0].handle;
    snprintf(line, cap, "%s", m_notes[0].line);
    memmove(&m_notes[0], &m_notes[1], sizeof(m_notes[0]) * (size_t)(m_notes_n - 1));
    m_notes_n--;
    return kind;
}
/* bt_ctrl_peer_name on target: the newest remote name a Remote Name Request Complete carried */
static uint8_t m_ctrl_name_bda[6];
static char m_ctrl_name[32];
static int m_ctrl_name_ok;
int bt_xrec_plat_ctrl_name(uint8_t bda[6], char *name, uint16_t cap)
{
    if (!m_ctrl_name_ok) {
        return 0;
    }
    memcpy(bda, m_ctrl_name_bda, 6u);
    snprintf(name, cap, "%s", m_ctrl_name);
    return 1;
}
static void note_push_ex(int kind, uint8_t status, uint8_t arg, uint16_t handle, const char *line)
{
    m_notes[m_notes_n].status = status;
    m_notes[m_notes_n].handle = handle;
    m_notes[m_notes_n].kind = kind;
    m_notes[m_notes_n].arg = arg;
    snprintf(m_notes[m_notes_n].line, sizeof(m_notes[0].line), "%s", line);
    m_notes_n++;
}
static void note_push(int kind, uint8_t arg, const char *line)
{
    m_notes[m_notes_n].status = 0u;
    m_notes[m_notes_n].handle = 0u;
    m_notes[m_notes_n].kind = kind;
    m_notes[m_notes_n].arg = arg;
    snprintf(m_notes[m_notes_n].line, sizeof(m_notes[0].line), "%s", line);
    m_notes_n++;
}
uint16_t bt_xrec_plat_spp_peer_mtu(uint32_t handle) { (void)handle; return m_peer_mtu; }
/* Live session blocks and their sizes: a block must go back to the heap zeroed (no key, no PAIR line left in it). */
static void *m_blk[8];
static size_t m_blk_n[8];
static int m_free_dirty;
void *bt_xrec_plat_alloc(size_t n)
{
    if (m_alloc_fail) {
        return NULL;
    }
    m_allocs++;
    void *p = malloc(n);
    for (int i = 0; p != NULL && i < 8; i++) {
        if (m_blk[i] == NULL) {
            m_blk[i] = p;
            m_blk_n[i] = n;
            break;
        }
    }
    return p;
}
void bt_xrec_plat_free(void *p)
{
    if (p != NULL) {
        m_frees++;
        for (int i = 0; i < 8; i++) {
            if (m_blk[i] == p) {
                const uint8_t *b = (const uint8_t *)p;
                for (size_t k = 0; k < m_blk_n[i]; k++) {
                    if (b[k] != 0u) {
                        m_free_dirty++;
                        break;
                    }
                }
                m_blk[i] = NULL;
                break;
            }
        }
    }
    free(p);
}
int bt_xrec_plat_start_allowed(void) { return m_start_allowed; }
void bt_xrec_plat_identity(uint8_t bd[6])
{
    static const uint8_t k[6] = { 0x1D, 0x01, 0x00, 0x55, 0xCA, 0x02 };
    memcpy(bd, k, 6u);
}
/* plan M2.3 pairing gate (firmware: bt_xrec_fw.c): entropy, ROM P-192 key, ROM RNG pool. m_pair_block models the
 * non-entropy reasons (key not replaced, pool ran dry). */
static const char *m_pair_block;
static int m_pair_block_calls;
const char *bt_xrec_plat_pairing_block(void)
{
    m_pair_block_calls++;
    if (m_entropy_fail) {
        return "entropy not credited (mock)";
    }
    return m_pair_block;
}

int bt_xrec_plat_entropy(void *out, size_t n)
{
    if (m_entropy_fail) {
        return -1;
    }
    uint8_t *o = (uint8_t *)out;
    for (size_t i = 0; i < n; i++) {
        m_entropy_ctr = m_entropy_ctr * 1103515245u + 12345u;
        o[i] = (uint8_t)(m_entropy_ctr >> 16);
    }
    return 0;
}

/* device ops: a deterministic 4 KiB "log" for large LOG replies */
static uint8_t m_logbuf[4096];
static uint32_t dev_read_log(void *user, uint32_t off, uint8_t *dst, uint32_t max_len, uint32_t *out_widx)
{
    (void)user;
    if (out_widx) {
        *out_widx = sizeof(m_logbuf);
    }
    if (dst == NULL || off >= sizeof(m_logbuf)) {
        return 0u;
    }
    uint32_t n = (uint32_t)sizeof(m_logbuf) - off;
    if (n > max_len) {
        n = max_len;
    }
    memcpy(dst, &m_logbuf[off], n);
    return n;
}
static const xrec_ops_t k_dev_ops = { .read_log = dev_read_log };
const xrec_ops_t *bt_xrec_plat_device_ops(void) { return &k_dev_ops; }
static const uint8_t m_key[32] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                   16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
static int m_have_key;           /* SD:/cassini.key present */
void bt_xrec_plat_load_bound_key(xrec_ctx_t *ctx)
{
    if (m_have_key) {
        xrec_set_bound_key(ctx, m_key);
    }
}
void bt_xrec_plat_save_bound_key(const uint8_t key[32]) { (void)key; m_saved_key++; }

int bt_xrec_plat_stack_up_begin(exfat_vol_t *vol, int sd_mounted, const uint8_t bd[6])
{
    (void)vol;
    (void)sd_mounted;
    (void)bd;
    m_up_calls++;
    m_job_kind = 1;
    m_job_pending = 1;
    return 0;
}
void bt_xrec_plat_stack_down_begin(void)
{
    m_down_calls++;
    m_job_kind = 2;
    m_job_pending = 1;
}
int bt_xrec_plat_job_done(int *result)
{
    if (!m_job_pending || (m_job_kind == 1 && m_hold_up) || (m_job_kind == 2 && m_hold_down)) {
        return 0;
    }
    *result = m_job_kind == 1 ? m_up_result : m_down_result;
    return 1;
}
int bt_xrec_plat_deadline_disarm(void) { return m_dl_latched; }
void bt_xrec_plat_hard_path(uint32_t reason) { m_hard_reason = (int)reason; }
void bt_xrec_plat_service(void) { m_service++; }
void bt_xrec_plat_wait(void) { m_now += 1u; }
uint32_t bt_xrec_plat_poll(void) { return (uint32_t)m_poll_fault; }
void bt_xrec_plat_session_end(void) { m_session_end++; }
void bt_xrec_plat_reboot(uint8_t mode) { m_reboot_mode = mode; }
uint32_t bt_xrec_plat_irq_count(void) { return 4242u; }

/* =============================================== mocks: Bluedroid =============================================== */
static esp_bt_gap_cb_t m_gap_cb;
static esp_spp_cb_t m_spp_cb;
static char m_name[64];
static esp_bt_cod_t m_cod;
static uint8_t m_iocap = 0xFF;
static esp_spp_cfg_t m_spp_cfg;
static esp_spp_start_srv_cfg_t m_srv_cfg;
static char m_srv_name[32];
static int m_scan_calls, m_scan_c = -1, m_scan_d = -1;
static int m_cfm_calls, m_cfm_accept = -1, m_pin_calls, m_pin_accept = -1, m_passkey_calls, m_passkey_accept = -1;
static uint8_t m_cfm_bda[6];
static int m_name_calls;
static uint8_t m_name_bda[6];
static int m_remove_calls, m_disc_calls;
static uint32_t m_disc_handle;
static esp_err_t m_spp_init_ret = ESP_OK, m_srv_ret = ESP_OK;
static uint8_t m_bonds[8][6];
static uint8_t m_bond_mitm[8];   /* stored link key type authenticated (bt_xrec_plat_bond_mitm) */
static int m_nbonds, m_purges;

esp_err_t esp_bt_gap_register_callback(esp_bt_gap_cb_t cb) { m_gap_cb = cb; return ESP_OK; }
esp_err_t esp_spp_register_callback(esp_spp_cb_t cb) { m_spp_cb = cb; return ESP_OK; }
esp_err_t esp_bt_gap_set_device_name(const char *name)
{
    snprintf(m_name, sizeof(m_name), "%s", name);
    return ESP_OK;
}
esp_err_t esp_bt_gap_set_cod(esp_bt_cod_t cod, esp_bt_cod_mode_t mode)
{
    m_cod = cod;
    return mode == ESP_BT_INIT_COD ? ESP_OK : ESP_FAIL;
}
esp_err_t esp_bt_gap_set_security_param(esp_bt_sp_param_t t, void *v, uint8_t len)
{
    if (t != ESP_BT_SP_IOCAP_MODE || len != 1u) {
        return ESP_ERR_INVALID_ARG;
    }
    m_iocap = *(uint8_t *)v;
    return ESP_OK;
}
esp_err_t esp_spp_enhanced_init(const esp_spp_cfg_t *cfg)
{
    m_spp_cfg = *cfg;
    return m_spp_init_ret;
}
esp_err_t esp_spp_start_srv_with_cfg(const esp_spp_start_srv_cfg_t *cfg)
{
    m_srv_cfg = *cfg;
    snprintf(m_srv_name, sizeof(m_srv_name), "%s", cfg->name ? cfg->name : "");
    return m_srv_ret;
}
esp_err_t esp_bt_gap_set_scan_mode(esp_bt_connection_mode_t c, esp_bt_discovery_mode_t d)
{
    m_scan_calls++;
    m_scan_c = (int)c;
    m_scan_d = (int)d;
    return ESP_OK;
}
esp_err_t esp_bt_gap_ssp_confirm_reply(esp_bd_addr_t bda, bool accept)
{
    memcpy(m_cfm_bda, bda, 6u);
    m_cfm_calls++;
    m_cfm_accept = accept;
    return ESP_OK;
}
esp_err_t esp_bt_gap_read_remote_name(esp_bd_addr_t bda)
{
    memcpy(m_name_bda, bda, 6u);
    m_name_calls++;
    return ESP_OK;
}
esp_err_t esp_bt_gap_pin_reply(esp_bd_addr_t bda, bool accept, uint8_t len, esp_bt_pin_code_t pin)
{
    (void)bda;
    (void)len;
    (void)pin;
    m_pin_calls++;
    m_pin_accept = accept;
    return ESP_OK;
}
esp_err_t esp_bt_gap_ssp_passkey_reply(esp_bd_addr_t bda, bool accept, uint32_t passkey)
{
    (void)bda;
    (void)passkey;
    m_passkey_calls++;
    m_passkey_accept = accept;
    return ESP_OK;
}
esp_err_t esp_bt_gap_remove_bond_device(esp_bd_addr_t bda)
{
    m_remove_calls++;
    for (int i = 0; i < m_nbonds; i++) {
        if (memcmp(m_bonds[i], bda, 6u) == 0) {
            memmove(m_bonds[i], m_bonds[i + 1], (size_t)(m_nbonds - i - 1) * 6u);
            memmove(&m_bond_mitm[i], &m_bond_mitm[i + 1], (size_t)(m_nbonds - i - 1));
            m_nbonds--;
            break;
        }
    }
    return ESP_OK;
}
int bt_xrec_plat_bond_mitm(const uint8_t bda[6])
{
    for (int i = 0; i < m_nbonds; i++) {
        if (memcmp(m_bonds[i], bda, 6u) == 0) {
            return m_bond_mitm[i];
        }
    }
    return 0;
}
void bt_xrec_plat_bond_purge(const uint8_t bda[6])
{
    m_purges++;
    int r = m_remove_calls;
    (void)esp_bt_gap_remove_bond_device((uint8_t *)bda);   /* the config entry goes at once */
    m_remove_calls = r;
}
int esp_bt_gap_get_bond_device_num(void) { return m_nbonds; }
esp_err_t esp_bt_gap_get_bond_device_list(int *dev_num, esp_bd_addr_t *dev_list)
{
    int n = *dev_num < m_nbonds ? *dev_num : m_nbonds;
    for (int i = 0; i < n; i++) {
        memcpy(dev_list[i], m_bonds[i], 6u);
    }
    *dev_num = n;
    return ESP_OK;
}
esp_err_t esp_spp_disconnect(uint32_t handle)
{
    m_disc_calls++;
    m_disc_handle = handle;
    return ESP_OK;
}

/* esp_spp_write model (ESP_SPP_MODE_CB, vendored btc_spp.c): BTC copies into a ring of m_btc_cap bytes; a write that
 * does not fit returns ESP_ERR_NO_MEM and sets upper_cong; draining below half fires ESP_SPP_CONG_EVT(false). */
static uint8_t m_wire[1 << 16];
static uint32_t m_wire_len, m_btc_used, m_btc_cap = 100000u;
static int m_writes, m_upper_cong, m_write_calls_after_stop;
static esp_err_t m_write_err = ESP_OK;
static int m_stopped;
esp_err_t esp_spp_write(uint32_t handle, int len, uint8_t *data)
{
    (void)handle;
    if (m_stopped) {
        m_write_calls_after_stop++;
    }
    if (m_write_err != ESP_OK) {
        return m_write_err;
    }
    if (len <= 0 || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (m_btc_used + (uint32_t)len > m_btc_cap) {
        m_upper_cong = 1;
        return ESP_ERR_NO_MEM;
    }
    memcpy(&m_wire[m_wire_len], data, (size_t)len);
    m_wire_len += (uint32_t)len;
    m_btc_used += (uint32_t)len;
    m_writes++;
    return ESP_OK;
}

/* =============================================== test drivers =============================================== */
static void spp_evt(esp_spp_cb_event_t e, esp_spp_cb_param_t *p) { m_spp_cb(e, p); }
static void gap_evt(esp_bt_gap_cb_event_t e, esp_bt_gap_cb_param_t *p) { m_gap_cb(e, p); }

/* BTC submitted n bytes to RFCOMM in chunks of `chunk`: WRITE_EVT each, CONG_EVT(false) at the low watermark. */
static void btc_drain(uint32_t handle, uint32_t n, uint32_t chunk)
{
    while (n > 0u && m_btc_used > 0u) {
        uint32_t k = n < chunk ? n : chunk;
        if (k > m_btc_used) {
            k = m_btc_used;
        }
        m_btc_used -= k;
        n -= k;
        esp_spp_cb_param_t p;
        memset(&p, 0, sizeof(p));
        p.write.status = ESP_SPP_SUCCESS;
        p.write.handle = handle;
        p.write.len = (int)k;
        p.write.cong = m_upper_cong != 0;
        spp_evt(ESP_SPP_WRITE_EVT, &p);
        if (m_upper_cong && m_btc_used <= m_btc_cap / 2u) {
            m_upper_cong = 0;
            memset(&p, 0, sizeof(p));
            p.cong.handle = handle;
            p.cong.cong = false;
            spp_evt(ESP_SPP_CONG_EVT, &p);
        }
    }
}

static void srv_open(uint32_t h, const uint8_t bda[6])
{
    esp_spp_cb_param_t p;
    memset(&p, 0, sizeof(p));
    p.srv_open.status = ESP_SPP_SUCCESS;
    p.srv_open.handle = h;
    memcpy(p.srv_open.rem_bda, bda, 6u);
    spp_evt(ESP_SPP_SRV_OPEN_EVT, &p);
}

static void spp_close(uint32_t h)
{
    esp_spp_cb_param_t p;
    memset(&p, 0, sizeof(p));
    p.close.handle = h;
    spp_evt(ESP_SPP_CLOSE_EVT, &p);
}

static void rx_raw(uint32_t h, const void *data, size_t len)
{
    esp_spp_cb_param_t p;
    memset(&p, 0, sizeof(p));
    p.data_ind.handle = h;
    p.data_ind.len = (uint16_t)len;
    p.data_ind.data = (uint8_t *)data;
    spp_evt(ESP_SPP_DATA_IND_EVT, &p);
}

static void rx(uint32_t h, const char *s)
{
    rx_raw(h, s, strlen(s));
    bt_xrec_poll();
}

static void cfm_req(const uint8_t bda[6], uint32_t num)
{
    esp_bt_gap_cb_param_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.cfm_req.bda, bda, 6u);
    p.cfm_req.num_val = num;
    gap_evt(ESP_BT_GAP_CFM_REQ_EVT, &p);
}

static void auth_cmpl(const uint8_t bda[6], esp_bt_status_t st, uint8_t lk)
{
    esp_bt_gap_cb_param_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.auth_cmpl.bda, bda, 6u);
    p.auth_cmpl.stat = st;
    p.auth_cmpl.lk_type = lk;
    gap_evt(ESP_BT_GAP_AUTH_CMPL_EVT, &p);
}

static void acl_disc(const uint8_t bda[6])
{
    esp_bt_gap_cb_param_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.acl_disconn_cmpl_stat.bda, bda, 6u);
    gap_evt(ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT, &p);
}

static void acl_conn_h(const uint8_t bda[6], uint16_t h)
{
    esp_bt_gap_cb_param_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.acl_conn_cmpl_stat.bda, bda, 6u);
    p.acl_conn_cmpl_stat.stat = ESP_BT_STATUS_SUCCESS;
    p.acl_conn_cmpl_stat.handle = h;
    gap_evt(ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT, &p);
}

static void acl_disc_h(const uint8_t bda[6], uint16_t h)
{
    esp_bt_gap_cb_param_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.acl_disconn_cmpl_stat.bda, bda, 6u);
    p.acl_disconn_cmpl_stat.handle = h;
    gap_evt(ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT, &p);
}

static void remote_name_fail(const uint8_t bda[6])
{
    esp_bt_gap_cb_param_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.read_rmt_name.bda, bda, 6u);
    p.read_rmt_name.stat = ESP_BT_STATUS_FAIL;
    gap_evt(ESP_BT_GAP_READ_REMOTE_NAME_EVT, &p);
}

static void remote_name(const uint8_t bda[6], const char *name)
{
    esp_bt_gap_cb_param_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.read_rmt_name.bda, bda, 6u);
    p.read_rmt_name.stat = ESP_BT_STATUS_SUCCESS;
    snprintf((char *)p.read_rmt_name.rmt_name, sizeof(p.read_rmt_name.rmt_name), "%s", name);
    gap_evt(ESP_BT_GAP_READ_REMOTE_NAME_EVT, &p);
}

static bt_xrec_ui_t ui(void)
{
    bt_xrec_ui_t u;
    bt_xrec_get_ui(&u);
    return u;
}

/* The local user answers the pending prompt (after the arming delay), like ENTER / BACK on the BT screen. */
static int user_answer(int accept)
{
    m_now += BT_XREC_SSP_ARM_MS;
    int r = bt_xrec_ssp_answer(accept);
    bt_xrec_poll();
    return r;
}

static void add_bond_type(const uint8_t bda[6], int mitm)
{
    m_bond_mitm[m_nbonds] = (uint8_t)mitm;
    memcpy(m_bonds[m_nbonds++], bda, 6u);
}
static void add_bond(const uint8_t bda[6]) { add_bond_type(bda, 1); }

static bool wire_has(const char *needle)
{
    size_t n = strlen(needle);
    for (uint32_t i = 0; i + n <= m_wire_len; i++) {
        if (memcmp(&m_wire[i], needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static void wire_clear(void) { m_wire_len = 0u; }

static bt_xrec_debug_t dbg(void)
{
    bt_xrec_debug_t d;
    bt_xrec_get_debug(&d);
    return d;
}

static bt_status_t status(void)
{
    bt_status_t st;
    bt_xrec_get_status(&st);
    return st;
}

static void mocks_reset(void)
{
    bt_xrec_test_reset();
    m_now = 1000u;
    m_allocs = m_frees = 0;
    m_lock_depth = m_lock_max = m_notifies = 0;
    m_alloc_fail = 0;
    m_start_allowed = 1;
    m_entropy_fail = 0;
    m_pair_block = NULL;
    m_pair_block_calls = 0;
    m_up_calls = m_down_calls = m_hold_up = m_hold_down = m_up_result = m_down_result = m_job_kind = 0;
    m_job_pending = 0;
    m_dl_latched = m_hard_reason = m_service = m_session_end = m_reboot_mode = m_saved_key = m_poll_fault = 0;
    m_peer_mtu = 660u;
    m_gap_cb = NULL;
    m_spp_cb = NULL;
    m_name[0] = '\0';
    memset(&m_cod, 0, sizeof(m_cod));
    m_iocap = 0xFF;
    memset(&m_spp_cfg, 0, sizeof(m_spp_cfg));
    memset(&m_srv_cfg, 0, sizeof(m_srv_cfg));
    m_srv_name[0] = '\0';
    m_scan_calls = 0;
    m_scan_c = m_scan_d = -1;
    m_notes_n = 0;
    m_ctrl_name_ok = 0;
    m_last_log[0] = '\0';
    m_cfm_logged = 0xFFFFFFFFu;
    m_cfm_calls = m_pin_calls = m_passkey_calls = m_remove_calls = m_disc_calls = 0;
    m_cfm_accept = m_pin_accept = m_passkey_accept = -1;
    memset(m_cfm_bda, 0, sizeof(m_cfm_bda));
    m_name_calls = 0;
    memset(m_name_bda, 0, sizeof(m_name_bda));
    m_disc_handle = 0u;
    m_spp_init_ret = m_srv_ret = ESP_OK;
    m_nbonds = m_purges = 0;
    m_have_key = 0;
    m_free_dirty = 0;
    memset(m_blk, 0, sizeof(m_blk));
    m_wire_len = m_btc_used = 0u;
    m_btc_cap = 100000u;
    m_writes = m_upper_cong = m_write_calls_after_stop = 0;
    m_write_err = ESP_OK;
    m_stopped = 0;
    for (uint32_t i = 0; i < sizeof(m_logbuf); i++) {
        m_logbuf[i] = (uint8_t)('A' + (i * 7u) % 26u);
    }
}

/* start -> up job done -> GAP/SPP config -> INIT_EVT -> START_EVT -> READY */
static void bring_up(void)
{
    CHECK(bt_xrec_start(NULL, 0) == 0, "start");
    bt_xrec_poll();
    CHECK(m_spp_cb != NULL && m_gap_cb != NULL, "callbacks registered after the up job");
    esp_spp_cb_param_t p;
    memset(&p, 0, sizeof(p));
    p.init.status = ESP_SPP_SUCCESS;
    spp_evt(ESP_SPP_INIT_EVT, &p);
    bt_xrec_poll();
    memset(&p, 0, sizeof(p));
    p.start.status = ESP_SPP_SUCCESS;
    p.start.scn = 1u;
    spp_evt(ESP_SPP_START_EVT, &p);
    bt_xrec_poll();
    CHECK(dbg().state == BT_XREC_ST_READY, "READY (state %u)", dbg().state);
}

static void stop_ok(void)
{
    m_stopped = 1;
    bt_xrec_stop();
    CHECK(dbg().state == BT_XREC_ST_OFF, "OFF after stop (state %u)", dbg().state);
    CHECK(m_allocs == m_frees, "session buffers freed (%d allocs, %d frees)", m_allocs, m_frees);
    CHECK(m_free_dirty == 0, "session block returned to the heap with %d non-zero block(s) (key / PAIR line)",
          m_free_dirty);
}

/* Numeric comparison inside the window, MITM bond, SPP open, XREC PAIR -> authenticated session. */
static void pair_and_auth(void)
{
    bt_xrec_open_pairing(60000u);
    cfm_req(PEER, 123456u);
    bt_xrec_poll();
    CHECK(ui().prompt && user_answer(1) == 0 && m_cfm_accept == 1, "user confirmed the numeric comparison");
    auth_cmpl(PEER, ESP_BT_STATUS_SUCCESS, ESP_BT_LINK_KEY_AUTHED_COMB_P192);
    add_bond(PEER);
    srv_open(H1, PEER);
    bt_xrec_poll();
    bt_status_t st = status();
    CHECK(st.pair_active && strlen(st.pair_pin) == 6u, "XREC PIN shown after SSP (pin '%s')", st.pair_pin);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "+XREC:PAIR,%s,%s\r\n", st.pair_pin,
             "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    rx(H1, cmd);
    CHECK(wire_has("+XREC:OK,PAIR"), "PAIR accepted");
    CHECK(status().xrec_auth == 1u, "XREC authenticated");
    CHECK(m_saved_key == 1, "bound key saved");
    wire_clear();
}

/* =============================================== scenarios =============================================== */
static void t_bring_up_config(void)
{
    printf("[1] bring-up and configuration (legacy naming, CoD, SPP channel 1, DisplayYesNo)\n");
    mocks_reset();
    CHECK(bt_xrec_start(NULL, 0) == 0, "start");
    CHECK(m_up_calls == 1, "up job posted");
    bt_status_t st = status();
    CHECK(st.active == 1u && st.hci_ready == 0u, "active, not ready yet");
    CHECK(strcmp(st.dev_name, "Cassini_1D") == 0, "legacy device name scheme, got '%s'", st.dev_name);
    bt_xrec_poll();
    CHECK(strcmp(m_name, "Cassini_1D") == 0, "GAP name '%s'", m_name);
    CHECK(m_cod.major == ESP_BT_COD_MAJOR_DEV_AV && m_cod.minor == 1u && m_cod.service == ESP_BT_COD_SRVC_AUDIO,
          "class of device 0x200404");
    CHECK(m_iocap == ESP_BT_IO_CAP_IO, "IO capability DisplayYesNo");
    CHECK(m_spp_cfg.mode == ESP_SPP_MODE_CB && !m_spp_cfg.enable_l2cap_ertm &&
          m_spp_cfg.tx_buffer_size == BT_XREC_SPP_TX_BUF, "SPP CB mode, basic mode, tx buffer");
    esp_spp_cb_param_t p;
    memset(&p, 0, sizeof(p));
    p.init.status = ESP_SPP_SUCCESS;
    spp_evt(ESP_SPP_INIT_EVT, &p);
    bt_xrec_poll();
    CHECK(m_srv_cfg.local_scn == 1u && m_srv_cfg.create_spp_record &&
          m_srv_cfg.sec_mask == (ESP_SPP_SEC_AUTHENTICATE | ESP_SPP_SEC_ENCRYPT | ESP_SPP_SEC_MITM) &&
          m_srv_cfg.role == ESP_SPP_ROLE_SLAVE && strcmp(m_srv_name, "Cassini") == 0,
          "SPP server: ch 1, SDP record, auth+encrypt+MITM, 'Cassini'");
    CHECK(m_scan_calls == 0, "not connectable before the server listens");
    memset(&p, 0, sizeof(p));
    p.start.status = ESP_SPP_SUCCESS;
    p.start.scn = 1u;
    spp_evt(ESP_SPP_START_EVT, &p);
    bt_xrec_poll();
    st = status();
    CHECK(st.hci_ready == 1u, "ready");
    CHECK(m_scan_c == ESP_BT_CONNECTABLE && m_scan_d == ESP_BT_NON_DISCOVERABLE,
          "connectable, not discoverable outside the window");
    CHECK(st.irq_count == 4242u, "irq_count from the platform");
    CHECK(m_lock_max == 1, "lock never nested");
    stop_ok();
    CHECK(m_session_end == 1, "session end (bond store flush)");
}

static void t_pairing_and_data(void)
{
    printf("[2] pairing window, numeric comparison, XREC PAIR, data path, MTU\n");
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    bt_xrec_poll();
    CHECK(m_scan_d == ESP_BT_GENERAL_DISCOVERABLE, "discoverable while the window is open");
    bt_status_t st = status();
    CHECK(st.pair_active && strlen(st.pair_pin) == 6u, "XREC PIN shown");
    cfm_req(PEER, 42u);
    bt_xrec_poll();
    CHECK(m_cfm_calls == 0, "numeric comparison inside the window waits for the user");
    bt_xrec_ui_t u = ui();
    CHECK(u.prompt && strcmp(u.digits, "000042") == 0 && memcmp(u.peer_bda, PEER, 6u) == 0,
          "prompt shows the value and the peer (prompt %u, '%s')", u.prompt, u.digits);
    CHECK(strcmp(u.peer, "11:22:33:44:55:66") == 0 && !u.peer_is_name, "peer address while no name is known, got '%s'",
          u.peer);
    CHECK(m_name_calls == 1 && memcmp(m_name_bda, PEER, 6u) == 0, "remote name requested for the prompt");
    remote_name(PEER, "laptop-A");
    CHECK(strcmp(ui().peer, "laptop-A") == 0 && ui().peer_is_name, "remote name shown once Bluedroid has it, got '%s'",
          ui().peer);
    st = status();
    CHECK(strcmp(st.pair_pin, "000042") != 0 && strlen(st.pair_pin) == 6u, "bt_status_t keeps the XREC PIN");
    CHECK(user_answer(1) == 0 && m_cfm_calls == 1 && m_cfm_accept == 1 && memcmp(m_cfm_bda, PEER, 6u) == 0,
          "ENTER: numeric comparison accepted");
    CHECK(!ui().prompt && dbg().ssp_accepted == 1u && dbg().ssp_user_accepted == 1u, "prompt closed after the answer");
    auth_cmpl(PEER, ESP_BT_STATUS_SUCCESS, ESP_BT_LINK_KEY_AUTHED_COMB_P256);
    add_bond(PEER);
    bt_xrec_poll();
    u = ui();
    CHECK(u.bonded && strcmp(u.peer, "laptop-A") == 0, "UI: bonded with laptop-A (bonded %u, '%s')", u.bonded, u.peer);
    CHECK(m_remove_calls == 0, "MITM bond kept");
    /* one bond per window (review finding): discoverability and numeric comparison end with the first MITM bond, the
     * XREC PIN stays for the phone's PAIR */
    CHECK(st.pair_active && m_scan_d == ESP_BT_NON_DISCOVERABLE && !dbg().ssp_window,
          "SSP window closed after the first bond (discoverable %d, ssp_window %u)", m_scan_d, dbg().ssp_window);
    cfm_req(PEER2, 77u);
    bt_xrec_poll();
    CHECK(m_cfm_calls == 2 && m_cfm_accept == 0 && !ui().prompt,
          "a second device's numeric comparison rejected in the same window, no prompt");
    auth_cmpl(PEER2, ESP_BT_STATUS_AUTH_FAILURE, 0u);
    bt_xrec_poll();
    srv_open(H1, PEER);
    bt_xrec_poll();
    CHECK(dbg().conn_authorized && dbg().mtu == 660u, "SPP open, authorized, mtu %u", dbg().mtu);
    u = ui();
    CHECK(u.connected && u.bonded && strcmp(u.peer, "laptop-A") == 0 && m_name_calls == 1,
          "UI: connected + bonded laptop-A, name not asked again (%d)", m_name_calls);
    CHECK(status().spp_open == 1u && status().acl_connected == 1u, "status spp_open/acl_connected");
    uint32_t rx0 = status().rx_acl_pkts;
    rx(H1, "+XREC:PING\r\n");
    CHECK(wire_has("+XREC:PONG,openfw_v5,v5,bound=0,auth=0,pair=1"), "PONG on the wire");
    CHECK(status().rx_acl_pkts == rx0 + 1u && status().tx_acl_pkts >= 1u, "activity counters");
    wire_clear();
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "+XREC:PAIR,%s,%s\r\n", st.pair_pin,
             "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    rx(H1, cmd);
    CHECK(wire_has("+XREC:OK,PAIR"), "PAIR ok");
    CHECK(status().xrec_bound && status().xrec_auth, "bound + auth");
    bt_xrec_poll();
    CHECK(!status().pair_active && m_scan_d == ESP_BT_NON_DISCOVERABLE, "PAIR closes the window + discoverability");
    wire_clear();
    /* a command split over three SPP frames */
    rx_raw(H1, "+XREC:", 6u);
    rx_raw(H1, "PI", 2u);
    rx(H1, "NG\r\n");
    CHECK(wire_has("+XREC:PONG,openfw_v5,v5,bound=1,auth=1,pair=0"), "split command parsed once");
    stop_ok();
}

static void t_policy(void)
{
    printf("[3] pairing policy: window, Just Works, legacy PIN, passkey, unauthenticated key\n");
    mocks_reset();
    bring_up();
    cfm_req(PEER, 7u);
    bt_xrec_poll();
    CHECK(m_cfm_calls == 1 && m_cfm_accept == 0, "numeric comparison rejected outside the window");
    CHECK(status().pair_pin[0] == '\0', "nothing shown outside the window");

    esp_bt_gap_cb_param_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.pin_req.bda, PEER, 6u);
    gap_evt(ESP_BT_GAP_PIN_REQ_EVT, &p);
    bt_xrec_poll();
    CHECK(m_pin_calls == 1 && m_pin_accept == 0, "legacy PIN pairing rejected");
    memset(&p, 0, sizeof(p));
    memcpy(p.key_req.bda, PEER, 6u);
    gap_evt(ESP_BT_GAP_KEY_REQ_EVT, &p);
    bt_xrec_poll();
    CHECK(m_passkey_calls == 1 && m_passkey_accept == 0, "passkey entry rejected");

    /* Just Works: btc_dm auto-confirms (no CFM_REQ reaches us); the unauthenticated key is removed */
    bt_xrec_open_pairing(60000u);
    auth_cmpl(PEER, ESP_BT_STATUS_SUCCESS, ESP_BT_LINK_KEY_UNAUTHED_COMB_P192);
    add_bond(PEER);   /* (Bluedroid stored it before AUTH_CMPL reached the app) */
    srv_open(H1, PEER);
    rx_raw(H1, "+XREC:PING\r\n", 12u);
    bt_xrec_poll();
    CHECK(m_remove_calls == 1 && m_nbonds == 0, "Just Works bond removed even inside the window");
    CHECK(m_disc_calls == 1 && m_disc_handle == H1, "SPP of the rejected link disconnected");
    CHECK(m_wire_len == 0u, "no XREC reply on a rejected link");
    CHECK(dbg().rx_dropped == 12u, "bytes of the rejected link dropped");
    spp_close(H1);
    esp_bt_gap_cb_param_t d;
    memset(&d, 0, sizeof(d));
    memcpy(d.acl_disconn_cmpl_stat.bda, PEER, 6u);
    gap_evt(ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT, &d);
    bt_xrec_poll();

    /* passkey display: no reply exists; the resulting (authenticated) bond is removed */
    memset(&p, 0, sizeof(p));
    memcpy(p.key_notif.bda, PEER2, 6u);
    p.key_notif.passkey = 999999u;
    gap_evt(ESP_BT_GAP_KEY_NOTIF_EVT, &p);
    auth_cmpl(PEER2, ESP_BT_STATUS_SUCCESS, ESP_BT_LINK_KEY_AUTHED_COMB_P192);
    bt_xrec_poll();
    CHECK(m_remove_calls == 2, "passkey bond removed");

    /* comparison accepted, but the key came out unauthenticated (peer said no MITM): removed */
    cfm_req(PEER, 5u);
    bt_xrec_poll();
    user_answer(1);
    auth_cmpl(PEER, ESP_BT_STATUS_SUCCESS, ESP_BT_LINK_KEY_UNAUTHED_COMB_P256);
    bt_xrec_poll();
    CHECK(m_cfm_accept == 1 && m_remove_calls == 3, "accepted comparison with an unauthenticated key removed");

    /* failed pairing: nothing removed */
    cfm_req(PEER, 6u);
    bt_xrec_poll();
    user_answer(1);
    auth_cmpl(PEER, ESP_BT_STATUS_AUTH_FAILURE, 0u);
    bt_xrec_poll();
    CHECK(m_remove_calls == 3, "failed pairing leaves no bond to remove");

    /* window expiry */
    m_now += 60001u;
    bt_xrec_poll();
    CHECK(!status().pair_active && m_scan_d == ESP_BT_NON_DISCOVERABLE, "window closes at its deadline");
    cfm_req(PEER, 8u);
    bt_xrec_poll();
    CHECK(m_cfm_accept == 0, "rejected again after expiry");
    stop_ok();
}

static void t_bonded_reconnect(void)
{
    printf("[4] bonded reconnect outside the window; second connection refused; unknown peer refused\n");
    mocks_reset();
    add_bond(PEER);
    bring_up();
    srv_open(H1, PEER);
    rx(H1, "+XREC:PING\r\n");
    CHECK(dbg().conn_authorized && wire_has("+XREC:PONG"), "bonded peer served without a window");
    srv_open(H2, PEER2);
    bt_xrec_poll();
    CHECK(m_disc_calls == 1 && m_disc_handle == H2 && dbg().conn_handle == H1, "second connection refused");
    spp_close(H1);
    bt_xrec_poll();
    CHECK(!dbg().conn_open, "closed");
    wire_clear();
    srv_open(H2, PEER2);   /* not bonded, never paired on this link */
    rx(H2, "+XREC:PING\r\n");
    CHECK(m_disc_calls == 2 && m_disc_handle == H2 && m_wire_len == 0u, "unbonded peer refused, no reply");
    spp_close(H2);
    bt_xrec_poll();
    /* a bond whose stored key is NOT MITM-authenticated (a Just Works key a stop / power loss / lost AUTH_CMPL left in
     * the store): bonded, but refused (review finding: is_bonded() alone authorized it) */
    add_bond_type(PEER2, 0);
    srv_open(H2, PEER2);
    rx(H2, "+XREC:PING\r\n");
    CHECK(m_disc_calls == 3 && m_wire_len == 0u && dbg().conns_authorized == 1u,
          "unauthenticated stored key refused (authorized %u)", dbg().conns_authorized);
    stop_ok();

    printf("    stale unauthenticated bond purged before the device is connectable\n");
    mocks_reset();
    add_bond(PEER);
    add_bond_type(PEER2, 0);
    bring_up();
    CHECK(m_nbonds == 1 && memcmp(m_bonds[0], PEER, 6u) == 0 && m_purges == 1 && dbg().bonds_removed == 1u,
          "only the MITM bond left (bonds %d, purges %d)", m_nbonds, m_purges);
    stop_ok();
}

static void t_rx_overflow(void)
{
    printf("[5] RX ring overflow closes the connection (no silent loss)\n");
    mocks_reset();
    add_bond(PEER);
    bring_up();
    srv_open(H1, PEER);
    bt_xrec_poll();
    static uint8_t junk[1000];
    memset(junk, 'x', sizeof(junk));

    /* Secondary (refused) SPP handle flooding DATA_IND before CLOSE_EVT must not starve or close H1 */
    srv_open(H2, PEER2);
    bt_xrec_poll();
    CHECK(m_disc_calls == 1 && m_disc_handle == H2, "H2 refused");
    rx_raw(H2, junk, sizeof(junk));
    rx_raw(H2, junk, sizeof(junk));
    rx_raw(H2, junk, sizeof(junk));
    rx(H1, "+XREC:PING\r\n");
    CHECK(dbg().conn_open && !dbg().conn_closing && dbg().rx_overflows == 0u && wire_has("+XREC:PONG"),
          "secondary handle DATA_IND flood must not overflow or close active session H1");
    spp_close(H2);
    bt_xrec_poll();
    wire_clear();

    /* Burst of >32 overflowing frames on H1 before poll must close H1 without overflowing evq into ST_FAULTED */
    rx_raw(H1, junk, sizeof(junk));
    rx_raw(H1, junk, sizeof(junk));
    rx_raw(H1, junk, sizeof(junk));   /* 3000 > 2048: the third frame overflows */
    for (int i = 0; i < 40; i++) {
        rx_raw(H1, junk, 100u);              /* 100 B frames: ring full, so the EVQ fault path is exercised */
    }
    bt_xrec_poll();
    CHECK(dbg().rx_overflows == 41u, "overflows counted (%u)", dbg().rx_overflows);
    CHECK(dbg().state == BT_XREC_ST_READY && dbg().evq_lost == 0u,
          "repeated RX overflows must coalesce instead of faulting EVQ (state %u, lost %u)",
          dbg().state, dbg().evq_lost);
    CHECK(m_disc_calls == 2 && m_disc_handle == H1 && dbg().conn_closing, "connection H1 closed");
    stop_ok();
}

/* Parse the captured stream as consecutive "+XREC:LOG,off,len,widx:<len bytes>" frames. */
static int check_log_frames(uint32_t first_off, int expect_frames)
{
    uint32_t i = 0u, off = first_off;
    int frames = 0;
    while (i < m_wire_len) {
        unsigned o, l, w;
        int hdr = 0;
        if (sscanf((const char *)&m_wire[i], "+XREC:LOG,%u,%u,%u:%n", &o, &l, &w, &hdr) != 3 || hdr == 0) {
            return -1;
        }
        if (o != off || i + (uint32_t)hdr + l > m_wire_len || memcmp(&m_wire[i + (uint32_t)hdr], &m_logbuf[o], l) != 0) {
            return -2;
        }
        i += (uint32_t)hdr + l;
        off += l;
        frames++;
    }
    return frames == expect_frames ? 0 : -3;
}

static void t_congestion_partial(void)
{
    printf("[6] congestion (ESP_ERR_NO_MEM / CONG_EVT), partial writes, in-flight window\n");
    mocks_reset();
    bring_up();
    pair_and_auth();
    /* (a) congestion: BTC takes 600 B, a LOG reply is ~540 B */
    m_btc_cap = 600u;
    rx(H1, "+XREC:LOG,0,512\r\n");
    int w1 = m_writes;
    rx(H1, "+XREC:LOG,512,512\r\n");
    bt_xrec_poll();
    CHECK(dbg().cong == 1u && dbg().tx_nomem == 1u, "NO_MEM -> congested");
    int w2 = m_writes;
    bt_xrec_poll();
    bt_xrec_poll();
    CHECK(m_writes == w2 && w2 == w1, "no esp_spp_write while congested");
    CHECK(dbg().tx_used > 0u, "app keeps the queued reply");
    btc_drain(H1, m_btc_used, 100u);   /* WRITE_EVTs + CONG_EVT(false) at the low watermark */
    bt_xrec_poll();
    CHECK(dbg().cong == 0u && dbg().tx_used == 0u, "resumed after CONG_EVT");
    btc_drain(H1, m_btc_used, 100u);
    bt_xrec_poll();
    CHECK(check_log_frames(0u, 2) == 0, "both replies on the wire, in order, intact (%d)", check_log_frames(0u, 2));
    CHECK(dbg().inflight == 0u, "in-flight accounting back to 0 (%u)", dbg().inflight);
    wire_clear();

    /* (b) in-flight window: BTC never reports, a reply is split at the window edge (partial write of a message) */
    m_btc_cap = 100000u;
    for (int k = 0; k < 4; k++) {
        char cmd[48];
        snprintf(cmd, sizeof(cmd), "+XREC:LOG,%u,512\r\n", (unsigned)(k * 512));
        rx(H1, cmd);
    }
    bt_xrec_debug_t d = dbg();
    CHECK(d.inflight == BT_XREC_INFLIGHT_MAX, "window full (%u)", d.inflight);
    CHECK(d.tx_used > 0u && d.tx_used < 600u, "tail of the 4th reply still queued (%u)", d.tx_used);
    btc_drain(H1, 300u, 77u);   /* odd chunk sizes */
    bt_xrec_poll();
    d = dbg();
    CHECK(d.tx_used == 0u, "rest written after WRITE_EVT (%u left)", d.tx_used);
    btc_drain(H1, m_btc_used, 1000u);
    bt_xrec_poll();
    CHECK(check_log_frames(0u, 4) == 0, "4 replies intact across the split (%d)", check_log_frames(0u, 4));
    CHECK(dbg().inflight == 0u && dbg().write_evt_bytes == dbg().tx_bytes, "every byte reported once");
    /* stale WRITE_EVT for another handle is ignored */
    esp_spp_cb_param_t p;
    memset(&p, 0, sizeof(p));
    p.write.status = ESP_SPP_SUCCESS;
    p.write.handle = H2;
    p.write.len = 10;
    spp_evt(ESP_SPP_WRITE_EVT, &p);
    bt_xrec_poll();
    CHECK(dbg().stale_events == 1u, "stale WRITE_EVT counted, not applied");
    stop_ok();
}

static void t_write_error(void)
{
    printf("[7] esp_spp_write error closes the link and clears XREC authorization\n");
    mocks_reset();
    bring_up();
    pair_and_auth();
    m_write_err = ESP_ERR_INVALID_STATE;
    rx(H1, "+XREC:PING\r\n");
    CHECK(dbg().tx_errors == 1u && dbg().conn_closing && m_disc_calls == 1, "closed after a write error");
    CHECK(status().xrec_auth == 0u, "authorization cleared");
    int w = m_writes;
    m_write_err = ESP_OK;
    rx(H1, "+XREC:PING\r\n");
    CHECK(m_writes == w, "nothing more written on a closing link");
    stop_ok();

    printf("    write error inside a frame: the rest of the frame runs unauthenticated\n");
    mocks_reset();
    bring_up();
    pair_and_auth();
    m_write_err = ESP_ERR_INVALID_STATE;
    rx(H1, "+XREC:PING\r\n+XREC:REBOOT\r\n");
    CHECK(dbg().reboot_mode == 0u, "REBOOT after the failed write was refused (auth cleared at once)");
    stop_ok();
}

static void t_disconnect_mid_transfer(void)
{
    printf("[8] disconnect mid-transfer cancels queued work; new session starts clean\n");
    mocks_reset();
    bring_up();
    pair_and_auth();
    m_btc_cap = 300u;
    rx(H1, "+XREC:LOG,0,512\r\n");
    CHECK(dbg().cong && dbg().tx_used > 0u, "congested with a queued reply");
    spp_close(H1);
    rx_raw(H1, "+XREC:PING\r\n", 12u);   /* late data of the old link (after CLOSE in order) */
    bt_xrec_poll();
    bt_xrec_debug_t d = dbg();
    CHECK(!d.conn_open && d.tx_used == 0u && d.inflight == 0u && !d.cong, "queue dropped, state reset");
    CHECK(status().xrec_auth == 0u, "authorization cleared");
    CHECK(d.rx_dropped == 12u, "late bytes of the old link dropped");
    /* BTC reports what it still had: stale for the closed handle */
    m_btc_used = 0u;
    m_btc_cap = 100000u;
    m_upper_cong = 0;
    wire_clear();
    srv_open(H2, PEER);   /* reconnect (bonded) */
    rx(H2, "+XREC:PING\r\n");
    CHECK(wire_has("+XREC:PONG,openfw_v5,v5,bound=1,auth=0"), "fresh session, not authenticated");
    CHECK(!wire_has("+XREC:LOG"), "no byte of the old session leaked into the new one");
    stop_ok();
}

static void t_stop_during_transfer(void)
{
    printf("[9] stop during transfer: coordinated shutdown, no writes after, late callbacks ignored\n");
    mocks_reset();
    bring_up();
    pair_and_auth();
    m_btc_cap = 300u;
    rx(H1, "+XREC:LOG,0,512\r\n");
    CHECK(dbg().tx_used > 0u, "queued");
    int n0 = m_notifies;
    stop_ok();
    CHECK(m_down_calls == 1 && m_session_end == 1, "down job + store flush");
    bt_status_t st = status();
    CHECK(!st.active && !st.spp_open && !st.xrec_auth, "status inactive");
    btc_drain(H1, m_btc_used, 100u);   /* late BTC callbacks */
    rx_raw(H1, "+XREC:PING\r\n", 12u);
    bt_xrec_poll();
    CHECK(m_notifies == n0 && m_write_calls_after_stop == 0, "late callbacks ignored, nothing written");
    bt_xrec_stop();   /* idempotent */
    CHECK(m_down_calls == 1, "second stop is a no-op");
    /* restart works */
    m_stopped = 0;
    bring_up();
    stop_ok();
}

static void t_deferred_reboot(void)
{
    printf("[10] deferred reboot waits for the final reply (congested), then shuts down first\n");
    mocks_reset();
    bring_up();
    pair_and_auth();
    m_btc_cap = 10u;   /* the OK,REBOOT reply cannot leave yet */
    rx(H1, "+XREC:REBOOT\r\n");
    CHECK(dbg().reboot_mode == 1u && dbg().tx_used > 0u, "reboot deferred, reply queued");
    m_now += 300u;
    bt_xrec_poll();
    CHECK(m_reboot_mode == 0, "not before the reply left");
    m_btc_cap = 100000u;
    btc_drain(H1, m_btc_used, 100u);
    m_upper_cong = 0;
    esp_spp_cb_param_t p;
    memset(&p, 0, sizeof(p));
    p.cong.handle = H1;
    spp_evt(ESP_SPP_CONG_EVT, &p);
    bt_xrec_poll();      /* reply written */
    CHECK(wire_has("+XREC:OK,REBOOT"), "reply on the wire");
    btc_drain(H1, m_btc_used, 1000u);
    bt_xrec_poll();
    CHECK(m_reboot_mode == 1 && m_down_calls == 1 && dbg().state == BT_XREC_ST_OFF, "shutdown, then reboot");
    CHECK(m_allocs == m_frees, "freed");

    printf("    reply never drains: reboot after the drain cap\n");
    mocks_reset();
    bring_up();
    pair_and_auth();
    m_btc_cap = 10u;
    rx(H1, "+XREC:DFU\r\n");
    CHECK(dbg().reboot_mode == 3u, "DFU reboot deferred");
    m_now += BT_XREC_REBOOT_DELAY_MS + 100u;
    bt_xrec_poll();
    CHECK(m_reboot_mode == 0, "still waiting");
    m_now += BT_XREC_REBOOT_DRAIN_MAX_MS;
    bt_xrec_poll();
    CHECK(m_reboot_mode == 3 && m_down_calls == 1, "rebooted to DFU after the cap, through bt_shutdown");
}

static void t_entropy_fail_closed(void)
{
    printf("[11] entropy not ready: pairing window refused (plan M2.3), XREC PIN refused, CHAL refused, link closed\n");
    mocks_reset();
    add_bond(PEER);
    bring_up();
    m_entropy_fail = 1;
    bt_xrec_open_pairing(60000u);
    bt_xrec_poll();
    CHECK(!status().pair_active && status().pair_pin[0] == '\0' && !dbg().ssp_window && !dbg().window_open,
          "no window at all without entropy (pair_active %u, ssp_window %u)", status().pair_active, dbg().ssp_window);
    CHECK(m_scan_d != ESP_BT_GENERAL_DISCOVERABLE, "not discoverable without entropy");
    cfm_req(PEER2, 4242u);
    bt_xrec_poll();
    CHECK(m_cfm_calls == 1 && m_cfm_accept == 0 && !ui().prompt, "numeric comparison rejected, no prompt");
    srv_open(H1, PEER);
    rx(H1, "+XREC:PAIR,000000,000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\r\n");
    CHECK(wire_has("+XREC:ERR,PAIRCLOSED"), "XREC PAIR closed without a PIN");
    wire_clear();
    rx(H1, "+XREC:CHAL\r\n");
    CHECK(!wire_has("+XREC:CHAL"), "no challenge without entropy");
    CHECK(m_disc_calls == 1 && dbg().conn_closing, "link closed (fail closed)");
    stop_ok();

    /* review finding: CHAL without entropy armed a challenge over an all-zero nonce, and an AUTH with HMAC(key, 0^16)
     * in the SAME SPP write was accepted before the next poll closed the link: the privileged command after it ran */
    printf("    CHAL + AUTH(HMAC over the zero nonce) + REBOOT in one write, entropy unavailable\n");
    mocks_reset();
    m_have_key = 1;
    add_bond(PEER);
    bring_up();
    m_entropy_fail = 1;
    srv_open(H1, PEER);
    bt_xrec_poll();
    CHECK(status().xrec_bound == 1u, "bound key loaded");
    uint8_t zero[16] = { 0 }, mac[32];
    openfw_hmac_sha256(m_key, 32u, zero, 16u, mac);
    char line[200] = "+XREC:CHAL\r\n+XREC:AUTH,";
    size_t o = strlen(line);
    for (int i = 0; i < 32; i++) {
        o += (size_t)snprintf(&line[o], sizeof(line) - o, "%02x", mac[i]);
    }
    snprintf(&line[o], sizeof(line) - o, "\r\n+XREC:REBOOT\r\n");
    CHECK(strlen(line) <= 128u, "one 128-byte drain chunk (%zu)", strlen(line));
    rx(H1, line);
    for (int i = 0; i < 5; i++) {
        m_now += 100u;
        bt_xrec_poll();
    }
    CHECK(status().xrec_auth == 0u && dbg().reboot_mode == 0u && m_reboot_mode == 0 && m_down_calls == 0,
          "zero-nonce AUTH not accepted, REBOOT not run (auth %u, reboot %u/%d)", status().xrec_auth,
          dbg().reboot_mode, m_reboot_mode);
    CHECK(m_disc_calls == 1, "link closed");
    stop_ok();
}

static void t_evq_overflow_and_faults(void)
{
    printf("[12] event-queue overflow and platform faults -> graceful recovery, BT off until restart\n");
    mocks_reset();
    bring_up();
    for (int i = 0; i < 40; i++) {
        cfm_req(PEER, (uint32_t)i);
    }
    bt_xrec_poll();
    CHECK(dbg().state == BT_XREC_ST_FAULTED && dbg().last_fault == BT_XREC_FAULT_EVQ_OVERFLOW, "faulted (state %u)",
          dbg().state);
    CHECK(m_down_calls == 1 && m_hard_reason == 0 && m_allocs == m_frees, "graceful teardown");
    CHECK(!status().active, "inactive");
    bring_up();
    m_poll_fault = 2;
    bt_xrec_poll();
    CHECK(dbg().state == BT_XREC_ST_FAULTED && dbg().last_fault == BT_XREC_FAULT_PLATFORM, "platform fault");
    m_poll_fault = 0;

    printf("    start refused / no memory\n");
    mocks_reset();
    m_start_allowed = 0;
    CHECK(bt_xrec_start(NULL, 0) == -1 && m_up_calls == 0, "refused after a BT-fault reboot");
    m_start_allowed = 1;
    m_alloc_fail = 1;
    CHECK(bt_xrec_start(NULL, 0) == -1 && m_up_calls == 0, "no memory: fail soft");
    m_alloc_fail = 0;

    printf("    bring-up failure / timeout, configuration failures / timeout\n");
    mocks_reset();
    m_up_result = 4;
    CHECK(bt_xrec_start(NULL, 0) == 0, "start");
    bt_xrec_poll();
    CHECK(dbg().state == BT_XREC_ST_FAULTED && dbg().last_fault == BT_XREC_FAULT_UP_FAIL, "up failure");
    mocks_reset();
    m_hold_up = 1;
    CHECK(bt_xrec_start(NULL, 0) == 0, "start");
    m_now += BT_XREC_UP_DEADLINE_MS;
    m_hold_up = 0;   /* the worker finishes "up", then runs "down" */
    m_job_pending = 0;
    bt_xrec_poll();
    CHECK(dbg().last_fault == BT_XREC_FAULT_UP_TIMEOUT && dbg().state == BT_XREC_ST_FAULTED, "up timeout");
    mocks_reset();
    m_spp_init_ret = ESP_FAIL;
    CHECK(bt_xrec_start(NULL, 0) == 0, "start");
    bt_xrec_poll();
    CHECK(dbg().last_fault == BT_XREC_FAULT_CONFIG && dbg().state == BT_XREC_ST_FAULTED, "SPP init call failed");
    mocks_reset();
    CHECK(bt_xrec_start(NULL, 0) == 0, "start");
    bt_xrec_poll();
    esp_spp_cb_param_t p;
    memset(&p, 0, sizeof(p));
    p.init.status = ESP_SPP_SUCCESS;
    spp_evt(ESP_SPP_INIT_EVT, &p);
    bt_xrec_poll();
    memset(&p, 0, sizeof(p));
    p.start.status = ESP_SPP_SUCCESS;
    p.start.scn = 3u;
    spp_evt(ESP_SPP_START_EVT, &p);
    bt_xrec_poll();
    CHECK(dbg().last_fault == BT_XREC_FAULT_CONFIG, "server on the wrong channel is a fault");
    mocks_reset();
    CHECK(bt_xrec_start(NULL, 0) == 0, "start");
    bt_xrec_poll();
    m_now += BT_XREC_CONFIG_DEADLINE_MS;
    bt_xrec_poll();
    CHECK(dbg().last_fault == BT_XREC_FAULT_CONFIG_TIMEOUT, "configuration timeout");
}

static void t_hard_paths(void)
{
    printf("[13] shutdown deadline and latched deadline take the hard path\n");
    mocks_reset();
    bring_up();
    pair_and_auth();
    m_hold_down = 1;   /* the worker blocks forever inside Bluedroid */
    uint32_t t0 = m_now;
    bt_xrec_stop();
    CHECK(m_hard_reason == BT_XREC_FAULT_DOWN_DEADLINE, "deadline -> hard path (%d)", m_hard_reason);
    CHECK(m_now - t0 >= BT_XREC_SHUTDOWN_DEADLINE_MS && m_now - t0 <= BT_XREC_SHUTDOWN_DEADLINE_MS + 2u,
          "one absolute 2 s deadline (%u ms)", (unsigned)(m_now - t0));
    CHECK(m_service >= (int)BT_XREC_SHUTDOWN_DEADLINE_MS - 2, "recorder serviced while waiting (%d)", m_service);
    CHECK(dbg().state == BT_XREC_ST_DEAD && m_allocs == m_frees, "dead, buffers freed");
    CHECK(bt_xrec_start(NULL, 0) == -1, "no restart after the hard path");

    mocks_reset();
    bring_up();
    m_dl_latched = 1;  /* teardown finished, but the deadline had latched bt_stop */
    bt_xrec_stop();
    CHECK(m_hard_reason == BT_XREC_FAULT_DOWN_LATCHED, "latched -> hard path (%d)", m_hard_reason);

    mocks_reset();
    bring_up();
    m_down_result = 3;
    bt_xrec_stop();
    CHECK(m_hard_reason == BT_XREC_FAULT_DOWN_FAIL, "teardown error -> hard path (%d)", m_hard_reason);
}

static void t_ssp_prompt(void)
{
    printf("[14] local numeric-comparison prompt: accept, reject, timeout, window closing, peer gone, second request\n");
    mocks_reset();
    bring_up();
    bt_xrec_ui_t u = ui();
    CHECK(!u.prompt && !u.window_open && !u.connected && u.peer[0] == '\0', "idle UI state");
    cfm_req(PEER, 1u);
    bt_xrec_poll();
    CHECK(m_cfm_calls == 1 && m_cfm_accept == 0 && !ui().prompt && m_name_calls == 0,
          "outside the window: rejected without a prompt, no name request");
    bt_xrec_open_pairing(60000u);
    bt_xrec_poll();
    u = ui();
    CHECK(u.window_open && u.window_s_left == 60u, "window open, %u s left", u.window_s_left);
    m_now += 15000u;
    CHECK(ui().window_s_left == 45u, "window countdown (%u s)", ui().window_s_left);

    printf("    accept (ENTER), with the arming delay\n");
    m_cfm_calls = 0;
    cfm_req(PEER, 654321u);
    bt_xrec_poll();
    u = ui();
    CHECK(u.prompt && !u.prompt_armed && u.prompt_s_left == 30u && strcmp(u.digits, "654321") == 0 && !u.bonded,
          "prompt up: armed %u, %u s, '%s'", u.prompt_armed, u.prompt_s_left, u.digits);
    CHECK(dbg().ssp_prompt == 1u, "debug: prompt pending");
    m_now += 100u;
    CHECK(bt_xrec_ssp_answer(1) == -2 && m_cfm_calls == 0 && ui().prompt, "a key in the first 0.5 s is no answer");
    m_now += BT_XREC_SSP_ARM_MS;
    CHECK(ui().prompt_armed, "armed after 0.5 s");
    CHECK(bt_xrec_ssp_answer(1) == 0 && m_cfm_calls == 1 && m_cfm_accept == 1, "accepted");
    CHECK(bt_xrec_ssp_answer(1) == -3 && m_cfm_calls == 1, "a second answer is swallowed (not 'open a window')");
    m_now += BT_XREC_SSP_AFTER_MS;
    CHECK(bt_xrec_ssp_answer(1) == -1 && m_cfm_calls == 1, "1 s after the answer a key is a normal key again");
    auth_cmpl(PEER, ESP_BT_STATUS_SUCCESS, ESP_BT_LINK_KEY_AUTHED_COMB_P256);
    add_bond(PEER);
    bt_xrec_poll();
    CHECK(m_remove_calls == 0 && dbg().ssp_user_accepted == 1u && ui().bonded, "MITM bond kept, UI bonded");
    stop_ok();

    printf("    reject (BACK)\n");
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    cfm_req(PEER, 111111u);
    bt_xrec_poll();
    CHECK(user_answer(0) == 0 && m_cfm_calls == 1 && m_cfm_accept == 0 && memcmp(m_cfm_bda, PEER, 6u) == 0,
          "BACK: rejected");
    CHECK(!ui().prompt && dbg().ssp_user_rejected == 1u && dbg().ssp_rejected == 1u && dbg().ssp_accepted == 0u,
          "counted as a user rejection");
    auth_cmpl(PEER, ESP_BT_STATUS_AUTH_FAILURE, 0u);
    bt_xrec_poll();
    CHECK(m_remove_calls == 0 && m_nbonds == 0, "no bond");
    /* a key that came out of a rejected comparison anyway (cannot happen with a conforming controller) is removed */
    auth_cmpl(PEER, ESP_BT_STATUS_SUCCESS, ESP_BT_LINK_KEY_AUTHED_COMB_P192);
    bt_xrec_poll();
    CHECK(m_remove_calls == 1 && dbg().ssp_window, "unconfirmed MITM key removed; window still open");
    stop_ok();

    printf("    timeout (30 s without an answer)\n");
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    cfm_req(PEER, 222222u);
    bt_xrec_poll();
    m_now += 10200u;
    bt_xrec_poll();
    CHECK(ui().prompt && ui().prompt_s_left == 20u, "countdown (%u s left)", ui().prompt_s_left);
    m_now += BT_XREC_SSP_PROMPT_MS - 10200u - 1u;
    bt_xrec_poll();
    CHECK(ui().prompt && m_cfm_calls == 0, "still waiting 1 ms before the timeout");
    m_now += 1u;
    bt_xrec_poll();
    CHECK(!ui().prompt && m_cfm_calls == 1 && m_cfm_accept == 0 && dbg().ssp_timeouts == 1u, "rejected at 30 s");
    CHECK(bt_xrec_ssp_answer(1) == -3 && m_cfm_calls == 1, "a late ENTER is swallowed, nothing sent");
    CHECK(bt_xrec_ssp_answer(0) == -3 && m_cfm_calls == 1, "a late BACK is swallowed (does not leave BT mode)");
    m_now += BT_XREC_SSP_AFTER_MS - 1u;
    CHECK(bt_xrec_ssp_answer(0) == -3, "still swallowed 1 ms before the end of the guard");
    m_now += 1u;
    CHECK(bt_xrec_ssp_answer(0) == -1, "after 1 s BACK is BACK again");
    stop_ok();

    printf("    window closing mid-prompt\n");
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(20000u);
    m_now += 15000u;
    cfm_req(PEER, 333333u);
    bt_xrec_poll();
    CHECK(ui().prompt, "prompt 5 s before the window ends");
    m_now += 5000u;
    bt_xrec_poll();
    CHECK(!ui().prompt && !ui().window_open && m_cfm_calls == 1 && m_cfm_accept == 0 && dbg().ssp_timeouts == 0u,
          "rejected when the window closed");
    CHECK(m_scan_d == ESP_BT_NON_DISCOVERABLE, "not discoverable");
    CHECK(bt_xrec_ssp_answer(1) == -3 && m_cfm_calls == 1, "ENTER right after the window closed is swallowed");
    stop_ok();

    printf("    peer disconnect / peer gives up mid-prompt\n");
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    cfm_req(PEER, 444444u);
    bt_xrec_poll();
    acl_disc(PEER2);
    bt_xrec_poll();
    CHECK(ui().prompt, "another peer's link going down leaves the prompt");
    acl_disc(PEER);
    bt_xrec_poll();
    CHECK(!ui().prompt && m_cfm_calls == 0 && dbg().ssp_withdrawn == 1u, "withdrawn without a reply");
    CHECK(user_answer(0) == -3 && m_cfm_calls == 0, "BACK 0.5 s after the peer left: swallowed, nothing sent");
    cfm_req(PEER, 555555u);
    bt_xrec_poll();
    auth_cmpl(PEER, ESP_BT_STATUS_AUTH_FAILURE, 0u);   /* the phone's user said no / LMP response timeout */
    bt_xrec_poll();
    CHECK(!ui().prompt && m_cfm_calls == 0 && dbg().ssp_withdrawn == 2u && m_remove_calls == 0,
          "withdrawn when the peer's pairing failed");
    stop_ok();

    printf("    second request while one is pending\n");
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    cfm_req(PEER, 123123u);
    bt_xrec_poll();
    m_now += 2000u;
    cfm_req(PEER2, 999999u);
    bt_xrec_poll();
    u = ui();
    CHECK(m_cfm_calls == 1 && m_cfm_accept == 0 && memcmp(m_cfm_bda, PEER2, 6u) == 0, "the newcomer is rejected");
    CHECK(u.prompt && strcmp(u.digits, "123123") == 0 && memcmp(u.peer_bda, PEER, 6u) == 0 && u.prompt_s_left == 28u,
          "the pending prompt is unchanged ('%s', %u s)", u.digits, u.prompt_s_left);
    CHECK(user_answer(1) == 0 && m_cfm_calls == 2 && m_cfm_accept == 1 && memcmp(m_cfm_bda, PEER, 6u) == 0,
          "the first one is accepted");
    auth_cmpl(PEER, ESP_BT_STATUS_SUCCESS, ESP_BT_LINK_KEY_AUTHED_COMB_P192);
    add_bond(PEER);
    bt_xrec_poll();
    CHECK(!dbg().ssp_window && !dbg().ssp_burned, "first one bonded: SSP part closed, key not spent");
    bt_xrec_open_pairing(60000u);
    bt_xrec_poll();
    cfm_req(PEER, 1u);
    bt_xrec_poll();
    cfm_req(PEER, 2u);                                  /* the same peer again: its first pairing is gone */
    bt_xrec_poll();
    CHECK(!ui().prompt && m_cfm_calls == 3 && m_cfm_accept == 0 && dbg().ssp_withdrawn == 1u,
          "same peer again: rejected, stale prompt withdrawn");
    stop_ok();

    printf("    BT stop with a prompt up; name handling\n");
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    cfm_req(PEER, 7u);
    bt_xrec_poll();
    remote_name(PEER, "Pixel\x01 7");
    CHECK(strcmp(ui().peer, "Pixel? 7") == 0, "control bytes replaced, got '%s'", ui().peer);
    remote_name(PEER2, "other");
    CHECK(strcmp(ui().peer, "11:22:33:44:55:66") == 0, "another peer's name is not shown, got '%s'", ui().peer);
    remote_name(PEER, "ABCDEFGHIJKLMNOPQRSTUV\xC3\xA9xyz");   /* 22 ASCII + 'e acute' (2 bytes): cut inside it */
    CHECK(strcmp(ui().peer, "ABCDEFGHIJKLMNOPQRSTUV") == 0, "cut at a character boundary, got '%s'", ui().peer);
    remote_name(PEER, "ABCDEFGHIJKLMNOPQRSTU\xC3\xA9xyz");    /* 21 + 2 = 23 bytes: fits */
    CHECK(strcmp(ui().peer, "ABCDEFGHIJKLMNOPQRSTU\xC3\xA9") == 0, "whole character kept, got '%s'", ui().peer);
    stop_ok();
    CHECK(!ui().prompt && m_cfm_calls == 0 && dbg().ssp_withdrawn == 1u, "stop withdraws the prompt");
    m_stopped = 0;
    bring_up();
    CHECK(!ui().prompt && ui().peer[0] == '\0', "nothing left after a restart");
    stop_ok();
}

static void t_pairing_gate(void)
{
    printf("[16] plan M2.3 pairing gate: SSP part refused / closed while the platform cannot back a pairing\n");
    /* entropy credited but the ROM key was not replaced (e.g. collection deferred at BT start): XREC PIN only */
    mocks_reset();
    bring_up();
    m_pair_block = "ROM P-192 key not replaced (mock)";
    bt_xrec_open_pairing(60000u);
    bt_xrec_poll();
    bt_status_t st = status();
    CHECK(st.pair_active && strlen(st.pair_pin) == 6u && !dbg().ssp_window, "XREC PIN window only (ssp_window %u)",
          dbg().ssp_window);
    CHECK(m_scan_d == ESP_BT_NON_DISCOVERABLE, "not discoverable (%d)", m_scan_d);
    cfm_req(PEER, 1234u);
    bt_xrec_poll();
    CHECK(m_cfm_calls == 1 && m_cfm_accept == 0 && !ui().prompt, "numeric comparison rejected without a prompt");
    stop_ok();

    /* gate holds when the window opens, then fails during a pairing (ROM RNG pool ran dry at the nonce): the SSP part
     * closes on the next poll and the pending prompt is rejected with it */
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    bt_xrec_poll();
    CHECK(dbg().ssp_window && m_scan_d == ESP_BT_GENERAL_DISCOVERABLE, "SSP window open with the gate holding");
    cfm_req(PEER, 4321u);
    bt_xrec_poll();
    CHECK(ui().prompt && m_cfm_calls == 0, "prompt up");
    int calls = m_pair_block_calls;
    m_pair_block = "ROM RNG pool ran dry this session (mock)";
    bt_xrec_poll();
    CHECK(m_pair_block_calls > calls, "gate re-checked while the SSP part is open");
    CHECK(!dbg().ssp_window && m_scan_d == ESP_BT_NON_DISCOVERABLE, "SSP part closed");
    CHECK(!ui().prompt && m_cfm_calls == 1 && m_cfm_accept == 0, "pending prompt rejected with the window");
    CHECK(user_answer(1) == -3 && m_cfm_calls == 1, "a late ENTER changes nothing (swallowed)");
    stop_ok();

    /* window opened right after bt_xrec_start (SD bench run, pair=1 at t=0; or ENTER during bring-up): the ROM key is
     * installed only by the controller start inside the up job, so the gate is decided once READY, not refused early */
    for (int c = 0; c < 3; c++) {
        mocks_reset();
        m_pair_block = "ROM P-192 key not replaced (mock: controller not started)";
        CHECK(bt_xrec_start(NULL, 0) == 0, "start");
        bt_xrec_open_pairing(60000u);
        CHECK(dbg().state == BT_XREC_ST_UP && dbg().ssp_pending && !dbg().ssp_window && dbg().ssp_gate_refused == 0u,
              "gate deferred before READY (state %u pending %u window %u refused %u)", dbg().state, dbg().ssp_pending,
              dbg().ssp_window, dbg().ssp_gate_refused);
        CHECK(status().pair_active && strlen(status().pair_pin) == 6u, "XREC PIN issued at once");
        m_pair_block = c == 0 ? NULL : "ROM P-192 key slots not as traced (mock)";  /* start() installed it, or not */
        m_entropy_fail = c == 2;                 /* c 2: no credit at all (the PIN is already out: kept) */
        bt_xrec_poll();
        esp_spp_cb_param_t p;
        memset(&p, 0, sizeof(p));
        p.init.status = ESP_SPP_SUCCESS;
        spp_evt(ESP_SPP_INIT_EVT, &p);
        bt_xrec_poll();
        CHECK(!dbg().ssp_window, "no SSP part before READY");
        memset(&p, 0, sizeof(p));
        p.start.status = ESP_SPP_SUCCESS;
        p.start.scn = 1u;
        spp_evt(ESP_SPP_START_EVT, &p);
        bt_xrec_poll();
        CHECK(dbg().state == BT_XREC_ST_READY && !dbg().ssp_pending, "READY, gate decided");
        if (c == 0) {
            CHECK(dbg().ssp_window && m_scan_d == ESP_BT_GENERAL_DISCOVERABLE && dbg().ssp_gate_refused == 0u,
                  "SSP part opened at READY (window %u scan %d)", dbg().ssp_window, m_scan_d);
            cfm_req(PEER, 777u);
            bt_xrec_poll();
            CHECK(ui().prompt && m_cfm_calls == 0, "deferred window prompts like any other");
        } else {
            CHECK(!dbg().ssp_window && m_scan_d == ESP_BT_NON_DISCOVERABLE && dbg().ssp_gate_refused == 1u,
                  "refused at READY, counted once (window %u scan %d refused %u)", dbg().ssp_window, m_scan_d,
                  dbg().ssp_gate_refused);
            CHECK(dbg().window_open && status().pair_active, "XREC PIN window kept");
        }
        m_entropy_fail = 0;
        stop_ok();
    }
    /* no entropy at all when the window was opened early: no PIN, and the deferred gate refusal closes everything */
    mocks_reset();
    m_entropy_fail = 1;
    CHECK(bt_xrec_start(NULL, 0) == 0, "start");
    bt_xrec_open_pairing(60000u);
    CHECK(dbg().window_open && status().pair_pin[0] == '\0' && dbg().ssp_pending, "window pending, no PIN");
    bt_xrec_poll();
    {
        esp_spp_cb_param_t p;
        memset(&p, 0, sizeof(p));
        p.init.status = ESP_SPP_SUCCESS;
        spp_evt(ESP_SPP_INIT_EVT, &p);
        bt_xrec_poll();
        memset(&p, 0, sizeof(p));
        p.start.status = ESP_SPP_SUCCESS;
        p.start.scn = 1u;
        spp_evt(ESP_SPP_START_EVT, &p);
        bt_xrec_poll();
    }
    CHECK(!dbg().window_open && !dbg().ssp_window && dbg().ssp_gate_refused == 1u && m_scan_d == ESP_BT_NON_DISCOVERABLE,
          "nothing opens (window %u ssp %u refused %u)", dbg().window_open, dbg().ssp_window, dbg().ssp_gate_refused);
    m_entropy_fail = 0;
    stop_ok();
}

static void t_bench_autoconfirm(void)
{
    printf("[15] BENCH autoconfirm override (bt_xrec_set_bench_autoconfirm)\n");
    mocks_reset();
    bt_xrec_set_bench_autoconfirm(1);   /* before start: survives bt_xrec_start */
    bring_up();
    cfm_req(PEER, 5u);
    bt_xrec_poll();
    CHECK(m_cfm_calls == 1 && m_cfm_accept == 0, "outside the window still rejected");
    bt_xrec_open_pairing(60000u);
    cfm_req(PEER, 4242u);
    bt_xrec_poll();
    CHECK(m_cfm_calls == 2 && m_cfm_accept == 1 && !ui().prompt && dbg().ssp_bench_autoconfirmed == 1u,
          "accepted at once without a prompt");
    CHECK(strcmp(status().pair_pin, "004242") == 0, "value shown through pair_pin, got '%s'", status().pair_pin);
    CHECK(m_cfm_logged == 4242u, "bench accept logged value %u, want the comparison value 4242",
          (unsigned)m_cfm_logged);
    auth_cmpl(PEER, ESP_BT_STATUS_SUCCESS, ESP_BT_LINK_KEY_AUTHED_COMB_P192);
    add_bond(PEER);
    bt_xrec_poll();
    CHECK(m_remove_calls == 0 && !dbg().ssp_window, "MITM bond kept, SSP window closed after it");
    bt_xrec_set_bench_autoconfirm(0);
    bt_xrec_open_pairing(60000u);
    cfm_req(PEER2, 1u);
    bt_xrec_poll();
    CHECK(m_cfm_calls == 2 && ui().prompt, "off again: the prompt is back");
    stop_ok();
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    cfm_req(PEER, 1u);
    bt_xrec_poll();
    CHECK(m_cfm_calls == 0 && ui().prompt && dbg().ssp_bench_autoconfirmed == 0u, "default off");
    stop_ok();

    /* bench XREC PIN (SD:/BTBENCH.RUN xrec_pin=): survives start, every window uses it, still fails closed without
     * entropy, bad values switch it off, and off means DRBG PINs again */
    mocks_reset();
    bt_xrec_set_bench_xrec_pin("012345");
    bring_up();
    bt_xrec_open_pairing(60000u);
    CHECK(strcmp(status().pair_pin, "012345") == 0, "bench PIN shown ('%s')", status().pair_pin);
    bt_xrec_open_pairing(60000u);
    CHECK(strcmp(status().pair_pin, "012345") == 0, "bench PIN on a re-opened window");
    stop_ok();
    mocks_reset();
    m_entropy_fail = 1;
    bring_up();
    bt_xrec_open_pairing(60000u);
    CHECK(status().pair_pin[0] == '\0' && !dbg().window_open, "no entropy: no bench PIN either");
    m_entropy_fail = 0;
    stop_ok();
    mocks_reset();
    bt_xrec_set_bench_xrec_pin("12345x");
    bring_up();
    bt_xrec_open_pairing(60000u);
    CHECK(strlen(status().pair_pin) == 6u && strcmp(status().pair_pin, "012345") != 0, "bad value: off (DRBG PIN '%s')",
          status().pair_pin);
    stop_ok();
    bt_xrec_set_bench_xrec_pin("654321");
    bt_xrec_set_bench_xrec_pin(NULL);
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    CHECK(strcmp(status().pair_pin, "654321") != 0, "NULL: off again");
    stop_ok();
}


/* Review findings (2026-11-24): the ROM never validates the peer's P-192 key, so an accepted pairing that fails tells
 * the peer the result of a DHKey check with this session's key (invalid-curve oracle): such a key is spent until the
 * next BT-mode entry. The gate is re-checked when a request arrives and when the user accepts (not only by the poll
 * after the event drain). */
/* Silicon bench 2026-11-24: the pairing window reopened while the controller held two ACL links; the ROM refuses
 * page scan then (0C1A -> 0x0C). The session logs the controller's link notes and sends its scan mode again when a
 * link goes, so the window becomes discoverable (and a closing window stops being discoverable) once it can. */
static void t_scan_refused(void)
{
    printf("[20] controller link notes; scan mode refused with two links, re-applied when a link goes\n");
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    bt_xrec_poll();
    int calls = m_scan_calls;
    CHECK(m_scan_d == ESP_BT_GENERAL_DISCOVERABLE, "window: discoverable requested");
    note_push(1, 0u, "BT HCI connection complete h=0x0081 st=0x00");
    note_push(4, 0x03u, "BT HCI scan enable 0x03 refused st=0x0C");
    bt_xrec_poll();
    CHECK(m_notes_n == 0 && dbg().ctrl_notes == 2u, "notes drained and logged (%u)", dbg().ctrl_notes);
    CHECK(strcmp(m_last_log, "BT HCI scan enable 0x03 refused st=0x0C") == 0, "last log '%s'", m_last_log);
    CHECK(dbg().scan_refused == 1u && m_scan_calls == calls, "refusal recorded, no retry while two links (%d calls)",
          m_scan_calls - calls);
    bt_xrec_poll();
    CHECK(m_scan_calls == calls, "no retry loop");
    acl_disc(PEER2);
    bt_xrec_poll();
    CHECK(m_scan_calls == calls + 1 && m_scan_d == ESP_BT_GENERAL_DISCOVERABLE && dbg().scan_reapplied == 1u,
          "scan mode sent again after a link went (%d calls, reapplied %u)", m_scan_calls - calls,
          dbg().scan_reapplied);
    acl_disc(PEER);
    bt_xrec_poll();
    CHECK(m_scan_calls == calls + 1, "re-applied once per refusal");
    stop_ok();
}

/* Review of the bench fix: a refusal must not wait for a link-down event the session may never get (the extra link
 * the ROM counts can be one the host never knew, e.g. a rejected re-page ending with a failed Connection Complete),
 * must be seen before a link drop queued with it, and a refused write that drops discoverability (window close) must
 * not leave the controller discoverable: fail closed with scan off (0x00, which the ROM always accepts). */
static void t_scan_refused_retry(void)
{
    printf("[21] scan mode refused: retried on a timer, seen before a queued link drop, fail-safe scan off on a "
           "refused window close\n");
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    bt_xrec_poll();
    int calls = m_scan_calls;
    /* (a) the link that caused the refusal ends without any ACL event to the session */
    note_push(4, 0x03u, "BT HCI scan enable 0x03 refused st=0x0C");
    bt_xrec_poll();
    CHECK(m_scan_calls == calls, "no immediate retry (%d calls)", m_scan_calls - calls);
    m_now += BT_XREC_SCAN_RETRY_MS - 1u;
    bt_xrec_poll();
    CHECK(m_scan_calls == calls, "retried before BT_XREC_SCAN_RETRY_MS (%d calls)", m_scan_calls - calls);
    m_now += 1u;
    bt_xrec_poll();
    CHECK(m_scan_calls == calls + 1 && m_scan_d == ESP_BT_GENERAL_DISCOVERABLE && dbg().scan_reapplied == 1u,
          "refused scan mode not retried on the timer (%d calls, reapplied %u)", m_scan_calls - calls,
          dbg().scan_reapplied);
    m_now += BT_XREC_SCAN_RETRY_MS;
    bt_xrec_poll();
    CHECK(m_scan_calls == calls + 1, "retried again without a new refusal (%d calls)", m_scan_calls - calls);
    /* (b) the refusal note and the GAP link drop are both pending when the poll runs */
    calls = m_scan_calls;
    note_push(4, 0x03u, "BT HCI scan enable 0x03 refused st=0x0C");
    acl_disc(PEER2);
    bt_xrec_poll();
    bt_xrec_poll();
    CHECK(m_scan_calls == calls + 1 && dbg().scan_reapplied == 2u, "refusal not seen before the queued link drop "
          "(%d calls, reapplied %u)", m_scan_calls - calls, dbg().scan_reapplied);
    /* (c) the window closes; BTA's two 0C1A 02 writes are refused while the ROM is still at 0x03 */
    calls = m_scan_calls;
    m_now += 60001u;
    bt_xrec_poll();
    CHECK(!dbg().window_open && m_scan_calls == calls + 1 && m_scan_c == ESP_BT_CONNECTABLE &&
          m_scan_d == ESP_BT_NON_DISCOVERABLE, "window close: connectable only (%d calls)", m_scan_calls - calls);
    note_push(4, 0x02u, "BT HCI scan enable 0x02 refused st=0x0C");
    note_push(4, 0x02u, "BT HCI scan enable 0x02 refused st=0x0C");
    bt_xrec_poll();
    CHECK(m_scan_calls == calls + 2 && m_scan_c == ESP_BT_NON_CONNECTABLE && m_scan_d == ESP_BT_NON_DISCOVERABLE &&
          dbg().scan_failsafe == 1u, "refused close left the controller discoverable: no fail-safe scan off (%d calls, "
          "c %d d %d, failsafe %u)", m_scan_calls - calls, m_scan_c, m_scan_d, dbg().scan_failsafe);
    CHECK(!dbg().scan_disc, "reported discoverable after the fail-safe");
    /* the fail-safe's own first write (BTM_SetDiscoverability, page scan still on in BTM) is refused as well */
    note_push(4, 0x02u, "BT HCI scan enable 0x02 refused st=0x0C");
    bt_xrec_poll();
    CHECK(m_scan_calls == calls + 2 && dbg().scan_failsafe == 1u, "fail-safe repeated (%d calls)",
          m_scan_calls - calls);
    m_now += BT_XREC_SCAN_RETRY_MS;
    bt_xrec_poll();
    CHECK(m_scan_calls == calls + 3 && m_scan_c == ESP_BT_CONNECTABLE && m_scan_d == ESP_BT_NON_DISCOVERABLE,
          "connectable mode not retried after the fail-safe (%d calls)", m_scan_calls - calls);
    /* (d) a refused 0x02 with nothing discoverable sent since: the ROM kept 0x02 or 0x00, no fail-safe */
    note_push(4, 0x02u, "BT HCI scan enable 0x02 refused st=0x0C");
    bt_xrec_poll();
    CHECK(m_scan_calls == calls + 3 && dbg().scan_failsafe == 1u, "fail-safe without a discoverable mode sent");
    m_now += BT_XREC_SCAN_RETRY_MS;
    bt_xrec_poll();
    CHECK(m_scan_calls == calls + 4 && m_scan_c == ESP_BT_CONNECTABLE, "connectable retry (%d calls)",
          m_scan_calls - calls);
    stop_ok();
}

static void t_ssp_key_spent(void)
{
    printf("[17] accepted pairing that fails: P-192 key spent, SSP refused until the next BT-mode entry\n");
    for (int variant = 0; variant < 3; variant++) {   /* 0 bench autoconfirm + AUTH_CMPL fail, 1 user + fail, 2 ACL drop */
        mocks_reset();
        if (variant == 0) {
            bt_xrec_set_bench_autoconfirm(1);
        }
        bring_up();
        bt_xrec_open_pairing(60000u);
        bt_xrec_poll();
        CHECK(dbg().ssp_window && m_scan_d == ESP_BT_GENERAL_DISCOVERABLE, "v%d: SSP window open", variant);
        cfm_req(PEER, 4242u);
        bt_xrec_poll();
        if (variant != 0) {
            CHECK(user_answer(1) == 0, "v%d: user accepts", variant);
        }
        CHECK(m_cfm_accept == 1 && !dbg().ssp_burned, "v%d: accepted, key not spent yet", variant);
        if (variant == 2) {
            acl_disc(PEER);
        } else {
            auth_cmpl(PEER, ESP_BT_STATUS_AUTH_FAILURE, 0u);   /* DHKey check failed (wrong guess of d * P') */
        }
        bt_xrec_poll();
        CHECK(dbg().ssp_burned && !dbg().ssp_window && m_scan_d == ESP_BT_NON_DISCOVERABLE,
              "v%d: key spent, SSP part closed (burned %u window %u scan %d)", variant, dbg().ssp_burned,
              dbg().ssp_window, m_scan_d);
        CHECK(dbg().window_open && status().pair_active, "v%d: the XREC PIN part of the window stays", variant);
        int calls = m_cfm_calls;
        cfm_req(PEER2, 1u);
        bt_xrec_poll();
        CHECK(m_cfm_calls == calls + 1 && m_cfm_accept == 0 && !ui().prompt, "v%d: next comparison rejected", variant);
        uint32_t refused = dbg().ssp_gate_refused;
        bt_xrec_open_pairing(60000u);
        bt_xrec_poll();
        CHECK(!dbg().ssp_window && dbg().ssp_gate_refused == refused + 1u && m_scan_d == ESP_BT_NON_DISCOVERABLE,
              "v%d: re-opened window: SSP refused, counted (toast)", variant);
        stop_ok();
        m_stopped = 0;
        bring_up();                                    /* next BT-mode entry: a new key */
        bt_xrec_open_pairing(60000u);
        bt_xrec_poll();
        CHECK(dbg().ssp_window && !dbg().ssp_burned, "v%d: next session offers SSP again", variant);
        stop_ok();
        if (variant == 0) {
            bt_xrec_set_bench_autoconfirm(0);
        }
    }
    /* a rejected (never accepted) comparison that fails spends nothing: the peer never reached the DHKey check */
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    cfm_req(PEER, 1u);
    bt_xrec_poll();
    CHECK(user_answer(0) == 0, "BACK");
    auth_cmpl(PEER, ESP_BT_STATUS_AUTH_FAILURE, 0u);
    bt_xrec_poll();
    CHECK(!dbg().ssp_burned && dbg().ssp_window, "rejected then failed: key not spent, window still open");
    stop_ok();

    printf("[18] M2.3 gate re-checked at the request and at the answer (pool ran dry between two polls)\n");
    mocks_reset();
    bt_xrec_set_bench_autoconfirm(1);
    bring_up();
    bt_xrec_open_pairing(60000u);
    bt_xrec_poll();
    CHECK(dbg().ssp_window, "SSP window open");
    m_pair_block = "ROM RNG pool ran dry this session (mock)";   /* the poll has not seen it yet */
    cfm_req(PEER, 999u);
    bt_xrec_poll();                                    /* drain_events() runs before the poll's own gate check */
    CHECK(m_cfm_calls == 1 && m_cfm_accept == 0 && dbg().ssp_bench_autoconfirmed == 0u && dbg().ssp_accepted == 0u,
          "bench autoconfirm does not accept a comparison the gate refuses (accept %d)", m_cfm_accept);
    CHECK(dbg().ssp_gate_cfm_rejects == 1u && !dbg().ssp_window, "counted, SSP part closed");
    bt_xrec_set_bench_autoconfirm(0);
    stop_ok();
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    cfm_req(PEER, 31337u);
    bt_xrec_poll();
    CHECK(ui().prompt, "prompt up with the gate holding");
    m_pair_block = "ROM RNG pool ran dry this session (mock)";
    m_now += BT_XREC_SSP_ARM_MS;
    CHECK(bt_xrec_ssp_answer(1) == 0, "ENTER before the next poll");
    CHECK(m_cfm_calls == 1 && m_cfm_accept == 0 && dbg().ssp_user_accepted == 0u && !ui().prompt &&
          dbg().ssp_gate_cfm_rejects == 1u, "ENTER rejects when the gate stopped holding (accept %d)", m_cfm_accept);
    stop_ok();

    printf("[19] re-opening a window that is still well open keeps its XREC PIN\n");
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    char pin1[8];
    snprintf(pin1, sizeof(pin1), "%s", status().pair_pin);
    m_now += 5000u;
    bt_xrec_open_pairing(60000u);                      /* ENTER again 5 s later */
    CHECK(strcmp(status().pair_pin, pin1) == 0 && ui().window_s_left == 60u, "PIN kept ('%s' vs '%s'), deadline moved "
          "(%u s)", status().pair_pin, pin1, ui().window_s_left);
    m_now += 50000u + 1u;                              /* 9.999 s left: the next ENTER draws a new PIN */
    bt_xrec_open_pairing(60000u);
    CHECK(strlen(status().pair_pin) == 6u && strcmp(status().pair_pin, pin1) != 0, "new PIN near the end ('%s')",
          status().pair_pin);
    stop_ok();
}

/* Silicon session 6 (BT_HW_RESULTS.md): (1) first pairing with a laptop whose name the device did not know: the
 * prompt showed its address for the whole prompt. BTM's own name request (link set-up) answered after the prompt
 * came, and only BTM saw that answer; the session's request failed (the ROM refuses a second one while the first
 * runs). A successful name complete passing through bt_ctrl now updates the prompt in place. (2) after BACK the
 * screen kept "connected": BTM disconnects a link whose pairing failed (0406), but the link's end was not reported
 * while the user looked. A link the host is ending is no longer "connected" on the screen. */
static void t_session6_name_and_link(void)
{
    printf("[22] silicon session 6: late peer name shown in place on the prompt; a link the host disconnects is not "
           "\"connected\"\n");
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    bt_xrec_poll();
    acl_conn_h(PEER, 0x0080u);
    bt_xrec_poll();
    CHECK(ui().connected && status().acl_connected && dbg().acl_links == 1u, "link up: connected");
    cfm_req(PEER, 333198u);
    bt_xrec_poll();
    CHECK(ui().prompt && !ui().peer_is_name && strchr(ui().peer, ':') != NULL, "prompt shows the address first ('%s')",
          ui().peer);
    remote_name_fail(PEER);                                  /* the session's own request: refused (0x0C on silicon) */
    note_push_ex(6, 0x0Cu, 0u, 0u, "BT HCI name request st=0x0C rom");
    bt_xrec_poll();
    CHECK(!ui().peer_is_name && dbg().name_failed == 1u, "failed name request counted (%u)", dbg().name_failed);
    m_now += 2000u;
    memcpy(m_ctrl_name_bda, PEER, 6u);                      /* BTM's link set-up request completes late */
    snprintf(m_ctrl_name, sizeof(m_ctrl_name), "martianbfg");
    m_ctrl_name_ok = 1;
    note_push_ex(BT_XREC_NOTE_NAME, 0x00u, 10u, 0u, "BT HCI name complete st=0x00 len=10");
    bt_xrec_poll();
    CHECK(ui().prompt && ui().peer_is_name && strcmp(ui().peer, "martianbfg") == 0, "prompt peer '%s' after the "
          "late name, want martianbfg", ui().peer);
    CHECK(dbg().names_from_ctrl == 1u, "name taken from the controller note (%u)", dbg().names_from_ctrl);
    CHECK(strcmp(m_last_log, "BT SSP prompt shows the peer name, ms after the prompt 2000") == 0, "log '%s'",
          m_last_log);
    CHECK(user_answer(0) == 0 && !ui().prompt, "BACK");
    CHECK(ui().connected, "still connected right after BACK (nothing disconnected yet)");
    note_push_ex(BT_XREC_NOTE_DISC_CMD, 0x00u, 0u, 0x0080u, "BT HCI disconnect cmd h=0x0080 st=0x00 rom");   /* BTM ends the link */
    bt_xrec_poll();
    CHECK(!ui().connected && !status().acl_connected, "link being disconnected shown as connected (ui %u status %u)",
          ui().connected, status().acl_connected);
    CHECK(dbg().acl_links == 1u && dbg().acl_closing == 1u, "link still counted until it is reported down (%u %u)",
          dbg().acl_links, dbg().acl_closing);
    acl_disc_h(PEER, 0x0080u);
    bt_xrec_poll();
    CHECK(dbg().acl_links == 0u && dbg().acl_closing == 0u && !ui().connected, "link down (%u %u)", dbg().acl_links,
          dbg().acl_closing);
    acl_conn_h(PEER2, 0x0080u);                             /* same handle again: a new link, connected */
    bt_xrec_poll();
    CHECK(ui().connected && dbg().acl_closing == 0u, "new link on a reused handle is connected");
    note_push_ex(BT_XREC_NOTE_DISC_CMD, 0x00u, 0u, 0x0081u, "BT HCI disconnect cmd h=0x0081 st=0x00 rom");   /* another handle */
    bt_xrec_poll();
    CHECK(ui().connected, "a disconnect of another handle does not hide this link");
    acl_disc_h(PEER2, 0x0080u);
    bt_xrec_poll();
    /* a prompt whose peer name never comes says so when it ends */
    cfm_req(PEER2, 111111u);
    bt_xrec_poll();
    CHECK(user_answer(0) == 0, "BACK (no name)");
    CHECK(strcmp(m_last_log, "BT SSP prompt ended without the peer name (address shown)") == 0, "log '%s'",
          m_last_log);
    stop_ok();
}

/* Review of the session 6 fix: (1) the 0406 note can be drained before the GAP link-up event of its own link (same
 * poll, notes first): the closing mark must still apply when the link comes up; a stale mark must not hide a later
 * link on the same handle. (2) a successful name complete with an empty name must not apply the older stored name to
 * the current peer. (3) a name drained by the poll's last note drain is already on screen: the prompt end log must
 * not say the address was shown. */
static void t_session6_review(void)
{
    printf("[23] session 6 review: early 0406 note, empty name complete, end-of-poll name\n");
    mocks_reset();
    bring_up();
    bt_xrec_open_pairing(60000u);
    bt_xrec_poll();
    /* (1) Connection Complete, the host's 0406 and the GAP link-up all reach the same poll, notes first */
    note_push_ex(BT_XREC_NOTE_CONN, 0x00u, 0u, 0x0080u, "BT HCI connection complete h=0x0080 st=0x00");
    note_push_ex(BT_XREC_NOTE_DISC_CMD, 0x00u, 0u, 0x0080u, "BT HCI disconnect cmd h=0x0080 st=0x00 rom");
    acl_conn_h(PEER, 0x0080u);
    bt_xrec_poll();
    CHECK(dbg().acl_links == 1u && dbg().acl_closing == 1u && !ui().connected && !status().acl_connected,
          "0406 noted before its link-up: link closing, not connected (links %u closing %u ui %u)", dbg().acl_links,
          dbg().acl_closing, ui().connected);
    acl_disc_h(PEER, 0x0080u);
    bt_xrec_poll();
    CHECK(dbg().acl_links == 0u && dbg().acl_closing == 0u, "link down");
    note_push_ex(BT_XREC_NOTE_CONN, 0x00u, 0u, 0x0080u, "BT HCI connection complete h=0x0080 st=0x00");
    acl_conn_h(PEER2, 0x0080u);
    bt_xrec_poll();
    CHECK(ui().connected && dbg().acl_closing == 0u, "a later link on the same handle is connected");
    acl_disc_h(PEER2, 0x0080u);
    bt_xrec_poll();
    /* a 0406 for a handle whose link-up never came, then a new link on that handle: the mark does not carry over */
    note_push_ex(BT_XREC_NOTE_DISC_CMD, 0x02u, 0u, 0x0090u, "BT HCI disconnect cmd h=0x0090 st=0x02 rom");
    bt_xrec_poll();
    note_push_ex(BT_XREC_NOTE_CONN, 0x00u, 0u, 0x0090u, "BT HCI connection complete h=0x0090 st=0x00");
    acl_conn_h(PEER, 0x0090u);
    bt_xrec_poll();
    CHECK(ui().connected && dbg().acl_closing == 0u, "stale 0406 mark cleared by the next Connection Complete");
    acl_disc_h(PEER, 0x0090u);
    bt_xrec_poll();
    CHECK(dbg().acl_links == 0u && !ui().connected, "all links down");

    /* (2) PEER2's name is known; a successful name complete with no name (PEER2's) while the controller still holds
     * PEER's older name must not rename PEER2 */
    acl_conn_h(PEER2, 0x0081u);
    remote_name(PEER2, "bob");
    bt_xrec_poll();
    cfm_req(PEER2, 222222u);
    bt_xrec_poll();
    CHECK(ui().prompt && ui().peer_is_name && strcmp(ui().peer, "bob") == 0, "prompt shows bob ('%s')", ui().peer);
    memcpy(m_ctrl_name_bda, PEER, 6u);
    snprintf(m_ctrl_name, sizeof(m_ctrl_name), "alice");
    m_ctrl_name_ok = 1;
    note_push_ex(BT_XREC_NOTE_NAME, 0x00u, 0u, 0u, "BT HCI name complete st=0x00 len=0");
    bt_xrec_poll();
    CHECK(ui().prompt && ui().peer_is_name && strcmp(ui().peer, "bob") == 0, "empty name complete keeps bob ('%s')",
          ui().peer);
    CHECK(dbg().names_from_ctrl == 0u, "empty name not taken (%u)", dbg().names_from_ctrl);
    CHECK(user_answer(0) == 0 && !ui().prompt, "BACK");
    acl_disc_h(PEER2, 0x0081u);
    bt_xrec_poll();

    /* (3) the name arrives through the last note drain of a poll and the user answers before the next poll */
    m_ctrl_name_ok = 0;
    acl_conn_h(PEER, 0x0082u);
    bt_xrec_poll();
    cfm_req(PEER, 333333u);
    bt_xrec_poll();
    CHECK(ui().prompt && !ui().peer_is_name, "prompt shows the address");
    memcpy(m_ctrl_name_bda, PEER, 6u);
    snprintf(m_ctrl_name, sizeof(m_ctrl_name), "carol");
    m_ctrl_name_ok = 1;
    for (int i = 0; i < 8; i++) {                            /* the first drain of the poll takes these */
        note_push_ex(BT_XREC_NOTE_CONN_REQ, 0x00u, 0u, 0u, "BT HCI connection request");
    }
    note_push_ex(BT_XREC_NOTE_NAME, 0x00u, 5u, 0u, "BT HCI name complete st=0x00 len=5");   /* the last drain */
    bt_xrec_poll();
    CHECK(m_notes_n == 0 && ui().peer_is_name && strcmp(ui().peer, "carol") == 0, "name on screen ('%s', %d left)",
          ui().peer, m_notes_n);
    m_last_log[0] = '\0';
    m_now += BT_XREC_SSP_ARM_MS;
    CHECK(bt_xrec_ssp_answer(0) == 0, "BACK before the next poll");
    CHECK(strcmp(m_last_log, "BT SSP prompt ended without the peer name (address shown)") != 0, "log '%s'",
          m_last_log);
    bt_xrec_poll();
    stop_ok();
}

int main(int argc, char **argv)
{
    m_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    t_bring_up_config();
    t_pairing_and_data();
    t_policy();
    t_bonded_reconnect();
    t_rx_overflow();
    t_congestion_partial();
    t_write_error();
    t_disconnect_mid_transfer();
    t_stop_during_transfer();
    t_deferred_reboot();
    t_entropy_fail_closed();
    t_evq_overflow_and_faults();
    t_hard_paths();
    t_ssp_prompt();
    t_bench_autoconfirm();
    t_pairing_gate();
    t_ssp_key_spent();
    t_scan_refused();
    t_scan_refused_retry();
    t_session6_name_and_link();
    t_session6_review();
    bt_xrec_test_reset();
    printf("test_bt_xrec: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
