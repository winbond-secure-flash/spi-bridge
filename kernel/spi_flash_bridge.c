#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>
#include <linux/pm_runtime.h>
#include <linux/ktime.h>
#include <linux/delay.h>

#define SPI_BRIDGE_VERSION "0.2"

//*****************************************************************************
// Static configuration tweaks, that can be defined outside of this file
//*****************************************************************************

// kernel module name that this module will create.
#ifndef DRIVER_NAME
#define DRIVER_NAME "spi_flash_bridge"
#endif

// flash that module will look for among all spi devices
#ifndef TARGET_JEDEC_VALUE
#define TARGET_JEDEC_VALUE {0xEF, 0x4a, 0x18} // default JEDEC IS of w77q128jv
#endif

// Working spi parameters for chip. Set to spi device since its original values 
// set by DT can not fit the chip. For instance rp4 bcm2835_spi sets to 125MHz,
// which is too high. Set WORKING_TARGET_SPI_SPEED to 0 to disable setting the
// spi interface to working frequency, or set explcit frequency value
#ifndef WORKING_TARGET_SPI_SPEED
#define WORKING_TARGET_SPI_SPEED 0  //5000000(working speed for rp4/spi_dev)
#endif

// Some spi controllers will not let proprietary ctag value pass in the address  
// phase. So we move ctag value to data phase.
// 0 - disabled
// 1 - enabled 
#ifndef MOVE_CTAG_TO_DATA_PHASE
#define MOVE_CTAG_TO_DATA_PHASE 0
#endif

// 0 - do NOT use spi_mem interface, use legacy spi only. 
// 1 - use mem_spi interface if available, if not available use legacy spi 
#ifndef USE_MEM_SPI
#define USE_MEM_SPI 1
#endif

// Some mem_spi interfaces ignore dummy cycles on zero data phase, in such 
// case workaround will be - read more data as dummy bytes and drop them later
// 0 - workaround is disables 
// 1 - workaround is enabled 
// f.i. on AST2700 this value is 1
#ifndef MEMSPI_IGNORES_DUMMY_ON_ZERO_ADDR_PHASE
#define MEMSPI_IGNORES_DUMMY_ON_ZERO_ADDR_PHASE 0
#endif 

//*****************************************************************************
// PRINT_ERR uses KERN_ERR; PRINT_INFO uses KERN_INFO. Console visibility:
//   dmesg -n 4  # errors and more severe messages
//   dmesg -n 6  # errors, info, and more severe messages
//*****************************************************************************
#define PRINT_ERR(fmt, ...)  printk(KERN_ERR "[" DRIVER_NAME "] " fmt, ##__VA_ARGS__)
#define PRINT_INFO(fmt, ...) printk(KERN_INFO "[" DRIVER_NAME "] " fmt, ##__VA_ARGS__)

//*****************************************************************************
// PRINT_DBG when CONFIG_DYNAMIC_DEBUG is available:
// Enable with:
//   echo 'file spi_flash_bridge.c +p' > /sys/kernel/debug/dynamic_debug/control
// Disable with:
//   echo 'file spi_flash_bridge.c -p' > /sys/kernel/debug/dynamic_debug/control
// If dynamic debug is unavailable, build with DEBUG to enable PRINT_DBG.
//*****************************************************************************
#define PRINT_DBG(fmt, ...)  pr_debug("[" DRIVER_NAME "] " fmt, ##__VA_ARGS__)

//*****************************************************************************
// Include common file that contains ioctl values 
//*****************************************************************************

#include "bridge_common.h"

//*****************************************************************************
// Module parameters
//*****************************************************************************

#define TARGET_JEDEC_LEN 3
static unsigned int jedec_id[TARGET_JEDEC_LEN] = TARGET_JEDEC_VALUE; 
static int jedec_id_count = TARGET_JEDEC_LEN; 
static u8 target_jedec_id[TARGET_JEDEC_LEN];

module_param_array(jedec_id, uint, &jedec_id_count, 0644);
MODULE_PARM_DESC(jedec_id, "Expected 3-byte JEDEC ID (Mfr, Type, Cap)");

