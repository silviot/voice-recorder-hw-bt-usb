/* bt_store: see bt_store.h.
 * SPDX-License-Identifier: MIT */
#include "bt_store.h"
#include <string.h>
#include "openfw_crypto.h"

typedef struct {
    char ns[BT_STORE_NAME_MAX + 1u];
    char key[BT_STORE_NAME_MAX + 1u];
    uint16_t off;
    uint16_t len;
    uint8_t used;
} entry_t;

typedef struct {
    char name[BT_STORE_NAME_MAX + 1u];
    uint8_t open_count;
    uint8_t writable;
} ns_slot_t;

static struct {
    bt_store_platform_t p;
    int have_p;
    entry_t e[BT_STORE_ENTRIES_MAX];
    uint8_t data[BT_STORE_DATA_MAX];
    uint16_t data_used;          /* data[] is kept compact: entries own [off, off+len) without gaps */
    ns_slot_t ns[BT_STORE_NS_MAX];
    int dirty;
    uint32_t gen;                /* bumped on every change; serialize remembers the one it wrote */
    uint32_t ser_gen;
    int initialised;
} g;

static void lock(void)   { if (g.have_p && g.p.lock) g.p.lock(g.p.ctx); }
static void unlock(void) { if (g.have_p && g.p.unlock) g.p.unlock(g.p.ctx); }

static void reset_entries(void)
{
    memset(g.e, 0, sizeof(g.e));
    memset(g.data, 0, sizeof(g.data));
    g.data_used = 0u;
    g.dirty = 0;
}

void bt_store_init(const bt_store_platform_t *platform)
{
    memset(&g, 0, sizeof(g));
    if (platform != NULL) {
        g.p = *platform;
        g.have_p = 1;
    }
    g.initialised = 1;
}

static int name_ok(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    size_t n = strnlen(s, BT_STORE_NAME_MAX + 1u);
    return n > 0u && n <= BT_STORE_NAME_MAX;
}

static entry_t *find(const char *ns, const char *key)
{
    for (unsigned i = 0; i < BT_STORE_ENTRIES_MAX; i++) {
        if (g.e[i].used && strcmp(g.e[i].ns, ns) == 0 && strcmp(g.e[i].key, key) == 0) {
            return &g.e[i];
        }
    }
    return NULL;
}

/* Remove an entry's bytes and close the gap so data[] stays compact. */
static void remove_entry(entry_t *x)
{
    uint16_t off = x->off, len = x->len;
    memmove(&g.data[off], &g.data[off + len], (size_t)(g.data_used - off - len));
    g.data_used = (uint16_t)(g.data_used - len);
    for (unsigned i = 0; i < BT_STORE_ENTRIES_MAX; i++) {
        if (g.e[i].used && g.e[i].off > off) {
            g.e[i].off = (uint16_t)(g.e[i].off - len);
        }
    }
    memset(x, 0, sizeof(*x));
}

/* ---- nvs API (handle = namespace slot index + 1) ---- */

esp_err_t nvs_flash_init(void)
{
    return g.initialised ? ESP_OK : ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g.initialised) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    if (!name_ok(ns)) {
        return ESP_ERR_NVS_INVALID_NAME;
    }
    lock();
    int slot = -1, free_slot = -1;
    for (unsigned i = 0; i < BT_STORE_NS_MAX; i++) {
        if (g.ns[i].open_count && strcmp(g.ns[i].name, ns) == 0) {
            slot = (int)i;
        } else if (!g.ns[i].open_count && free_slot < 0) {
            free_slot = (int)i;
        }
    }
    if (slot < 0) {
        if (free_slot < 0) {
            unlock();
            return ESP_ERR_NO_MEM;
        }
        slot = free_slot;
        strcpy(g.ns[slot].name, ns);
        g.ns[slot].writable = 0u;
    }
    if (g.ns[slot].open_count == 0xFFu) {
        unlock();
        return ESP_ERR_NO_MEM;
    }
    g.ns[slot].open_count++;
    if (mode == NVS_READWRITE) {
        g.ns[slot].writable = 1u;
    }
    *h = (nvs_handle_t)(slot + 1);
    unlock();
    return ESP_OK;
}

