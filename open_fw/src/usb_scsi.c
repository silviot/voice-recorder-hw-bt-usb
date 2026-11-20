/*
 * Smartlink SL6801 OpenFW USB Mass Storage SCSI Bulk-Only Transport (BOT)
 *
 * SPDX-License-Identifier: MIT
 */
#include "openfw_usb_msc.h"

#define CBW_SIGNATURE 0x43425355u /* "USBC" */
#define CSW_SIGNATURE 0x53425355u /* "USBS" */

static inline uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline uint16_t get_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)
         | (uint32_t)p[3];
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static inline void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFFu);
}

static void mem_zero(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0u; i < n; i++) {
        d[i] = 0u;
    }
}

static void mem_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0u; i < n; i++) {
        d[i] = s[i];
    }
}

static void set_sense(usb_msc_state_t *st, uint8_t key, uint8_t asc, uint8_t ascq)
{
    st->sense_key  = key;
    st->sense_asc  = asc;
    st->sense_ascq = ascq;
}

static void clear_sense(usb_msc_state_t *st)
{
    set_sense(st, 0u, 0u, 0u);
}

static void fail_cmd(usb_msc_state_t *st, uint32_t cbw_data_len, uint8_t cbw_flags,
                     uint8_t status, uint8_t key, uint8_t asc, uint8_t ascq)
{
    set_sense(st, key, asc, ascq);
    st->csw_status  = status;
    st->csw_residue = cbw_data_len;
    if (cbw_data_len > 0u && (cbw_flags & 0x80u) != 0u) {
        /* Emit a zero-length IN packet before CSW so BOT short-transfers cleanly */
        st->send_zlp    = 1u;
        st->is_blk_xfer = 0u;
        st->data_rem    = 0u;
        st->inline_pos  = 0u;
        st->inline_len  = 0u;
        st->phase       = MSC_PHASE_DATA_IN;
    } else if (cbw_data_len > 0u && (cbw_flags & 0x80u) == 0u) {
        st->is_blk_xfer = 0u;
        st->data_rem    = cbw_data_len;
        st->phase       = MSC_PHASE_DATA_OUT;
    } else {
        st->phase       = MSC_PHASE_CSW;
    }
}

static void start_inline_in(usb_msc_state_t *st, uint32_t cbw_data_len, uint32_t resp_len)
{
    uint32_t xfer = (resp_len < cbw_data_len) ? resp_len : cbw_data_len;
    st->csw_status  = 0u;
    st->csw_residue = cbw_data_len - xfer;
    st->is_blk_xfer = 0u;
    st->inline_pos  = 0u;
    st->inline_len  = (uint16_t)xfer;
    st->data_rem    = xfer;
    st->send_zlp    = (xfer == 0u && cbw_data_len > 0u) ? 1u : 0u;
    st->phase       = (xfer > 0u || st->send_zlp) ? MSC_PHASE_DATA_IN : MSC_PHASE_CSW;
}

static uint32_t effective_sectors(const usb_msc_state_t *st)
{
    if (!st->dev || st->medium_ejected) {
        return 0u;
    }
    return st->dev->total_sectors;
}