static int bridge_validate_params(void)
{
    ASSERT_MSG_RET( jedec_id_count == TARGET_JEDEC_LEN, -EINVAL,
                    "JEDEC ID input error: Must be %d values, provided %d.\n", TARGET_JEDEC_LEN, jedec_id_count);
   
    for (int i = 0; i < TARGET_JEDEC_LEN; i++)
    {
        ASSERT_MSG_RET(jedec_id[i] <= 0xFF, -EINVAL, "JEDEC ID input error: 0x%X is not a byte\n", jedec_id[i]);
        target_jedec_id[i] = (u8)jedec_id[i];
    }

    PRINT_INFO("Target JEDEC ID set to %02X,%02X,%02X.\n", target_jedec_id[0], target_jedec_id[1], target_jedec_id[2]);
    return 0;
}

//*****************************************************************************
// Globals
//*****************************************************************************

struct bridge_context {
    struct spi_device *spi;
    struct bridge_transaction trans ____cacheline_aligned;
    bool bus_is_locked;
    ktime_t lock_t0;                // when LOCK_BUS was taken
    struct mutex context_lock;
};

// Global pointer for the found device
static struct spi_device *target_spi_dev = NULL;
static u32 target_original_mode;
static u32 target_original_speed;
static dev_t dev_num;
static struct cdev my_cdev;
static struct class *my_class;

// inter process lock
static struct
{
    struct mutex lock;              // Protects this global state 
    struct bridge_context *owner;   // Which context currently holds the SPI bus
} global_bus_mgr;


//*****************************************************************************
// spi_XXX_send functions: spi_mem_send for mem-spi, and spi_msg_send for legacy
//*****************************************************************************

static bool uses_mem_spi(struct spi_controller *ctlr)
{
    return ctlr->mem_ops && USE_MEM_SPI;
}

static unsigned char dummy_tx_buf[MAX_SPI_BUF] = {0};
static int spi_msg_send(      struct spi_device *spi,
                              bool               is_bus_locked,
                              const uint8_t*     tx,
                              uint32_t           cmd_len,
                              uint32_t           addr_len,
                              uint32_t           data_out_len,
                              uint32_t           dummy_len,
                              uint8_t*           data_in,
                              uint32_t           data_in_len)
{
    struct spi_message m;
    int ret = 0;
    struct spi_transfer t_tx =      {.tx_buf = tx,              .len = cmd_len + addr_len + data_out_len };
    struct spi_transfer t_dummy =   {.tx_buf = dummy_tx_buf,    .len = dummy_len};
    struct spi_transfer t_rx =      {.rx_buf = data_in,         .len = data_in_len};

    ASSERT_MSG_RET(spi, -ENODEV, "Flash handle is not initialized\n");

    spi_message_init(&m);

    if ((cmd_len + addr_len + data_out_len) > 0)
    {
       spi_message_add_tail(&t_tx, &m);
    }
    
    if (dummy_len > 0)
    {
       spi_message_add_tail(&t_dummy, &m);
    }
       
    if (data_in_len > 0)
    {
       spi_message_add_tail(&t_rx, &m);
    }

    ret = is_bus_locked ? spi_sync_locked(spi, &m) : spi_sync(spi, &m);
    ASSERT_MSG_RET(0 == ret, ret, "spi_sync%s failure, status %d\n", is_bus_locked ? "_locked" : "", ret);

    PRINT_DBG("spi_sync%s success: cmd=%02x, addr_len=%u, dummy=%u, data_in_len=%u, data_out_len=%u\n",
                is_bus_locked ? "_locked" : "",  tx[0], addr_len, dummy_len, data_in_len, data_out_len);
        
    return 0;
}




static int spi_mem_exec_op_locked(struct spi_mem *mem, const struct spi_mem_op *op)
{
    struct spi_controller *ctlr = mem->spi->controller;

    // bus_lock_mutex + io_mutex are already held by IOCTL_LOCK_BUS
    ASSERT_MSG_RET(ctlr->mem_ops && ctlr->mem_ops->exec_op, -EOPNOTSUPP,
                   "Controller has no exec_op\n");
    
    return ctlr->mem_ops->exec_op(mem, op);
}




