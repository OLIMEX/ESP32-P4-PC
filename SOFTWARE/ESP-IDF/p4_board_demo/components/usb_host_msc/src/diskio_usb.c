/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * FatFS diskio driver for USB MSC (ESP-IDF < 6.0).
 *
 * ff_diskio_register_msc() hangs SCSI-backed init/status/read/write/ioctl
 * callbacks on a FatFS drive number:
 *
 *   fopen / VFS -> FatFS -> usb_disk_* -> scsi_cmd_read10/write10 -> BOT/USB
 *
 * On ESP-IDF 6.0+ this file is not compiled. FatFS uses diskio_bdl.c instead
 * and MSC supplies an esp_blockdev handle from msc_host_get_blockdev().
 */

#include "diskio_impl.h"
#include "ffconf.h"
#include "ff.h"
#include "esp_log.h"
#include "diskio_usb.h"
#include "msc_scsi_bot.h"
#include "msc_common.h"
#include "usb/usb_types_stack.h"
#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    usb_disk_t *disk;
    uint32_t sector_offset;
    uint32_t sector_count;
} usb_disk_slot_t;

static usb_disk_slot_t s_disks[FF_VOLUMES] = { 0 };

static const char *TAG = "diskio_usb";

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p)
{
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

static bool is_fat_boot_sector(const uint8_t *sector, uint32_t sector_size)
{
    if (sector_size < 512 || read_le16(sector + 510) != 0xaa55) {
        return false;
    }
    if (sector[0] != 0xeb && sector[0] != 0xe9 && sector[0] != 0xe8) {
        return false;
    }
    if (memcmp(sector + 82, "FAT32   ", 8) == 0) {
        return true;
    }

    const uint16_t bytes_per_sector = read_le16(sector + 11);
    const uint8_t sectors_per_cluster = sector[13];
    const uint8_t fat_count = sector[16];
    return bytes_per_sector == sector_size && sectors_per_cluster != 0 &&
           (sectors_per_cluster & (sectors_per_cluster - 1)) == 0 &&
           read_le16(sector + 14) != 0 && (fat_count == 1 || fat_count == 2);
}

/*
 * ESP-IDF 5.5 builds FatFs with FF_LBA64=0, which also compiles out GPT
 * discovery. Present the first GPT Microsoft Basic Data FAT partition as a
 * sector-zero disk so normal FatFs mounting works. MBR and super-floppy media
 * retain the identity mapping.
 */
static void configure_gpt_fat_mapping(usb_disk_slot_t *slot)
{
    usb_disk_t *disk = slot->disk;
    if (disk->block_size < 512 || disk->block_count < 3) {
        return;
    }

    msc_device_t *dev = __containerof(disk, msc_device_t, disk);
    uint8_t *buf = calloc(1, disk->block_size);
    if (buf == NULL) {
        ESP_LOGW(TAG, "No memory for GPT detection; using raw disk mapping");
        return;
    }

    if (scsi_cmd_read10(dev, buf, 0, 1, disk->block_size) != ESP_OK ||
            read_le16(buf + 510) != 0xaa55) {
        free(buf);
        return;
    }

    bool protective_mbr = false;
    for (size_t i = 0; i < 4; ++i) {
        if (buf[446 + i * 16 + 4] == 0xee) {
            protective_mbr = true;
            break;
        }
    }
    if (!protective_mbr ||
            scsi_cmd_read10(dev, buf, 1, 1, disk->block_size) != ESP_OK ||
            memcmp(buf, "EFI PART", 8) != 0) {
        free(buf);
        return;
    }

    const uint32_t header_size = read_le32(buf + 12);
    const uint64_t entries_lba = read_le64(buf + 72);
    const uint32_t entry_count = read_le32(buf + 80);
    const uint32_t entry_size = read_le32(buf + 84);
    if (header_size < 92 || header_size > disk->block_size ||
            entries_lba > UINT32_MAX || entry_size < 128 ||
            entry_size > disk->block_size || entry_count == 0) {
        ESP_LOGW(TAG, "Unsupported GPT layout; using raw disk mapping");
        free(buf);
        return;
    }

    static const uint8_t microsoft_basic_data_guid[16] = {
        0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
        0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7,
    };
    uint32_t loaded_lba = UINT32_MAX;
    const uint32_t entries_to_scan = entry_count < 128 ? entry_count : 128;

    for (uint32_t i = 0; i < entries_to_scan; ++i) {
        const uint64_t byte_offset = (uint64_t)i * entry_size;
        const uint64_t table_lba = entries_lba + byte_offset / disk->block_size;
        const uint32_t in_sector = byte_offset % disk->block_size;
        if (table_lba > UINT32_MAX || in_sector + entry_size > disk->block_size) {
            continue;
        }
        if ((uint32_t)table_lba != loaded_lba) {
            if (scsi_cmd_read10(dev, buf, (uint32_t)table_lba, 1,
                                disk->block_size) != ESP_OK) {
                break;
            }
            loaded_lba = (uint32_t)table_lba;
        }

        const uint8_t *entry = buf + in_sector;
        if (memcmp(entry, microsoft_basic_data_guid,
                   sizeof(microsoft_basic_data_guid)) != 0) {
            continue;
        }

        const uint64_t first_lba = read_le64(entry + 32);
        const uint64_t last_lba = read_le64(entry + 40);
        if (first_lba > UINT32_MAX || last_lba < first_lba ||
                last_lba >= disk->block_count ||
                last_lba - first_lba + 1 > UINT32_MAX) {
            continue;
        }

        // Loading the candidate VBR overwrites the current GPT-entry sector.
        if (scsi_cmd_read10(dev, buf, (uint32_t)first_lba, 1,
                            disk->block_size) == ESP_OK &&
                is_fat_boot_sector(buf, disk->block_size)) {
            slot->sector_offset = (uint32_t)first_lba;
            slot->sector_count = (uint32_t)(last_lba - first_lba + 1);
            ESP_LOGI(TAG, "GPT FAT partition mapped: start=%"PRIu32
                     " sectors=%"PRIu32, slot->sector_offset,
                     slot->sector_count);
            break;
        }
        loaded_lba = UINT32_MAX;
    }

    if (slot->sector_offset == 0) {
        ESP_LOGW(TAG, "GPT detected, but no FAT partition was found");
    }
    free(buf);
}

static DSTATUS usb_disk_initialize(BYTE pdrv)
{
    return RES_OK;
}

static DSTATUS usb_disk_status(BYTE pdrv)
{
    return RES_OK;
}

static DRESULT usb_disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    assert(pdrv < FF_VOLUMES);
    assert(s_disks[pdrv].disk);

    usb_disk_slot_t *slot = &s_disks[pdrv];
    usb_disk_t *disk = slot->disk;
    size_t sector_size = disk->block_size;
    msc_device_t *dev = __containerof(disk, msc_device_t, disk);

    if (count > slot->sector_count || sector > slot->sector_count - count) {
        return RES_PARERR;
    }

    esp_err_t err = scsi_cmd_read10(dev, buff, sector + slot->sector_offset,
                                    count, sector_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scsi_cmd_read10 failed (%d)", err);
        return RES_ERROR;
    }

    return RES_OK;
}