static void handle_cbw(usb_msc_state_t *st, const uint8_t *pkt, uint8_t len)
{
    if (len != 31u || get_le32(pkt) != CBW_SIGNATURE) {
        st->csw_tag     = (len >= 8u) ? get_le32(pkt + 4u) : 0u;
        st->csw_status  = 2u; /* Phase error */
        st->csw_residue = 0u;
        set_sense(st, 0x05u, 0x20u, 0x00u);
        st->phase = MSC_PHASE_CSW;
        return;
    }

    uint32_t tag      = get_le32(pkt + 4u);
    uint32_t data_len = get_le32(pkt + 8u);
    uint8_t  flags    = pkt[12];
    uint8_t  lun      = pkt[13] & 0x0Fu;
    uint8_t  cb_len   = pkt[14] & 0x1Fu;
    const uint8_t *cb = pkt + 15u;

    st->csw_tag       = tag;
    st->csw_residue   = data_len;
    st->csw_status    = 0u;
    st->send_zlp      = 0u;
    st->is_blk_xfer   = 0u;
    st->is_vendor_out = 0u;

    if (lun != 0u || cb_len == 0u || cb_len > 16u) {
        fail_cmd(st, data_len, flags, 1u, 0x05u, 0x24u, 0x00u);
        return;
    }

    uint8_t op = cb[0];
    if (op != 0x03u) {
        /* Clear previous sense upon starting any new command other than REQUEST SENSE */
        clear_sense(st);
    }

    switch (op) {
    case 0x00u: /* TEST UNIT READY */
        if (!st->dev || !st->dev->read_sectors || st->medium_ejected) {
            fail_cmd(st, data_len, flags, 1u, 0x02u, 0x3Au, 0x00u);
        } else {
            st->csw_residue = data_len;
            st->phase       = MSC_PHASE_CSW;
        }
        break;

    case 0x03u: /* REQUEST SENSE */ {
        uint32_t alloc = cb[4];
        if (alloc > 18u) {
            alloc = 18u;
        }
        mem_zero(st->inline_buf, 18u);
        st->inline_buf[0]  = 0x70u;
        st->inline_buf[2]  = st->sense_key & 0x0Fu;
        st->inline_buf[7]  = 10u;
        st->inline_buf[12] = st->sense_asc;
        st->inline_buf[13] = st->sense_ascq;
        clear_sense(st);
        start_inline_in(st, data_len, alloc);
        break;
    }

    case 0x12u: /* INQUIRY */ {
        if ((cb[1] & 0x01u) != 0u) {
            fail_cmd(st, data_len, flags, 1u, 0x05u, 0x24u, 0x00u);
            break;
        }
        static const char vendor[]  = "Cassini ";
        static const char product[] = "OpenFW Recorder ";
        static const char rev[]     = "1.0 ";
        mem_zero(st->inline_buf, 36u);
        st->inline_buf[0] = 0x00u; /* Direct-access block device */
        st->inline_buf[1] = 0x80u; /* RMB = 1 */
        st->inline_buf[2] = 0x04u; /* SPC-2 */
        st->inline_buf[3] = 0x02u; /* Response data format */
        st->inline_buf[4] = 31u;   /* Additional length */
        mem_copy(&st->inline_buf[8], vendor, 8u);
        mem_copy(&st->inline_buf[16], product, 16u);
        mem_copy(&st->inline_buf[32], rev, 4u);
        uint32_t alloc = get_be16(cb + 3u);
        if (alloc > 36u) {
            alloc = 36u;
        }
        start_inline_in(st, data_len, alloc);
        break;
    }

    case 0x1Au: /* MODE SENSE (6) */ {
        uint32_t alloc = cb[4];
        if (alloc > 4u) {
            alloc = 4u;
        }
        mem_zero(st->inline_buf, 4u);
        st->inline_buf[0] = 3u;
        st->inline_buf[2] = (st->dev && st->dev->write_sectors) ? 0x00u : 0x80u;
        start_inline_in(st, data_len, alloc);
        break;
    }

    case 0x5Au: /* MODE SENSE (10) */ {
        uint32_t alloc = get_be16(cb + 7u);
        if (alloc > 8u) {
            alloc = 8u;
        }
        mem_zero(st->inline_buf, 8u);
        put_be16(&st->inline_buf[0], 6u);
        st->inline_buf[3] = (st->dev && st->dev->write_sectors) ? 0x00u : 0x80u;
        start_inline_in(st, data_len, alloc);
        break;
    }

    case 0x1Bu: /* START STOP UNIT */
        if ((cb[4] & 0x03u) == 0x02u) {
            st->medium_ejected = 1u;
        } else if ((cb[4] & 0x01u) != 0u) {
            st->medium_ejected = 0u;
        }
        st->csw_residue = data_len;
        st->phase       = MSC_PHASE_CSW;
        break;

    case 0x1Eu: /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x2Fu: /* VERIFY (10) */
    case 0x35u: /* SYNCHRONIZE CACHE (10) */
        st->csw_residue = data_len;
        st->phase       = MSC_PHASE_CSW;
        break;

    case 0x23u: /* READ FORMAT CAPACITIES */ {
        uint32_t sectors = effective_sectors(st);
        if (sectors == 0u) {
            fail_cmd(st, data_len, flags, 1u, 0x02u, 0x3Au, 0x00u);
            break;
        }
        mem_zero(st->inline_buf, 12u);
        st->inline_buf[3] = 8u;
        put_be32(&st->inline_buf[4], sectors);
        st->inline_buf[8]  = 0x02u; /* Formatted Media */
        st->inline_buf[10] = 0x02u; /* 512 B block length (0x000200) */
        uint32_t alloc = get_be16(cb + 7u);
        if (alloc > 12u) {
            alloc = 12u;
        }
        start_inline_in(st, data_len, alloc);
        break;
    }

    case 0x25u: /* READ CAPACITY (10) */ {
        uint32_t sectors = effective_sectors(st);
        if (sectors == 0u) {
            fail_cmd(st, data_len, flags, 1u, 0x02u, 0x3Au, 0x00u);
            break;
        }
        put_be32(&st->inline_buf[0], sectors - 1u);
        put_be32(&st->inline_buf[4], 512u);
        start_inline_in(st, data_len, 8u);
        break;
    }

    case 0x28u: /* READ (10) */ {
        uint32_t lba     = get_be32(cb + 2u);
        uint32_t blocks  = get_be16(cb + 7u);
        uint32_t sectors = effective_sectors(st);
        if (!st->dev || !st->dev->read_sectors || sectors == 0u) {
            fail_cmd(st, data_len, flags, 1u, 0x02u, 0x3Au, 0x00u);
            break;
        }
        if ((uint64_t)lba + (uint64_t)blocks > (uint64_t)sectors) {
            fail_cmd(st, data_len, flags, 1u, 0x05u, 0x21u, 0x00u);
            break;
        }
        uint32_t byte_len = blocks * 512u;
        uint32_t xfer     = (byte_len < data_len) ? byte_len : data_len;
        if (xfer == 0u) {
            st->csw_residue = data_len;
            st->phase       = MSC_PHASE_CSW;
            break;
        }
        st->is_blk_xfer = 1u;
        st->cur_lba     = lba;
        st->blocks_rem  = blocks;
        st->sec_off     = 0u;
        st->buf_len     = 0u; /* Load first chunk lazily in get_tx_packet */
        st->data_rem    = xfer;
        st->csw_residue = data_len - xfer;
        st->phase       = MSC_PHASE_DATA_IN;
        break;
    }

    case 0x2Au: /* WRITE (10) */ {
        uint32_t lba     = get_be32(cb + 2u);
        uint32_t blocks  = get_be16(cb + 7u);
        uint32_t sectors = effective_sectors(st);
        if (!st->dev || sectors == 0u) {
            fail_cmd(st, data_len, flags, 1u, 0x02u, 0x3Au, 0x00u);
            break;
        }
        if (!st->dev->write_sectors) {
            fail_cmd(st, data_len, flags, 1u, 0x07u, 0x27u, 0x00u);
            break;
        }
        if ((uint64_t)lba + (uint64_t)blocks > (uint64_t)sectors) {
            fail_cmd(st, data_len, flags, 1u, 0x05u, 0x21u, 0x00u);
            break;
        }
        uint32_t byte_len = blocks * 512u;
        uint32_t xfer     = (byte_len < data_len) ? byte_len : data_len;
        if (xfer == 0u) {
            st->csw_residue = data_len;
            st->phase       = MSC_PHASE_CSW;
            break;
        }
        st->is_blk_xfer = 1u;
        st->cur_lba     = lba;
        st->blocks_rem  = blocks;
        st->sec_off     = 0u;
        st->buf_len     = 0u;
        st->data_rem    = xfer;
        st->csw_residue = data_len - xfer;
        st->phase       = MSC_PHASE_DATA_OUT;
        break;
    }

    case 0xE0u: /* OpenFW Vendor Diagnostic / Control Command */ {
        if (!st->vendor_cb) {
            fail_cmd(st, data_len, flags, 1u, 0x05u, 0x20u, 0x00u);
            break;
        }
        if ((flags & 0x80u) != 0u || data_len == 0u) {
            uint16_t resp_len = (uint16_t)(data_len < 64u ? data_len : 64u);
            mem_zero(st->inline_buf, 64u);
            if (st->vendor_cb(st->vendor_user, cb, st->inline_buf, &resp_len, 0) != 0) {
                fail_cmd(st, data_len, flags, 1u, 0x05u, 0x24u, 0x00u);
            } else {
                if (resp_len > 64u) {
                    resp_len = 64u;
                }
                start_inline_in(st, data_len, resp_len);
            }
        } else {
            if (data_len > 64u) {
                fail_cmd(st, data_len, flags, 1u, 0x05u, 0x24u, 0x00u);
                break;
            }
            mem_copy(st->vendor_cdb, cb, 16u);
            st->inline_pos    = 0u;
            st->inline_len    = (uint16_t)data_len;
            st->is_vendor_out = 1u;
            st->is_blk_xfer   = 0u;
            st->data_rem      = data_len;
            st->phase         = MSC_PHASE_DATA_OUT;
        }
        break;
    }

    default:
        fail_cmd(st, data_len, flags, 1u, 0x05u, 0x20u, 0x00u); /* Invalid command op */
        break;
    }
}

