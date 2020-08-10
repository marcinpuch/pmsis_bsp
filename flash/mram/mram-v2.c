/*
 * Copyright (C) 2019 GreenWaves Technologies
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* 
 * Authors: Germain Haugou, GreenWaves Technologies (germain.haugou@greenwaves-technologies.com)
 */

#include "pmsis.h"
#include "bsp/flash/mram.h"
#include "mram-v2.h"


#define TRIM_CFG_SIZE 7

#define MRAM_CMD_POWER_UP    0x00
#define MRAM_CMD_TRIM_CFG    0x01
#define MRAM_CMD_PROGRAM     0x02
#define MRAM_CMD_ERASE_CHIP  0x04
#define MRAM_CMD_ERASE_SECT  0x08
#define MRAM_CMD_ERASE_WORD  0x10
#define MRAM_CMD_PWDN        0x20
#define MRAM_CMD_READ        0x40

#define SECTOR_ERASE 1
#define NUM_PULSE    1

#define POS_MRAM_PENDING_ERASE_CHIP   0
#define POS_MRAM_PENDING_ERASE_SECTOR 1
#define POS_MRAM_PENDING_ERASE_WORD   2
#define POS_MRAM_PENDING_PROGRAM      3
#define POS_MRAM_PENDING_READ         4

#define POS_MRAM_WORD_SIZE_LOG2 3

PI_FC_L1 pos_mram_t pos_mram[ARCHI_UDMA_NB_MRAM];

PI_L2 uint32_t trim_cfg_buffer[TRIM_CFG_SIZE];


void pos_mram_handler_asm();

static void mram_erase_exec(pos_mram_t *mram, uint32_t addr, int size);
static void mram_erase_sector_exec(pos_mram_t *mram, uint32_t addr);
static void mram_erase_chip_exec(pos_mram_t *mram);
static void mram_read_exec(pos_mram_t *mram, uint32_t mram_addr, uint32_t data, uint32_t size);
static void mram_program_exec(pos_mram_t *mram, uint32_t mram_addr, uint32_t data, uint32_t size);


static int pos_get_div(pos_mram_t *mram, int freq)
{
    int periph_freq = pi_freq_get(PI_FREQ_DOMAIN_PERIPH);

    if (freq == 0 || freq >= periph_freq)
    {
        mram->freq = periph_freq;
        return 0;
    }
    else
    {
        // Round-up the divider to obtain a frequency which is below the maximum.
        // Also take care that the divider always divide by 2.
        int div = (periph_freq + 2*freq - 1) / (freq * 2);
        mram->freq = periph_freq / (div * 2);
        return div;
    }
}


static inline void __rt_mram_trim_cfg_exec(pos_mram_t *mram, void *data, void *addr, size_t size)
{
    unsigned int base = mram->base;

    udma_mram_mode_t mode = { .raw = udma_mram_mode_get(base) };
    mode.operation = MRAM_CMD_TRIM_CFG;
    udma_mram_mode_set(base, mode.raw);

    udma_mram_trans_addr_set(base, (uint32_t)data);
    udma_mram_trans_size_set(base, size);
    udma_mram_trans_cfg_set(base, UDMA_MRAM_TRANS_CFG_VALID(1));

}


static void __rt_mram_do_trim(pos_mram_t *mram, void *_trim_cfg_buff)
{
    unsigned int *trim_cfg_buff = _trim_cfg_buff;
    unsigned int base = mram->base;

    pi_task_t task;

    // Init the CFG zone in L2 to scan the TRIM CGF in the MRAM
    // TODO this should come from efuses
    for(int index = 0; index < TRIM_CFG_SIZE; index ++)
    {
        trim_cfg_buff[index] = 0x00000000u;
    }

    // Write the info to num Pulses and Sector enable
    //trim_cfg_buff[123] = (SECTOR_ERASE << 24) | ( NUM_PULSE << 17 );

    trim_cfg_buff[3] = (SECTOR_ERASE << 24) | ( NUM_PULSE << 17 );

    // section erase_en = cr_lat[3960]
    // prog_pulse_cfg   = cr_lat[3955:3953]
    //                    cr_lat[3955:3953]= 000 => number of program pulse = 8 (the default)
    //                    cr_lat[3955:3953]= 001 => number of program pulse = 1
    //                    cr_lat[3955:3953]= 010 => number of program pulse = 2
    //                    cr_lat[3955:3953]= 011 => number of program pulse = 3
    //                    cr_lat[3955:3953]= 100 => number of program pulse = 4
    //                    cr_lat[3955:3953]= 101 => number of program pulse = 5
    //                    cr_lat[3955:3953]= 110 => number of program pulse = 6
    //                    cr_lat[3955:3953]= 111 => number of program pulse = 7

    mram->pending_copy = pi_task_block(&task);
    __rt_mram_trim_cfg_exec(mram, trim_cfg_buff, 0, TRIM_CFG_SIZE);
    pi_task_wait_on(&task);
}


