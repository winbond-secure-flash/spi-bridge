#ifndef BRIDGE_COMMON_H
#define BRIDGE_COMMON_H

#include <linux/ioctl.h>

#define BRIDGE_IOC_MAGIC 'k'
#define MAX_SPI_BUF (256 + 8)  // page size + metadata  


#ifndef PRINT_ERR
#define PRINT_ERR(fmt, ...)   //printf(fmt, ##__VA_ARGS__);  
#endif //PRINT_ERR

#ifndef ASSERT_MSG_RET
#define ASSERT_MSG_RET(cond, err, fmt, ...)     \
{                                               \
    if (!(cond))                                \
    {                                           \
        PRINT_ERR(fmt, ##__VA_ARGS__);          \
        return err;                             \
    }                                           \
}
#endif //ASSERT_MSG_RET

#ifndef ASSERT_MSG
#define ASSERT_MSG(cond, fmt, ...)       \
{                                        \
    if (!(cond))                         \
    {                                    \
        PRINT_ERR(fmt, ##__VA_ARGS__);   \
    }                                    \
}
#endif //ASSERT_MSG

#ifndef ASSERT_MSG_GOTO
#define ASSERT_MSG_GOTO(cond, label, fmt, ...)  \
{                                               \
    if (!(cond))                                \
    {                                           \
        PRINT_ERR(fmt, ##__VA_ARGS__);          \
        goto label;                             \
    }                                           \
}
#endif //ASSERT_MSG_GOTO


struct bridge_transaction {

    unsigned char tx_buf[MAX_SPI_BUF];
    unsigned char rx_buf[MAX_SPI_BUF];

    uint32_t    cmd_len;
    uint32_t    addr_len;
    uint32_t    data_out_len;
    uint32_t    dummy_len;
    uint32_t    data_in_len;

    uint32_t    flags;        // User-defined flags
    uint32_t    status;       // Return status
 
};

#define IOCTL_SEND_SPI   _IOWR(BRIDGE_IOC_MAGIC, 1, struct bridge_transaction)

#define IOCTL_LOCK_BUS   _IO(BRIDGE_IOC_MAGIC, 2)

#define IOCTL_UNLOCK_BUS _IO(BRIDGE_IOC_MAGIC, 3)

#endif
