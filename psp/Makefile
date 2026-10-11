# pspcap.prx — kernel plugin, loaded via ms0:/seplugins/game.txt
#
#   make                 build pspcap.prx (log enabled)
#   make LOG=0           build without the ms0 log
#   make GIT_REV=abc1234 embed a build id (CI passes the commit sha)

TARGET = pspcap
OBJS   = src/main.o src/log.o src/usb.o src/display.o src/audio.o src/session.o src/SystemCtrlForKernel.o

LOG     ?= 1
GIT_REV ?= local

INCDIR  = ../common
CFLAGS  = -Os -G0 -Wall -Wextra -fno-pic -std=gnu99 \
          -DPSPCAP_LOG=$(LOG) -DPSPCAP_GIT_REV=\"$(GIT_REV)\"
ASFLAGS = $(CFLAGS)
LDFLAGS = -nostartfiles
LIBS    = -lpspusbbus_driver -lpspusb_driver

BUILD_PRX      = 1
PRX_EXPORTS    = exports.exp
USE_KERNEL_LIBS = 1
PSP_FW_VERSION = 660

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak
