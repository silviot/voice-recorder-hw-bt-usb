/*
 * Host test: +XREC PUT target REC,<stem>.cwx (transcript sidecar next to /record/<stem>.wav).
 *
 * Drives the real src/xrec_proto.c over a file-backed exFAT image (mkfs.exfat, see the test-host target):
 * happy path + idempotent resend + strict order, replace, name rejection (never a recording, never a hidden or long
 * name), disconnect abort, interleaving with CAS and OTA staging, DEL of the busy temp, LOCKED; a refused header's
 * payload is discarded unparsed; only for an existing <stem>.wav (NOENT), DEL of a recording takes its sidecar;
 * CRC mismatch, idempotent PUTEND, SDIO failure mid-upload, power loss mid-upload (checkpointed temp is freed).
 *
 * Built a second time with -DOPENFW_BT_BENCH_IMAGE=1 (+ src/bench_meas.c): section 10 checks the bench-only +XREC:DBG
 * reply and the GETS byte count (docs/BT_SYNC_SPEED_AND_RECORDING.md F0).
 *
 * Usage: test_xrec_rec <exfat image>
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openfw_crypto.h"
#include "openfw_fs.h"
#include "openfw_xrec.h"
#include "openfw_bench_meas.h"

static char     g_out[16384];
static uint32_t g_out_len;
static uint32_t g_tx_room_cap;   /* 0 = unlimited; else all-or-nothing send limit (like bt_xrec_bluedroid xo_send) */

static uint32_t fake_tx_free(void *user)
{
    (void)user;
    return g_tx_room_cap ? g_tx_room_cap : 65536u;
}

static int cap_send(void *user, const uint8_t *data, uint16_t len)
{
    (void)user;
    if (g_tx_room_cap != 0u && len > g_tx_room_cap) {
        return -1;
    }
    assert(g_out_len + len < sizeof(g_out));
    memcpy(&g_out[g_out_len], data, len);
    g_out_len += len;
    g_out[g_out_len] = '\0';
    return 0;
}

static int file_read_sectors(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    FILE *fp = (FILE *)ctx;
    if (fseek(fp, (long)lba * 512L, SEEK_SET) != 0) {
        return -1;
    }
    return fread(buf, 512u, count, fp) == count ? 0 : -1;
}

static int g_fail_writes;   /* fault injection: every sector write fails while set */

static int file_write_sectors(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    FILE *fp = (FILE *)ctx;
    if (g_fail_writes) {
        return -1;
    }
    if (fseek(fp, (long)lba * 512L, SEEK_SET) != 0) {
        return -1;
    }
    if (fwrite(buf, 512u, count, fp) != count) {
        return -1;
    }
    return fflush(fp) == 0 ? 0 : -1;
}

static exfat_vol_t g_vol;
static xrec_ctx_t  g_x;
static uint32_t    g_now = 1000u;   /* ops.now_ms: the transfer status' clock */

static uint32_t fake_now(void *user)
{
    (void)user;
    return g_now;
}
static const uint8_t g_key[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

static void rx(const void *data, size_t len)
{
    assert(xrec_on_rx(&g_x, (const uint8_t *)data, (uint16_t)len) == (uint16_t)len);
    xrec_poll(&g_x);
}

static void rx_str(const char *s)
{
    rx(s, strlen(s));
}

static void clear_out(void)
{
    g_out_len = 0u;
    g_out[0]  = '\0';
}

/* Sends one command line and returns the reply text (all lines). */
static const char *cmd(const char *line)
{
    clear_out();
    rx_str(line);
    rx_str("\r");
    return g_out;
}

static void authenticate(void)
{
    const char *r = cmd("+XREC:CHAL");
    assert(strncmp(r, "+XREC:CHAL,", 11) == 0);
    uint8_t nonce[16];
    for (int i = 0; i < 16; i++) {
        unsigned v = 0u;
        assert(sscanf(&r[11 + 2 * i], "%2x", &v) == 1);
        nonce[i] = (uint8_t)v;
    }
    uint8_t mac[32];
    openfw_hmac_sha256(g_key, 32u, nonce, 16u, mac);
    char line[128];
    int pos = snprintf(line, sizeof(line), "+XREC:AUTH,");
    for (int i = 0; i < 32; i++) {
        pos += snprintf(&line[pos], sizeof(line) - (size_t)pos, "%02x", mac[i]);
    }
    assert(strcmp(cmd(line), "+XREC:OK,AUTH\r\n") == 0);
}

/* One PUT chunk; returns the reply text. */
static const char *put(uint32_t off, const char *target, const uint8_t *data, uint16_t len)
{
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "+XREC:PUT,%u,%u,%u,%s:", (unsigned)off, (unsigned)len,
             (unsigned)openfw_crc16_ccitt_false(data, len), target);
    clear_out();
    rx_str(hdr);
    rx(data, len);
    return g_out;
}