static void pos_mram_handle_event(int event, void *arg)
{
    pos_mram_t *mram = (pos_mram_t *)arg;
    pi_task_t *task = mram->pending_copy;
    mram->pending_copy = NULL;

    pos_task_push_locked(task);

    task = mram->waiting_first;
    if (task)
    {
        mram->waiting_first = task->next;
        mram->pending_copy = task;

        switch (task->data[0])
        {
            case POS_MRAM_PENDING_ERASE_CHIP:
                mram_erase_chip_exec(mram);
                break;
            case POS_MRAM_PENDING_ERASE_SECTOR:
                mram_erase_sector_exec(mram, task->data[1]);
                break;
            case POS_MRAM_PENDING_ERASE_WORD:
                mram_erase_exec(mram, task->data[1], task->data[2]);
                break;
            case POS_MRAM_PENDING_PROGRAM:
                mram_program_exec(mram, task->data[1], task->data[2], task->data[3]);
              break;
            case POS_MRAM_PENDING_READ:
                mram_read_exec(mram, task->data[1], task->data[2], task->data[3]);
              break;
        }
    }
}


static int mram_open(struct pi_device *device)
{
    struct pi_mram_conf *conf = (struct pi_mram_conf *)device->config;
    int id = conf->itf;

    MRAM_TRACE(POS_LOG_INFO, "Opening MRAM device (device: %p, id: %d)\n",
           device, id);

    pos_mram_t *mram = &pos_mram[id];

    uint32_t base = mram->base;

    mram->open_count++;
    if (mram->open_count == 1)
    {
        mram->rx_channel = pos_udma_linear_channels_alloc();
        mram->tx_channel = pos_udma_linear_channels_alloc();

        pos_pmu_change_domain_power(NULL, RT_PMU_MRAM_ID, RT_PMU_STATE_ON, 0);

        udma_reset_clr(ARCHI_UDMA_ADDR, mram->periph_id);
        udma_clockgate_clr(ARCHI_UDMA_ADDR, mram->periph_id);

        soc_eu_fcEventMask_setEvent(ARCHI_SOC_EVENT_MRAM_ERASE);
        pos_soc_event_register_callback(ARCHI_SOC_EVENT_MRAM_ERASE, pos_mram_handle_event, (void *)mram);
        soc_eu_fcEventMask_setEvent(ARCHI_SOC_EVENT_MRAM_TX);
        pos_soc_event_register_callback(ARCHI_SOC_EVENT_MRAM_TX, pos_mram_handle_event, (void *)mram);
        soc_eu_fcEventMask_setEvent(ARCHI_SOC_EVENT_MRAM_RX);
        pos_soc_event_register_callback(ARCHI_SOC_EVENT_MRAM_RX, pos_mram_handle_event, (void *)mram);
        soc_eu_fcEventMask_setEvent(ARCHI_SOC_EVENT_MRAM_TRIM);
        pos_soc_event_register_callback(ARCHI_SOC_EVENT_MRAM_TRIM, pos_mram_handle_event, (void *)mram);

        udma_mram_rx_dest_set(base, mram->rx_channel);
        udma_mram_tx_dest_set(base, mram->tx_channel);

        udma_mram_clk_div_set(base, pos_get_div(mram, conf->baudrate));

        udma_mram_trans_mode_set(base, UDMA_MRAM_TRANS_MODE_AUTO_ENA(1));

        udma_mram_ier_set(base,
             UDMA_MRAM_IER_ERASE_DONE(1) |
             UDMA_MRAM_IER_PROGRAM_DONE(1) |
             UDMA_MRAM_IER_TRIM_CONFIG_DONE(1) |
             UDMA_MRAM_IER_RX_DONE(1)
        );


        // // Setup clock divider
        // // TODO should take periph clock into account
        udma_mram_clk_div_set(base,
            UDMA_MRAM_CLK_DIV_ENABLE(1)   |
            UDMA_MRAM_CLK_DIV_VALID(1)    |
            UDMA_MRAM_CLK_DIV_DATA(2)
        );

        // Set PORb, RETb, RSTb, NVR, TMEN, AREF, DPD, ECCBYPS to 0
        udma_mram_mode_set(base, 0x0);

        // Perform Setup sequence : POR-RET-RST
        udma_mram_mode_set(base,
            UDMA_MRAM_MODE_PORB(1)
        );

        udma_mram_mode_set(base,
            UDMA_MRAM_MODE_PORB(1) |
            UDMA_MRAM_MODE_RETB(1)
        );

        pi_time_wait_us(5);

        udma_mram_mode_set(base,
            UDMA_MRAM_MODE_PORB(1) |
            UDMA_MRAM_MODE_RETB(1) |
            UDMA_MRAM_MODE_RSTB(1)
        );

        __rt_mram_do_trim(mram, trim_cfg_buffer);
    }

    device->data = (void *)mram;

    return 0;
}


