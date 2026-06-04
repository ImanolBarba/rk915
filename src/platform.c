/*
 * rk915 platform abstraction
 *
 * Power sequencing via GPIO0_A2, MMC card rescan via mmc_detect_change(),
 * and OOB IRQ registration. Replaces Rockchip BSP rfkill-wlan hooks
 * (rockchip_wifi_power / rockchip_wifi_set_carddetect) with mainline APIs.
 *
 * Stock DTS has:
 *   wireless-wlan { WIFI,poweren_gpio = <&gpio0 2 0x00>; }
 * Power enable is GPIO0_A2, active HIGH.
 *
 * The MMC host (dw_mmc SDIO controller) must NOT have mmc-pwrseq — we handle
 * power cycling here so it happens immediately before firmware download,
 * not at boot time (the chip's boot ROM times out of firmware-accept mode
 * within seconds of power-on).
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_func.h>

#include "core.h"
#include "if_io.h"
#include "platform.h"

/* Track whether we're using SDIO in-band IRQ vs OOB GPIO IRQ */
static bool using_sdio_inband_irq;

extern int hal_irq_handler(struct hal_priv *p);

/* Fallback if no DT node found — GPIO0_A2 = bank 0 * 32 + 2. */
#define WIFI_POWER_GPIO_FALLBACK	2

static struct mmc_host *wifi_mmc;
static int wifi_power_gpio = -1;

/**
 * resolve_wifi_gpio - resolve WiFi power GPIO from DT (once)
 *
 * Looks for a "rockchip,rk915" compatible node with "power-gpios" property.
 * Falls back to hardcoded GPIO0_A2 if not found in DT.
 */
static void resolve_wifi_gpio(void)
{
	struct device_node *np;

	if (wifi_power_gpio >= 0)
		return;

	np = of_find_compatible_node(NULL, NULL, "rockchip,rk915");
	if (np) {
		wifi_power_gpio = of_get_named_gpio(np, "power-gpios", 0);
		of_node_put(np);
	}

	if (wifi_power_gpio < 0) {
		RPU_INFO_MAIN("no DT power-gpios, falling back to GPIO %d\n",
			      WIFI_POWER_GPIO_FALLBACK);
		wifi_power_gpio = WIFI_POWER_GPIO_FALLBACK;
	} else {
		RPU_INFO_MAIN("WiFi power GPIO %d from DT\n", wifi_power_gpio);
	}
}

/**
 * rk915_set_mmc_host - cache the MMC host from SDIO probe
 *
 * Called from sdio_probe() once the SDIO func is available.
 * After this, find_wifi_mmc() returns immediately without DT lookup.
 */
void rk915_set_mmc_host(struct mmc_host *host)
{
	wifi_mmc = host;
}

/**
 * find_wifi_mmc - locate the MMC host for the WiFi SDIO slot
 *
 * First checks the cached pointer (set by sdio_probe via rk915_set_mmc_host).
 * On cold start (before first probe), walks the DT to find the dw-mshc
 * controller marked as SDIO-only (no-sd + no-mmc properties), then finds
 * the mmc_host registered as its child device.
 */
static int match_mmc_child(struct device *dev, const void *data)
{
	/* mmc_host class devices are named "mmc0", "mmc1", etc. */
	return !strncmp(dev_name(dev), "mmc", 3);
}

static struct mmc_host *find_wifi_mmc(void)
{
	struct device_node *np = NULL;
	struct platform_device *pdev;
	struct device *child;

	if (wifi_mmc)
		return wifi_mmc;

	/* Find the SDIO-only dw-mshc controller: has no-sd + no-mmc in DT */
	while ((np = of_find_compatible_node(np, NULL, "rockchip,rk3288-dw-mshc"))) {
		if (of_property_read_bool(np, "no-sd") &&
		    of_property_read_bool(np, "no-mmc"))
			break;
	}
	if (!np) {
		RPU_ERROR_MAIN("can't find SDIO dw-mshc node in DT\n");
		return NULL;
	}

	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev) {
		RPU_ERROR_MAIN("no platform device for SDIO dw-mshc\n");
		return NULL;
	}

	/* mmc_host registers its class_dev as a child of the controller */
	child = device_find_child(&pdev->dev, NULL, match_mmc_child);
	put_device(&pdev->dev);
	if (!child) {
		RPU_ERROR_MAIN("no mmc_host child on SDIO controller\n");
		return NULL;
	}

	wifi_mmc = container_of(child, struct mmc_host, class_dev);
	put_device(child);

	return wifi_mmc;
}

