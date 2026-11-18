# Spi callback for QLIB in Linux OS

QLIB's integration requires to implement SPI tunneling callback 
PLAT_SPI_WriteReadTransaction(), so security-related spi commands can reach the W77Q chip.
This project show several ways to implement such function in Linux user mode environment.

* [Using spi_dev interface](#using-spi_dev-interface)  
* [Using spi_bridge kernel module](#using-spi_bridge-kernel-module)


## Using spi_dev interface

When spi connection es exposed via SPI_DEV interface the callback implementation is 
straightforward - it will use linux built in SPI_DEV ioctl to perform spi transactions.
You might need to change only the number of the spi device ( default is /dev/spidev0.0 ).

If you connect w77q chip to pins breakout this spi is almost sure exposed via SPI_DEV. 
We test this on raspberry pi 4 with w77q128jv chip connected to SPI0 CS0, therefore SPI_DEV 
ioctl will use /dev/spidev0.0.

Files:
* platform/spi_dev_platform.c - the spi callback implementation via spi_dev ioctl

### Let's simple test it

Before connecting callback implementation to qlib, we can do simple test with two commands 
"GET JEDEC ID" and "GET_SECURE_STATUS_REGISTER" and see if we get expected results.

```shell
app@rp4:~/proj/spi_kernel_bridge $ make 
Usage:
  make spi_dev       - Build 'test' for SPI device
  make kernel_ioctl  - Build 'test' for Kernel IOCTL
  make clean         - Remove binary
app@rp4:~/proj/spi_kernel_bridge $ make spi_dev
------------------------------------------------
Building test for: SPI DEVICE
------------------------------------------------
gcc -I. -I./platform -DUSE_SPI_DEV -o test test_spi_bridge.c
app@rp4:~/proj/spi_kernel_bridge $ sudo ./test 
Platform init over spi dev
1. Sending JEDEC ID Command (0x9F)...
   Data Read: 0xEF 0x4A 0x18
2. Sending GET SSR Command (0xAO)...
   Data Read: 0x00 0x00 0x10 0xF2
app@rp4:~/proj/spi_kernel_bridge $ 
```

When running the test we can see that JEDEC ID EF 4A 18 is retrieved correctly 
( w77q128jv chip ) along ans SSR value makes sense.


## Using spi_bridge kernel module

Sometimes chip is exposed via SPI_NOR/MTD interface. In this case spi_dev will not be available and we must look for another way to implement the spi callback and this is where kernel spi_bridge module comes to help. The idea is that kernel module will find w77q flash and expose ioctl function to user mode. Files:

* kernel/spi_flash_bridge.c - the kernel module code
* bridge_common.h - common header for kernel module and user application
* platform/ioctl_platform.c - userland callback implementation via ioctl to kernel module

### General points

* **Flash lookup according to JEDEC ID passed to module upon init**   
* Kernel module on initialization will send each spi device JEDEC ID(0x9f) command and compare the response to the expected flash chip parameters. If they are equal, our flash is found.

* **Userland ioctls are exposed**   
Ioctl is used to implement qlib spi callback. 


* **Default target jedec id**   
If no parameter passed to the module default one will be used: EF 4A 18 ( w77q128jv).

 
### How to build kernel module

Build and insert:

```shell
app@rp4:~/proj/spi_kernel_bridge/kernel $ make
make -C /lib/modules/6.12.47+rpt-rpi-v8/build M=/home/app/proj/spi_kernel_bridge/kernel modules
make[1]: Entering directory '/usr/src/linux-headers-6.12.47+rpt-rpi-v8'
  CC [M]  /home/app/proj/spi_kernel_bridge/kernel/spi_flash_bridge.o
  MODPOST /home/app/proj/spi_kernel_bridge/kernel/Module.symvers
  LD [M]  /home/app/proj/spi_kernel_bridge/kernel/spi_flash_bridge.ko
make[1]: Leaving directory '/usr/src/linux-headers-6.12.47+rpt-rpi-v8'
app@rp4:~/proj/spi_kernel_bridge/kernel $ sudo insmod spi_flash_bridge.ko 
```

The kernel Makefile also accepts optional compile-time configuration overrides. Leave an option
unset to use the default from `kernel/spi_flash_bridge.c`. For example:

```shell
make DRIVER_NAME=spi_flash_bridge_test WORKING_TARGET_SPI_SPEED=5000000
make 'TARGET_JEDEC_VALUE={0xEF,0x4A,0x18}' MOVE_CTAG_TO_DATA_PHASE=1
make SHARED_INCLUDE_DIR=/path/to/shared/headers
```

For a Raspberry Pi 4 setup that needs a 5 MHz working SPI speed, the example wrapper script
`kernel/rp4_make.sh` runs the kernel build with `WORKING_TARGET_SPI_SPEED=5000000`. Run it from
the `kernel/` directory:

```shell
./rp4_make.sh
```

This is a platform-specific example; edit or copy it for other platform-specific Makefile
overrides.

`SHARED_INCLUDE_DIR` specifies where shared headers such as `bridge_common.h` are located. It
defaults to the project root (one directory above `kernel/`); set it when the header is stored
elsewhere.

Other supported configuration variables are:

- `DRIVER_NAME` — module and character-device name. Defaults to `spi_flash_bridge`.
- `TARGET_JEDEC_VALUE` — three-byte JEDEC ID used to identify the target flash. Defaults to
  `{0xEF, 0x4a, 0x18}`.
- `WORKING_TARGET_SPI_SPEED` — maximum SPI speed in Hz applied to the target during initialization.
  `0` leaves the configured speed and mode unchanged; the source default is `0`.
- `MOVE_CTAG_TO_DATA_PHASE` — set to `1` to move the `0xA1` CTAG byte from the address phase to
  the data-out phase for controllers that cannot transmit it in the address phase. Defaults to `0`.
- `USE_MEM_SPI` — set to `1` to use the SPI-mem interface when the controller supports it,
  otherwise use legacy SPI transfers. Set to `0` to always use legacy SPI. Defaults to `1`.
- `MEMSPI_IGNORES_DUMMY_ON_ZERO_ADDR_PHASE` — set to `1` to enable the SPI-mem workaround for
  controllers that ignore dummy cycles when the address phase is absent. Defaults to `0`.

We did not pass any JEDEC ID during insertion, so default JEDEC ID was used which is EF 4A 18 ( w77q128jv).
Since this is a flash connected we will can confirm it is found via kernel log

```shell
app@rp4:~ $ dmesg | tail -n 5
[  430.768649] [spi_flash_bridge] spi bridge version 0.1 init 
[  430.768665] [spi_flash_bridge] Target JEDEC ID set to EF,4A,18.
[  430.768687] [spi_flash_bridge] Original parameters are mode:4 and speed:125000000 
[  430.768693] [spi_flash_bridge] Working parameters are mode:0 and speed:5000000 
[  430.768725] [spi_flash_bridge] Dev: spi0.0, ID: 0xEF4A18 - Found flash via SPI_LEGACY!
```

Let's see what would happen if we pass another JEDEC ID.
```shell
app@rp4:~/proj/spi_kernel_bridge/kernel $ sudo rmmod spi_flash_bridge.ko 
app@rp4:~/proj/spi_kernel_bridge/kernel $ sudo insmod spi_flash_bridge.ko jedec_id=0xEF,0x4A,0x17
insmod: ERROR: could not insert module spi_flash_bridge.ko: No such device
app@rp4:~/proj/spi_kernel_bridge/kernel $ 
```
You can see that module does not find the flash, and that is the reason module will not load and
return **No such device** status. If we look to kernel log we can see that target JEDEC ID set
to EF, 4A, 17 and which which devices were scanned.

```shell
pp@rp4:~ $ dmesg | tail -n 10
[66180.777934] [spi_flash_bridge] Target JEDEC ID set to EF, 4A, 17.
[66180.778036] [spi_flash_bridge] Scanned device: spi0.0, ID: 0xEF4A18 - Mismatch!
[66180.778089] [spi_flash_bridge] Scanned device: spi0.1, ID: 0x000000 - Mismatch!
[66180.778103] [spi_flash_bridge] JEDEC ID 0xEF4A17 not found
app@rp4:~ $ 
```

Now let's run simple test from userspace and see if can use ioctl:
1) same JEDEC ID 2) SSR value ( Secure status register) . 

