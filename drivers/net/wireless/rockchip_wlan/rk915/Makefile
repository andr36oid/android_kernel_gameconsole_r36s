BUSIF=SDIO
MY_PWD=$(PWD)
ROM=true
CHIP=RK915

ifeq ($(CONFIG_RK915),)
CONFIG_RK915=m
endif

L_BUSIF=$(shell echo $(BUSIF) | tr A-Z a-z)

NAME     = rk915

EXTRA_CFLAGS += -Idrivers/net/wireless/rockchip_wlan/rk915/inc \
                -Idrivers/net/wireless/rockchip_wlan/rk915/shared

EXTRA_CFLAGS += -DDEBUG

OBJS        =   src/main.o \
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
		src/init.o

## To enable sleep functionality
## we need to enable ROM
ifeq ($(ROM),true)
EXTRA_CFLAGS += -DRPU_SLEEP_ENABLE
#EXTRA_CFLAGS += -DPS_SLEEP_TEST
# Force the RPU firmware to never deep-sleep. With sleep on (the vendor
# default) the RK915 on the R36 Ultra misses keepalives and the AP deauths it
# (802.11 reason 2) in a ~4 s connect/drop loop that never gets usable Wi-Fi.
# No-sleep costs a little idle power but keeps the link up.
EXTRA_CFLAGS += -DRPU_NO_SLEEP_FLAG
#EXTRA_CFLAGS += -DRPU_ENABLE_PS
else
EXTRA_CFLAGS += -DNO_HP_READY_WAR
endif
# BUSIF
ifeq ($(BUSIF),SDIO)
EXTRA_CFLAGS += -DHAL_SDIO
OBJS	   +=	src/sdio.o
endif

#EXTRA_CFLAGS += -DSKIP_DL_FW
ifeq ($(CHIP),RK915)
EXTRA_CFLAGS += -DRK915
endif

#EXTRA_CFLAGS += -DRK3036_DONGLE
#EXTRA_CFLAGS += -DSTA_AP_COEXIST
EXTRA_CFLAGS += -DSDIO_CLOCK_SWITCH
EXTRA_CFLAGS += -Wno-array-bounds -Wno-error=array-bounds

GCC_VER_49 := $(shell echo `$(CC) -dumpversion | cut -f1-2 -d.` \>= 4.9 | bc )
ifeq ($(GCC_VER_49),1)
EXTRA_CFLAGS += -Wno-date-time  # Fix compile error && warning on gcc 4.9 and later
#EXTRA_CFLAGS += -Wno-error=date-time
endif

obj-$(CONFIG_RK915) += $(NAME).o
$(NAME)-objs= $(OBJS)

ifneq ($(KERNELRELEASE),)

#EXTRA_LDFLAGS += --strip-debug

else

all:
	@echo "BUSIF is $(L_BUSIF)"
	@echo "Compiling for $(BUSIF)"
	@make -C $(KROOT) M=$(MY_PWD) modules
clean:
	@make -C $(KROOT) M=$(MY_PWD) clean
	rm -f src/*.o

endif

