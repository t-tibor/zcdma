// Override the fmt string preprocessor, so that every
// log message contains the module name.
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>

#include <zcdma.h>

/* Right now the I/O concept is very simple -- all reads and writes
 * are blocking, and concurrent reads and writes are not allowed.
 * Concurrent open is also disallowed.
 */
enum dma_fsm_state {
    DMA_IDLE = 0,
    DMA_IN_FLIGHT = 1,
    DMA_COMPLETING = 3,
};


enum transfer_result {
    TRANSFER_RESULT_OK = 0,
    TRANSFER_RESULT_TIMEOUT,
    TRANSFER_RESULT_ERROR,
};

#define TRANSFER_RESULT_IS_OK(result_enm) ( TRANSFER_RESULT_OK == (result_enm) )
#define TRANSFER_RESULT_IS_NOK(result_enm) ( TRANSFER_RESULT_OK != (result_enm) )

#define ZCDMA_DIR_TO_TRANFER_DIR(dir)     ( (ZCDMA_DIR_READ==(dir))?(DMA_DEV_TO_MEM):(DMA_MEM_TO_DEV) )
#define ZCDMA_DIR_TO_DATA_DIR(dir)        ( (ZCDMA_DIR_READ==(dir))?(DMA_FROM_DEVICE):(DMA_TO_DEVICE) )

struct zcdma {
    // DMA HW channel to use for the transfer
    const struct zcdma_hw_info* hw;

    // Semaphore to protect the runtime data
    struct semaphore sem;

    // User memory to be used by the DMA
    char* __user    userbuf;
    size_t          userbuf_len;
    unsigned int    userbuf_page_offset;

    // Pages that cover the user buffer
    struct page** 	pages;
    unsigned int 	pages_cnt;
    bool            pages_are_pinned;

    // scatterlist 
    struct sg_table sg_table;
    bool            sg_table_is_allocated;
    bool            sg_table_is_mapped;

    // descriptor
    struct dma_async_tx_descriptor* tx_descriptor;

    // cookie
    dma_cookie_t        cookie;

    // transfer status
    enum dma_fsm_state  state;
    spinlock_t          state_lock;

    // completion to wait for the 
    struct completion   transfer_done_completion;

    // remaining data after the transfer
    unsigned long residue; 
};


static int _set_target_memory(struct zcdma* cntx, 
        char __user* userbuf,
        size_t userbuf_len)
{
    pr_devel("Userbuf address: 0x%08X, length: %lu.", userbuf, userbuf_len);

    cntx->userbuf = userbuf;
    cntx->userbuf_len = userbuf_len;
}


/// @brief Get the memory pages of the user buffer and pin the in the memory.
/// @param cntx Context to perform the operation in.
/// @return Number of pages collected and pinned.
static int _collect_pages(struct zcdma* cntx)
{
    int retval = 0;

    BUG_ON( cntx->pages ); // should be null

    cntx->userbuf_page_offset = offset_in_page(cntx->userbuf);
    // determine how many pages long the user memory is
    cntx->pages_cnt = (cntx->userbuf_page_offset + cntx->userbuf_len + PAGE_SIZE-1) / PAGE_SIZE;
    pr_devel("Userbuffer page offset: %lu, page count: %lu", 
        cntx->userbuf_page_offset,
        cntx->pages_cnt);
    // allocate kernel memory for the page pointers
    // TODO maybe re-use the memory later?
    cntx->pages = kmalloc( cntx->pages_cnt * sizeof(struct page*), GFP_KERNEL);
    // check the memory allocation
    if(NULL == cntx->pages )
    {
        pr_err("Could not allocate memory for page list. [Line:%d]", __LINE__);
        retval = -ENOMEM;
        goto err;
    }

    // Get the page structures
    retval = get_user_pages_fast(
                (unsigned long)cntx->userbuf,    // start
                cntx->pages_cnt,
                (ZCDMA_DIR_READ == cntx->hw->direction), // write
                cntx->pages
            );
    if( retval != cntx->pages )
    {
        pr_err("get_user_pages_fast() returned %d, expected %lu\n",
            retval, cntx->pages_cnt);
        goto err;
    }
    else
    {
        pr_devel("Pinning of the user space pages is successful.");
        cntx->pages_are_pinned = true;
    }

err:
    return retval;
}


