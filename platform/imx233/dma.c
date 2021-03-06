#include "platform/imx233/system.h"
#include "platform/imx233/dma.h"
#include "utils/str.h"

// statistics about unaligned transfers
static int apb_nr_unaligned[32];

void imx233_dma_init(void)
{
    /* Enable APHB and APBX */
    imx233_reset_block(&HW_APBH_CTRL0);
    imx233_reset_block(&HW_APBX_CTRL0);
}

void imx233_dma_reset_channel(unsigned chan)
{
    volatile uint32_t *ptr;
    uint32_t bm;
    if(APB_IS_APBX_CHANNEL(chan))
    {
        ptr = &HW_APBX_CHANNEL_CTRL;
        bm = HW_APBX_CHANNEL_CTRL__RESET_CHANNEL(APB_GET_DMA_CHANNEL(chan));
    }
    else
    {
        ptr = &HW_APBH_CTRL0;
        bm = HW_APBH_CTRL0__RESET_CHANNEL(APB_GET_DMA_CHANNEL(chan));
    }
    __REG_SET(*ptr) = bm;
    /* wait for end of reset */
    while(*ptr & bm)
        ;
}

void imx233_dma_clkgate_channel(unsigned chan, bool enable_clock)
{
    if(APB_IS_APBX_CHANNEL(chan))
        return;
    if(enable_clock)
        __REG_CLR(HW_APBH_CTRL0) =
            HW_APBH_CTRL0__CLKGATE_CHANNEL(APB_GET_DMA_CHANNEL(chan));
    else
        __REG_SET(HW_APBH_CTRL0) =
            HW_APBH_CTRL0__CLKGATE_CHANNEL(APB_GET_DMA_CHANNEL(chan));
}

void imx233_dma_freeze_channel(unsigned chan, bool freeze)
{
    volatile uint32_t *ptr;
    uint32_t bm;
    if(APB_IS_APBX_CHANNEL(chan))
    {
        ptr = &HW_APBX_CHANNEL_CTRL;
        bm = HW_APBX_CHANNEL_CTRL__FREEZE_CHANNEL(APB_GET_DMA_CHANNEL(chan));
    }
    else
    {
        ptr = &HW_APBH_CTRL0;
        bm = HW_APBH_CTRL0__FREEZE_CHANNEL(APB_GET_DMA_CHANNEL(chan));
    }

    if(freeze)
        __REG_SET(*ptr) = bm;
    else
        __REG_CLR(*ptr) = bm;
}

void imx233_dma_enable_channel_interrupt(unsigned chan, bool enable)
{
    volatile uint32_t *ptr;
    uint32_t bm;
    if(APB_IS_APBX_CHANNEL(chan))
    {
        ptr = &HW_APBX_CTRL1;
        bm = HW_APBX_CTRL1__CHx_CMDCMPLT_IRQ_EN(APB_GET_DMA_CHANNEL(chan));
    }
    else
    {
        ptr = &HW_APBH_CTRL1;
        bm = HW_APBH_CTRL1__CHx_CMDCMPLT_IRQ_EN(APB_GET_DMA_CHANNEL(chan));
    }

    if(enable)
    {
        __REG_SET(*ptr) = bm;
        imx233_dma_clear_channel_interrupt(chan);
    }
    else
        __REG_CLR(*ptr) = bm;
}

void imx233_dma_clear_channel_interrupt(unsigned chan)
{
    if(APB_IS_APBX_CHANNEL(chan))
    {
        __REG_CLR(HW_APBX_CTRL1) =
            HW_APBX_CTRL1__CHx_CMDCMPLT_IRQ(APB_GET_DMA_CHANNEL(chan));
        __REG_CLR(HW_APBX_CTRL2) =
            HW_APBX_CTRL2__CHx_ERROR_IRQ(APB_GET_DMA_CHANNEL(chan));
    }
    else
    {
        __REG_CLR(HW_APBH_CTRL1) =
            HW_APBH_CTRL1__CHx_CMDCMPLT_IRQ(APB_GET_DMA_CHANNEL(chan));
        __REG_CLR(HW_APBH_CTRL2) =
            HW_APBH_CTRL2__CHx_ERROR_IRQ(APB_GET_DMA_CHANNEL(chan));
    }
}

bool imx233_dma_is_channel_error_irq(unsigned chan)
{
    if(APB_IS_APBX_CHANNEL(chan))
        return !!(HW_APBX_CTRL2 &
            HW_APBX_CTRL2__CHx_ERROR_IRQ(APB_GET_DMA_CHANNEL(chan)));
    else
        return !!(HW_APBH_CTRL2 &
            HW_APBH_CTRL2__CHx_ERROR_IRQ(APB_GET_DMA_CHANNEL(chan)));
}

/* Commit and/or discard all DMA descriptors and buffers pointed by them,
 * handle circular lists. At the same time, convert virtual pointers to
 * real ones */