// TODO: ensure cmdSize is one at upper layers                                                 
static int spi_mem_send(      struct spi_device *spi,
                              bool               is_bus_locked,
                              const uint8_t*     tx,
                              uint32_t           cmd_len,
                              uint32_t           addr_len,
                              uint32_t           data_out_len,
                              uint32_t           dummy_len,
                              uint8_t*           data_in,
                              uint32_t           data_in_len)
{
    struct spi_mem mem;
    struct spi_mem_op op;
    u64 addr_val = 0;
    const u8 *p_addr;
    const u8 *p_data_out;
    u32 effective_addr_len;
    u32 i;
    int ret;
    bool discard_dummy_bytes = false;

    ASSERT_MSG_RET(spi && tx, -EINVAL, "Null pointers provided\n");
    ASSERT_MSG_RET(cmd_len == 1, -EINVAL, "Opcode size(%d) should be 1 byte\n", cmd_len);
    

    
    // dataOutStream = [ Opcode (1) | Address (addressSize) | DataOut (dataOutSize) ]
    u8 opcode = tx[0];
    p_addr = tx + cmd_len;
    p_data_out = tx + cmd_len + addr_len;

    memset(&mem, 0, sizeof(mem));
    mem.spi = spi;
    
    // CASE 1: READ OPERATION (or Read with params)
    // We map 'address' + 'dataOut' bytes into the SPI_MEM ADDRESS phase.
    if (data_in_len > 0)
    {
        effective_addr_len = addr_len + data_out_len;
        ASSERT_MSG_RET(effective_addr_len <= 8, -EINVAL, "Read Op: Addr + DataOut (%d) > 8\n", effective_addr_len); 

        // Pack Address + DataOut into u64 addr_val 
        // Pack the actual Address bytes
        for (i = 0; i < addr_len; i++)
        {
            addr_val = (addr_val << 8) | (p_addr[i] & 0xFF);
        }

        // Pack the DataOut bytes (treated as extended address)
        for (i = 0; i < data_out_len; i++)
        {
            addr_val = (addr_val << 8) | (p_data_out[i] & 0xFF);
        }

        if (MEMSPI_IGNORES_DUMMY_ON_ZERO_ADDR_PHASE != 0 && effective_addr_len == 0 && dummy_len > 0)
        {

            ASSERT_MSG_RET(data_in_len + dummy_len <= MAX_SPI_BUF, -EINVAL,
                           "Read Op: data(%u) + dummy(%u) > %d\n", data_in_len, dummy_len, MAX_SPI_BUF);

            op = (struct spi_mem_op)SPI_MEM_OP(
                SPI_MEM_OP_CMD(opcode, 1),
                SPI_MEM_OP_NO_ADDR,
                SPI_MEM_OP_NO_DUMMY,
                SPI_MEM_OP_DATA_IN(data_in_len + dummy_len, data_in, 1)
            );
            
            discard_dummy_bytes = true;
        }
        else
        {
            op = (struct spi_mem_op)SPI_MEM_OP(
                SPI_MEM_OP_CMD(opcode, 1),
                SPI_MEM_OP_ADDR(effective_addr_len, addr_val, 1),
                SPI_MEM_OP_DUMMY(dummy_len, 1),
                SPI_MEM_OP_DATA_IN(data_in_len, data_in, 1)
            );
        }
    } 
    // CASE 2: WRITE OPERATION
    // We keep Address separate and use DataOut as the payload.
    else 
    {
        ASSERT_MSG_RET(dummy_len == 0, -EINVAL, "Dummy(%d) should be 0 when no read\n", dummy_len);
            
        effective_addr_len = addr_len;
        ASSERT_MSG_RET(effective_addr_len <= 8, -EINVAL, "Write Op: Address (%d) > 8\n", effective_addr_len);
        
        // Pack only the Address bytes
        for (i = 0; i < addr_len; i++) {
            addr_val = (addr_val << 8) | (p_addr[i] & 0xFF);
        }

        /* Construct Write Operation */
        op = (struct spi_mem_op)SPI_MEM_OP(
            SPI_MEM_OP_CMD(opcode, 1),
            SPI_MEM_OP_ADDR(effective_addr_len, addr_val, 1),
            SPI_MEM_OP_NO_DUMMY,
            SPI_MEM_OP_DATA_OUT(data_out_len, p_data_out, 1)
        );
        
    }

    ASSERT_MSG_RET (spi_mem_supports_op(&mem, &op), -EINVAL,
                    "Transaction rejected by controller: cmd=%02x, addr_len=%u, dummy=%u, data_len=%u\n",
                     opcode, op.addr.nbytes,
                     dummy_len, (data_in_len ? data_in_len : data_out_len));
                        
    ret = is_bus_locked ? spi_mem_exec_op_locked(&mem, &op) : spi_mem_exec_op(&mem, &op);

    ASSERT_MSG_RET(0 == ret, ret, "spi_mem_exec_op%s failure, status %d\n", is_bus_locked ? "_locked" : "", ret);

    // drop the bytes clocked during the dummy phase
    if (MEMSPI_IGNORES_DUMMY_ON_ZERO_ADDR_PHASE != 0 && discard_dummy_bytes)
    {
        memmove(data_in, data_in + dummy_len, data_in_len);
    }
   
    if (data_in_len > 0)
    {
        PRINT_DBG("op %02X addr[%u]=%llx dummy %u IN %u : %*ph\n",
                   opcode, op.addr.nbytes, (unsigned long long)op.addr.val, dummy_len,
                   data_in_len, min_t(int, data_in_len, 16), data_in);
    }
    else
    {
        PRINT_DBG("op %02X addr[%u]=%llx OUT %u : %*ph\n",
                   opcode, op.addr.nbytes, (unsigned long long)op.addr.val,
                   data_out_len, min_t(int, data_out_len, 16), p_data_out);
    }
                    
    return 0;
}