/// @brief  Create a scatter-gather table for the memory pages
///         occupied by the user memory.
/// @param cntx Context to perform the opertion in.
/// @return 0 in case of no error.
static int _build_sgtable(  struct zcdma* cntx )
{
    int retval = 0;

    int idx; // index of the current processed page/scatterlist
    struct scatterlist* sg; // scatterlist iterator
    struct page* current_page; // page iterator
    size_t len;   
    size_t offset;
    size_t left_to_map;

    // Allocate a scatter-gather table
    retval = sg_alloc_table( &cntx->sg_table, cntx->pages_cnt, GFP_KERNEL );
    if( 0 == retval )
    {
        // Build the scatterlist
        left_to_map = cntx->userbuf_len;
        for_each_sg( cntx->sg_table.sgl, sg, cntx->pages_cnt, idx )
        {
            current_page = cntx->pages[idx];
            // determine the offset and length of the memory region inside the current page
            // that should be used by the DMA
            if( 0 == idx )
            {
                // when mapping the first page use the difference
                // between the start of the user memory and the start of the page
                offset = cntx->userbuf_page_offset;
                pr_debug("First page offset: %lu.", offset);
            }
            else
            {   
                // after the first page we go from the beginning
                // of the pages
                offset = 0;
            }
            if( (offset + left_to_map) > (size_t)PAGE_SIZE )
            {
                len = (size_t)PAGE_SIZE - offset;
            }
            else
            {				
                len = left_to_map;
                pr_debug("Last page length: %lu.", left_to_map);
            }

            sg_set_page(sg, cntx->pages[idx], len, offset);

            left_to_map -= len;
        }        

        cntx->sg_table_is_allocated = true;
    }
    else
    {
        cntx->sg_table_is_allocated = false;
        pr_err("sg_alloc_table() returned %d\n", 
                retval);
    }


    return retval;
}


/// @brief Map the scatter-gather list and the user memory
///         so that the DMA HW can access the latest memory content.
/// @param dma_dev  Device to assign the mapping to.
/// @param cntx Context to operation on.
/// @return -ENOMEM in case the mapping fails, 0 otherwise.
static int _map_sgtable( struct zcdma*   cntx    )
{
    int                     retval = 0;
    int                     mapped_page_cnt;
    struct device* const    dma_dev = dmaengine_get_dma_device(cntx->hw->dma_chan);
    
    if( false != cntx->sg_table_is_allocated )
    {   
        mapped_page_cnt = dma_map_sg(dma_dev, 
                            cntx->sg_table.sgl, 
                            cntx->pages_cnt,
                            ZCDMA_DIR_TO_DATA_DIR(cntx->hw->direction)
                            );
        if(mapped_page_cnt != cntx->pages_cnt)
        {
            cntx->sg_table_is_mapped = false;
            pr_err("dma_map_sg() returned %d, expected %d\n", 
                        mapped_page_cnt, cntx->pages_cnt);
            retval = -ENOMEM;
        }
        else
        {
            cntx->sg_table_is_mapped = true;
            pr_debug("SG table mapped successfully.");
        }
    }

    return retval;
}


/// @brief Create a tx descriptor for the sg list.
/// @param cntx Context to operate upon.
/// @return     -ENOMEM in case the preparation failed, 0 otherwise.
static int _prepare_slave_sg(struct zcdma* cntx )
{
    int retval = 0;

    cntx->tx_descriptor = dmaengine_prep_slave_sg(
                        cntx->hw->dma_chan,
                        cntx->sg_table.sgl,
                        cntx->pages_cnt,
                        ZCDMA_DIR_TO_TRANFER_DIR(cntx->hw->direction),
                        DMA_PREP_INTERRUPT
                        );    // requenst an interrupt 

    if( NULL != cntx->tx_descriptor )
    {
        cntx->tx_descriptor->callback = _dmaengine_callback_func;
        cntx->tx_descriptor->callback_param = cntx;
    }
    else
    {
        pr_err("dmaengine_prep_slave_sg() failed\n");
        retval = -ENOMEM;
    }

    return retval;
}