static void mram_close(struct pi_device *device)
{
    pos_mram_t *mram = (pos_mram_t *)device->data;

    mram->open_count--;
    if (mram->open_count == 0)
    {
        uint32_t base = mram->base;

        // Deactivate MRAM channels
        udma_mram_rx_dest_set(base, UDMA_CTRL_NO_CHANNEL);
        udma_mram_tx_dest_set(base, UDMA_CTRL_NO_CHANNEL);

        // And free them
        pos_udma_linear_channels_free(mram->rx_channel);
        pos_udma_linear_channels_free(mram->tx_channel);

        // Reactivate clock-gating and reset
        udma_clockgate_set(ARCHI_UDMA_ADDR, mram->periph_id);
        udma_reset_set(ARCHI_UDMA_ADDR, mram->periph_id);
    }
}


static inline __attribute__((always_inline)) void pos_mram_enqueue_pending_copy_3(
    pos_mram_t *mram, pi_task_t *task, uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    task->data[0] = arg0;
    task->data[1] = arg1;
    task->data[2] = arg2;

    if (mram->waiting_first)
        mram->waiting_last->next = task;
    else
        mram->waiting_first = task;

    mram->waiting_last = task;
    task->next = NULL;
}


static inline __attribute__((always_inline)) void pos_mram_enqueue_pending_copy_4(
    pos_mram_t *mram, pi_task_t *task, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
    task->data[0] = arg0;
    task->data[1] = arg1;
    task->data[2] = arg2;
    task->data[3] = arg3;

    if (mram->waiting_first)
        mram->waiting_last->next = task;
    else
        mram->waiting_first = task;

    mram->waiting_last = task;
    task->next = NULL;
}


static inline __attribute__((always_inline)) void pos_mram_enqueue_pending_copy_7(
    pos_mram_t *mram, pi_task_t *task, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6)
{
    task->data[0] = arg0;
    task->data[1] = arg1;
    task->data[2] = arg2;
    task->data[3] = arg3;
    task->data[4] = arg4;
    task->data[5] = arg5;
    task->data[6] = arg6;

    if (mram->waiting_first)
        mram->waiting_last->next = task;
    else
        mram->waiting_first = task;

    mram->waiting_last = task;
    task->next = NULL;
}


void pos_mram_copy_2d(struct pi_device *device,
  uint32_t mram_addr, void *addr, uint32_t size, uint32_t stride, uint32_t length, struct pi_task *task, int is_read)
{
    pos_mram_t *mram = (pos_mram_t *)device->data;
    
    MRAM_TRACE(POS_LOG_TRACE, "%s transfer (device: %p, mram_addr: 0x%lx, buffer: %p, size: 0x%lx, stride: 0x%lx, length: 0x%lx, task: %p)\n", stride == 0 ? (is_read ? "Read" : "Write") : (is_read ? "2D read" : "2D write"), device, mram_addr, addr, size, stride, length, task);

    int irq = hal_irq_disable();

    if (likely(!mram->pending_copy))
    {
        mram->pending_copy = task;

        pos_mram_copy_2d_exec(mram, (uint32_t)addr, size, mram_addr, stride, length, is_read);

        hal_irq_restore(irq);

        return;
    }

    pos_mram_enqueue_pending_copy_7(mram, task, POS_MRAM_PENDING_READ, (int)addr, size, mram_addr, stride, length, is_read);

    hal_irq_restore(irq);
}