void usb_msc_init_state(usb_msc_state_t *st, blkdev_t *dev)
{
    if (!st) {
        return;
    }
    mem_zero(st, sizeof(*st));
    st->dev   = dev;
    st->phase = MSC_PHASE_IDLE;
}

void usb_msc_set_vendor_cb(usb_msc_state_t *st, usb_msc_vendor_cb_t cb, void *user)
{
    if (!st) {
        return;
    }
    st->vendor_cb   = cb;
    st->vendor_user = user;
}

void usb_msc_reset(usb_msc_state_t *st)
{
    if (!st) {
        return;
    }
    blkdev_t           *dev         = st->dev;
    usb_msc_vendor_cb_t vendor_cb   = st->vendor_cb;
    void               *vendor_user = st->vendor_user;
    mem_zero(st, sizeof(*st));
    st->dev         = dev;
    st->vendor_cb   = vendor_cb;
    st->vendor_user = vendor_user;
    st->phase       = MSC_PHASE_IDLE;
}

void usb_msc_on_rx_packet(usb_msc_state_t *st, const uint8_t *pkt, uint8_t len)
{
    if (!st || (!pkt && len > 0u)) {
        return;
    }

    if (st->phase == MSC_PHASE_IDLE) {
        handle_cbw(st, pkt, len);
        return;
    }

    if (st->phase != MSC_PHASE_DATA_OUT) {
        return;
    }

    uint32_t pos = 0u;
    while (pos < len && st->data_rem > 0u) {
        if (st->is_vendor_out) {
            uint32_t space = (uint32_t)st->inline_len - (uint32_t)st->inline_pos;
            uint32_t take  = len - pos;
            if (take > space) {
                take = space;
            }
            if (take > st->data_rem) {
                take = st->data_rem;
            }
            mem_copy(&st->inline_buf[st->inline_pos], pkt + pos, take);
            st->inline_pos = (uint16_t)(st->inline_pos + take);
            pos           += take;
            st->data_rem  -= take;
            continue;
        }

        if (!st->is_blk_xfer) {
            /* Drain unwanted OUT payload on a failed command */
            uint32_t drop = len - pos;
            if (drop > st->data_rem) {
                drop = st->data_rem;
            }
            pos          += drop;
            st->data_rem -= drop;
            continue;
        }

        uint32_t space = USB_MSC_BUF_BYTES - st->sec_off;
        uint32_t take  = len - pos;
        if (take > space) {
            take = space;
        }
        if (take > st->data_rem) {
            take = st->data_rem;
        }
        mem_copy(&st->sec_buf[st->sec_off], pkt + pos, take);
        st->sec_off  = (uint16_t)(st->sec_off + take);
        pos         += take;
        st->data_rem -= take;

        if (st->sec_off == USB_MSC_BUF_BYTES || (st->data_rem == 0u && st->sec_off >= 512u)) {
            uint32_t nsec = (uint32_t)(st->sec_off / 512u);
            if (st->csw_status == 0u &&
                st->dev->write_sectors(st->dev->ctx, st->cur_lba, nsec, st->sec_buf) != 0) {
                st->csw_status = 1u;
                set_sense(st, 0x03u, 0x0Cu, 0x00u); /* Medium write error */
            }
            st->cur_lba += nsec;
            st->blocks_rem = (st->blocks_rem > nsec) ? (st->blocks_rem - nsec) : 0u;
            st->sec_off = 0u;
        }
    }

    if (st->data_rem == 0u) {
        if (st->is_vendor_out) {
            uint16_t io_len = st->inline_len;
            st->is_vendor_out = 0u;
            if (!st->vendor_cb ||
                st->vendor_cb(st->vendor_user, st->vendor_cdb, st->inline_buf, &io_len, 1) != 0) {
                fail_cmd(st, 0u, 0u, 1u, 0x05u, 0x24u, 0x00u);
            } else {
                st->csw_status  = 0u;
                st->csw_residue = 0u;
                st->phase       = MSC_PHASE_CSW;
            }
        } else {
            st->is_blk_xfer = 0u;
            st->phase       = MSC_PHASE_CSW;
        }
    }
}