static void expect_put_ok(uint32_t off, const char *target, const uint8_t *data, uint16_t len)
{
    char want[64];
    snprintf(want, sizeof(want), "+XREC:OK,PUT,%u,%u\r\n", (unsigned)off, (unsigned)len);
    const char *r = put(off, target, data, len);
    if (strcmp(r, want) != 0) {
        fprintf(stderr, "PUT %s @%u: got '%s' want '%s'\n", target, (unsigned)off, r, want);
        assert(0);
    }
}

static int exists(const char *path)
{
    exfat_entry_t e;
    return exfat_find_path(&g_vol, path, &e) == 0;
}

static void expect_file(const char *path, const uint8_t *data, uint32_t len)
{
    exfat_entry_t e;
    assert(exfat_find_path(&g_vol, path, &e) == 0 && !e.is_dir);
    assert(e.size == len);
    static uint8_t buf[4096];
    uint32_t got = 0u;
    assert(len <= sizeof(buf));
    assert(exfat_read_file(&g_vol, &e, 0u, buf, len, &got) == 0 && got == len);
    assert(memcmp(buf, data, len) == 0);
}

/* LS listing (text between the replies). */
static const char *ls(void)
{
    const char *r = cmd("+XREC:LS");
    assert(strstr(r, "+XREC:END\r\n") != NULL);
    return r;
}

static void make_wav(const char *name, const uint8_t *data, uint32_t len)
{
    exfat_writer_t w;
    (void)exfat_mkdir(&g_vol, "/record");
    assert(exfat_create_file(&g_vol, "/record", name, &w) == 0);
    assert(exfat_write(&w, data, len) == 0);
    assert(exfat_close_file(&w) == 0);
}

