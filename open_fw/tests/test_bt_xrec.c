/* Host unit tests for bt_port/bt_xrec_bluedroid.c (plan M4 XREC session): the real session code and the real
 * xrec_proto against mocked esp_spp / esp_bt_gap APIs and a mocked platform (tests drive the BTC callbacks directly,
 * single-threaded). Scenarios: bring-up/config, data path, congestion, partial writes and the in-flight window, write
 * errors, disconnect mid-transfer, pairing-window policy (numeric comparison, Just Works, legacy PIN, passkey),
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
void bt_xrec_plat_log(const char *msg)
{
    if (m_verbose) {
        printf("    log: %s\n", msg);
    }
}
void bt_xrec_plat_log_u32(const char *msg, uint32_t v)
{
    if (m_verbose) {
        printf("    log: %s %u\n", msg, (unsigned)v);
    }
}
uint16_t bt_xrec_plat_spp_peer_mtu(uint32_t handle) { (void)handle; return m_peer_mtu; }
void *bt_xrec_plat_alloc(size_t n)
{
    if (m_alloc_fail) {
        return NULL;
    }
    m_allocs++;
    return malloc(n);
}
void bt_xrec_plat_free(void *p)
{
    if (p != NULL) {
        m_frees++;
    }
    free(p);
}
int bt_xrec_plat_start_allowed(void) { return m_start_allowed; }
void bt_xrec_plat_identity(uint8_t bd[6])
{
    static const uint8_t k[6] = { 0x1D, 0x01, 0x00, 0x55, 0xCA, 0x02 };
    memcpy(bd, k, 6u);
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
void bt_xrec_plat_load_bound_key(xrec_ctx_t *ctx) { (void)ctx; }
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
static int m_remove_calls, m_disc_calls;
static uint32_t m_disc_handle;
static esp_err_t m_spp_init_ret = ESP_OK, m_srv_ret = ESP_OK;
static uint8_t m_bonds[8][6];
static int m_nbonds;

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
    (void)bda;
    m_cfm_calls++;
    m_cfm_accept = accept;
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
            m_nbonds--;
            break;
        }
    }
    return ESP_OK;
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

static void add_bond(const uint8_t bda[6])
{
    memcpy(m_bonds[m_nbonds++], bda, 6u);
}

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
    m_cfm_calls = m_pin_calls = m_passkey_calls = m_remove_calls = m_disc_calls = 0;
    m_cfm_accept = m_pin_accept = m_passkey_accept = -1;
    m_disc_handle = 0u;
    m_spp_init_ret = m_srv_ret = ESP_OK;
    m_nbonds = 0;
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
}

/* Numeric comparison inside the window, MITM bond, SPP open, XREC PAIR -> authenticated session. */
static void pair_and_auth(void)
{
    bt_xrec_open_pairing(60000u);
    cfm_req(PEER, 123456u);
    bt_xrec_poll();
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
          m_srv_cfg.sec_mask == (ESP_SPP_SEC_AUTHENTICATE | ESP_SPP_SEC_ENCRYPT) && m_srv_cfg.role == ESP_SPP_ROLE_SLAVE &&
          strcmp(m_srv_name, "Cassini") == 0, "SPP server: ch 1, SDP record, auth+encrypt, 'Cassini'");
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
    CHECK(m_cfm_calls == 1 && m_cfm_accept == 1, "numeric comparison accepted inside the window");
    st = status();
    CHECK(strcmp(st.pair_pin, "000042") == 0, "comparison value shown through pair_pin (INTERIM), got '%s'",
          st.pair_pin);
    auth_cmpl(PEER, ESP_BT_STATUS_SUCCESS, ESP_BT_LINK_KEY_AUTHED_COMB_P256);
    add_bond(PEER);
    bt_xrec_poll();
    st = status();
    CHECK(strcmp(st.pair_pin, "000042") != 0 && strlen(st.pair_pin) == 6u, "back to the XREC PIN after AUTH_CMPL");
    CHECK(m_remove_calls == 0, "MITM bond kept");
    srv_open(H1, PEER);
    bt_xrec_poll();
    CHECK(dbg().conn_authorized && dbg().mtu == 660u, "SPP open, authorized, mtu %u", dbg().mtu);
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
    auth_cmpl(PEER, ESP_BT_STATUS_SUCCESS, ESP_BT_LINK_KEY_UNAUTHED_COMB_P256);
    bt_xrec_poll();
    CHECK(m_cfm_accept == 1 && m_remove_calls == 3, "accepted comparison with an unauthenticated key removed");

    /* failed pairing: nothing removed */
    cfm_req(PEER, 6u);
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
    rx_raw(H1, junk, sizeof(junk));
    rx_raw(H1, junk, sizeof(junk));
    rx_raw(H1, junk, sizeof(junk));   /* 3000 > 2048: the third frame overflows */
    bt_xrec_poll();
    CHECK(dbg().rx_overflows == 1u, "overflow counted");
    CHECK(m_disc_calls == 1 && dbg().conn_closing, "connection closed");
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
    printf("[11] entropy not ready: XREC PIN refused, CHAL refused and the link closed\n");
    mocks_reset();
    add_bond(PEER);
    bring_up();
    m_entropy_fail = 1;
    bt_xrec_open_pairing(60000u);
    bt_xrec_poll();
    CHECK(status().pair_active && status().pair_pin[0] == '\0', "SSP window open, no XREC PIN");
    CHECK(m_scan_d == ESP_BT_GENERAL_DISCOVERABLE, "discoverable for SSP");
    srv_open(H1, PEER);
    rx(H1, "+XREC:PAIR,000000,000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\r\n");
    CHECK(wire_has("+XREC:ERR,PAIRCLOSED"), "XREC PAIR closed without a PIN");
    wire_clear();
    rx(H1, "+XREC:CHAL\r\n");
    CHECK(!wire_has("+XREC:CHAL"), "no challenge without entropy");
    CHECK(m_disc_calls == 1 && dbg().conn_closing, "link closed (fail closed)");
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
    bt_xrec_test_reset();
    printf("test_bt_xrec: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