static int kernel_spi_bus_lock(struct spi_controller *ctlr)
{
    if (uses_mem_spi(ctlr))
    {
        if (ctlr->auto_runtime_pm)
        {
            int ret = pm_runtime_resume_and_get(ctlr->dev.parent);
            ASSERT_MSG_RET(ret >= 0, ret, "Failed to power controller: %d\n", ret);
        }

        mutex_lock(&ctlr->bus_lock_mutex);
        mutex_lock(&ctlr->io_mutex);
    }
    else
    {
        spi_bus_lock(ctlr);
    }
    
    return 0;
}

static void kernel_spi_bus_unlock(struct spi_controller *ctlr)
{
    if (uses_mem_spi(ctlr))
    {
        mutex_unlock(&ctlr->io_mutex);
        mutex_unlock(&ctlr->bus_lock_mutex);

        if (ctlr->auto_runtime_pm)
        {
            pm_runtime_put(ctlr->dev.parent);
        }
    }
    else
    {
        spi_bus_unlock(ctlr);
    }
}


static int kernel_spi_send(          struct spi_device *spi,
                                     bool               is_bus_locked,
                                     const uint8_t*     tx,
                                     uint32_t           cmd_len,
                                     uint32_t           addr_len,
                                     uint32_t           data_out_len,
                                     uint32_t           dummy_len,
                                     uint8_t*           data_in,
                                     uint32_t           data_in_len)
{


    ASSERT_MSG_RET((cmd_len + addr_len + data_out_len) <= MAX_SPI_BUF, -EINVAL, "Tx size exceeds %d\n", MAX_SPI_BUF);
    ASSERT_MSG_RET(data_in_len <= MAX_SPI_BUF,                         -EINVAL, "Rx size exceeds %d\n", MAX_SPI_BUF);

    if (MOVE_CTAG_TO_DATA_PHASE == 0)
    {
        if (tx[0] == 0xa1 && addr_len > 0)
        {
            data_out_len += addr_len;
            addr_len = 0;
        }
    }
    
    return  uses_mem_spi(spi->controller) ? 
            // flow for NXP, ST QSPI, etc. which use fsl-qspi
            spi_mem_send(spi, is_bus_locked, tx, cmd_len, addr_len, data_out_len, dummy_len, data_in, data_in_len) :
            // flow for those controllers which do not support spi-mem
            spi_msg_send(spi, is_bus_locked, tx, cmd_len, addr_len, data_out_len, dummy_len, data_in, data_in_len);
}