static ns_slot_t *slot_of(nvs_handle_t h)
{
    if (h == 0u || h > BT_STORE_NS_MAX || g.ns[h - 1u].open_count == 0u) {
        return NULL;
    }
    return &g.ns[h - 1u];
}

void nvs_close(nvs_handle_t h)
{
    lock();
    ns_slot_t *s = slot_of(h);
    if (s != NULL && --s->open_count == 0u) {
        memset(s, 0, sizeof(*s));
    }
    unlock();
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    if (len == NULL || !name_ok(key)) {
        return len == NULL ? ESP_ERR_INVALID_ARG : ESP_ERR_NVS_INVALID_NAME;
    }
    lock();
    ns_slot_t *s = slot_of(h);
    if (s == NULL) {
        unlock();
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    entry_t *x = find(s->name, key);
    if (x == NULL) {
        unlock();
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out == NULL) {          /* length query */
        *len = x->len;
        unlock();
        return ESP_OK;
    }
    if (*len < x->len) {
        *len = x->len;
        unlock();
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, &g.data[x->off], x->len);
    *len = x->len;
    unlock();
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len)
{
    if (!name_ok(key)) {
        return ESP_ERR_NVS_INVALID_NAME;
    }
    if (val == NULL && len != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > BT_STORE_DATA_MAX) {
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    lock();
    ns_slot_t *s = slot_of(h);
    if (s == NULL) {
        unlock();
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (!s->writable) {
        unlock();
        return ESP_ERR_NVS_READ_ONLY;
    }
    entry_t *x = find(s->name, key);
    size_t old_len = x ? x->len : 0u;
    if ((size_t)g.data_used - old_len + len > BT_STORE_DATA_MAX) {
        unlock();
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;   /* old value kept intact */
    }
    if (x == NULL) {
        for (unsigned i = 0; i < BT_STORE_ENTRIES_MAX && x == NULL; i++) {
            if (!g.e[i].used) {
                x = &g.e[i];
            }
        }
        if (x == NULL) {
            unlock();
            return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        }
    } else {
        remove_entry(x);
    }
    strcpy(x->ns, s->name);
    strcpy(x->key, key);
    x->off = g.data_used;
    x->len = (uint16_t)len;
    x->used = 1u;
    if (len != 0u) {
        memcpy(&g.data[x->off], val, len);
    }
    g.data_used = (uint16_t)(g.data_used + len);
    g.dirty = 1;
    g.gen++;
    unlock();
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    return nvs_get_blob(h, key, out, len);
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val)
{
    if (val == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_set_blob(h, key, val, strlen(val) + 1u);
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    if (!name_ok(key)) {
        return ESP_ERR_NVS_INVALID_NAME;
    }
    lock();
    ns_slot_t *s = slot_of(h);
    esp_err_t r = ESP_OK;
    if (s == NULL) {
        r = ESP_ERR_NVS_INVALID_HANDLE;
    } else if (!s->writable) {
        r = ESP_ERR_NVS_READ_ONLY;
    } else {
        entry_t *x = find(s->name, key);
        if (x == NULL) {
            r = ESP_ERR_NVS_NOT_FOUND;
        } else {
            remove_entry(x);
            g.dirty = 1;
            g.gen++;
        }
    }
    unlock();
    return r;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    lock();
    ns_slot_t *s = slot_of(h);
    esp_err_t r = ESP_OK;
    if (s == NULL) {
        r = ESP_ERR_NVS_INVALID_HANDLE;
    } else if (!s->writable) {
        r = ESP_ERR_NVS_READ_ONLY;
    } else {
        for (unsigned i = 0; i < BT_STORE_ENTRIES_MAX; i++) {
            if (g.e[i].used && strcmp(g.e[i].ns, s->name) == 0) {
                remove_entry(&g.e[i]);
                g.dirty = 1;
                g.gen++;
            }
        }
    }
    unlock();
    return r;
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    lock();
    ns_slot_t *s = slot_of(h);
    int flush = s != NULL && g.dirty;
    unlock();
    if (s == NULL) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (flush && g.have_p && g.p.request_flush != NULL) {
        g.p.request_flush(g.p.ctx);
    }
    return ESP_OK;
}

/* ---- serialisation ---- */

static const uint8_t k_magic[4] = { 'B', 'T', 'S', '1' };

size_t bt_store_serialize(uint8_t *buf, size_t cap)
{
    lock();
    size_t n = 0u;
    unsigned count = 0u;
#define PUT(src, l) do { if (n + (l) > cap) { unlock(); return 0u; } memcpy(buf + n, (src), (l)); n += (l); } while (0)
    PUT(k_magic, 4u);
    for (unsigned i = 0; i < BT_STORE_ENTRIES_MAX; i++) {
        count += g.e[i].used;
    }
    uint8_t c2[2] = { (uint8_t)count, (uint8_t)(count >> 8) };
    PUT(c2, 2u);
    for (unsigned i = 0; i < BT_STORE_ENTRIES_MAX; i++) {
        const entry_t *x = &g.e[i];
        if (!x->used) {
            continue;
        }
        uint8_t nl = (uint8_t)strlen(x->ns), kl = (uint8_t)strlen(x->key);
        uint8_t l2[2] = { (uint8_t)x->len, (uint8_t)(x->len >> 8) };
        PUT(&nl, 1u);
        PUT(x->ns, nl);
        PUT(&kl, 1u);
        PUT(x->key, kl);
        PUT(l2, 2u);
        PUT(&g.data[x->off], x->len);
    }
    uint8_t digest[32];
    openfw_sha256(buf, (uint32_t)n, digest);
    PUT(digest, 8u);
#undef PUT
    g.ser_gen = g.gen;          /* dirty stays set until the caller stored the image (bt_store_mark_saved) */
    unlock();
    return n;
}

void bt_store_mark_saved(void)
{
    lock();
    if (g.gen == g.ser_gen) {
        g.dirty = 0;            /* nothing changed since that image was serialised */
    }
    unlock();
}

esp_err_t bt_store_load(const uint8_t *img, size_t len)
{
    if (img == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    reset_entries();
    esp_err_t r = ESP_ERR_INVALID_SIZE;
    if (len < 4u + 2u + 8u || len > BT_STORE_IMAGE_MAX) {
        goto fail;
    }
    if (memcmp(img, k_magic, 4u) != 0) {
        r = ESP_ERR_INVALID_VERSION;
        goto fail;
    }
    uint8_t digest[32];
    openfw_sha256(img, (uint32_t)(len - 8u), digest);
    if (!openfw_ct_memeq(digest, img + len - 8u, 8u)) {
        r = ESP_ERR_INVALID_CRC;
        goto fail;
    }
    size_t end = len - 8u, n = 4u;
    unsigned count = (unsigned)img[n] | ((unsigned)img[n + 1u] << 8);
    n += 2u;
    if (count > BT_STORE_ENTRIES_MAX) {
        goto fail;
    }
    for (unsigned i = 0; i < count; i++) {
        entry_t *x = &g.e[i];
        for (unsigned part = 0; part < 2u; part++) {
            if (n >= end) {
                goto fail;
            }
            uint8_t l = img[n++];
            if (l == 0u || l > BT_STORE_NAME_MAX || l > end - n) {
                goto fail;
            }
            char *dst = part == 0u ? x->ns : x->key;
            memcpy(dst, img + n, l);
            dst[l] = '\0';
            if (strlen(dst) != l) {     /* embedded NUL */
                goto fail;
            }
            n += l;
        }
        if (end - n < 2u) {
            goto fail;
        }
        uint16_t vl = (uint16_t)(img[n] | (img[n + 1u] << 8));
        n += 2u;
        if (vl > end - n || (size_t)g.data_used + vl > BT_STORE_DATA_MAX) {
            goto fail;
        }
        if (find(x->ns, x->key) != NULL) {   /* duplicate key */
            goto fail;
        }
        x->off = g.data_used;
        x->len = vl;
        memcpy(&g.data[x->off], img + n, vl);
        g.data_used = (uint16_t)(g.data_used + vl);
        n += vl;
        x->used = 1u;
    }
    if (n != end) {
        goto fail;
    }
    unlock();
    return ESP_OK;
fail:
    reset_entries();
    unlock();
    return r;
}

int bt_store_dirty(void)
{
    lock();
    int d = g.dirty;
    unlock();
    return d;
}

void bt_store_reset(void)
{
    lock();
    reset_entries();
    g.gen++;
    unlock();
}