static DRESULT usb_disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    assert(pdrv < FF_VOLUMES);
    assert(s_disks[pdrv].disk);

    usb_disk_slot_t *slot = &s_disks[pdrv];
    usb_disk_t *disk = slot->disk;
    size_t sector_size = disk->block_size;
    msc_device_t *dev = __containerof(disk, msc_device_t, disk);

    if (count > slot->sector_count || sector > slot->sector_count - count) {
        return RES_PARERR;
    }

    esp_err_t err = scsi_cmd_write10(dev, buff, sector + slot->sector_offset,
                                     count, sector_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scsi_cmd_write10 failed (%d)", err);
        return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT usb_disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    assert(pdrv < FF_VOLUMES);
    assert(s_disks[pdrv].disk);

    usb_disk_slot_t *slot = &s_disks[pdrv];
    usb_disk_t *disk = slot->disk;

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *((DWORD *) buff) = slot->sector_count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        // FatFS sector window is sized from this WORD; refuse sizes that would
        // truncate or exceed FF_MAX_SS while disk_read still transfers block_size.
        if (disk->block_size < FF_MIN_SS ||
                disk->block_size > FF_MAX_SS ||
                disk->block_size > UINT16_MAX ||
                (disk->block_size & (disk->block_size - 1)) != 0) {
            ESP_LOGE(TAG, "Unsupported block_size %"PRIu32, (uint32_t)disk->block_size);
            return RES_ERROR;
        }
        *((WORD *) buff) = (WORD) disk->block_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        return RES_ERROR;
    }
    return RES_ERROR;
}

void ff_diskio_register_msc(BYTE pdrv, usb_disk_t *disk)
{
    assert(pdrv < FF_VOLUMES);

    static const ff_diskio_impl_t usb_disk_impl = {
        .init = &usb_disk_initialize,
        .status = &usb_disk_status,
        .read = &usb_disk_read,
        .write = &usb_disk_write,
        .ioctl = &usb_disk_ioctl
    };
    s_disks[pdrv] = (usb_disk_slot_t) {
        .disk = disk,
        .sector_offset = 0,
        .sector_count = disk->block_count,
    };
    configure_gpt_fat_mapping(&s_disks[pdrv]);
    ff_diskio_register(pdrv, &usb_disk_impl);
}

BYTE ff_diskio_get_pdrv_disk(const usb_disk_t *disk)
{
    for (int i = 0; i < FF_VOLUMES; i++) {
        if (disk == s_disks[i].disk) {
            return i;
        }
    }
    return 0xff;
}
