/*
 * Host test: +XREC PUT target REC,<stem>.cwx (transcript sidecar next to /record/<stem>.wav).
 *
 * Drives the real src/xrec_proto.c over a file-backed exFAT image (mkfs.exfat, see the test-host target):
 * happy path + idempotent resend + strict order, replace, name rejection (never a recording, never a hidden or long
 * name), disconnect abort, interleaving with CAS and OTA staging, DEL of the busy temp, LOCKED.
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

static char     g_out[16384];
static uint32_t g_out_len;

static int cap_send(void *user, const uint8_t *data, uint16_t len)
{
    (void)user;
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

static int file_write_sectors(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    FILE *fp = (FILE *)ctx;
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

    /* 1. Happy path on a card without /record: created, staged as a dot-file, committed by PUTEND */
    assert(!exists("/record"));
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
    assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:ERR,NOPUT\r\n") == 0);
    assert(strcmp(ls(), "+XREC:F,700,REC_0001.cwx\r\n+XREC:END\r\n") == 0);

    /* a recording next to it */
    {
        exfat_writer_t w;
        assert(exfat_create_file(&g_vol, "/record", "REC_0001.wav", &w) == 0);
        assert(exfat_write(&w, c, 300u) == 0);
        assert(exfat_close_file(&w) == 0);
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
            assert(strcmp(cmd(""), "") == 0);   /* bare CR re-syncs: the payload went to the line parser */
            assert(g_x.put_stream_mode == XREC_PUT_NONE);
        }
        assert(!exists("/record/.xrec_rec.tmp"));
        expect_file("/record/REC_0001.wav", c, 300u);
        expect_file("/record/REC_0001.cwx", b, 30u);
        /* 30 chars is the limit */
        expect_put_ok(0u, "REC,abcdefghijklmnopqrstuvwxyz.cwx", a, 5u);
        assert(strcmp(cmd("+XREC:PUTEND"), "+XREC:OK,PUTEND,5\r\n") == 0);
        expect_file("/record/abcdefghijklmnopqrstuvwxyz.cwx", a, 5u);
        assert(strcmp(cmd("+XREC:DEL,abcdefghijklmnopqrstuvwxyz.cwx"), "+XREC:OK,DEL\r\n") == 0);
    }

    /* 4. Disconnect mid-upload: temp deleted, target unchanged, a re-authenticated session starts clean */
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
