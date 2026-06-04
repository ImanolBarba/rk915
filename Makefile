#
# RK915 WiFi driver — ported from Rockchip BSP 4.4 to mainline 6.x
#
# Out-of-tree build: make -C /path/to/kernel M=$(pwd) modules
# Batocera package: see package/batocera/wifi/rk915/
#

CONFIG_RK915 ?= m

# Include paths (relative to this directory)
ccflags-y += -I$(src)/inc -I$(src)/shared

# Feature flags
ccflags-y += -DDEBUG
ccflags-y += -DHAL_SDIO
ccflags-y += -DRK915
ccflags-y += -DRPU_SLEEP_ENABLE
ccflags-y += -DSDIO_CLOCK_SWITCH

# Suppress harmless warnings from BSP code
ccflags-y += -Wno-array-bounds -Wno-error=array-bounds

obj-$(CONFIG_RK915) += rk915.o

rk915-objs := \
	src/main.o \
	src/hal.o \
	src/umac_if.o \
	src/rpu_if.o \
	src/tx.o \
	src/rx.o \
	src/beacon.o \
	src/p2p.o \
	src/pktgen.o \
	src/procfs.o \
	src/utils.o \
	src/vif.o \
	src/wow.o \
	src/soc.o \
	src/hal_io.o \
	src/platform.o \
	src/firmware.o \
	src/init.o \
	src/sdio.o \