static void __attribute__((constructor)) pos_mram_init()
{
    for (int i=0; i<ARCHI_UDMA_NB_MRAM; i++)
    {
        pos_mram_t *mram = &pos_mram[i];
        mram->open_count = 0;
        mram->pending_copy = NULL;
        mram->waiting_first = NULL;
        mram->id = i;
        mram->periph_id = ARCHI_UDMA_MRAM_ID(i);
        mram->base = UDMA_MRAM_ADDR(i);
        //pos_irq_mask_set(1<<(ARCHI_FC_EVT_MRAM0 + i));
    }

    //pos_irq_set_handler(ARCHI_FC_EVT_MRAM0, pos_mram_handler_asm);
}

void pos_mram_handler()
{
    pos_mram_t *mram = &pos_mram[0];

    pi_task_t *task = mram->pending_copy;
    mram->pending_copy = NULL;

    pos_task_push_locked(task);

    task = mram->waiting_first;
    if (task)
    {
        mram->waiting_first = task->next;
        mram->pending_copy = task;

        pos_mram_copy_2d_exec(mram, task->data[0], task->data[1], task->data[2], task->data[3], task->data[4], task->data[5]);
    }
}


static void mram_read_exec(pos_mram_t *mram, uint32_t mram_addr, uint32_t data, uint32_t size)
{
    unsigned int base = mram->base;
    
    udma_mram_mode_t mode = { .raw = udma_mram_mode_get(base) };
    mode.operation = MRAM_CMD_READ;
    udma_mram_mode_set(base, mode.raw);

    udma_mram_trans_addr_set(mram->base, data);
    udma_mram_trans_size_set(mram->base, size);
    udma_mram_ext_addr_set(mram->base, mram_addr);
    udma_mram_trans_cfg_set(mram->base, UDMA_MRAM_TRANS_CFG_RXTX(1) | UDMA_MRAM_TRANS_CFG_VALID(1));

    udma_mram_trans_cfg_set(base, UDMA_MRAM_TRANS_CFG_VALID(1));
}


static void mram_read_async(struct pi_device *device, uint32_t addr, void *data, uint32_t size, pi_task_t *task)
{
    pos_mram_t *mram = (pos_mram_t *)device->data;

    MRAM_TRACE(POS_LOG_TRACE, "Read (device: %p, mram_addr: 0x%x, data: %p, size: 0x%x, task: %p)\n",
      device, addr, data, size, task);

    int irq = hal_irq_disable();

    if (likely(!mram->pending_copy))
    {
        mram->pending_copy = task;

        mram_read_exec(mram, addr, (uint32_t)data, size);

        hal_irq_restore(irq);

        return;
    }

    pos_mram_enqueue_pending_copy_4(mram, task, POS_MRAM_PENDING_READ, addr, (uint32_t)data, size);

    hal_irq_restore(irq);
}


static void mram_program_exec(pos_mram_t *mram, uint32_t mram_addr, uint32_t data, uint32_t size)
{
    unsigned int base = mram->base;
    
    udma_mram_mode_t mode = { .raw = udma_mram_mode_get(base) };
    mode.operation = MRAM_CMD_PROGRAM;
    udma_mram_mode_set(base, mode.raw);

    udma_mram_trans_addr_set(base, data);
    udma_mram_trans_size_set(base, size);
    udma_mram_ext_addr_set(base, mram_addr);
    udma_mram_trans_cfg_set(base, UDMA_MRAM_TRANS_CFG_RXTX(0) | UDMA_MRAM_TRANS_CFG_VALID(1));
}


