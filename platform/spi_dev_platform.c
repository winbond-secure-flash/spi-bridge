
/*---------------------------------------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------------------------------------*/
/*                                                INCLUDES                                                 */
/*---------------------------------------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------------------------------------*/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

#include "qlib_platform.h"


#define PRINT_ERR(fmt, ...)   printf(fmt, ##__VA_ARGS__);  

#include "bridge_common.h" 



#define SPI_DEVICE "/dev/spidev0.0"
#define SPI_MODE    SPI_MODE_0
#define SPI_SPEED   5000000  // 5 MHz

    
#define SPI_BITS    8




// this is needed when core reset is done by qlib at the end of the fw update sequence
void CORE_RESET(void){}// dummy

int open_spi_fd()
{
    int ret, fd;

    // Open SPI device
    fd = open(SPI_DEVICE, O_RDWR);
    if (fd < 0)
    {
        PRINT_ERR("Error: Failed to open %s\n", SPI_DEVICE);
        return -1;
    }

    // Set SPI mode
    ret = ioctl(fd, SPI_IOC_WR_MODE, &(uint8_t){SPI_MODE});
    if (ret < 0)
    {
        PRINT_ERR("Error: Failed to set SPI mode\n");
        close(fd);
        return -1;
    }

    // Set SPI speed
    ret = ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &(uint32_t){SPI_SPEED});
    if (ret < 0)
    {
        PRINT_ERR("Error: Failed to set SPI speed\n");
        close(fd);
        return -1;
    }
    
    return fd;
}

void close_spi_fd(int fd)
{
    close(fd);
}

int PLAT_SPI_WriteReadTransaction(const void*     userData,
                                  QLIB_BUS_MODE_T format,
                                  uint32_t        flags,
                                  const uint8_t*  dataOutStream,
                                  uint32_t        cmdSize,
                                  uint32_t        addressSize,
                                  uint32_t        dataOutSize,
                                  uint32_t        dummyCycles,
                                  uint8_t*        dataIn,
                                  uint32_t        dataInSize)
{
    int ret = 0;
    uint32_t total_tx_len = cmdSize + addressSize + dataOutSize;
    uint32_t dummy_bytes = dummyCycles / 8;
    uint32_t total_rx_len = total_tx_len + dummy_bytes + dataInSize;
    uint8_t rx_buf[MAX_SPI_BUF];  // Stack buffer, but spidev ioctl will copy it to kernel DMA-safe buffer
    struct spi_ioc_transfer xfer;

    ASSERT_MSG_RET(userData, -1, "Error: Plaftform handle is NULL\n");
    
    int fd = *((int *)userData);
     
    // Validate inputs
    ASSERT_MSG_RET(total_tx_len <= MAX_SPI_BUF && total_rx_len <= MAX_SPI_BUF, -1,
        "Error: Transfer length exceeds %d\n", MAX_SPI_BUF);

    // Limitation of the spi-mem
    ASSERT_MSG_RET(dataInSize <= 0 || (addressSize + dataOutSize) <= 8, -1,
        "Error mem-spi limit: Read transfer %d (addressSize + dataOutSize) length exceeds 8\n", addressSize + dataOutSize);    

    
    // Prepare SPI transfer - pass user buffer directly
    // The kernel will copy_from_user internally to DMA-safe buffers
    memset(&xfer, 0, sizeof(xfer));
    xfer.tx_buf = (unsigned long)dataOutStream;  // User buffer - kernel handles copy_from_user
    xfer.rx_buf = (unsigned long)rx_buf;         // Stack buffer - kernel handles copy_to_user
    xfer.len = total_rx_len;
    xfer.speed_hz = SPI_SPEED;
    xfer.bits_per_word = SPI_BITS;

    // Execute SPI transfer
    ret = ioctl(fd, SPI_IOC_MESSAGE(1), &xfer);
    if (ret < 0)
    {
        PRINT_ERR("Error: SPI transfer failed\n");

        return -1;
    }

    // Copy RX data to output buffer (skip dummy bytes and TX echo)
    if (dataInSize > 0 && dataIn)
    {
        memcpy(dataIn, rx_buf + total_tx_len + dummy_bytes, dataInSize);
    }


    return 0;
}




