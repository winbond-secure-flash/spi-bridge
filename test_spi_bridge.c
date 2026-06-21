


#ifdef USE_SPI_DEV

#include "spi_dev_platform.c"

#elif defined(USE_KERNEL_IOCTL)

#include "ioctl_platform.c"

#else
#error "unknown spi platform to use"
#endif



int main(int argc, char** argv) {

    int fd, ret;
    QLIB_BUS_MODE_T fmt = QLIB_BUS_MODE_1_1_1;
        

    fd = open_spi_fd();
    if (fd < 0) {
        perror("Failed to open device");
        return 1;
    }
    
    //  Perform Transaction (Read JEDEC ID)
    printf("1. Sending JEDEC ID Command (0x9F)...\n");

    unsigned char jdec_tx_data[] = {0x9F};
    unsigned char jdec_rx_data[3];

    ret = PLAT_SPI_WriteReadTransaction(&fd, fmt, 0, jdec_tx_data, 1, 0, 0, 0, jdec_rx_data, 3);
    if (ret)
    {
        perror("Failed spi transfer");
    }
   
    printf("   Data Read: 0x%02X 0x%02X 0x%02X\n", jdec_rx_data[0], jdec_rx_data[1], jdec_rx_data[2]);


    // Perform Transaction (Read JEDEC ID)
    printf("2. Sending GET SSR Command (0xAO)...\n");

    unsigned char ssr_tx_data[] = {0xA0};
    unsigned char ssr_rx_data[4];
    
    ret = PLAT_SPI_WriteReadTransaction(&fd, fmt, 0, ssr_tx_data, 1, 0, 0, 8, ssr_rx_data, 4);
    if (ret )
    {
        perror("Failed spi transfer");
    }
  
  
    printf("   Data Read: 0x%02X 0x%02X 0x%02X 0x%02X\n", ssr_rx_data[0], ssr_rx_data[1], ssr_rx_data[2], ssr_rx_data[3]);

    
    close(fd);
    return 0;
}
