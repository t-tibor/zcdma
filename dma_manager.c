/**
 * Copyright (C) 2021 Xilinx, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

/* DMA Proxy
 *
 * This module is designed to be a small example of a DMA device driver that is
 * a client to the DMA Engine using the AXI DMA / MCDMA driver. It serves as a proxy
 * for kernel space DMA control to a user space application.
 *
 * A zero copy scheme is provided by allowing user space to mmap a kernel allocated
 * memory region into user space, referred to as a set of channel buffers. Ioctl functions 
 * are provided to start a DMA transfer (non-blocking), finish a DMA transfer (blocking) 
 * previously started, or start and finish a DMA transfer blocking until it is complete.
 * An input argument which specifies a channel buffer number (0 - N) to be used for the
 * transfer is required.
 *
 * By default the kernel memory allocated for user space mapping is going to be 
 * non-cached at this time. Non-cached memory is pretty slow for the application.
 * A h/w coherent system for MPSOC has been tested and is recommended for higher
 * performance applications. 
 *
 * Hardware coherency requires the following items in the system as documented on the 
 * Xilinx wiki and summarized below::
 *   The AXI DMA read and write channels AXI signals must be tied to the correct state to
 *    generate coherent transactions.
 *   An HPC slave port on MPSOC is required
 *   The CCI of MPSOC must be initialized prior to the APU booting Linux
 *   A dma-coherent property is added in the device tree for the proxy driver.
 *
 * There is an associated user space application, dma_proxy_test.c, and dma_proxy.h
 * that works with this device driver.
 *
 * The hardware design was tested with an AXI DMA / MCDMA  with scatter gather and
 * with the transmit channel looped back to the receive channel. It should
 * work with or without scatter gather as the scatter gather mentioned in the 
 * driver is only at the s/w framework level rather than in the hw.
 *
 * This driver is character driver which creates devices that user space can
 * access for each DMA channel, such as /dev/dma_proxy_rx and /dev/dma_proxy_tx.
 * The number and names of channels are taken from the device tree.
 * Multiple instances of the driver (with multiple IPs) are also supported.

 * An internal test mode is provided to allow it to be self testing without the 
 * need for a user space application and this mode is good for making bigger
 * changes to this driver.
 *
 * This driver is designed to be simple to help users get familiar with how to 
 * use the DMA driver provided by Xilinx which uses the Linux DMA Engine. 
 *
 * To use this driver a node must be added into the device tree.  Add a 
 * node similar to the examples below adjusting the dmas property to match the
 * name of the AXI DMA / MCDMA node.
 * 
 * The dmas property contains pairs with the first of each pair being a reference
 * to the DMA IP in the device tree and the second of each pair being the
 * channel of the DMA IP. For the AXI DMA IP the transmit channel is always 0 and
 * the receive is always 1. For the AXI MCDMA IP the 1st transmit channel is
 * always 0 and receive channels start at 16 since there can be a maximum of 16
 * transmit channels. Each name in the dma-names corresponds to a pair in the dmas
 * property and is only a logical name that allows user space access to the channel
 * such that the name can be any name as long as it is unique.
 *
 *	For h/w coherent systems with MPSoC, the property dma-coherent can be added
 * to the node in the device tree. 
 * 
 * Example device tree nodes: 
 *
 * For AXI DMA with transmit and receive channels with a loopback in hardware
 * 
 * dma_proxy {
 *   compatible ="xlnx,dma_proxy";
 *   dmas = <&axi_dma_1_loopback 0  &axi_dma_1_loopback 1>;
 *   dma-names = "dma_proxy_tx", "dma_proxy_rx";
 * };
 *
 * For AXI DMA with only the receive channel
 * 
 * dma_proxy2 {
 *   compatible ="xlnx,dma_proxy";
 *   dmas = <&axi_dma_0_noloopback 1>;
 *   dma-names = "dma_proxy_rx_only";
 * };
 *
 * For AXI MCDMA with two channels 
 *
 * dma_proxy3 {
 *   compatible ="xlnx,dma_proxy";
 *   dmas = <&axi_mcdma_0 0  &axi_mcdma_0 16 &axi_mcdma_0 1 &axi_mcdma_0 17> ;
 *   dma-names = "dma_proxy_tx_0", "dma_proxy_rx_0", "dma_proxy_tx_1", "dma_proxy_rx_1";
 * };
 */