/// @brief  Submit the descriptor to the DMA channel and start the transfer.
/// @param cntx     Context to operate upon.
/// @return     -EINVAL in case the submit operation failed, 0 otherwise.
static int _start(  struct zcdma* cntx  )
{
    int retval = 0;

    spin_lock_irq(&cntx->state_lock);

    cntx->cookie = dmaengine_submit(cntx->tx_descriptor);
    if( cntx->cookie < DMA_MIN_COOKIE )
    {
        pr_err("dmaengine_submit() returned %d\n", cntx->cookie);
        retval = -EINVAL;      
        cntx->state = DMA_IDLE;
    }
    else
    {
        cntx->state = DMA_IN_FLIGHT;
        dma_async_issue_pending(cntx->hw->dma_chan);
        pr_debug("DMA transfer started.");
    }

    spin_unlock(&cntx->state_lock);

    return retval;
}

static void _dmaengine_callback_func(void* data)
{
    struct zcdma* cntx = (struct zcdma*)data;
    unsigned long iflags;

    // sine we read, check and write the transfer status, grab the state lock
    spin_lock_irqsave(&cntx->state_lock, iflags);

    if( DMA_IN_FLIGHT == cntx->state )
    {
        // the DMA was started, and someone might wait for the result
        cntx->state = DMA_COMPLETING;
        complete(&cntx->transfer_done_completion);
    }
    // else: something does not add up...

    spin_lock_irqrestore(&cntx->state_lock, iflags);

    return;
}

static void _unmap_sgtable( struct zcdma* cntx  )
{
    struct device* const dma_dev = dmaengine_get_dma_device(cntx->hw->dma_chan);

    if( false != cntx->sg_table_is_mapped )
    {
        pr_debug("Unmapping sg list.");
        dma_unmap_sg(dma_dev,
            cntx->sg_table.sgl,
            cntx->pages_cnt,
            ZCDMA_DIR_TO_DATA_DIR(cntx->hw->direction)
            );

        cntx->sg_table_is_mapped = false;
    }

    return;
}

static void _deinit_sgtable( struct zcdma* cntx )
{
    if( false != cntx->sg_table_is_allocated)
    {
        pr_debug("Freeing sg table.");
        sg_free_table(&cntx->sg_table);
        cntx->sg_table_is_allocated = false;
    }

    return;    
}

static void _release_pages( struct zcdma* const cntx)
{
    int pidx;
    struct page* page;

    if( false != cntx->pages_are_pinned )
    {
        for(pidx=0; pidx<cntx->pages_cnt; pidx++)
        {
            /* Mark all pages dirty for now (not sure how to do this more
             * efficiently yet -- dmaengine API doesn't seem to return any
             * notion of how much data was actually transferred).
             */            
            page = cntx->pages[pidx];
            set_page_dirty( page );
            put_page( page );
        }
        pr_debug("%d pages are unmapped.", cntx->pages_cnt);

        cntx->pages_are_pinned = false;
    }

    if( NULL != cntx->pages )
    {
        kfree(cntx->pages);
        cntx->pages = NULL;

        cntx->pages_cnt = 0;
    }

    return;
}


static int _check_not_in_flight(struct zcdma* cntx)
{
    int retval;

    spin_lock_irq(&cntx->state_lock);
    retval = (DMA_IN_FLIGHT != cntx->state);
    spin_unlock_irq(&cntx->state_lock);

    return retval;
}


static int cleanup_transfer_data( struct zcdma* cntx)
{
    pr_devel("Unpreparing zerocopy operation.");

    _unmap_sgtable(cntx);

    _deinit_sgtable(cntx);

    // Release the collected pages. 
    // Also mark the pages as dirty to make sure that the cache is refreshed before 
    // the CPU tries to access it.
    // TODO Maybe the sg_table unmap does this?
    _release_pages(cntx);

    spin_lock_irq(&cntx->state_lock);
    cntx->state = DMA_IDLE;
    spin_unlock_irq(&cntx->state_lock);

    return;
}


static int start_dma_transfer(  struct zcdma* cntx,
                                char __user* userbuf,
                                size_t count                        )
{
    int retval = 0;



    // Saving the target memory block that the DMA operation should use.
    pr_devel("Saving target memory...");
    _set_target_memory(cntx, userbuf, count);
    
    // Collect the pages that cover the user memory and pin them.
    pr_devel("Collecting pages...");
    retval = _collect_pages(cntx);
    if( 0 != retval )
    {
        goto err;
    }

    // Allocate a scatterlist, where for every page we create a different item.
    pr_devel("Building sg table.");
    retval = _build_sgtable(cntx);
    if( 0 != retval )
    {
        goto err;
    }

    // Map the scatterlist, this is needed so that the dma sees the same 
    // data as the CPU.
    pr_devel("Mapping scatterlist for the DMA.");
    retval = _map_sgtable(cntx);
    if( 0 != retval )
    {
        goto err;
    }

    // Get a descriptor for the transaction and register the transfer callback.
    // This is the first time when the DMA backend driver gets our request.
    pr_debug("Prepare tx descriptor.");
    retval = _prepare_slave_sg(cntx);
    if( 0 != retval )
    {
        goto err;
    }

    // Start the transaction.
    pr_debug("Starting dma transfer.");
    retval = _start(cntx);
    if( 0 != retval )
    {
        goto err;
    }

err:
    if( 0 != retval )
    {
        pr_err("Reverting zerocopy prepare operation.");
        cleanup_transfer_data(cntx);
    }

    return retval;
}    

#define DMA_TIMEOUT ((unsigned long)10) /* TODO verify it!!!*/
static enum transfer_result wait_transfer(struct zcdma* cntx)
{
    enum transfer_result retval;
    unsigned long remaining_timeout;
    enum dma_status status;
    struct dma_tx_state state;

    pr_debug("Start waiting for the dma transfer to be done. Timeout: %lu", DMA_TIMEOUT);
    remaining_timeout =  wait_for_completion_timeout(&cntx->transfer_done_completion, DMA_TIMEOUT);
    pr_debug("Waiting for dma transfer completion ended. Remaining timeout: %lu.", remaining_timeout);

    // check the result of the dma transfer
    status = dmaengine_tx_status(cntx->hw->dma_chan, cntx->cookie, &state);
    if(0 == remaining_timeout)
    {
        pr_warn("DMA timed out... Status: %d, residue: %lu.", (int)status, (unsigned long)state.residue);
        retval = TRANSFER_RESULT_TIMEOUT;
        
    } else if( DMA_COMPLETE != status )
    {
        pr_warn("No timeout but the completion is not signalled. Status: %d\n.", (int)status);
        retval = TRANSFER_RESULT_ERROR;
    }
    else
    {
        retval = TRANSFER_RESULT_OK;
    }

    // cleanup
    cleanup_transfer_data(cntx);

    return retval;
}


static ssize_t start_and_wait_transfer( struct zcdma* cntx,
                                        char __user* userbuf,
                                        size_t count            )
{
    ssize_t retval;
    int start_retval;
    enum transfer_result wait_retval;

    start_retval = start_dma_transfer(cntx, userbuf, count);
    if( 0 == start_retval )
    {
        wait_retval = wait_transfer(cntx);
        if( TRANSFER_RESULT_IS_OK(wait_retval) )
        {
            pr_debug("Read operation completed successfully.\n");
            retval = count;
        }
        else
        {
            pr_error("Read operation failed. Could not wait for the transfer to finish. Error code: %d.\n", (int)wait_retval);
            retval = -EIO;
        }
    }
    else
    {
        pr_error("Read operation failed. Coult not start the dma transfer. Error code: %d.\n", (int)start_retval);
        retval = -EIO;
    }

    return retval;
}

/// @brief Lock the given zcdma context to get unique access to the internal members.
/// @param cntx   Context to lock.
/// @return false when the internal semaphore locking fails, true otherwise.
static bool zcdma_lock( struct zcdma* cntx )
{
    int sem_retval = down_interruptible(&cntx->sem);
    bool lock_success = (0 == sem_retval);
    return lock_success;
}


/// @brief Unlock the given zcdma context that was locked before.
/// @param cntx   Context to unlock.
static void zcdma_unlock( struct zcdma* cntx )
{
    up(&cntx->sem);
    return;
}


struct zcdma* zcdma_alloc(const struct dma_hw_channel_info* const hw_info)
{
    struct zcdma* retval;

    retval = (struct zcdma*)kmalloc(sizeof(struct zcdma), GFP_KERNEL);
    if( NULL != retval )
    {        
        zcdma_init(retval, hw_info);
    }

    return retval;
}


/// @brief  Initialize the given zerocopy context.
///         This shall be called before using any other zerocopy function.
/// @param cntx     Zerocopy context to initialize.
/// @param dma_hw_info  Pointer to a structure containing info about
///                     the DMA HW channel to be used for the zerocopy transfer later.
static bool zcdma_init( struct zcdma*  cntx,
                        const struct zcdma_hw_info* const   dma_hw_info)
{
    bool init_success = true;

    pr_info("Initializing a zerocopy dma context with direction: %d. "
        "Dma channel Id:%d. "
        "Dma channel name: %s. ",    
        (int)dma_hw_info->direction,
        dma_hw_info->dma_chan->chan_id,
        dma_hw_info->dma_chan->name
        );

    // Save a reference to the dma channel we want to use for the transfer.
    cntx->hw = dma_hw_info;

    // init the internal objects
    spin_lock_init(&cntx->state_lock);
    sema_init(&cntx->sem, 1);
    init_completion(&cntx->transfer_done_completion);

    return init_success;
}

ssize_t zcdma_read(struct zcdma* cntx, char __user* userbuf, size_t len)
{
    ssize_t retval = 0;

    if(ZCDMA_DIR_READ == cntx->hw->direction)
    {
        // get a unique access to the context, 
        // so that no other transfers run in parallel
        if( false != zcdma_lock(cntx) )
        {
            // start the dma transfer and wait until it finishes
            retval = start_and_wait_transfer(cntx, userbuf, len);

            zcdma_unlock(cntx);
        }
        else
        {
            pr_err("DMA read error: cannot lock the dma channel. "
                "Name of the DMA channel: %s. "
                "Direction of the DMA channel: %d. ",
                cntx->hw->dma_chan->name,
                cntx->hw->direction
            );            
            retval = -ERESTARTSYS;
        }
    }
    else
    {
        pr_err("DMA read error: cannot read from a non-read channel. "
            "Name of the DMA channel: %s. "
            "Direction of the DMA channel: %d. ",
            cntx->hw->dma_chan->name,
            cntx->hw->direction
        );
        retval = -EINVAL;
    }

    return retval;
}

ssize_t zcdma_write(struct zcdma* cntx, const char __user* userbuf, size_t len)
{
    ssize_t retval = 0;

    if(ZCDMA_DIR_WRITE == cntx->hw->direction)
    {   
        if( false != zcdma_lock(cntx) )
        {
            // start the dma transfer and wait until it finishes
            retval = start_and_wait_transfer(cntx, userbuf, len);

            zcdma_unlock(cntx);
        }
        else
        {
            pr_err("DMA read error: cannot lock the dma channel. "
                "Name of the DMA channel: %s. "
                "Direction of the DMA channel: %d. ",
                cntx->hw->dma_chan->name,
                cntx->hw->direction
            );
            retval = -ERESTARTSYS;
        }
    }
    else
    {
        pr_err("DMA write error: cannot write to a non-write channel. "
            "Name of the DMA channel: %s. "
            "Direction of the DMA channel: %d. ",
            cntx->hw->dma_chan->name,
            cntx->hw->direction
        );
        retval = -EINVAL;
    }

    return retval;
}


/// @brief Deinitialize a zcdma session.
///        It will free all the allocated resources.
/// @param session Session to deinit.
static void zcdma_deinit(struct zcdma* session)
{
    const struct zcdma_hw_info* hw_info = session->hw;

    pr_info("Deinitializing a zerocopy dma context with direction: %d. "
        "Dma channel Id:%d. "
        "Dma channel name: %s. ",    
        (int)hw_info->direction,
        hw_info->dma_chan->chan_id,
        hw_info->dma_chan->name
        );

    cleanup_transfer_data(session);

    return;
}


void zcdma_free(struct zcdma* session)
{
    if( NULL != session )
    {
        zcdma_deinit(session);
        free(session);
    }

    return;
}