int usb_msc_need_tx(const usb_msc_state_t *st)
{
    if (!st) {
        return 0;
    }
    return (st->phase == MSC_PHASE_DATA_IN || st->phase == MSC_PHASE_CSW) ? 1 : 0;
}

uint8_t usb_msc_get_tx_packet(usb_msc_state_t *st, uint8_t out[64])
{
    if (!st || !out) {
        return 0u;
    }

    if (st->phase == MSC_PHASE_DATA_IN) {
        if (st->send_zlp) {
            st->send_zlp = 0u;
            st->phase    = MSC_PHASE_CSW;
            return 0u;
        }

        if (!st->is_blk_xfer) {
            uint16_t rem = st->inline_len - st->inline_pos;
            uint8_t  n   = (rem > 64u) ? 64u : (uint8_t)rem;
            if (n > 0u) {
                mem_copy(out, &st->inline_buf[st->inline_pos], n);
                st->inline_pos = (uint16_t)(st->inline_pos + n);
            }
            if (st->inline_pos >= st->inline_len) {
                st->phase = MSC_PHASE_CSW;
            }
            return n;
        }

        if (st->sec_off >= st->buf_len) {
            uint32_t nsec = st->blocks_rem;
            if (nsec > USB_MSC_BUF_SECTORS) {
                nsec = USB_MSC_BUF_SECTORS;
            }
            if (nsec == 0u) {
                nsec = 1u;
            }
            uint32_t nbytes = nsec * 512u;
            if (st->csw_status == 0u &&
                st->dev->read_sectors(st->dev->ctx, st->cur_lba, nsec, st->sec_buf) != 0) {
                mem_zero(st->sec_buf, nbytes);
                st->csw_status = 1u;
                set_sense(st, 0x03u, 0x11u, 0x00u); /* Medium read error */
            }
            st->cur_lba += nsec;
            st->blocks_rem = (st->blocks_rem > nsec) ? (st->blocks_rem - nsec) : 0u;
            st->buf_len = (uint16_t)nbytes;
            st->sec_off = 0u;
        }

        uint16_t sec_rem = (uint16_t)(st->buf_len - st->sec_off);
        uint32_t take    = (sec_rem < 64u) ? sec_rem : 64u;
        if (take > st->data_rem) {
            take = st->data_rem;
        }
        mem_copy(out, &st->sec_buf[st->sec_off], take);
        st->sec_off   = (uint16_t)(st->sec_off + take);
        st->data_rem -= take;

        if (st->data_rem == 0u) {
            st->is_blk_xfer = 0u;
            st->phase       = MSC_PHASE_CSW;
        }
        return (uint8_t)take;
    }

    if (st->phase == MSC_PHASE_CSW) {
        put_le32(&out[0], CSW_SIGNATURE);
        put_le32(&out[4], st->csw_tag);
        put_le32(&out[8], st->csw_residue);
        out[12]   = st->csw_status;
        st->phase = MSC_PHASE_IDLE;
        return 13u;
    }

    return 0u;
}