//***************************************************************************************************************
// Working spi parameters for chip. Set to spi device since its original values can not fit. For instance rp4 
// bcm2835_spi sets to 125MHz, which is too high. set WORKING_TARGET_SPI_SPEED to 0 to disable 
// the working parameters setting.
//***************************************************************************************************************
static int enforce_working_spi_params(struct spi_device *spi, u32 *orig_mode, u32 *orig_speed)
{
    if (WORKING_TARGET_SPI_SPEED == 0)
    {
        return 0;
    }

    // Save original parameters
    *orig_mode = spi->mode;
    *orig_speed = spi->max_speed_hz;

    PRINT_INFO("Original parameters are mode:%d and speed:%d \n", spi->mode, spi->max_speed_hz);
    
    // Set new parameters
    spi->mode = SPI_MODE_0;
    spi->max_speed_hz = WORKING_TARGET_SPI_SPEED;
    spi->mode &= ~SPI_LSB_FIRST; 

    PRINT_INFO("Working parameters are mode:%d and speed:%d \n", spi->mode, spi->max_speed_hz);

    int ret = spi_setup(spi);
    if (ret < 0) // restore if fail
    {
        spi->mode = *orig_mode;
        spi->max_speed_hz = *orig_speed;
        PRINT_ERR("Failed to setup SPI parameters: %d\n", ret);
        return ret;
    }
    return 0;
}

static void restore_original_spi_params(struct spi_device *spi, u32 orig_mode, u32 orig_speed)
{    
    if (WORKING_TARGET_SPI_SPEED == 0)
    {
       return;
    }

    spi->mode = orig_mode;
    spi->max_speed_hz = orig_speed;    
    spi_setup(spi);
}


//*****************************************************************************
// JEDEC ID flash look up
//*****************************************************************************

static int find_target_device(struct device *dev, void *data)
{
    struct spi_device *spi = to_spi_device(dev);
    int ret;
    u32 original_mode, original_speed;
    u8 *tx_cmd = NULL;
    u8 *rx_id_buf = NULL;
    
    ASSERT_MSG_RET(spi, 0, "spi device provided is NULL\n");
    
    if (target_spi_dev) // already found
    {
        return 1;
    }
    
    // Allocate DMA-safe buffers
    tx_cmd = kzalloc(1, GFP_KERNEL | GFP_DMA);
    ASSERT_MSG_RET(tx_cmd, 0, "Failed to allocate tx_cmd buffer\n");
    
    rx_id_buf = kzalloc(3, GFP_KERNEL | GFP_DMA);
    if (!rx_id_buf)
    {
        kfree(tx_cmd);
        PRINT_ERR("Failed to allocate rx_id_buf buffer\n");
        return 0;
    }
    
    ret = enforce_working_spi_params(spi, &original_mode, &original_speed);
    if (ret != 0)
    {
        PRINT_ERR("enforce_working_spi_params failed, status %d\n", ret);
        kfree(tx_cmd);
        kfree(rx_id_buf);
        return 0;
    }

    *tx_cmd = 0x9F;
    ret = kernel_spi_send(spi, false, tx_cmd, 1, 0, 0, 0, rx_id_buf, 3);
    if (ret != 0)
    {
        PRINT_ERR("GET JDEC failed for %s. Error: %d\n", dev_name(&spi->dev), ret);
        restore_original_spi_params(spi, original_mode, original_speed);
        kfree(tx_cmd);
        kfree(rx_id_buf);
        return 0;
    }
        
    if (0 == memcmp(target_jedec_id, rx_id_buf, 3))
    {        
        PRINT_INFO("Dev: %s, ID: 0x%02X%02X%02X - Found flash via %s!\n",
            dev_name(&spi->dev), rx_id_buf[0], rx_id_buf[1], rx_id_buf[2],
            uses_mem_spi(spi->controller) ? "MEM_SPI" : "SPI_LEGACY");
        target_spi_dev = spi_dev_get(spi);
        target_original_mode = original_mode;
        target_original_speed = original_speed;
    }
    else
    {
        PRINT_INFO("Device: %s, ID: 0x%02X%02X%02X - Mismatch!\n",
                    dev_name(&spi->dev), rx_id_buf[0], rx_id_buf[1], rx_id_buf[2]);
        restore_original_spi_params(spi, original_mode, original_speed);
    }
    
    // Clean up allocated buffers
    kfree(tx_cmd);
    kfree(rx_id_buf);
    
    return target_spi_dev ? 1 : 0;
}