// Override the fmt string preprocessor, so that every
// log message contains the module name.
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt


#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/of_dma.h>
#include <linux/ioctl.h>
#include <linux/uaccess.h>

#include <zcdma.h>

MODULE_LICENSE("GPL");

// ------------------ Local constants ------------------
#define DRIVER_NAME 			"dma_manager"
#define MAX_NAME_LENG			32
#define ERROR 					-1
#define TIMEOUT_DEFAULT_MSECS	3000


// ------------------ Local types ------------------
struct dma_frontend {
    // frontend parameters
    char name[MAX_NAME_LENG];			    /* channel name */

    // Used DMA channel
    enum dma_transfer_direction direction;	/* DMA_MEM_TO_DEV or DMA_DEV_TO_MEM */
    struct dma_chan* dma_channel;           /* dma support */

    // Zerocopy backend
    struct zcdma* zcdma;

    // Char device API
    dev_t           dev_node;
    struct cdev     cdev;
    struct device*  char_device;
};


struct dma_manager {
    struct platform_device*         platform_device;
    int                             frontend_count;
    const char**                    frontend_names;
    struct dma_frontend*            frontends;
};



// ------------------ Local functions ------------------
static int dma_manager_probe(struct platform_device *pdev);
static void dma_manager_remove(struct platform_device *pdev);
static int __init dma_manager_init(void);
static void __exit dma_manager_exit(void);



// ------------------ Global variables ------------------
/**
 * @brief Global sysfs level device class.
 * This class will be used for all the devices created
 * for the character device interface.
 * This class is created upon module loading.
 * 
 * 
 */
static struct class* dma_char_device_class = NULL;


// TODO
/* The following module parameter controls if the internal test runs when the module is inserted.
 * Note that this test requires a transmit and receive channel to function and uses the first
 * transmit and receive channnels when multiple channels exist.
 */
static unsigned internal_test = 0;
module_param(internal_test, int, S_IRUGO);



// ------------------ Function definitions ------------------

/* Open the device file and set up the data pointer to the proxy channel data for the
 * proxy channel such that the ioctl function can access the data structure later.
 */
static int local_open(struct inode *ino, struct file *file)
{
    file->private_data = container_of(ino->i_cdev, struct dma_frontend, cdev);

    return 0;
}

/* Close the file and there's nothing to do for it
 */
static int release(struct inode *ino, struct file *file)
{
#if 0
    struct dma_frontend *pchannel_p = (struct dma_frontend *)file->private_data;
    struct dma_device *dma_device = pchannel_p->channel_p->device;

    /* Stop all the activity when the channel is closed assuming this
     * may help if the application is aborted without normal closure
     * This is not working and causes an issue that may need investigation in the 
     * DMA driver at the lower level.
     */
    dma_device->device_terminate_all(pchannel_p->channel_p);
#endif
    return 0;
}


static ssize_t read(struct file *file, char __user *userbuf, size_t count, loff_t *f_pos)
{
    int read_size = 0;
    struct dma_frontend* frontend = (struct dma_frontend *)file->private_data;

    pr_debug("DMA read API is called with parameters: userbuf=0x%08p, count=%d, offset=%lld.\n", 
                                                                        userbuf,
                                                                        count,
                                                                        *f_pos);

    if (DMA_DEV_TO_MEM != frontend->direction)
    {
        pr_err("Can't read, '%s' is a TX device\n", frontend->name);
        return -EINVAL;
    }

    read_size = zcdma_read(frontend->zcdma, userbuf, count);	
    pr_debug("zcdma_read return: %d.\n", read_size);
    if (read_size <= 0)
    {
        pr_err("Can't read() on channel '%s', no data and timeout or error occurred.\n", frontend->name);
        return -EPERM;
    }
    
    return (ssize_t)read_size;
}


