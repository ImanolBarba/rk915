/*
 * Rockchip vendor storage reader — eMMC implementation
 *
 * Reads factory-provisioned data from the Rockchip vendor storage area
 * on internal eMMC. The vendor storage is a small structured region
 * (typically 64 KB, 2 redundant copies) that lives in the reserved area
 * before the first user partition on eMMC. Rockchip's factory tools
 * (rkdeveloptool, RKDevTool) write WiFi/BT MACs, serial numbers, and
 * calibration data here during manufacturing.
 *
 * Format is based on the GPL-licensed rk_vendor_storage.c from public
 * Rockchip BSP kernels. The on-disk layout:
 *
 *   Byte 0..1023:    struct rkvs_header (tag, version, item table)
 *   Byte 1024..end:  data area (items reference offsets within this)
 *
 * This implementation scans eMMC block devices for the RKVS magic at
 * known Rockchip byte offsets. If multiple copies exist, the one with
 * the highest version number is used (Rockchip's write strategy).
 *
 * Copyright (c) 2021, Fuzhou Rockchip Electronics Co., Ltd (original format)
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/etherdevice.h>

#include "vendor_storage.h"
#include "core.h"

/*
 * Known Rockchip vendor storage byte offsets on eMMC.
 *
 * These come from public Rockchip BSP sources and rkbin partition
 * layouts across RK3326/PX30, RK3399, RK3566/RK3568, etc.
 * The vendor storage sits in the reserved area between the loader
 * and the first GPT/Rockchip partition. Exact offset varies by
 * SoC and firmware version — we try all known locations.
 *
 * Each offset has RKVS_NUM_COPIES consecutive copies of RKVS_PART_SIZE
 * bytes (typically 2 × 64 KB). We check both copies and use the one
 * with the higher version number.
 */
static const loff_t rkvs_known_offsets[] = {
	0x300000,	/* 3 MB — PX30/RK3326 BSP default */
	0x3E0000,	/* 3.875 MB — some newer layouts */
	0x400000,	/* 4 MB — seen on some RK3568 */
	0x200000,	/* 2 MB — older RK3288/RK3228 layouts */
	0x100000,	/* 1 MB — fallback for compact layouts */
	0x600000,	/* 6 MB — some RK3399 layouts */
};

/* eMMC block devices to probe. We try both mmcblk0 and mmcblk1
 * since the numbering depends on probe order (SD vs eMMC). */
static const char * const emmc_devices[] = {
	"/dev/mmcblk0",
	"/dev/mmcblk1",
	"/dev/mmcblk2",
};

/*
 * Try to read and validate an RKVS header from a block device at a
 * given byte offset. If valid, search for the WiFi MAC item (ID 2)
 * and copy it to @mac.
 *
 * Returns:
 *   > 0: RKVS version number (success, MAC copied to @mac)
 *     0: valid header but WiFi MAC item not found
 *   < 0: not a valid RKVS header at this offset, or I/O error
 */