```shell
app@rp4:~/proj/spi_kernel_bridge $ make
Usage:
  make spi_dev       - Build 'test' for SPI device
  make kernel_ioctl  - Build 'test' for Kernel IOCTL
  make clean         - Remove binary
app@rp4:~/proj/spi_kernel_bridge $ make kernel_ioctl 
------------------------------------------------
Building test for: KERNEL IOCTL
------------------------------------------------
gcc -I. -I./platform -DUSE_KERNEL_IOCTL -o test test_spi_bridge.c
app@rp4:~/proj/spi_kernel_bridge $ sudo ./test 
Platform init over kernel ioctl
1. Sending JEDEC ID Command (0x9F)...
   Data Read: 0xEF 0x4A 0x18
2. Sending GET SSR Command (0xAO)...
   Data Read: 0x40 0x00 0x00 0xF2
app@rp4:~/proj/spi_kernel_bridge $  
```
We can see how the JEDEC ID and SSR values are sucessfully retrieved via spi transaction to flash.  


## Troubleshoot
### I get dmesg output like this: Transaction rejected by controller: ...

There can me several reasons underlying controller rejects the transaction:
1. size of the data buffer qlib tries to send exceeds the maximum spi controller supports.  

In that case adjust qlib definitions QLIB_MAX_SPI_INPUT_SIZE/QLIB_MAX_SPI_OUTPUT_SIZE to values that will satisfy your spi controller size limit.


## MISC: Project structure  
```shell
── spi_kernel_bridge
   ├── README.md                        => this file
   ├── bridge_common.h                  => common header for kernel module and user application
   ├── test_spi_bridge.h                => C test to check spi callback
   ├── Makefile
   └── platform
        ├── qlib_platform.c             => spi callback interface required by qlib
        ├── ioctl_platform.c            => spi callback interface via kernel ioctl
        └── spi_dev_platform.c          => spi callback interface via spi_dev ioctl
   └── kernel
        ├── Makefile
        └── spi_flash_bridge.c           => spi_bridge kernel module code       
   
```