static ssize_t write(struct file *file, const char __user *userbuf, size_t count, loff_t *f_pos)
{
    int write_size = 0;
    struct dma_frontend *frontend = (struct dma_frontend *)file->private_data;

    pr_debug("DMA write API is called with parameters: userbuf=0x%08p, count=%d, offset=%lld.\n", 
                                                                        userbuf,
                                                                        count,
                                                                        *f_pos);

    if (DMA_MEM_TO_DEV != frontend->direction)
    {
        pr_err("Can't write, '%s' is an RX device.\n", frontend->name);
        return -EINVAL;
    }

    write_size = zcdma_write(frontend->zcdma, userbuf, count);
    pr_debug("zcdma_write return: %d.\n", write_size);
    if (write_size <= 0)
    {
        pr_err("Can't write() on channel '%s', no data and timeout or error occurred.\n", frontend->name);
        return -EPERM;
    }
    
    return (ssize_t)write_size;
}


static struct file_operations dma_zero_file_ops = {
    .owner    = THIS_MODULE,
    .open     = local_open,
    .read     = read,
    .write    = write,
    .release  = release
};


/**
 * @brief Create a character device interface for the given dma zero channel.
 * 
 * @param frontend Channel to create a char device interface for.
 * @return int 0 when no error occurs, error code otherwise 
 */
static int cdevice_init(struct dma_frontend* frontend)
{
    int error_code;

    /* Allocate a major/minor chardev number region for 
     the new character device.
     */
    error_code = alloc_chrdev_region(&frontend->dev_node, 0, 1, DRIVER_NAME);
    if (error_code)
    {
        pr_err("Unable to get a char device number.\n");
        return error_code;
    }

    /* Initialize the cdev structure then register the character 
     * device into the kernel.
     */
    cdev_init(&frontend->cdev, &dma_zero_file_ops);
    frontend->cdev.owner = THIS_MODULE;
    error_code = cdev_add(&frontend->cdev, frontend->dev_node, 1);
    if (error_code)
    {
        pr_err("Unable to add char device.\n");
        goto init_error1;
    }

    /* Create the device node in /dev so the device is accessible
     * as a character device
     */
    frontend->char_device = device_create(
            dma_char_device_class,  // class
            NULL,                   // parent device
            frontend->dev_node,     // dev_t 
            (void*)frontend,        // drv_data
            "%s", frontend->name);

    if ((NULL==frontend->char_device) || IS_ERR(frontend->char_device))
    {
        pr_err("Unable to create the device for the char device interface with name: '%s'\n",frontend->name);
        goto init_error2;
    }

    return 0;


init_error2:
    cdev_del(&frontend->cdev);

init_error1:
    unregister_chrdev_region(frontend->dev_node, 1);
    return error_code;
}


/**
 * @brief Remove the character device interface for the given frontend.
 * 
 * @param frontend Frontend object to remove the char device interface for.
 */
static void cdevice_deinit(struct dma_frontend* frontend)
{
    // Deinit the character device interface.
    if(frontend->char_device)
    {
        device_destroy(dma_char_device_class, frontend->dev_node);
        frontend->char_device = NULL;

        cdev_del(&frontend->cdev);
        memset(&frontend->cdev, 0, sizeof(frontend->cdev));

        unregister_chrdev_region(frontend->dev_node, 1);
        frontend->dev_node = 0;
    }

    return;
}


/**
 * @brief                   Create a frontend by getting a DMA channel from the DMA Engine and then setting
 *                          up a character device to allow user space control.
 *                          After this function the object is either fully initialized,
 *                          or filled completely with 0.
 * 
 * @param frontend          Frontend to initialize.
 * @param frontend_name     Name of the frontend.
 * @param dma_channel       Dma cannel to be used by the frontend.
 * @param direction         Direction of the dma channel.
 * @return true             In case the initialization succeeds.
 * @return false            In case of an error. In this case the frontend does not need to be uninitialized.
 */
