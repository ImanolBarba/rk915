/*
 * Rockchip vendor storage reader
 *
 * Reads factory-provisioned data (WiFi MAC, BT MAC, serial, etc.)
 * from the Rockchip vendor storage area on eMMC.
 *
 * Based on the GPL-licensed rk_vendor_storage.c from public Rockchip
 * BSP kernels (drivers/soc/rockchip/rk_vendor_storage.c).
 *
 * Copyright (c) 2021, Fuzhou Rockchip Electronics Co., Ltd (original format)
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _RK_VENDOR_STORAGE_H_
#define _RK_VENDOR_STORAGE_H_

#include <linux/types.h>

/* Vendor storage header magic: "RKVS" in little-endian */
#define RKVS_TAG		0x524B5653

/* Standard Rockchip vendor item IDs */
#define RKVS_ID_SN		1
#define RKVS_ID_WIFI_MAC	2
#define RKVS_ID_LAN_MAC	3
#define RKVS_ID_BT_MAC		4

/* Vendor storage geometry */
#define RKVS_MAX_ITEMS		126
#define RKVS_HEADER_SIZE	1024	/* sizeof(struct rkvs_header) */
#define RKVS_PART_SIZE		(64 * 1024)	/* 64 KB per copy */
#define RKVS_NUM_COPIES		2

/*
 * Item descriptor in the vendor storage tag table.
 * offset is relative to the data area (immediately after the header).
 */
struct rkvs_item {
	__le16 id;
	__le16 offset;
	__le16 size;
	__le16 flag;
};

/*
 * Vendor storage header. Sits at byte 0 of each vendor storage copy.
 * Total size: 4+4+2+2+2+2 + 126*8 = 1024 bytes.
 * Data area follows immediately at offset 1024.
 */
struct rkvs_header {
	__le32 tag;		/* RKVS_TAG */
	__le32 version;		/* Monotonically increasing; highest = current */
	__le16 next_index;	/* Write cursor (wear leveling) */
	__le16 item_num;	/* Number of populated items in table */
	__le16 free_offset;	/* Next free byte in data area */
	__le16 free_size;	/* Remaining bytes in data area */
	struct rkvs_item item[RKVS_MAX_ITEMS];
};

/*
 * Read WiFi MAC address from Rockchip vendor storage.
 *
 * Scans eMMC block devices for the RKVS header at known Rockchip
 * sector offsets, then extracts the WiFi MAC (item ID 2).
 *
 * @mac: output buffer, must be at least 6 bytes
 * Returns 0 on success, negative errno on failure.
 */
int rkvs_read_wifi_mac(u8 *mac);

#endif /* _RK_VENDOR_STORAGE_H_ */