//*****************************************************************************
// ioctl exposed
//*****************************************************************************
static long bridge_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct bridge_context *ctx = file->private_data;
    int ret = 0;

    mutex_lock(&ctx->context_lock);

    switch (cmd) 
    {
    case IOCTL_LOCK_BUS:

        // This process 
        if (ctx->bus_is_locked)
        {
            ret = -EBUSY;
            goto out;
        }
        

        // protection from another process
        mutex_lock(&global_bus_mgr.lock);
        if (global_bus_mgr.owner != NULL)
        {
            mutex_unlock(&global_bus_mgr.lock);
            ret = -EBUSY; 
            PRINT_INFO("Hardware is busy with another app.\n");
            goto out;
        }
        
        // Claim it globally 
        global_bus_mgr.owner = ctx;
        mutex_unlock(&global_bus_mgr.lock);

        ret = kernel_spi_bus_lock(ctx->spi->controller);
        if (ret)
        {
            mutex_lock(&global_bus_mgr.lock);
            global_bus_mgr.owner = NULL;
            mutex_unlock(&global_bus_mgr.lock);
            goto out;
        }
        
        ctx->bus_is_locked = true;
        ctx->lock_t0 = ktime_get();
        PRINT_INFO("Bus LOCKED globally by this process (%s).\n",
                   bridge_is_mem_path(ctx->spi) ? "spi-mem mutexes" : "spi_bus_lock");
        break;

        
    case IOCTL_UNLOCK_BUS:

        // This process
        if (!ctx->bus_is_locked) {
            ret = -EINVAL;
            goto out;
        }

        // Release Hardware
        kernel_spi_bus_unlock(ctx->spi->controller);
                
        // Release Global Claim
        mutex_lock(&global_bus_mgr.lock);
        global_bus_mgr.owner = NULL;
        mutex_unlock(&global_bus_mgr.lock);

        ctx->bus_is_locked = false;
        PRINT_INFO("Bus UNLOCKED globally, held %lld us.\n",
                   (long long)ktime_us_delta(ktime_get(), ctx->lock_t0));
        break;

    case IOCTL_SEND_SPI:
        
        memset(&ctx->trans, 0, sizeof(struct bridge_transaction));   
        if (copy_from_user(&ctx->trans, (void __user *)arg, sizeof(struct bridge_transaction)))
        {
            ret = -EFAULT;
            goto out;
        }
        
        ret = kernel_spi_send(ctx->spi, ctx->bus_is_locked, ctx->trans.tx_buf, 
                             ctx->trans.cmd_len, ctx->trans.addr_len, ctx->trans.data_out_len,
                             ctx->trans.dummy_len, ctx->trans.rx_buf, ctx->trans.data_in_len);

        ASSERT_MSG(0 == ret, "kernel_spi_transfer returned non-zero status %d\n", ret);
        
        ctx->trans.status = ret;
        if (copy_to_user((void __user *)arg, &ctx->trans, sizeof(struct bridge_transaction)))
        {
            ret = -EFAULT;
            goto out;
        }
                
        
        break;

    default:
        ret = -ENOTTY;
        PRINT_ERR("Unsupported ioctl %d\n", cmd);
    }