static void fill(uint8_t *b, uint32_t n, uint8_t seed)
{
    for (uint32_t i = 0u; i < n; i++) {
        b[i] = (uint8_t)(seed + i * 13u);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <exfat image>\n", argv[0]);
        return 2;
    }
    FILE *fp = fopen(argv[1], "r+b");
    assert(fp != NULL);
    assert(fseek(fp, 0L, SEEK_END) == 0);
    long sz = ftell(fp);
    blkdev_t dev = { file_read_sectors, file_write_sectors, (uint32_t)(sz / 512L), fp };
    assert(exfat_mount(&g_vol, &dev) == 0);

    xrec_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.send = cap_send;
    ops.tx_free = fake_tx_free;
    ops.now_ms = fake_now;
    xrec_init(&g_x, &g_vol, &ops, NULL);
    xrec_set_bound_key(&g_x, g_key);

    static uint8_t a[640], b[640], c[640];
    fill(a, sizeof(a), 0x11u);
    fill(b, sizeof(b), 0x5Au);
    fill(c, sizeof(c), 0xC3u);

    /* 0. Unauthenticated: LOCKED, nothing staged */
    assert(strcmp(put(0u, "REC,REC_0001.cwx", a, 16u), "+XREC:ERR,LOCKED\r\n") == 0);
    assert(!exists("/record"));
    authenticate();

    /* 1a. Only for an existing recording: no /record/REC_0001.wav -> NOENT, nothing created */
    assert(strcmp(put(0u, "REC,REC_0001.cwx", a, 16u), "+XREC:ERR,NOENT\r\n") == 0);
    assert(!exists("/record"));
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:ERR,NOPUT\r\n") == 0);

    /* a recording */
    make_wav("REC_0001.wav", c, 300u);

    /* 1. Happy path: staged as a dot-file, committed by PUTEND */
    expect_put_ok(0u, "REC,REC_0001.cwx", a, 600u);
    assert(exists("/record/.xrec_rec.tmp") && !exists("/record/REC_0001.cwx"));
    assert(strstr(ls(), ".xrec_rec") == NULL);                              /* temp hidden from LS */
    assert(strcmp(cmd("+XREC:DEL,.xrec_rec.tmp"), "+XREC:ERR,BUSY\r\n") == 0);   /* its writer is open */
    expect_put_ok(600u, "REC,REC_0001.cwx", b, 100u);
    expect_put_ok(600u, "REC,REC_0001.cwx", b, 100u);                       /* idempotent resend: not written twice */
    assert(strcmp(put(600u, "REC,REC_0001.cwx", b, 100u + 1u), "+XREC:ERR,OFF\r\n") == 0);   /* not the last chunk */
    assert(strcmp(put(800u, "REC,REC_0001.cwx", b, 10u), "+XREC:ERR,OFF\r\n") == 0);         /* gap */
    assert(strcmp(put(700u, "REC,REC_0002.cwx", b, 10u), "+XREC:ERR,OFF\r\n") == 0);         /* other name */
    assert(strcmp(put(700u, "CAS,REC_0001.cwx", b, 10u), "+XREC:ERR,OFF\r\n") == 0);         /* other target */
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,700\r\n") == 0);
    {
        static uint8_t want[700];
        memcpy(want, a, 600u);
        memcpy(&want[600], b, 100u);
        expect_file("/record/REC_0001.cwx", want, 700u);
    }
    assert(!exists("/record/.xrec_rec.tmp"));
    assert(!exists("/cassini/REC_0001.cwx"));
    /* PUTEND is idempotent until the next PUT: a retry after a lost reply gets the same OK */
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,700\r\n") == 0);
    {
        const char *l = ls();
        assert(strstr(l, "+XREC:F,700,REC_0001.cwx\r\n") != NULL && strstr(l, "+XREC:F,300,REC_0001.wav\r\n") != NULL);
        assert(strstr(l, ".xrec") == NULL);
    }

    /* 2. Replace the existing sidecar (shorter), upper-case extension accepted; the .wav is untouched */
    expect_put_ok(0u, "REC,REC_0001.CWX", c, 50u);
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,50\r\n") == 0);
    expect_file("/record/REC_0001.cwx", c, 50u);
    expect_file("/record/REC_0001.wav", c, 300u);
    {
        const char *l = ls();
        assert(strstr(l, "+XREC:F,50,REC_0001.") != NULL && strstr(l, "+XREC:F,300,REC_0001.wav\r\n") != NULL);
        assert(strstr(l, ".xrec") == NULL);
    }
    /* offset 0 restarts a staged upload */
    expect_put_ok(0u, "REC,REC_0001.cwx", a, 20u);
    expect_put_ok(0u, "REC,REC_0001.cwx", b, 30u);
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,30\r\n") == 0);
    expect_file("/record/REC_0001.cwx", b, 30u);

    /* 3. Names that are not a transcript sidecar: ERR,NAME, nothing staged, recordings never touched */
    {
        static const char *bad[] = {
            "REC,REC_0001.wav", "REC,../x.cwx", "REC,x/y.cwx", "REC,..\\x.cwx", "REC,.cwx", "REC,.hidden.cwx",
            "REC,.xrec_rec.tmp", "REC,abcdefghijklmnopqrstuvwxyzA.cwx" /* 31 chars */,
            "REC,REC_0001.cwx.wav", "REC,REC_0001.cw", "REC,REC_0001", "REC,cwx", "REC,a.cwx ", "REC,a.cwx.",
            "REC,", "REC,\xc3\xa9t\xc3\xa9.cwx", "REC,a:b.cwx",
        };
        for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); i++) {
            static const uint8_t junk[8] = { 'j', 'u', 'n', 'k', 'j', 'u', 'n', 'k' };
            const char *r = put(0u, bad[i], junk, sizeof(junk));
            /* "a:b.cwx": the header ends at the first ':' -> "REC,a" (NAME) */
            if (strcmp(r, "+XREC:ERR,NAME\r\n") != 0) {
                fprintf(stderr, "bad name '%s': got '%s'\n", bad[i], r);
                assert(0);
            }
            assert(strcmp(cmd(""), "") == 0);   /* the payload was discarded, nothing pending */
            assert(g_x.put_stream_mode == XREC_PUT_NONE);
        }
        assert(!exists("/record/.xrec_rec.tmp"));
        expect_file("/record/REC_0001.wav", c, 300u);
        expect_file("/record/REC_0001.cwx", b, 30u);
        /* 30 chars is the limit */
        make_wav("abcdefghijklmnopqrstuvwxyz.wav", c, 10u);
        expect_put_ok(0u, "REC,abcdefghijklmnopqrstuvwxyz.cwx", a, 5u);
        assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,5\r\n") == 0);
        expect_file("/record/abcdefghijklmnopqrstuvwxyz.cwx", a, 5u);
        assert(strcmp(cmd("+XREC:DEL,abcdefghijklmnopqrstuvwxyz.cwx"), "+XREC:OK,DEL\r\n") == 0);
        expect_file("/record/abcdefghijklmnopqrstuvwxyz.wav", c, 10u);       /* DEL of a sidecar keeps the .wav */
        assert(strcmp(cmd("+XREC:DEL,abcdefghijklmnopqrstuvwxyz.wav"), "+XREC:OK,DEL\r\n") == 0);
    }

    /* 3b. A refused header's payload is eaten unparsed: no command inside it runs, no ERR,LINE, no extra reply */
    {
        static uint8_t evil[600];
        memset(evil, 'x', sizeof(evil));
        static const char inj[] = "\r+XREC:DEL,REC_0001.wav\r+XREC:PING\r";
        memcpy(&evil[10], inj, sizeof(inj) - 1u);
        memcpy(&evil[400], inj, sizeof(inj) - 1u);
        static const char *refused[][2] = {
            { "REC,ABCDEFGHIJKLMNOPQRSTUVWXYZ0.cwx", "+XREC:ERR,NAME\r\n" },   /* 31 chars */
            { "REC,REC_0001.wav", "+XREC:ERR,NAME\r\n" },
            { "CAS,../x", "+XREC:ERR,NAME\r\n" },
            { "FOO,REC_0001.cwx", "+XREC:ERR,ARG\r\n" },                       /* unknown target (as old fw) */
        };
        for (size_t i = 0u; i < sizeof(refused) / sizeof(refused[0]); i++) {
            const char *r = put(0u, refused[i][0], evil, sizeof(evil));
            if (strcmp(r, refused[i][1]) != 0) {
                fprintf(stderr, "refused '%s': got '%s'\n", refused[i][0], r);
                assert(0);
            }
            expect_file("/record/REC_0001.wav", c, 300u);
        }
        /* bad crc field (> 0xFFFF), payload split across receives */
        clear_out();
        rx_str("+XREC:PUT,0,600,70000,REC,REC_0001.cwx:");
        rx(evil, 7u);
        rx(&evil[7], 500u);
        rx(&evil[507], 93u);
        assert(strcmp(g_out, "+XREC:ERR,ARG\r\n") == 0);
        assert(strncmp(cmd("+XREC:PING"), "+XREC:PONG,", 11) == 0);   /* parser in sync right after */
        expect_file("/record/REC_0001.wav", c, 300u);
        expect_file("/record/REC_0001.cwx", b, 30u);
    }

    /* 3c. CRC mismatch: ERR,CRC, the staged upload survives, the good chunk is then accepted */
    {
        expect_put_ok(0u, "REC,REC_0001.cwx", a, 600u);
        char hdr[96];
        snprintf(hdr, sizeof(hdr), "+XREC:PUT,600,40,%u,REC,REC_0001.cwx:",
                 (unsigned)(openfw_crc16_ccitt_false(b, 40u) ^ 1u));
        clear_out();
        rx_str(hdr);
        rx(b, 40u);
        assert(strcmp(g_out, "+XREC:ERR,CRC\r\n") == 0);
        assert(exists("/record/.xrec_rec.tmp"));
        expect_put_ok(600u, "REC,REC_0001.cwx", b, 40u);
        assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,640\r\n") == 0);
        static uint8_t want[640];
        memcpy(want, a, 600u);
        memcpy(&want[600], b, 40u);
        expect_file("/record/REC_0001.cwx", want, 640u);
        /* back to the earlier content for the checks below */
        expect_put_ok(0u, "REC,REC_0001.cwx", b, 30u);
        assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,30\r\n") == 0);
    }

    /* 4. Disconnect mid-upload: temp deleted, target unchanged, a re-authenticated session starts clean */
    make_wav("REC_0002.wav", c, 100u);
    make_wav("REC_0003.wav", c, 100u);
    expect_put_ok(0u, "REC,REC_0001.cwx", a, 640u);
    expect_put_ok(0u, "REC,REC_0002.cwx", a, 640u);
    assert(exists("/record/.xrec_rec.tmp"));
    xrec_on_disconnect(&g_x);
    assert(!exists("/record/.xrec_rec.tmp"));
    assert(!exists("/record/REC_0002.cwx"));
    expect_file("/record/REC_0001.cwx", b, 30u);
    authenticate();
    assert(strcmp(put(640u, "REC,REC_0002.cwx", a, 10u), "+XREC:ERR,OFF\r\n") == 0);
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:ERR,NOPUT\r\n") == 0);

    /* 5. Interleaving: a new CAS upload aborts a staged REC one and vice versa; OTA likewise */
    expect_put_ok(0u, "REC,REC_0002.cwx", a, 100u);
    expect_put_ok(0u, "CAS,demo.cwx", b, 40u);
    assert(!exists("/record/.xrec_rec.tmp") && exists("/cassini/.xrec_cas.tmp"));
    assert(strcmp(put(100u, "REC,REC_0002.cwx", a, 10u), "+XREC:ERR,OFF\r\n") == 0);
    expect_put_ok(40u, "CAS,demo.cwx", c, 8u);
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,48\r\n") == 0);
    {
        uint8_t want[48];
        memcpy(want, b, 40u);
        memcpy(&want[40], c, 8u);
        expect_file("/cassini/demo.cwx", want, 48u);
    }
    assert(!exists("/record/REC_0002.cwx") && !exists("/record/demo.cwx"));

    expect_put_ok(0u, "CAS,demo2.cwx", b, 40u);
    expect_put_ok(0u, "REC,REC_0002.cwx", c, 64u);
    assert(!exists("/cassini/.xrec_cas.tmp") && exists("/record/.xrec_rec.tmp"));
    assert(strcmp(put(40u, "CAS,demo2.cwx", b, 10u), "+XREC:ERR,OFF\r\n") == 0);
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,64\r\n") == 0);
    expect_file("/record/REC_0002.cwx", c, 64u);
    assert(!exists("/cassini/demo2.cwx"));

    expect_put_ok(0u, "REC,REC_0003.cwx", a, 100u);
    expect_put_ok(0u, "OTA", b, 100u);
    assert(!exists("/record/.xrec_rec.tmp") && exists("/.xrec_ota.tmp"));
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:ERR,NOPUT\r\n") == 0);        /* PUTEND never commits an OTA image */
    expect_put_ok(0u, "REC,REC_0003.cwx", a, 100u);
    assert(!exists("/.xrec_ota.tmp") && exists("/record/.xrec_rec.tmp"));
    assert(strcmp(cmd("+XREC:OTA"), "+XREC:ERR,NOOTA\r\n") == 0);           /* OTA never commits a REC upload */
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,100\r\n") == 0);
    expect_file("/record/REC_0003.cwx", a, 100u);
    assert(!exists("/update.up"));

    /* 6. DEL of a recording takes its sidecar; a sidecar whose recording vanished mid-upload is never committed */
    assert(strcmp(cmd("+XREC:DEL,REC_0003.wav"), "+XREC:OK,DEL\r\n") == 0);
    assert(!exists("/record/REC_0003.cwx") && !exists("/record/REC_0003.wav"));
    make_wav("REC_0003.wav", c, 100u);
    expect_put_ok(0u, "REC,REC_0003.cwx", a, 100u);
    assert(strcmp(cmd("+XREC:DEL,REC_0003.WAV"), "+XREC:OK,DEL\r\n") == 0);
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:ERR,NOENT\r\n") == 0);
    assert(!exists("/record/REC_0003.cwx") && !exists("/record/.xrec_rec.tmp"));
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:ERR,NOPUT\r\n") == 0);
    make_wav("REC_0003.wav", c, 100u);
    expect_put_ok(0u, "REC,REC_0003.cwx", a, 100u);
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,100\r\n") == 0);

    /* 7. SDIO failure mid-upload: ERR,SDIO, upload dropped (later chunks ERR,OFF), the old sidecar stays */
    expect_put_ok(0u, "REC,REC_0002.cwx", a, 600u);
    g_fail_writes = 1;
    assert(strcmp(put(600u, "REC,REC_0002.cwx", b, 600u), "+XREC:ERR,SDIO\r\n") == 0);
    g_fail_writes = 0;
    assert(strcmp(put(1200u, "REC,REC_0002.cwx", b, 10u), "+XREC:ERR,OFF\r\n") == 0);
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:ERR,NOPUT\r\n") == 0);
    expect_file("/record/REC_0002.cwx", c, 64u);
    expect_put_ok(0u, "REC,REC_0002.cwx", c, 64u);   /* a new upload starts clean (any leftover temp deleted) */
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,64\r\n") == 0);
    assert(!exists("/record/.xrec_rec.tmp"));

    /* 8. Power loss / reset mid-upload: the temp left behind owns its clusters (checkpointed after every chunk), so
     * the next upload's delete frees them (fsck below finds no leaked cluster) */
    {
        expect_put_ok(0u, "REC,REC_0002.cwx", a, 600u);
        expect_put_ok(600u, "REC,REC_0002.cwx", b, 600u);
        expect_put_ok(1200u, "REC,REC_0002.cwx", c, 600u);
        /* reset: RAM state (writer, context, volume) is lost without any close */
        assert(exfat_mount(&g_vol, &dev) == 0);
        xrec_init(&g_x, &g_vol, &ops, NULL);
        xrec_set_bound_key(&g_x, g_key);
        exfat_entry_t e;
        assert(exfat_find_path(&g_vol, "/record/.xrec_rec.tmp", &e) == 0 && e.size >= 1536u && e.first_cluster >= 2u);
        expect_file("/record/REC_0002.cwx", c, 64u);
        authenticate();
        expect_put_ok(0u, "REC,REC_0002.cwx", b, 64u);
        assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,64\r\n") == 0);
        expect_file("/record/REC_0002.cwx", b, 64u);
        assert(!exists("/record/.xrec_rec.tmp"));
    }

    /* 9. Transfer status for the BT screen (xrec_get_xfer; the user saw "Sending 3162 of 41"): GETS progress is the
     * highest offset sent of the file's size on the card, held XREC_XFER_HOLD_MS after the last chunk so the pauses
     * of an app sync do not flicker, a GETS of another file switches the name, PUT shows the staged bytes (total
     * unknown), a disconnect clears it. */
    {
        xrec_xfer_t x;
        g_now += 5000u;
        xrec_get_xfer(&g_x, g_now, &x);
        assert(!x.active && x.name[0] == '\0' && x.done == 0u && x.total == 0u);   /* the earlier uploads timed out */

        static uint8_t big[3000];
        fill(big, sizeof(big), 0x2Bu);
        make_wav("XFER_LONG_NAME_0123456789.wav", big, sizeof(big));
        cmd("+XREC:GETS,0,1000,256,XFER_LONG_NAME_0123456789.wav");
        assert(strncmp(g_out, "XREC", 4) == 0);
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active && !x.upload && x.done == 1000u && x.total == 3000u);
        assert(strcmp(x.name, "XFER_LONG_NAME_01234\xE2\x80\xA6") == 0);     /* cut to 20 bytes + U+2026 + NUL */
        g_now += 1900u;                                                        /* a pause between bursts */
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active && x.done == 1000u);
        cmd("+XREC:GETS,1000,2000,640,XFER_LONG_NAME_0123456789.wav");
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active && x.done == 3000u && x.total == 3000u);
        g_now += 1500u;
        cmd("+XREC:GETS,1000,500,256,XFER_LONG_NAME_0123456789.wav");          /* a retried window: never goes back */
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active && x.done == 3000u);
        cmd("+XREC:GETS,3000,100,64,XFER_LONG_NAME_0123456789.wav");           /* EOF probe: no byte moves, no change */
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active && x.done == 3000u && x.total == 3000u);
        cmd("+XREC:GETS,0,500,256,XFER_LONG_NAME_0123456789.wav");             /* a fresh download from 0 starts over */
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active && x.done == 500u && x.total == 3000u);
        g_now += 1999u;
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active);
        g_now += 1u;
        xrec_get_xfer(&g_x, g_now, &x);
        assert(!x.active);                                                     /* 2 s without a chunk */
        cmd("+XREC:GETS,0,200,64,XFER_LONG_NAME_0123456789.wav");              /* after the timeout: starts over */
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active && x.done == 200u);

        /* another file: the name switches at once; an LS in between does not touch it */
        g_now += 100u;
        cmd("+XREC:GETS,0,100,64,REC_0003.wav");
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active && !x.upload && strcmp(x.name, "REC_0003.wav") == 0 && x.done == 100u && x.total == 100u);
        ls();
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active && strcmp(x.name, "REC_0003.wav") == 0);
        assert(strcmp(cmd("+XREC:GETS,0,100,64,NOPE.wav"), "+XREC:ERR,NOENT\r\n") == 0);
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active && strcmp(x.name, "REC_0003.wav") == 0);
        cmd("+XREC:GETS,5000,100,64,XFER_LONG_NAME_0123456789.wav");           /* past EOF of another file: no view */
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active && strcmp(x.name, "REC_0003.wav") == 0 && x.done == 100u && x.total == 100u);
        g_now += 2000u;
        cmd("+XREC:GETS,5000,100,64,XFER_LONG_NAME_0123456789.wav");
        xrec_get_xfer(&g_x, g_now, &x);
        assert(!x.active);

        /* upload: bytes staged, total unknown; PUTEND keeps it shown, then it times out */
        g_now += 100u;
        expect_put_ok(0u, "REC,REC_0001.cwx", a, 600u);
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active && x.upload && strcmp(x.name, "REC_0001.cwx") == 0 && x.done == 600u && x.total == 0u);
        g_now += 1000u;
        expect_put_ok(600u, "REC,REC_0001.cwx", b, 40u);
        expect_put_ok(600u, "REC,REC_0001.cwx", b, 40u);                      /* resend: counted once */
        assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,640\r\n") == 0);
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active && x.upload && x.done == 640u);
        expect_put_ok(0u, "REC,REC_0001.cwx", b, 40u);                        /* the same file again from 0 */
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active && x.upload && x.done == 40u);
        assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,40\r\n") == 0);
        g_now += 2000u;
        xrec_get_xfer(&g_x, g_now, &x);
        assert(!x.active);

        /* a UTF-8 name longer than the display copy is cut on a character boundary (12 x U+00E0 = 24 bytes) */
        static const char utf_name[] = "\xC3\xA0\xC3\xA0\xC3\xA0\xC3\xA0\xC3\xA0\xC3\xA0\xC3\xA0\xC3\xA0\xC3\xA0\xC3\xA0"
                                       "\xC3\xA0\xC3\xA0.bin";
        char tgt[64];
        snprintf(tgt, sizeof(tgt), "CAS,%s", utf_name);
        expect_put_ok(0u, tgt, c, 32u);
        xrec_get_xfer(&g_x, g_now, &x);
        assert(x.active && x.upload && strlen(x.name) == 23u && memcmp(x.name, utf_name, 20u) == 0 &&
               memcmp(x.name + 20, "\xE2\x80\xA6", 4u) == 0);                    /* 10 letters + U+2026 */
        /* an odd cut (2-byte letters after one ASCII byte) backs up to the character boundary */
        snprintf(tgt, sizeof(tgt), "CAS,x%s", utf_name);
        expect_put_ok(0u, tgt, c, 32u);
        xrec_get_xfer(&g_x, g_now, &x);
        assert(strlen(x.name) == 22u && memcmp(x.name, "x", 1u) == 0 && memcmp(x.name + 1, utf_name, 18u) == 0 &&
               memcmp(x.name + 19, "\xE2\x80\xA6", 4u) == 0);
        /* exactly 23 bytes: shown whole, no ellipsis */
        expect_put_ok(0u, "CAS,ABCDEFGHIJKLMNOPQRSTUVW", c, 32u);
        xrec_get_xfer(&g_x, g_now, &x);
        assert(strcmp(x.name, "ABCDEFGHIJKLMNOPQRSTUVW") == 0);

        /* the link drops: nothing shown (the staged upload is aborted) */
        xrec_on_disconnect(&g_x);
        xrec_get_xfer(&g_x, g_now, &x);
        assert(!x.active);
        authenticate();
        printf("xrec transfer status (BT screen) tests: OK\n");
    }

