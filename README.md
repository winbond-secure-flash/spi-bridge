# Spi callback for QLIB in Linux OS

QLIB's integration requires to implement SPI tunneling callback 
PLAT_SPI_WriteReadTransaction(), so security-related spi commands can reach the W77Q chip.
This project show several ways to implement such function in Linux user mode environment.

1. [Using spi_dev interface](#using-spi_dev-interface)  
2. [Using spi_bridge kernel module](#using-spi_bridge-kernel-module)


## 1. Using spi_dev interface

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


## 2. Using spi_bridge kernel module {#spi_bridge_kernel}

Sometimes chip is exposed via SPI_NOR/MTD interface. In this case spi_dev will not be available and we must look for another way to implement the spi callback and this is where kernel spi_bridge module comes to help. The idea is that kernel module will find w77q flash and expose ioctl function to user mode. Files:

* kernel/spi_flash_bridge.c - the kernel module code
* bridge_common.h - common header for kernel module and user application
* platform/ioctl_platform.c - userland callback implementation via ioctl to kernel module

### General points

* **Flash lookup according to JEDEC ID passed to module upon init**   
* Kernel module on initialization will send each spi device JEDEC ID(0x9f) command and compare the response to the expected flash chip parameters. If they are equal, our flash is found.

* **Userland ioctls are exposed**   
Ioctl is used to implement qlib spi callback. 

* **Working spi parameters are applied during usage**   
It turns out that sometimes linux will apply maximum spi speed possible for the devices,
and this number will not fit for our spi nor flash. For instance rp4 uses bcm2835_spi and
sets speed to 125MHz. To solve that module sets speed to confirmed working 5MHZ 
and spi mode single, see functions : enforce_working_spi_params / restore_original_spi_params.
Set **WORKING_TARGET_SPI_SPEED** to 0 to disable the working parameters setting and keeping 
what ever protocol driver set before.

* **Debugging**   
Uncomment PRINT_DBG macro definition to see every spi transaction in kernel log

* **Default target jedec id**   
If no parameter passed to the module default one will be used: EF 4A 18 ( w77q128jv).

### How to build kernel module  

Build and insert

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
We did not pass any jdec id during insertion, so default JEDEC ID was used which is EF 4A 18 ( w77q128jv).
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

Now let's run simple test from userspace and see if can get fusing ioctl:
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