out:
        
    mutex_unlock(&ctx->context_lock);
    return ret;
}


//*****************************************************************************
// release the locks if the user application closes the file descriptor (or crashes)
//*****************************************************************************

static int bridge_open(struct inode *inode, struct file *file)
{
    struct bridge_context *ctx;

    ASSERT_MSG_RET(target_spi_dev, -ENODEV, "No flash to work with found\n");

    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL | GFP_DMA);
    ASSERT_MSG_RET(ctx, -ENOMEM, "kzalloc failed\n");

    ctx->spi = target_spi_dev;
    mutex_init(&ctx->context_lock);
    file->private_data = ctx;

    return 0;
}

static int bridge_release(struct inode *inode, struct file *file)
{
    struct bridge_context *ctx = file->private_data;

    if (ctx->bus_is_locked)
    {
        PRINT_ERR("App closed/crashed while holding lock (held %lld us). Cleaning up.\n",
                  (long long)ktime_us_delta(ktime_get(), ctx->lock_t0));
        
        kernel_spi_bus_unlock(ctx->spi->controller);
        ctx->bus_is_locked = false;

        // Clear global owner
        mutex_lock(&global_bus_mgr.lock);
        if (global_bus_mgr.owner == ctx)
        {
            global_bus_mgr.owner = NULL;
        }
        mutex_unlock(&global_bus_mgr.lock);
    }

    kfree(ctx);
    return 0;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open    = bridge_open,
    .unlocked_ioctl = bridge_ioctl,
    .release = bridge_release, 
};

static int __init bridge_init(void)
{
    PRINT_INFO("spi bridge version " SPI_BRIDGE_VERSION " init ");

    int ret = bridge_validate_params();
    ASSERT_MSG_RET(0 == ret, ret, "bridge_validate_params failed. Error: %d\n", ret);

    bus_for_each_dev(&spi_bus_type, NULL, NULL, find_target_device);

    ASSERT_MSG_RET( target_spi_dev, -ENODEV, "JEDEC ID 0x%02X%02X%02X not found\n",
                    target_jedec_id[0], target_jedec_id[1], target_jedec_id[2]);

    mutex_init(&global_bus_mgr.lock);
    global_bus_mgr.owner = NULL;

    ret = alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
    if (ret)
    {
        PRINT_ERR("alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    cdev_init(&my_cdev, &fops);
    ret = cdev_add(&my_cdev, dev_num, 1);
    if (ret)
    {
        PRINT_ERR("cdev_add failed: %d\n", ret);
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }

    my_class = class_create(DRIVER_NAME);
    if (IS_ERR(my_class))
    {
        ret = PTR_ERR(my_class);
        PRINT_ERR("class_create failed: %d\n", ret);
        cdev_del(&my_cdev);
        unregister_chrdev_region(dev_num, 1);
        my_class = NULL;
        return ret;
    }

    struct device *device = device_create(my_class, NULL, dev_num, NULL, DRIVER_NAME);
    if (IS_ERR(device))
    {
        ret = PTR_ERR(device);
        PRINT_ERR("device_create failed: %d\n", ret);
        class_destroy(my_class);
        cdev_del(&my_cdev);
        unregister_chrdev_region(dev_num, 1);
        my_class = NULL;
        return ret;
    }

    return 0;
}

static void __exit bridge_exit(void)
{
    PRINT_INFO("spi bridge version " SPI_BRIDGE_VERSION " finit ");
    mutex_lock(&global_bus_mgr.lock);
    if (global_bus_mgr.owner != NULL)
    {
        PRINT_ERR("Module unloading while bus was locked\n");
    }
    mutex_unlock(&global_bus_mgr.lock);
    
    device_destroy(my_class, dev_num);
    class_destroy(my_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev_num, 1);

    if (target_spi_dev)
    {
        restore_original_spi_params(target_spi_dev, target_original_mode, target_original_speed);
    }

    spi_dev_put(target_spi_dev);
    target_spi_dev = NULL;
}


module_init(bridge_init);
module_exit(bridge_exit);
MODULE_LICENSE("GPL");