static bool frontend_init(  struct dma_frontend*    frontend, 
                            const char*             frontend_name,
                            struct dma_chan*        dma_channel,
                            u32                     direction   )
{
    bool init_error = false;
    int rc, bd;
    struct dma_chan*            dma;
    struct dma_hw_channel_info  dma_channel_hw_info;

    // Init the frontend parameters
    strncpy(frontend->name, frontend_name, MAX_NAME_LENG);

    // Init the dma channel parameters
    frontend->dma_channel = dma_channel;
    frontend->direction = direction;    

    // Initialize a zerocopy engine for the given channel.
    dma_channel_hw_info.direction = direction;
    dma_channel_hw_info.dma_chan = dma_channel;
    frontend->zcdma = zcdma_alloc(&dma_channel_hw_info);
    if(NULL == frontend->zcdma)
    {
        pr_error("Zerocopy engine initialization failed for frontend '%s'.\n", frontend_name);
        init_error = true;
        memset(frontend, 0, sizeof(*frontend));
    }

    // Initialize the character device for the dma channel
    if(!init_error)
    {
        rc = cdevice_init(frontend);
        if (0 != rc)
        {
            init_error = true;
            // revert the zcdma initialization
            zcdma_free(frontend->zcdma);
            memset(frontend, 0, sizeof(*frontend));
        }
    }

    return init_error;
}


/**
 * @brief Deinitialize a frontend object.
 *        It accepts both initialized and full 0 frontend objects.
 * 
 * @param frontend Object to deinitialize.
 */
static void frontend_deinit(struct dma_frontend* frontend)
{
    // clean up the char device interface
    cdevice_deinit(frontend);

    // clean up the zcdma data
    if(NULL != frontend->zcdma)
    {
        zcdma_free(frontend->zcdma);
    }

    // terminate all the dma transfers, and release the dma channel
    if(NULL != frontend->dma_channel)
    {
        dmaengine_terminate_sync(frontend->dma_channel);
        dma_release_channel(frontend->dma_channel);
    }

    // clear all the data from the frontend object
    memset(frontend, 0, sizeof(*frontend));

    return;
}


/**
 * @brief Try to initialize the driver for the given platform device.
 * 
 * @param pdev Platform device to initialize the driver for.
 * @return int Error code.
 */
static int dma_manager_probe(struct platform_device *pdev)
{
    int ret = 0;
    int error_code, i;
    bool frontend_init_success                  =   false;
    struct dma_manager*         manager         =   NULL;
    struct dma_frontend*        frontend        =   NULL;
    const char*                 frontend_name   =   NULL;
    struct dma_chan*            dma_channel     =   NULL;

    if(NULL != pdev->name)
    {
        dev_info(&pdev->dev, "Probing the driver with the device '%s'...\n", pdev->name);
    }
    
    // Allocate memory for the driver data and register it into the driver.
    manager = (struct dma_manager *) devm_kmalloc(&pdev->dev, sizeof(struct dma_manager), GFP_KERNEL);
    if (!manager) {		
        dev_err(&pdev->dev, "Cound not allocate DMA manager.\n");
        return -ENOMEM;
    }
    dev_set_drvdata(&pdev->dev, (void*)manager);
    manager->platform_device = pdev;

    // Figure out how many channels are there from the device tree
    // based on the number of strings in the dma-names property.
    manager->frontend_count = device_property_read_string_array(&pdev->dev,
                                                                "dma-names", 
                                                                NULL, 
                                                                0);
    if (manager->frontend_count <= 0)
    {
        dev_warn(&pdev->dev, "No DMA channels are defined for the manager.\n");
        return 0;
    }
    dev_info(&pdev->dev, "Device Tree Channel count: %d\n", manager->frontend_count);

    // Allocate the memory for channel names and then get the names
    // from the device tree.
    manager->frontend_names = devm_kmalloc_array(&pdev->dev, manager->frontend_count, 
                                                    sizeof(char*), GFP_KERNEL);
    if (NULL == manager->frontend_names)
    {
        dev_err(&pdev->dev, "Cannot allocate memory for the frontend names.\n");
        return -ENOMEM;
    }

    error_code = device_property_read_string_array(
                    &pdev->dev, 
                    "dma-names", 
                    (const char **)manager->frontend_names, 
                    manager->frontend_count
                );
    if (error_code < 0)
    {
        dev_err(&pdev->dev, "Could not get the dma channel names from the device tree.\n");
        return error_code;
    }

    //  Allocate the memory for the channel structures.
    manager->frontends = devm_kmalloc(&pdev->dev, sizeof(struct dma_frontend) * manager->frontend_count, GFP_KERNEL);
    if (NULL == manager->frontends)
    {
        dev_err(&pdev->dev, "Cannot allocate memory for the frontends.\n");
        return -ENOMEM;
    }
    // clear the frontends, full 0 is a valid uninitialized state
    memset(frontend, 0, sizeof(struct dma_frontend) * manager->frontend_count);
    // Initialize a frontend for every specified dma channel.
    for (i = 0; i < manager->frontend_count; i++)
    {
        frontend_name = manager->frontend_names[i];        
        frontend = &manager->frontends[i];
        pr_debug("Initializing frontend for dma channel: '%s'.>\n", frontend_name);
        // Request the DMA channel from the DMA engine.
        dma_channel = dma_request_chan(&manager->platform_device->dev, frontend_name);
        if(IS_ERR(dma_channel))
        {
            ret = PTR_ERR(dma_channel);
            dev_err(&pdev->dev, "Could not get the dma chanel with name '%s' from the dmaengine, error:%d!\n",
                        frontend_name,
                        ret);
            // terminate the init cycle
            break;
        }
        else
        {
            dev_info(&pdev->dev, "Initializing frontend '%s'.\n", frontend_name);
            frontend_init_success = frontend_init(  frontend,
                                                    frontend_name,
                                                    dma_channel,
                                                    DMA_MEM_TO_DEV);
            if (false == frontend_init_success)
            {
                dev_err(&pdev->dev, "Error while creating frontend object for dma channel with name: '%s'\n",
                    frontend_name
                );
                ret = -ENODEV;
            } 
        }
    }

    // error handling
    if(0 != ret)
    {
        // We have encountered error during the frontend initializations,
        // so clean up the half initialized frontend array.
        for (i = 0; i < manager->frontend_count; i++)
        {
            frontend_deinit(&manager->frontends[i]);
        }
    }

    return ret;
}
 

