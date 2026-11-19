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

typedef struct {
    blkdev_t       *dev;
    usb_msc_phase_t phase;
    uint32_t        csw_tag;
    uint32_t        csw_residue;
    uint8_t         csw_status;
    uint8_t         medium_ejected;
    uint8_t         send_zlp;
    uint8_t         sense_key;
    uint8_t         sense_asc;
    uint8_t         sense_ascq;

    /* Streaming / inline transfer state */
    uint8_t         is_blk_xfer;
    uint32_t        cur_lba;
    uint32_t        blocks_rem;
    uint32_t        data_rem;
    uint16_t        sec_off;
    uint16_t        inline_pos;
    uint16_t        inline_len;
    uint8_t         inline_buf[64];
    uint8_t         sec_buf[512];
} usb_msc_state_t;

void    usb_msc_init_state(usb_msc_state_t *st, blkdev_t *dev);
void    usb_msc_reset(usb_msc_state_t *st);
void    usb_msc_on_rx_packet(usb_msc_state_t *st, const uint8_t *pkt, uint8_t len);
int     usb_msc_need_tx(const usb_msc_state_t *st);
uint8_t usb_msc_get_tx_packet(usb_msc_state_t *st, uint8_t out[64]);

#endif /* OPENFW_USB_MSC_H */
