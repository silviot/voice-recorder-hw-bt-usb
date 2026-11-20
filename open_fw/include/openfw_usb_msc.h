/*
 * Smartlink SL6801 OpenFW Hardware-Independent USB Mass Storage (SCSI BOT)
 *
 * Implements the 31-byte CBW ('USBC') / 13-byte CSW ('USBS') Bulk-Only
 * Transport state machine and SBC/SPC SCSI block command handler over any
 * 512-byte blkdev_t (microSD or host test RAM disk).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENFW_USB_MSC_H
#define OPENFW_USB_MSC_H

#include <stdint.h>
#include "openfw_fs.h"

typedef enum {
    MSC_PHASE_IDLE = 0,
    MSC_PHASE_DATA_IN,
    MSC_PHASE_DATA_OUT,
    MSC_PHASE_CSW,
} usb_msc_phase_t;

typedef int (*usb_msc_vendor_cb_t)(void *user, const uint8_t cdb[16],
                                   uint8_t *io_buf, uint16_t *io_len, int is_out);

typedef struct {
    int  (*read_start)(void *ctx, uint32_t lba, uint32_t count, void *buf);
    int  (*read_wait)(void *ctx);
    void (*read_abort)(void *ctx);
    void *ctx;
} usb_msc_async_read_t;

#define USB_MSC_PP_SECTORS  16u
#define USB_MSC_PP_BYTES    (USB_MSC_PP_SECTORS * 512u)
#define USB_MSC_BUF_SECTORS 32u
#define USB_MSC_BUF_BYTES   (USB_MSC_BUF_SECTORS * 512u)

typedef struct {
    blkdev_t           *dev;
    usb_msc_phase_t     phase;
    uint32_t            csw_tag;
    uint32_t            csw_residue;
    uint8_t             csw_status;
    uint8_t             medium_ejected;
    uint8_t             send_zlp;
    uint8_t             sense_key;
    uint8_t             sense_asc;
    uint8_t             sense_ascq;

    /* Optional vendor SCSI 0xE0 callback (preserved across usb_msc_reset) */
    usb_msc_vendor_cb_t vendor_cb;
    void               *vendor_user;
    uint8_t             is_vendor_out;
    uint8_t             vendor_cdb[16];

    /* Optional read-only async IDMAC ping-pong callbacks (preserved across usb_msc_reset) */
    usb_msc_async_read_t async_read;
    uint8_t             pp_active;
    uint8_t             pp_prefetch;
    uint16_t            pp_prefetch_secs;
    uint32_t            pp_prefetch_lba;

    /* Streaming / inline transfer state */
    uint8_t             is_blk_xfer;
    uint32_t            cur_lba;
    uint32_t            blocks_rem;
    uint32_t            data_rem;
    uint16_t            sec_off;
    uint16_t            buf_len;
    uint16_t            inline_pos;
    uint16_t            inline_len;
    uint8_t             inline_buf[64];
    uint8_t             sec_buf[USB_MSC_BUF_BYTES] __attribute__((aligned(4)));
} usb_msc_state_t;

void    usb_msc_init_state(usb_msc_state_t *st, blkdev_t *dev);
void    usb_msc_set_vendor_cb(usb_msc_state_t *st, usb_msc_vendor_cb_t cb, void *user);
void    usb_msc_set_async_read(usb_msc_state_t *st, const usb_msc_async_read_t *ops);
void    usb_msc_reset(usb_msc_state_t *st);
void    usb_msc_on_rx_packet(usb_msc_state_t *st, const uint8_t *pkt, uint8_t len);
int     usb_msc_need_tx(const usb_msc_state_t *st);
uint8_t usb_msc_get_tx_packet(usb_msc_state_t *st, uint8_t out[64]);

#endif /* OPENFW_USB_MSC_H */