#if OPENFW_BENCH_MEAS
    /* 10. Bench images: +XREC:DBG (authenticated) and the GETS payload count. */
    {
        bench_meas_reset(&g_bench_meas);
        static uint8_t gb[1500];
        fill(gb, sizeof(gb), 0x77u);
        make_wav("DBG_GETS.wav", gb, sizeof(gb));
        bench_meas_rec_start(&g_bench_meas, "REC_0042.wav", 1u, 20u, 3u, 0u, 1000u);
        cmd("+XREC:GETS,0,1500,640,DBG_GETS.wav");          /* 640 + 640 + 220 */
        assert(g_bench_meas.gets_b_total == 1500u && g_bench_meas.gets_frames_total == 3u &&
               g_bench_meas.cur.gets_b == 1500u);
        const char *r = cmd("+XREC:DBG");
        assert(strncmp(r, "+XREC:DBG,LIVE,seq=1,file=REC_0042.wav,rec=1,ch=1,gain=20,dsp=3,vor=0,", 70) == 0);
        assert(strstr(r, ",gets_b=1500,") != NULL && strstr(r, ",gets_tot=1500,gets_fr=3,idle_pm=-,") != NULL);
        assert(strstr(r, ",acct=0,") != NULL && strstr(r, ",idle_min_pm=-,") != NULL);
        size_t n = strlen(r);
        assert(n <= XREC_REPLY_MAX && strcmp(&r[n - 2], "\r\n") == 0 && strchr(r, '\n') == &r[n - 1]);
        assert(strcmp(cmd("+XREC:DBG,REC,0"), "+XREC:ERR,NOENT\r\n") == 0);   /* nothing finished yet */
        bench_meas_capture_end(&g_bench_meas, 2000u);
        bench_meas_rec_stop(&g_bench_meas, 0u, 0u);
        r = cmd("+XREC:DBG,REC,0");
        assert(strncmp(r, "+XREC:DBG,REC,0,seq=1,file=REC_0042.wav,rec=0,", 46) == 0);
        assert(strstr(r, "gets_tot") == NULL && strstr(r, ",t_ms=1,") != NULL);
        assert(strcmp(cmd("+XREC:DBG,REC,1"), "+XREC:ERR,NOENT\r\n") == 0);
        assert(strcmp(cmd("+XREC:DBG,REC,x"), "+XREC:ERR,ARGS\r\n") == 0);
        assert(strcmp(cmd("+XREC:DBG,REC,0,1"), "+XREC:ERR,ARGS\r\n") == 0);
        assert(strcmp(cmd("+XREC:DBG,FOO"), "+XREC:ERR,ARGS\r\n") == 0);
        assert(strcmp(cmd("+XREC:DBGX"), "+XREC:ERR,CMD\r\n") == 0);
        /* Worst-case +XREC:DBG,LIVE with exactly XREC_REPLY_MAX bytes free in the TX ring must not be dropped */
        {
            bench_meas_rec_t *wr = &g_bench_meas.cur;
            memset(wr, 0xFF, sizeof(*wr));
            memcpy(wr->file, "ABCDEFGHIJKLMNO", 16u);
            wr->idle_min_pm = 65534u;
            wr->dma_blk = 0x1FFFFFu;
            wr->ov = 4000000000u;
            wr->comp = 1000000000u;
            wr->wav_frames = 3219126272u;
            wr->el_ms = 134217600u;
            wr->el_us_rem = 0u;
            wr->b_el_ms = 134217600u;
            wr->b_el_us_rem = 0u;
            wr->b_first_blk = 0u;
            wr->b_last_blk = 0x1FFFFFu;
            g_bench_meas.take_max_us = 0xFFFFFFFFu;
            g_bench_meas.gets_b_total = g_bench_meas.gets_frames_total = 0xFFFFFFFFu;
            g_bench_meas.idle_pm = g_bench_meas.app_pm = g_bench_meas.btc_pm = 0xFFFFu;
            g_bench_meas.cpu_win_ms = 0xFFFFu;
            g_tx_room_cap = XREC_REPLY_MAX;
            r = cmd("+XREC:DBG");
            g_tx_room_cap = 0u;
            n = strlen(r);
            assert(n > 0u && n <= XREC_REPLY_MAX && strncmp(r, "+XREC:DBG,LIVE,", 15) == 0);
            assert(strstr(r, ",take_us=4294967295\r\n") != NULL);
        }
        xrec_on_disconnect(&g_x);                            /* a new link: DBG needs AUTH like every data command */
        assert(strcmp(cmd("+XREC:DBG"), "+XREC:ERR,LOCKED\r\n") == 0);
        authenticate();
        printf("xrec bench DBG / GETS count tests: OK\n");
    }
#endif

    /* final LS: recordings + sidecars, no temps */
    {
        const char *l = ls();
        assert(strstr(l, ".xrec") == NULL && strstr(l, "REC_0001.wav") != NULL &&
               strstr(l, "+XREC:F,64,REC_0002.cwx\r\n") != NULL && strstr(l, "+XREC:F,100,REC_0003.cwx\r\n") != NULL);
    }
    expect_file("/record/REC_0001.wav", c, 300u);

    fclose(fp);
    printf("xrec PUT REC (transcript sidecar) tests: OK\n");
    return 0;
}
