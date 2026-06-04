# rk915

Linux kernel module for the Rockchip RK915 WiFi chipset (802.11b/g/n, 2.4 GHz, 1T1R, SDIO), ported from the Rockchip BSP kernel to mainline Linux 6.18.

The RK915 is found in several rk3326-based handheld gaming devices (R36 Ultra, possibly others). Despite its SDIO vendor ID (`0x0296`) suggesting a Microchip/Wilcomm origin, the WiFi engine is an **Imagination Technologies UCCP**

## Status

**Fully working** on mainline kernel 6.18 (arm64). Tested on the R36 Ultra running self-compiled Batocera v44.

- Association, DHCP, and data transfer confirmed
- Throughput: ~1.5-2.0 MB/s (matches stock console firmware)
- Suspend/resume: power-off on suspend, re-init on resume
- Deterministic MAC address derived from SoC OTP

## Kernel patches

The RK915 requires three MMC subsystem patches, gated behind the `MMC_CAP2_WIFI_RK915` flag so they don't affect other SDIO devices:

1. **`3000-mmc-add-rk915-wifi-cap2-flag.patch`** — adds the `MMC_CAP2_WIFI_RK915` capability flag
2. **`3001-dw_mmc-rk915-cmd52-prv-dat-wait-and-no-lowpwr.patch`** — CMD52 timing fix for RK915's SDIO controller quirks
3. **`3002-sdio-cis-rk915-quirks.patch`** — CIS size and block size quirks

These patches were derived from this commit: https://github.com/lcdyk0517/arkos.bsp.4.4/commit/3e479178fef5903172b22462cb6fb5a2d21f805c#diff-8b739392c760bcc1f84600a7fde00ebfb8fc24bbd353319b01588defa07e7604, I assume it's ported from changes in BSP kernel to mainline.

## Device Tree

Two DT nodes are needed:

### WiFi power sequencing (in SDIO host section)

```dts
wifi-pwrseq {
    compatible = "mmc-pwrseq-simple";
    reset-gpios = <&gpio0 RK_PA2 GPIO_ACTIVE_LOW>;
    post-power-on-delay-ms = <200>;
    power-off-delay-us = <1000>;
};
```

### OOB IRQ node (mandatory — at root level)

```dts
rk915-wifi {
    compatible = "rockchip,rk915";
    interrupt-parent = <&gpio0>;
    interrupts = <RK_PA1 IRQ_TYPE_EDGE_RISING>;
    status = "okay";
};
```

The driver resolves the OOB interrupt from this node via `of_find_compatible_node()` + `of_irq_get()`. An SDIO in-band IRQ fallback path exists but OOB is the intended (and tested) configuration.

## Firmware

The driver loads firmware via direct `filp_open()` / `kernel_read()` (BSP heritage, not `request_firmware()`). Place these files in `/lib/firmware/`:

| File | Required | Description |
|------|----------|-------------|
| `rk915_fw.bin` | **Yes** | Main firmware |
| `rk915_patch.bin` | **Yes** | Patch blob |
| `rk915_cal.bin` | No | RF calibration data |
| `rk915_rf_para.txt` | No | RF parameters |

Firmware files are under `firmware/`

## MAC address

The driver tries these sources in order:

1. **Module parameter** — `insmod rk915.ko mac_addr=AABBCCDDEEFF`
2. **Rockchip vendor storage** — reads from eMMC at known offsets (factory-provisioned)
3. **SoC OTP** — reads the PX30 chip ID via `of_nvmem_cell_get()`, XOR-folds 16 bytes to 6 with salt `"rk915w"` for a deterministic per-device MAC
4. **BSP compatibility** — `rockchip_wifi_mac_addr()` weak symbol
5. **Random** — `eth_random_addr()` as last resort

On the R36 Ultra with no vendor storage provisioned, the OTP path produces a stable MAC across reboots.

## Building

### Out-of-tree

```bash
make -C /path/to/kernel M=$(pwd) modules
```

## Port changelog

All BSP→mainline API changes are documented in [CHANGELOG.md](CHANGELOG.md). Key changes include:

- `complete_and_exit` → `kthread_complete_and_exit`
- `del_timer` / `del_timer_sync` → `timer_delete` / `timer_delete_sync`
- `ieee80211_tx_status` → `ieee80211_tx_status_skb`
- `ieee80211_beacon_get(hw, vif)` → `ieee80211_beacon_get(hw, vif, 0)` (link_id for non-MLO)
- `bss_conf->assoc` → `vif->cfg.assoc`, `bss_conf->chandef` → `bss_conf->chanreq.oper`
- `u64 changed` in `bss_info_changed` callback, `bool suspend` in `stop` callback
- `set_fs` / `get_fs` removal → `kernel_write`
- Header moves (`asm/unaligned.h` → `linux/unaligned.h`, `linux/of.h` explicit include)
- `MODULE_IMPORT_NS()` string syntax
- SDIO in-band IRQ fallback path added
- OOB IRQ resolution via DT (`of_irq_get`)
- Persistent MAC from SoC OTP via nvmem

## Known limitations

- Single device instance only (global state pointers) — fine for embedded use
- Firmware loading uses `filp_open()` instead of `request_firmware()` — works post-rootfs but not from initramfs
- SDIO sleep/clock switching compiled in but not actively tested
- ~40 `-Wmissing-prototypes` warnings from BSP code (non-fatal, suppressed)

## License

GPL-2.0-only