static void mram_program_async(struct pi_device *device, uint32_t mram_addr, const void *data, uint32_t size, pi_task_t *task)
{
    pos_mram_t *mram = (pos_mram_t *)device->data;

    MRAM_TRACE(POS_LOG_TRACE, "Program (device: %p, mram_addr: 0x%x, data: %p, size: 0x%x, task: %p)\n",
      device, mram_addr, data, size, task);

    int irq = hal_irq_disable();

    if (likely(!mram->pending_copy))
    {
        mram->pending_copy = task;

        mram_program_exec(mram, mram_addr, (uint32_t)data, size);

        hal_irq_restore(irq);

        return;
    }
    
    pos_mram_enqueue_pending_copy_4(mram, task, POS_MRAM_PENDING_PROGRAM, mram_addr, (uint32_t)data, size);

    hal_irq_restore(irq);
}


static void mram_erase_chip_exec(pos_mram_t *mram)
{
    unsigned int base = mram->base;
    
    udma_mram_mode_t mode = { .raw = udma_mram_mode_get(base) };
    mode.operation = MRAM_CMD_ERASE_CHIP;
    udma_mram_mode_set(base, mode.raw);

    udma_mram_trans_cfg_set(base, UDMA_MRAM_TRANS_CFG_VALID(1));
}


static void mram_erase_chip_async(struct pi_device *device, pi_task_t *task)
{
    pos_mram_t *mram = (pos_mram_t *)device->data;

    MRAM_TRACE(POS_LOG_TRACE, "Chip erase (device: %p, task: %p)\n",
      device, task);

    int irq = hal_irq_disable();

    if (likely(!mram->pending_copy))
    {
        mram->pending_copy = task;

        mram_erase_chip_exec(mram);

        hal_irq_restore(irq);

        return;
    }
    
    pos_mram_enqueue_pending_copy_3(mram, task, POS_MRAM_PENDING_ERASE_CHIP, 0, 0);

    hal_irq_restore(irq);
}


static void mram_erase_sector_exec(pos_mram_t *mram, uint32_t addr)
{
    unsigned int base = mram->base;
    
    udma_mram_mode_t mode = { .raw = udma_mram_mode_get(base) };
    mode.operation = MRAM_CMD_ERASE_SECT;
    udma_mram_mode_set(base, mode.raw);

    udma_mram_erase_addr_set(base, (((unsigned int)addr) + ARCHI_MRAM_ADDR) >> POS_MRAM_WORD_SIZE_LOG2);
    udma_mram_trans_cfg_set(base, UDMA_MRAM_TRANS_CFG_VALID(1));
}


static void mram_erase_sector_async(struct pi_device *device, uint32_t addr, pi_task_t *task)
{
    pos_mram_t *mram = (pos_mram_t *)device->data;

    MRAM_TRACE(POS_LOG_TRACE, "Sector erase (device: %p, mram_addr: 0x%lx, task: %p)\n",
      device, addr, task);

    int irq = hal_irq_disable();

    if (likely(!mram->pending_copy))
    {
        mram->pending_copy = task;

        mram_erase_sector_exec(mram, addr);

        hal_irq_restore(irq);

        return;
    }
    
    pos_mram_enqueue_pending_copy_3(mram, task, POS_MRAM_PENDING_ERASE_SECTOR, (int)addr, 0);

    hal_irq_restore(irq);
}


static void mram_erase_exec(pos_mram_t *mram, uint32_t addr, int size)
{
    unsigned int base = mram->base;

    udma_mram_mode_t mode = { .raw = udma_mram_mode_get(base) };
    mode.operation = MRAM_CMD_ERASE_WORD;
    udma_mram_mode_set(base, mode.raw);

    udma_mram_erase_addr_set(base, (((unsigned int)addr) + ARCHI_MRAM_ADDR) >> POS_MRAM_WORD_SIZE_LOG2);
    udma_mram_erase_size_set(base, (size >> POS_MRAM_WORD_SIZE_LOG2) - 1);
    udma_mram_trans_cfg_set(base, UDMA_MRAM_TRANS_CFG_VALID(1));
}