/**
 * @brief Unload the driver for the given platform device.
 * 
 * @param pdev Platform device to deinit
 */
static void dma_manager_remove(struct platform_device *pdev)
{
    int i;
    struct device*      dev = &pdev->dev;
    struct dma_manager* manager = dev_get_drvdata(dev);

    dev_info(&pdev->dev, "Driver is unloading...\n");

    /* Take care of the char device infrastructure for each
     * channel except for the last channel. Handle the last
     * channel separately.
     */
    for (i = 0; i < manager->frontend_count; i++)
    {
        frontend_deinit(&manager->frontends[i]);
    }

    return;
}


static const struct of_device_id dma_manager_of_ids[] = {
    { .compatible = "xlnx,dma_manager",},
    // ensure backward comptability with the dma proxy driver
    // of xilinx
    { .compatible = "xlnx,dma_proxy",},
    {}
};


static struct platform_driver dma_manager_driver = {
    .driver = {
        .name = "dma_manager_driver",
        .owner = THIS_MODULE,
        .of_match_table = dma_manager_of_ids,
    },
    .probe = dma_manager_probe,
    .remove = dma_manager_remove,
};


/**
 * @brief Kernel module initializer, allocated a class for the
 *          character device interface and registers the dma_manager
 *          platform driver.
 * 
 * @return int 0 on success, ERROR otherwise.
 */
static int __init dma_manager_init(void)
{
    int errorCode = 0;

    pr_debug("Initializing the DMA manager module.");

    // Create a device class in the sysfs.
    // This will be used to create the individual devices for every dma channels.
    dma_char_device_class = class_create(THIS_MODULE, DRIVER_NAME);
    if ((NULL==dma_char_device_class) || IS_ERR(dma_char_device_class)) {
        pr_err("Unable to create class\n");
        errorCode = ERROR;
    }
    else
    {
        pr_debug("Registering the dma manager platform driver.");
        errorCode = platform_driver_register(&dma_manager_driver);
    }

    return errorCode;    
}


/**
 * @brief Module cleanup function.
 * 
 */
static void __exit dma_manager_exit(void)
{
    platform_driver_unregister(&dma_manager_driver);

    if((NULL!= dma_char_device_class) && !IS_ERR(dma_char_device_class))
    {
        class_destroy(dma_char_device_class);
        dma_char_device_class = NULL;
    }
}

module_init(dma_manager_init)
module_exit(dma_manager_exit)

MODULE_AUTHOR("Tusori Tibor");
MODULE_DESCRIPTION("DMA manager driver");
MODULE_LICENSE("GPL v2");
