#!/usr/bin/env bash
# Build spi_flash_bridge.ko for AST2700 (ASPEED FMC, spi-aspeed-smc) with a W77T01NW boot flash.
# Same idea as rp4_make.sh: only make variables, no change to the module code.
#
#   MEMSPI_IGNORES_DUMMY_ON_ZERO_ADDR_PHASE=1  spi-aspeed-smc ignores dummy cycles when there is
#                                              no address phase (e.g. A0h GET_SSR) -> workaround on
#   TARGET_JEDEC_VALUE={0xEF,0x8E,0x1B}        W77T01NW JEDEC ID (default in code is W77Q128JV EF 4A 18)
#
# AST2700 is normally cross-built (Yocto / OpenBMC). Run from the kernel/ directory:
#   KERNEL_SRC=<kernel build dir> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- ./ast2700_make.sh
# In OpenBMC the same variables are passed by the spi-flash-bridge recipe.
# Without KERNEL_SRC it builds against the running kernel (native build).
set -x
make -C "${KERNEL_SRC:-/lib/modules/$(uname -r)/build}" M="$(pwd)" \
     MEMSPI_IGNORES_DUMMY_ON_ZERO_ADDR_PHASE=1 \
     'TARGET_JEDEC_VALUE={0xEF,0x8E,0x1B}' \
     modules