void rk915_rescan_card(unsigned insert)
{
	struct mmc_host *host = find_wifi_mmc();

	if (!host) {
		RPU_ERROR_MAIN("can't find WiFi MMC host for rescan\n");
		return;
	}
	RPU_INFO_MAIN("triggering MMC rescan (insert=%u)\n", insert);
	mmc_detect_change(host, 0);
}

void rk915_poweron(void)
{
	int ret;

	resolve_wifi_gpio();

	ret = gpio_request(wifi_power_gpio, "wifi-power");
	if (ret) {
		RPU_ERROR_MAIN("can't request GPIO %d (%d) — is mmc-pwrseq removed from DTS?\n",
			       wifi_power_gpio, ret);
		return;
	}

	/* Full power cycle: off -> 100ms -> on -> 200ms
	 * Puts chip back into boot ROM firmware-download mode. */
	gpio_direction_output(wifi_power_gpio, 0);
	mdelay(100);
	gpio_direction_output(wifi_power_gpio, 1);
	mdelay(200);
	gpio_free(wifi_power_gpio);

	RPU_INFO_MAIN("WiFi chip power cycled (GPIO %d)\n", wifi_power_gpio);
}

void rk915_poweroff(void)
{
	int ret;

	resolve_wifi_gpio();

	ret = gpio_request(wifi_power_gpio, "wifi-power");
	if (ret)
		return;

	gpio_direction_output(wifi_power_gpio, 0);
	gpio_free(wifi_power_gpio);

	RPU_INFO_MAIN("WiFi chip powered off (GPIO %d)\n", wifi_power_gpio);
}

static irqreturn_t hal_interrupt(int irq, void *dev_id)
{
	hal_irq_handler(hpriv);
	return IRQ_HANDLED;
}

/* SDIO in-band IRQ handler wrapper — sdio_claim_irq() expects
 * void (*handler)(struct sdio_func *), not irqreturn_t. */
static void hal_sdio_interrupt(struct sdio_func *func)
{
	hal_irq_handler(hpriv);
}

void rk915_irq_enable(int enable)
{
	/*if (enable) {
		enable_irq(hpriv->io_info->irq);
	} else {
		disable_irq(hpriv->io_info->irq);
	}*/
}

int rk915_register_irq(struct host_io_info *host)
{
	int ret;

	if (host->irq > 0) {
		/* OOB GPIO IRQ from device tree */
		ret = devm_request_irq(host->dev, host->irq, hal_interrupt,
				       IRQF_TRIGGER_RISING, "rk915", hpriv);
		if (ret == 0) {
			using_sdio_inband_irq = false;
			host->irq_request = true;
			RPU_INFO_MAIN("OOB IRQ %d registered\n", host->irq);
		}
	} else {
		/* No OOB IRQ — use SDIO in-band interrupt (DAT[1] line) */
		struct sdio_func *func = (struct sdio_func *)host->priv_data;

		sdio_claim_host(func);
		ret = sdio_claim_irq(func, hal_sdio_interrupt);
		sdio_release_host(func);
		if (ret == 0) {
			using_sdio_inband_irq = true;
			host->irq_request = true;
			RPU_INFO_MAIN("SDIO in-band IRQ registered\n");
		} else {
			RPU_ERROR_MAIN("sdio_claim_irq failed: %d\n", ret);
		}
	}

	return ret;
}

int rk915_free_irq(struct host_io_info *host)
{
	if (host->irq_request) {
		if (using_sdio_inband_irq) {
			struct sdio_func *func = (struct sdio_func *)host->priv_data;

			sdio_claim_host(func);
			sdio_release_irq(func);
			sdio_release_host(func);
		} else {
			devm_free_irq(host->dev, host->irq, hpriv);
		}
		host->irq_request = false;
	}

	return 0;
}

int rk915_bus_register_driver(void)
{
	return rk915_sdio_register_driver();
}

void rk915_bus_unregister_driver(void)
{
	rk915_sdio_unregister_driver();
}

int rk915_platform_bus_init(struct host_io_info *phost)
{
	if (!phost->bus_init)
		return rk915_sdio_init(phost);
	else
		return 0;
}

int rk915_platform_bus_rec_init(struct host_io_info *phost)
{
	return rk915_sdio_recovery_init(phost);
}

int rk915_platform_bus_deinit(struct host_io_info *phost)
{
	if (phost->bus_init)
		return rk915_sdio_deinit(phost);
	else
		return 0;
}