static void imx233_dma_commit_and_discard(unsigned chan, struct apb_dma_command_t *cmd)
{
    /* We handle circular descriptors by using unused bits:
     * bits 8-11 are not used by the hardware so we first go through the whole
     * list and mark them all a special value at the same time we commit buffers
     * and then we go through the list another time to clear the mark and
     * commit the descriptors */
    struct apb_dma_command_t *cur = cmd;
    
    while((cur->cmd & HW_APB_CHx_CMD__UNUSED_BM) != HW_APB_CHx_CMD__UNUSED_MAGIC)
    {
        cur->cmd = (cur->cmd & ~HW_APB_CHx_CMD__UNUSED_BM) | HW_APB_CHx_CMD__UNUSED_MAGIC;
        if((uint32_t)cur->buffer % CACHEALIGN_SIZE)
            apb_nr_unaligned[chan]++;
        /* Virtual to physical buffer pointer conversion */
        cur->buffer = PHYSICAL_ADDR(cur->buffer);
        /* chain ? */
        if(cur->cmd & HW_APB_CHx_CMD__CHAIN)
            cur = cur->next;
        else
            break;
    }

    cur = cmd;
    while((cur->cmd & HW_APB_CHx_CMD__UNUSED_BM) != 0)
    {
        cur->cmd = cur->cmd & ~HW_APB_CHx_CMD__UNUSED_BM;
        int sz = __XTRACT_EX(cur->cmd, HW_APB_CHx_CMD__CMDWORDS) * sizeof(uint32_t);
        /* commit descriptor and discard descriptor */
        /* chain ? */
        if(cur->cmd & HW_APB_CHx_CMD__CHAIN)
        {
            struct apb_dma_command_t *next = cur->next;
            cur->next = PHYSICAL_ADDR(cur->next);
            commit_dcache_range(cur, sizeof(struct apb_dma_command_t) + sz);
            cur = next;
        }
        else
        {
            commit_dcache_range(cur, sizeof(struct apb_dma_command_t) + sz);
            break;
        }
    }
}

void imx233_dma_start_command(unsigned chan, struct apb_dma_command_t *cmd)
{
    imx233_dma_commit_and_discard(chan, cmd);
    if(APB_IS_APBX_CHANNEL(chan))
    {
        HW_APBX_CHx_NXTCMDAR(APB_GET_DMA_CHANNEL(chan)) = (uint32_t)PHYSICAL_ADDR(cmd);
        HW_APBX_CHx_SEMA(APB_GET_DMA_CHANNEL(chan)) = 1;
    }
    else
    {
        HW_APBH_CHx_NXTCMDAR(APB_GET_DMA_CHANNEL(chan)) = (uint32_t)PHYSICAL_ADDR(cmd);
        HW_APBH_CHx_SEMA(APB_GET_DMA_CHANNEL(chan)) = 1;
    }
}

struct imx233_dma_info_t imx233_dma_get_info(unsigned chan, unsigned flags)
{
    struct imx233_dma_info_t s;
    memset(&s, 0, sizeof(s));
    bool apbx = APB_IS_APBX_CHANNEL(chan);
    int dmac = APB_GET_DMA_CHANNEL(chan);
    if(flags & DMA_INFO_CURCMDADDR)
        s.cur_cmd_addr = apbx ? HW_APBX_CHx_CURCMDAR(dmac) : HW_APBH_CHx_CURCMDAR(dmac);
    if(flags & DMA_INFO_NXTCMDADDR)
        s.nxt_cmd_addr = apbx ? HW_APBX_CHx_NXTCMDAR(dmac) : HW_APBH_CHx_NXTCMDAR(dmac);
    if(flags & DMA_INFO_CMD)
        s.cmd = apbx ? HW_APBX_CHx_CMD(dmac) : HW_APBH_CHx_CMD(dmac);
    if(flags & DMA_INFO_BAR)
        s.bar = apbx ? HW_APBX_CHx_BAR(dmac) : HW_APBH_CHx_BAR(dmac);
    if(flags & DMA_INFO_AHB_BYTES)
        s.ahb_bytes = apbx ? __XTRACT_EX(HW_APBX_CHx_DEBUG2(dmac), HW_APBX_CHx_DEBUG2__AHB_BYTES) :
                __XTRACT_EX(HW_APBH_CHx_DEBUG2(dmac), HW_APBH_CHx_DEBUG2__AHB_BYTES);
    if(flags & DMA_INFO_APB_BYTES)
        s.apb_bytes = apbx ? __XTRACT_EX(HW_APBX_CHx_DEBUG2(dmac), HW_APBX_CHx_DEBUG2__APB_BYTES) :
                __XTRACT_EX(HW_APBH_CHx_DEBUG2(dmac), HW_APBH_CHx_DEBUG2__APB_BYTES);
    if(flags & DMA_INFO_FREEZED)
        s.freezed = apbx ? HW_APBX_CHANNEL_CTRL & HW_APBX_CHANNEL_CTRL__FREEZE_CHANNEL(dmac) :
                HW_APBH_CTRL0 & HW_APBH_CTRL0__FREEZE_CHANNEL(dmac);
    if(flags & DMA_INFO_GATED)
        s.gated = apbx ? false : HW_APBH_CTRL0 & HW_APBH_CTRL0__CLKGATE_CHANNEL(dmac);
    if(flags & DMA_INFO_INTERRUPT)
    {
        s.int_enabled = apbx ? HW_APBX_CTRL1 & HW_APBX_CTRL1__CHx_CMDCMPLT_IRQ_EN(dmac) :
                HW_APBH_CTRL1 & HW_APBH_CTRL1__CHx_CMDCMPLT_IRQ_EN(dmac);
        s.int_cmdcomplt = apbx ? HW_APBX_CTRL1 & HW_APBX_CTRL1__CHx_CMDCMPLT_IRQ(dmac) :
                HW_APBH_CTRL1 & HW_APBH_CTRL1__CHx_CMDCMPLT_IRQ(dmac);
        s.int_error = apbx ? HW_APBX_CTRL2 & HW_APBX_CTRL2__CHx_ERROR_IRQ(dmac) :
            HW_APBH_CTRL2 & HW_APBH_CTRL2__CHx_ERROR_IRQ(dmac);
    }
    s.nr_unaligned = apb_nr_unaligned[chan];
    return s;
}

