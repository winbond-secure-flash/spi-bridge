obj-m += spi_flash_bridge.o

# Optional compile-time overrides for spi_flash_bridge.c.
# Leave a variable unset to use the default defined in the source file.
# SHARED_INCLUDE_DIR defaults to the project root (where bridge_common.h lives);
# override it if the shared header is stored elsewhere.
#
# Examples:
#   make DRIVER_NAME=spi_flash_bridge_test WORKING_TARGET_SPI_SPEED=5000000
#   make SHARED_INCLUDE_DIR=/path/to/shared/headers
#   make WORKING_TARGET_SPI_SPEED=5000000
#   make MOVE_CTAG_TO_DATA_PHASE=1 USE_MEM_SPI=1
#   make MEMSPI_IGNORES_DUMMY_ON_ZERO_ADDR_PHASE=1
#   make 'TARGET_JEDEC_VALUE={0xEF,0x4a,0x18}'

SHARED_INCLUDE_DIR ?= $(src)/..
DRIVER_NAME ?=
TARGET_JEDEC_VALUE ?=
WORKING_TARGET_SPI_SPEED ?=
MOVE_CTAG_TO_DATA_PHASE ?=
USE_MEM_SPI ?=
MEMSPI_IGNORES_DUMMY_ON_ZERO_ADDR_PHASE ?=

ccflags-y += -I$(SHARED_INCLUDE_DIR)

ifneq ($(DRIVER_NAME),)
ccflags-y += -DDRIVER_NAME=\"$(DRIVER_NAME)\"
endif

ifneq ($(TARGET_JEDEC_VALUE),)
ccflags-y += -DTARGET_JEDEC_VALUE='$(TARGET_JEDEC_VALUE)'
endif

ifneq ($(WORKING_TARGET_SPI_SPEED),)
ccflags-y += -DWORKING_TARGET_SPI_SPEED=$(WORKING_TARGET_SPI_SPEED)
endif

ifneq ($(MOVE_CTAG_TO_DATA_PHASE),)
ccflags-y += -DMOVE_CTAG_TO_DATA_PHASE=$(MOVE_CTAG_TO_DATA_PHASE)
endif

ifneq ($(USE_MEM_SPI),)
ccflags-y += -DUSE_MEM_SPI=$(USE_MEM_SPI)
endif

ifneq ($(MEMSPI_IGNORES_DUMMY_ON_ZERO_ADDR_PHASE),)
ccflags-y += -DMEMSPI_IGNORES_DUMMY_ON_ZERO_ADDR_PHASE=$(MEMSPI_IGNORES_DUMMY_ON_ZERO_ADDR_PHASE)
endif

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