static int rkvs_try_offset(struct file *f, loff_t offset, u8 *mac)
{
	struct rkvs_header *hdr;
	u8 *buf;
	ssize_t n;
	loff_t pos;
	u32 version;
	u16 item_num;
	int i, ret = -EINVAL;

	/* Read header + enough data area for MAC (header is 1024 bytes,
	 * MAC is 6 bytes somewhere in the data area — 4 KB covers it) */
	buf = kvmalloc(RKVS_PART_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	pos = offset;
	n = kernel_read(f, buf, RKVS_PART_SIZE, &pos);
	if (n < RKVS_HEADER_SIZE) {
		ret = -EIO;
		goto out;
	}

	hdr = (struct rkvs_header *)buf;

	/* Validate magic */
	if (le32_to_cpu(hdr->tag) != RKVS_TAG) {
		ret = -EINVAL;
		goto out;
	}

	version = le32_to_cpu(hdr->version);
	item_num = le16_to_cpu(hdr->item_num);

	RPU_INFO_SDIO("RKVS: found valid header at offset 0x%llx "
		       "(version=%u, items=%u)\n",
		       offset, version, item_num);

	if (item_num > RKVS_MAX_ITEMS)
		item_num = RKVS_MAX_ITEMS;

	/* Search item table for WiFi MAC */
	for (i = 0; i < item_num; i++) {
		u16 id = le16_to_cpu(hdr->item[i].id);
		u16 item_offset = le16_to_cpu(hdr->item[i].offset);
		u16 item_size = le16_to_cpu(hdr->item[i].size);

		if (id != RKVS_ID_WIFI_MAC)
			continue;

		if (item_size < 6) {
			RPU_ERROR_SDIO("RKVS: WiFi MAC item too small (%u bytes)\n",
				       item_size);
			ret = 0;
			goto out;
		}

		/* Data area starts at offset RKVS_HEADER_SIZE from the
		 * start of the vendor storage copy. Item's offset field
		 * is relative to the data area start. */
		if (RKVS_HEADER_SIZE + item_offset + 6 > n) {
			RPU_ERROR_SDIO("RKVS: WiFi MAC data beyond read range\n");
			ret = -EIO;
			goto out;
		}

		memcpy(mac, buf + RKVS_HEADER_SIZE + item_offset, 6);

		if (!is_valid_ether_addr(mac)) {
			RPU_ERROR_SDIO("RKVS: WiFi MAC %pM is not valid\n", mac);
			ret = 0;
			goto out;
		}

		RPU_INFO_SDIO("RKVS: WiFi MAC from vendor storage: %pM\n", mac);
		ret = (int)version;
		if (ret == 0)
			ret = 1;	/* version 0 → return 1 to signal success */
		goto out;
	}

	/* Header valid but no WiFi MAC item */
	RPU_INFO_SDIO("RKVS: valid header but no WiFi MAC item (id=%d) found\n",
		       RKVS_ID_WIFI_MAC);
	ret = 0;

out:
	kvfree(buf);
	return ret;
}

/*
 * Scan one block device for RKVS at all known offsets.
 * Checks both redundant copies at each offset and uses the one
 * with the highest version.
 *
 * Returns 0 on success (MAC written to @mac), negative on failure.
 */
static int rkvs_scan_device(const char *devpath, u8 *mac)
{
	struct file *f;
	int i, j, ret;
	int best_version = -1;
	u8 candidate[6];

	f = filp_open(devpath, O_RDONLY, 0);
	if (IS_ERR(f))
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(rkvs_known_offsets); i++) {
		/* Try each redundant copy at this base offset */
		for (j = 0; j < RKVS_NUM_COPIES; j++) {
			loff_t off = rkvs_known_offsets[i] +
				     (j * RKVS_PART_SIZE);

			ret = rkvs_try_offset(f, off, candidate);
			if (ret > 0 && ret > best_version) {
				best_version = ret;
				memcpy(mac, candidate, 6);
			}
		}
	}

	filp_close(f, NULL);

	if (best_version > 0) {
		RPU_INFO_SDIO("RKVS: using WiFi MAC %pM (version %d) from %s\n",
			       mac, best_version, devpath);
		return 0;
	}

	return -ENODEV;
}

/*
 * Read WiFi MAC address from Rockchip vendor storage on eMMC.
 *
 * Probes all likely eMMC block devices at known Rockchip vendor
 * storage byte offsets. If found, copies the 6-byte MAC to @mac.
 *
 * Returns 0 on success, -ENODEV if no vendor storage found.
 */
int rkvs_read_wifi_mac(u8 *mac)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(emmc_devices); i++) {
		ret = rkvs_scan_device(emmc_devices[i], mac);
		if (ret == 0)
			return 0;
	}

	RPU_INFO_SDIO("RKVS: no vendor storage found on any eMMC device\n");
	return -ENODEV;
}