static void mram_erase_async(struct pi_device *device, uint32_t addr, int size, pi_task_t *task)
{
    pos_mram_t *mram = (pos_mram_t *)device->data;

    MRAM_TRACE(POS_LOG_TRACE, "Word erase (device: %p, mram_addr: 0x%lx, size: 0x%lx, task: %p)\n",
      device, addr, size, task);

    int irq = hal_irq_disable();

    if (likely(!mram->pending_copy))
    {
        mram->pending_copy = task;

        mram_erase_exec(mram, addr, size);

        hal_irq_restore(irq);

        return;
    }
    
    pos_mram_enqueue_pending_copy_3(mram, task, POS_MRAM_PENDING_ERASE_WORD, (int)addr, size);

    hal_irq_restore(irq);
}



static int mram_copy_async(struct pi_device *device, uint32_t flash_addr, void *buffer, uint32_t size, int ext2loc, pi_task_t *task)
{
    return 0;
}



static int mram_copy_2d_async(struct pi_device *device, uint32_t flash_addr, void *buffer, uint32_t size, uint32_t stride, uint32_t length, int ext2loc, pi_task_t *task)
{
    return 0;
}

static int mram_read(struct pi_device *device, uint32_t flash_addr, void *data, uint32_t size)
{
    struct pi_task task;
    mram_read_async(device, flash_addr, data, size, pi_task_block(&task));
    pi_task_wait_on(&task);
    return 0;
}


static int mram_program(struct pi_device *device, uint32_t flash_addr, const void *data, uint32_t size)
{
    struct pi_task task;
    mram_program_async(device, flash_addr, data, size, pi_task_block(&task));
    pi_task_wait_on(&task);
    return 0;
}

static inline int mram_erase_chip(struct pi_device *device)
{
    struct pi_task task;
    mram_erase_chip_async(device, pi_task_block(&task));
    pi_task_wait_on(&task);
    return 0;
}

static inline int mram_erase_sector(struct pi_device *device, uint32_t flash_addr)
{
    struct pi_task task;
    mram_erase_sector_async(device, flash_addr, pi_task_block(&task));
    pi_task_wait_on(&task);
    return 0;
}

static inline int mram_erase(struct pi_device *device, uint32_t flash_addr, int size)
{
    struct pi_task task;
    mram_erase_async(device, flash_addr, size, pi_task_block(&task));
    pi_task_wait_on(&task);
    return 0;
}

static inline int mram_reg_set(struct pi_device *device, uint32_t pi_flash_addr, uint8_t *value)
{
    return 0;
}

static inline int mram_reg_get(struct pi_device *device, uint32_t pi_flash_addr, uint8_t *value)
{
    return 0;
}

static inline int mram_copy(struct pi_device *device, uint32_t pi_flash_addr, void *buffer, uint32_t size, int ext2loc)
{
    return 0;
}

static inline int mram_copy_2d(struct pi_device *device, uint32_t pi_flash_addr, void *buffer, uint32_t size, uint32_t stride, uint32_t length, int ext2loc)
{
    return 0;
}

static int32_t mram_ioctl(struct pi_device *device, uint32_t cmd, void *arg)
{
    return 0;
}

static void mram_reg_set_async(struct pi_device *device, uint32_t addr, uint8_t *value, pi_task_t *task)
{
}



static void mram_reg_get_async(struct pi_device *device, uint32_t addr, uint8_t *value, pi_task_t *task)
{
}



static pi_flash_api_t mram_api =
{
    .open                 = &mram_open,
    .close                = &mram_close,
    .ioctl                = &mram_ioctl,
    .read_async           = &mram_read_async,
    .program_async        = &mram_program_async,
    .erase_chip_async     = &mram_erase_chip_async,
    .erase_sector_async   = &mram_erase_sector_async,
    .erase_async          = &mram_erase_async,
    .reg_set_async        = &mram_reg_set_async,
    .reg_get_async        = &mram_reg_get_async,
    .copy_async           = &mram_copy_async,
    .copy_2d_async        = &mram_copy_2d_async,
    .read                 = &mram_read,
    .program              = &mram_program,
    .erase_chip           = &mram_erase_chip,
    .erase_sector         = &mram_erase_sector,
    .erase                = &mram_erase,
    .reg_set              = &mram_reg_set,
    .reg_get              = &mram_reg_get,
    .copy                 = &mram_copy,
    .copy_2d              = &mram_copy_2d,
};


void pi_mram_conf_init(struct pi_mram_conf *conf)
{
    conf->flash.api = &mram_api;
    conf->itf = 0;
    conf->baudrate = 0;
}
