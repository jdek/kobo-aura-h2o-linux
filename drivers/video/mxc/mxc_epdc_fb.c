/*
 * Copyright (C) 2010-2012 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */
/*
 * Based on STMP378X LCDIF
 * Copyright 2008 Embedded Alley Solutions, Inc All Rights Reserved.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/uaccess.h>
#include <linux/cpufreq.h>
#include <linux/firmware.h>
#include <linux/kthread.h>
#include <linux/dmaengine.h>
#include <linux/pxp_dma.h>
#include <linux/mxcfb.h>
#include <linux/mxcfb_epdc_kernel.h>
#include <linux/gpio.h>
#ifdef USE_PMIC
#include <linux/regulator/driver.h>
#endif
#include <linux/fsl_devices.h>
// Frank.Lin 2011.05.09
#include <mach/hardware.h>
#include <mach/gpio.h>
#include <mach/iomux-mx50.h>

#include <linux/time.h>
#include <linux/bitops.h>

#include <asm/cacheflush.h>
#include "epdc_regs.h"
#include "lk_tps65185.h"
#include "eink_processing2.h"

#define GDEBUG 0
#include <linux/gallen_dbg.h>

//#define DO_NOT_POWEROFF	1
#define NTX_AUTOMODE_PATCH						1
#define NTX_WFM_MODE_OPTIMIZED				1
//#define NTX_WFM_MODE_OPTIMIZED_REAGL	1
#define REGAL_ENABLE									1
//#define USE_QUEUESPIN_LOCK				1
//
/*
 * Enable this define to have a default panel
 * loaded during driver initialization
 */
/*#define DEFAULT_PANEL_HW_INIT*/

#define NO_CUS_REAGL_MODE		1
#define NO_AUTO_REAGL_MODE		1


#define NUM_SCREENS_MIN	2
#define EPDC_NUM_LUTS 16
//#define EPDC_MAX_NUM_UPDATES 20
#define EPDC_MAX_NUM_UPDATES giEPDC_MAX_NUM_UPDATES
#define EPDC_MAX_NUM_BUFFERS	2
#define INVALID_LUT -1

#define DEFAULT_TEMP_INDEX	0
#define DEFAULT_TEMP		20 /* room temp in deg Celsius */

#define INIT_UPDATE_MARKER	0x12345678
#define PAN_UPDATE_MARKER	0x12345679

#define POWER_STATE_OFF	0
#define POWER_STATE_ON	1

#define MERGE_OK	0
#define MERGE_FAIL	1
#define MERGE_BLOCK	2

// Frank.Lin 2011.05.09
#define GPIO_PWRALL     (0*32 + 27) /* GPIO_1_27 */
#define EPDC_VCOM	(3*32 + 21)	/*GPIO_4_21 */

#define NTX_WFM_MODE_INIT			0
#define NTX_WFM_MODE_DU				1
#define NTX_WFM_MODE_GC16			2
#define NTX_WFM_MODE_GC4			3
#define NTX_WFM_MODE_A2				4
#define NTX_WFM_MODE_GL16			5
#define NTX_WFM_MODE_GLR16		6
#define NTX_WFM_MODE_GLD16		7
#define NTX_WFM_MODE_TOTAL		8
static int giNTX_waveform_modeA[NTX_WFM_MODE_TOTAL];


unsigned char gbModeVersion ;
unsigned char gbWFM_REV ;
unsigned char gbFPL_Platform ;

static int vcom_nominal;

static unsigned long default_bpp = 16;
static int giEPDC_MAX_NUM_UPDATES=2;

static unsigned bufPixFormat = EPDC_FORMAT_BUF_PIXEL_FORMAT_P4N; /* 4-bit pixel buffer */

struct update_marker_data {
	struct list_head full_list;
	struct list_head upd_list;
	u32 update_marker;
	struct completion update_completion;
	int lut_num;
	bool waiting;
};

struct update_desc_list {
	struct list_head list;
	struct mxcfb_update_data upd_data;/* Update parameters */
	u32 epdc_offs;		/* Added to buffer ptr to resolve alignment */
	struct list_head upd_marker_list; /* List of markers for this update */
	u32 update_order;	/* Numeric ordering value for update */
};

/* This structure represents a list node containing both
 * a memory region allocated as an output buffer for the PxP
 * update processing task, and the update description (mode, region, etc.) */
struct update_data_list {
	struct list_head list;
	dma_addr_t phys_addr;		/* Pointer to phys address of processed Y buf */
	void *virt_addr;
	struct update_desc_list *update_desc;
	int lut_num;			/* Assigned before update is processed into working buffer */
	int collision_mask;		/* Set when update results in collision */
					/* Represents other LUTs that we collide with */
};

struct mxc_epdc_fb_data {
	struct fb_info info;
	struct fb_var_screeninfo epdc_fb_var; /* Internal copy of screeninfo
						so we can sync changes to it */
	u32 pseudo_palette[16];
	char fw_str[24];
	struct list_head list;
	struct mxc_epdc_fb_mode *cur_mode;
	struct mxc_epdc_fb_platform_data *pdata;
	int blank;
	u32 max_pix_size;
	ssize_t map_size;
	dma_addr_t phys_start;
	u32 fb_offset;
	int default_bpp;
	int native_width;
	int native_height;
	int num_screens;
	int epdc_irq;
	struct device *dev;
	int power_state;
	int wait_for_powerdown;
	struct completion powerdown_compl;
	struct clk *epdc_clk_axi;
	struct clk *epdc_clk_pix;
#ifdef USE_PMIC
	struct regulator *display_regulator;
	struct regulator *vcom_regulator;
#endif
	bool fw_default_load;

	/* FB elements related to EPDC updates */
	bool in_init;
	bool hw_ready;
	bool waiting_for_idle;
	u32 auto_mode;
	u32 upd_scheme;
	struct list_head upd_pending_list;
	struct list_head upd_buf_queue;
	struct list_head upd_buf_free_list;
	struct list_head upd_buf_collision_list;
	struct update_data_list *cur_update;
#ifdef USE_QUEUESPIN_LOCK //[
	spinlock_t queue_lock;
#else //][ !USE_QUEUESPIN_LOCK
	struct mutex queue_mutex;
#endif //] USE_QUEUESPIN_LOCK
	int trt_entries;
	int temp_index;
	u8 *temp_range_bounds;
	int wfm;
	struct mxcfb_waveform_modes wv_modes;
	u32 *waveform_buffer_virt;
	u32 waveform_buffer_phys;
	u32 waveform_buffer_size;
	u32 *working_buffer_virt;
	u32 working_buffer_phys;
	u32 working_buffer_size;
	dma_addr_t *phys_addr_updbuf;
	void **virt_addr_updbuf;
	u32 upd_buffer_num;
	u32 max_num_buffers;
	dma_addr_t phys_addr_copybuf;	/* Phys address of copied update data */
	void *virt_addr_copybuf;	/* Used for PxP SW workaround */
	u32 order_cnt;
	struct list_head full_marker_list;
	u32 lut_update_order[EPDC_NUM_LUTS];
	u32 luts_complete_wb;
	struct completion updates_done;
	struct delayed_work epdc_done_work;
	struct workqueue_struct *epdc_submit_workqueue;
	struct work_struct epdc_submit_work;
//#ifdef FW_IN_RAM //[

	struct work_struct epdc_firmware_work;
//#endif//]FW_IN_RAM
	struct workqueue_struct *epdc_intr_workqueue;
	struct work_struct epdc_intr_work;
	bool waiting_for_wb;
	bool waiting_for_lut;
	bool waiting_for_lut15;
	struct completion update_res_free;
	struct completion lut15_free;
	struct completion eof_event;
	int eof_sync_period;
	struct mutex power_mutex;
	bool powering_down;
	bool updates_active;
	int pwrdown_delay;
	unsigned long tce_prevent;
	/* FB elements related to gen2 waveform data */
	u8 *waveform_vcd_buffer;
	u8 *waveform_acd_buffer;
	u32 waveform_magic_number;
	u8 *waveform_xwi_buffer;
	u32 waveform_mc;
	u32 waveform_trc;

	int merge_on_waveform_mismatch;

	/* FB elements related to PxP DMA */
	struct completion pxp_tx_cmpl;
	struct pxp_channel *pxp_chan;
	struct pxp_config_data pxp_conf;
	struct dma_async_tx_descriptor *txd;
	dma_cookie_t cookie;
	struct scatterlist sg[2];
	struct mutex pxp_mutex; /* protects access to PxP */
};

struct waveform_data_header {
	unsigned int wi0;
	unsigned int wi1;
	unsigned int wi2;
	unsigned int wi3;
	unsigned int wi4;
	unsigned int wi5;
	unsigned int wi6;
	unsigned int xwia:24;
	unsigned int cs1:8;
	unsigned int wmta:24;
	unsigned int fvsn:8;
	unsigned int luts:8;
	unsigned int mc:8;
	unsigned int trc:8;
	unsigned int awv:8;
	unsigned int eb:8;
	unsigned int sb:8;
	unsigned int reserved0_1:8;
	unsigned int reserved0_2:8;
	unsigned int reserved0_3:8;
	unsigned int reserved0_4:8;
	unsigned int reserved0_5:8;
	unsigned int cs2:8;
};

struct mxcfb_waveform_data_file {
	struct waveform_data_header wdh;
	u32 *data;	/* Temperature Range Table + Waveform Data */
};

#include "ntx_hwconfig.h"
extern volatile NTX_HWCONFIG *gptHWCFG;

extern volatile int giRootDevNum;
extern volatile int giRootPartNum;

void __iomem *epdc_base;

struct mxc_epdc_fb_data *g_fb_data;

/* forward declaration */
static int mxc_epdc_fb_get_temp_index(struct mxc_epdc_fb_data *fb_data,
						int temp);
static void mxc_epdc_fb_flush_updates(struct mxc_epdc_fb_data *fb_data);
static int mxc_epdc_fb_blank(int blank, struct fb_info *info);
static int mxc_epdc_fb_init_hw(struct fb_info *info);
static int pxp_process_update(struct mxc_epdc_fb_data *fb_data,
			      u32 src_width, u32 src_height,
			      struct mxcfb_rect *update_region);
static int pxp_complete_update(struct mxc_epdc_fb_data *fb_data, u32 *hist_stat);

static void draw_mode0(struct mxc_epdc_fb_data *fb_data);
static bool is_free_list_full(struct mxc_epdc_fb_data *fb_data);

static void mxc_epdc_fb_fw_handler(const struct firmware *fw,void *context);

//#ifdef DEBUG
#if 1
static void dump_pxp_config(struct mxc_epdc_fb_data *fb_data,
			    struct pxp_config_data *pxp_conf)
{
	dev_info(fb_data->dev, "S0 fmt 0x%x",
		pxp_conf->s0_param.pixel_fmt);
	dev_info(fb_data->dev, "S0 width 0x%x",
		pxp_conf->s0_param.width);
	dev_info(fb_data->dev, "S0 height 0x%x",
		pxp_conf->s0_param.height);
	dev_info(fb_data->dev, "S0 ckey 0x%x",
		pxp_conf->s0_param.color_key);
	dev_info(fb_data->dev, "S0 ckey en 0x%x",
		pxp_conf->s0_param.color_key_enable);

	dev_info(fb_data->dev, "OL0 combine en 0x%x",
		pxp_conf->ol_param[0].combine_enable);
	dev_info(fb_data->dev, "OL0 fmt 0x%x",
		pxp_conf->ol_param[0].pixel_fmt);
	dev_info(fb_data->dev, "OL0 width 0x%x",
		pxp_conf->ol_param[0].width);
	dev_info(fb_data->dev, "OL0 height 0x%x",
		pxp_conf->ol_param[0].height);
	dev_info(fb_data->dev, "OL0 ckey 0x%x",
		pxp_conf->ol_param[0].color_key);
	dev_info(fb_data->dev, "OL0 ckey en 0x%x",
		pxp_conf->ol_param[0].color_key_enable);
	dev_info(fb_data->dev, "OL0 alpha 0x%x",
		pxp_conf->ol_param[0].global_alpha);
	dev_info(fb_data->dev, "OL0 alpha en 0x%x",
		pxp_conf->ol_param[0].global_alpha_enable);
	dev_info(fb_data->dev, "OL0 local alpha en 0x%x",
		pxp_conf->ol_param[0].local_alpha_enable);

	dev_info(fb_data->dev, "Out fmt 0x%x",
		pxp_conf->out_param.pixel_fmt);
	dev_info(fb_data->dev, "Out width 0x%x",
		pxp_conf->out_param.width);
	dev_info(fb_data->dev, "Out height 0x%x",
		pxp_conf->out_param.height);

	dev_info(fb_data->dev,
		"drect left 0x%x right 0x%x width 0x%x height 0x%x",
		pxp_conf->proc_data.drect.left, pxp_conf->proc_data.drect.top,
		pxp_conf->proc_data.drect.width,
		pxp_conf->proc_data.drect.height);
	dev_info(fb_data->dev,
		"srect left 0x%x right 0x%x width 0x%x height 0x%x",
		pxp_conf->proc_data.srect.left, pxp_conf->proc_data.srect.top,
		pxp_conf->proc_data.srect.width,
		pxp_conf->proc_data.srect.height);
	dev_info(fb_data->dev, "Scaling en 0x%x", pxp_conf->proc_data.scaling);
	dev_info(fb_data->dev, "HFlip en 0x%x", pxp_conf->proc_data.hflip);
	dev_info(fb_data->dev, "VFlip en 0x%x", pxp_conf->proc_data.vflip);
	dev_info(fb_data->dev, "Rotation 0x%x", pxp_conf->proc_data.rotate);
	dev_info(fb_data->dev, "BG Color 0x%x", pxp_conf->proc_data.bgcolor);
}

static void dump_epdc_reg(void)
{
	#define KERN_DEBUG	
	printk(KERN_DEBUG "\n\n");
	printk(KERN_DEBUG "EPDC_CTRL 0x%x\n", __raw_readl(EPDC_CTRL));
	printk(KERN_DEBUG "EPDC_WVADDR 0x%x\n", __raw_readl(EPDC_WVADDR));
	printk(KERN_DEBUG "EPDC_WB_ADDR 0x%x\n", __raw_readl(EPDC_WB_ADDR));
	printk(KERN_DEBUG "EPDC_RES 0x%x\n", __raw_readl(EPDC_RES));
	printk(KERN_DEBUG "EPDC_FORMAT 0x%x\n", __raw_readl(EPDC_FORMAT));
	printk(KERN_DEBUG "EPDC_FIFOCTRL 0x%x\n", __raw_readl(EPDC_FIFOCTRL));
	printk(KERN_DEBUG "EPDC_UPD_ADDR 0x%x\n", __raw_readl(EPDC_UPD_ADDR));
	printk(KERN_DEBUG "EPDC_UPD_FIXED 0x%x\n", __raw_readl(EPDC_UPD_FIXED));
	printk(KERN_DEBUG "EPDC_UPD_CORD 0x%x\n", __raw_readl(EPDC_UPD_CORD));
	printk(KERN_DEBUG "EPDC_UPD_SIZE 0x%x\n", __raw_readl(EPDC_UPD_SIZE));
	printk(KERN_DEBUG "EPDC_UPD_CTRL 0x%x\n", __raw_readl(EPDC_UPD_CTRL));
	printk(KERN_DEBUG "EPDC_TEMP 0x%x\n", __raw_readl(EPDC_TEMP));
	printk(KERN_DEBUG "EPDC_TCE_CTRL 0x%x\n", __raw_readl(EPDC_TCE_CTRL));
	printk(KERN_DEBUG "EPDC_TCE_SDCFG 0x%x\n", __raw_readl(EPDC_TCE_SDCFG));
	printk(KERN_DEBUG "EPDC_TCE_GDCFG 0x%x\n", __raw_readl(EPDC_TCE_GDCFG));
	printk(KERN_DEBUG "EPDC_TCE_HSCAN1 0x%x\n", __raw_readl(EPDC_TCE_HSCAN1));
	printk(KERN_DEBUG "EPDC_TCE_HSCAN2 0x%x\n", __raw_readl(EPDC_TCE_HSCAN2));
	printk(KERN_DEBUG "EPDC_TCE_VSCAN 0x%x\n", __raw_readl(EPDC_TCE_VSCAN));
	printk(KERN_DEBUG "EPDC_TCE_OE 0x%x\n", __raw_readl(EPDC_TCE_OE));
	printk(KERN_DEBUG "EPDC_TCE_POLARITY 0x%x\n", __raw_readl(EPDC_TCE_POLARITY));
	printk(KERN_DEBUG "EPDC_TCE_TIMING1 0x%x\n", __raw_readl(EPDC_TCE_TIMING1));
	printk(KERN_DEBUG "EPDC_TCE_TIMING2 0x%x\n", __raw_readl(EPDC_TCE_TIMING2));
	printk(KERN_DEBUG "EPDC_TCE_TIMING3 0x%x\n", __raw_readl(EPDC_TCE_TIMING3));
	printk(KERN_DEBUG "EPDC_IRQ_MASK 0x%x\n", __raw_readl(EPDC_IRQ_MASK));
	printk(KERN_DEBUG "EPDC_IRQ 0x%x\n", __raw_readl(EPDC_IRQ));
	printk(KERN_DEBUG "EPDC_STATUS_LUTS 0x%x\n", __raw_readl(EPDC_STATUS_LUTS));
	printk(KERN_DEBUG "EPDC_STATUS_NEXTLUT 0x%x\n", __raw_readl(EPDC_STATUS_NEXTLUT));
	printk(KERN_DEBUG "EPDC_STATUS_COL 0x%x\n", __raw_readl(EPDC_STATUS_COL));
	printk(KERN_DEBUG "EPDC_STATUS 0x%x\n", __raw_readl(EPDC_STATUS));
	printk(KERN_DEBUG "EPDC_DEBUG 0x%x\n", __raw_readl(EPDC_DEBUG));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT0 0x%x\n", __raw_readl(EPDC_DEBUG_LUT0));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT1 0x%x\n", __raw_readl(EPDC_DEBUG_LUT1));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT2 0x%x\n", __raw_readl(EPDC_DEBUG_LUT2));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT3 0x%x\n", __raw_readl(EPDC_DEBUG_LUT3));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT4 0x%x\n", __raw_readl(EPDC_DEBUG_LUT4));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT5 0x%x\n", __raw_readl(EPDC_DEBUG_LUT5));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT6 0x%x\n", __raw_readl(EPDC_DEBUG_LUT6));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT7 0x%x\n", __raw_readl(EPDC_DEBUG_LUT7));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT8 0x%x\n", __raw_readl(EPDC_DEBUG_LUT8));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT9 0x%x\n", __raw_readl(EPDC_DEBUG_LUT9));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT10 0x%x\n", __raw_readl(EPDC_DEBUG_LUT10));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT11 0x%x\n", __raw_readl(EPDC_DEBUG_LUT11));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT12 0x%x\n", __raw_readl(EPDC_DEBUG_LUT12));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT13 0x%x\n", __raw_readl(EPDC_DEBUG_LUT13));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT14 0x%x\n", __raw_readl(EPDC_DEBUG_LUT14));
	printk(KERN_DEBUG "EPDC_DEBUG_LUT15 0x%x\n", __raw_readl(EPDC_DEBUG_LUT15));
	printk(KERN_DEBUG "EPDC_GPIO 0x%x\n", __raw_readl(EPDC_GPIO));
	printk(KERN_DEBUG "EPDC_VERSION 0x%x\n", __raw_readl(EPDC_VERSION));
	printk(KERN_DEBUG "\n\n");
}

static void dump_update_data(struct device *dev,
			     struct update_data_list *upd_data_list)
{
	dev_info(dev,
		"X = %d, Y = %d, Width = %d, Height = %d, WaveMode = %d, "
		"LUT = %d, Coll Mask = 0x%x, order = %d\n",
		upd_data_list->update_desc->upd_data.update_region.left,
		upd_data_list->update_desc->upd_data.update_region.top,
		upd_data_list->update_desc->upd_data.update_region.width,
		upd_data_list->update_desc->upd_data.update_region.height,
		upd_data_list->update_desc->upd_data.waveform_mode,
		upd_data_list->lut_num,
		upd_data_list->collision_mask,
		upd_data_list->update_desc->update_order);
}

static void dump_collision_list(struct mxc_epdc_fb_data *fb_data)
{
	struct update_data_list *plist;

	dev_info(fb_data->dev, "Collision List:\n");
	if (list_empty(&fb_data->upd_buf_collision_list))
		dev_info(fb_data->dev, "Empty");
	list_for_each_entry(plist, &fb_data->upd_buf_collision_list, list) {
		dev_info(fb_data->dev, "Virt Addr = 0x%x, Phys Addr = 0x%x ",
			(u32)plist->virt_addr, plist->phys_addr);
		dump_update_data(fb_data->dev, plist);
	}
}

static void dump_free_list(struct mxc_epdc_fb_data *fb_data)
{
	struct update_data_list *plist;

	dev_info(fb_data->dev, "Free List:\n");
	if (list_empty(&fb_data->upd_buf_free_list))
		dev_info(fb_data->dev, "Empty");
	list_for_each_entry(plist, &fb_data->upd_buf_free_list, list)
		dev_info(fb_data->dev, "Virt Addr = 0x%x, Phys Addr = 0x%x ",
			(u32)plist->virt_addr, plist->phys_addr);
}

static void dump_queue(struct mxc_epdc_fb_data *fb_data)
{
	struct update_data_list *plist;

	dev_info(fb_data->dev, "Queue:\n");
	if (list_empty(&fb_data->upd_buf_queue))
		dev_info(fb_data->dev, "Empty");
	list_for_each_entry(plist, &fb_data->upd_buf_queue, list) {
		dev_info(fb_data->dev, "Virt Addr = 0x%x, Phys Addr = 0x%x ",
			(u32)plist->virt_addr, plist->phys_addr);
		dump_update_data(fb_data->dev, plist);
	}
}

static void dump_desc_data(struct device *dev,
			     struct update_desc_list *upd_desc_list)
{
	dev_info(dev,
		"X = %d, Y = %d, Width = %d, Height = %d, WaveMode = %d, "
		"order = %d\n",
		upd_desc_list->upd_data.update_region.left,
		upd_desc_list->upd_data.update_region.top,
		upd_desc_list->upd_data.update_region.width,
		upd_desc_list->upd_data.update_region.height,
		upd_desc_list->upd_data.waveform_mode,
		upd_desc_list->update_order);
}

static void dump_pending_list(struct mxc_epdc_fb_data *fb_data)
{
	struct update_desc_list *plist;

	dev_info(fb_data->dev, "Queue:\n");
	if (list_empty(&fb_data->upd_pending_list))
		dev_info(fb_data->dev, "Empty");
	list_for_each_entry(plist, &fb_data->upd_pending_list, list)
		dump_desc_data(fb_data->dev, plist);
}

static void dump_all_updates(struct mxc_epdc_fb_data *fb_data)
{
	dump_free_list(fb_data);
	dump_queue(fb_data);
	dump_collision_list(fb_data);
	dev_info(fb_data->dev, "Current update being processed:\n");
	if (fb_data->cur_update == NULL)
		dev_info(fb_data->dev, "No current update\n");
	else
		dump_update_data(fb_data->dev, fb_data->cur_update);
}
#else
static inline void dump_pxp_config(struct mxc_epdc_fb_data *fb_data,
				   struct pxp_config_data *pxp_conf) {}
static inline void dump_epdc_reg(void) {}
static inline void dump_update_data(struct device *dev,
			     struct update_data_list *upd_data_list) {}
static inline void dump_collision_list(struct mxc_epdc_fb_data *fb_data) {}
static inline void dump_free_list(struct mxc_epdc_fb_data *fb_data) {}
static inline void dump_queue(struct mxc_epdc_fb_data *fb_data) {}
static inline void dump_all_updates(struct mxc_epdc_fb_data *fb_data) {}

#endif

/* 
 * EPDC Voltage Control data handler
 */
struct epd_vc_data {
		unsigned version:16;
		unsigned v1:16;
		unsigned v2:16;
		unsigned v3:16;
		unsigned v4:16;
		unsigned v5:16;
		unsigned v6:16;
		unsigned v7:8;
		u8	cs:8;
	};
void fetch_Epdc_Pmic_Voltages( struct epd_vc_data *vcd, struct mxc_epdc_fb_data *fb_data,
	u32 waveform_mode,
	u32 waveform_tempRange)
{
	/* fetch and display the voltage control data  */
	if (fb_data->waveform_vcd_buffer) {

		/* fetch the voltage control data */
		if (mxc_epdc_fb_fetch_vc_data( fb_data->waveform_vcd_buffer, waveform_mode, waveform_tempRange, fb_data->waveform_mc, fb_data->waveform_trc, (unsigned char *) vcd) < 0)
			dev_err(fb_data->dev, " *** Extra Waveform Data checksum error ***\n");
		else 
		{
#if 0
			dev_info(fb_data->dev, " -- VC Data version 0x%04x : v1 = 0x%04x, v2 = 0x%04x, v3 = 0x%04x, v4 = 0x%04x, v5 = 0x%04x, v6 = 0x%04x, v7 = 0x%02x --\n",
				vcd->version, vcd->v1, vcd->v2, vcd->v3, vcd->v4, vcd->v5, vcd->v6, vcd->v7 );
#endif 
		}


	}
}


/********************************************************
 * Start Low-Level EPDC Functions
 ********************************************************/

static inline void epdc_lut_complete_intr(u32 lut_num, bool enable)
{
	if (enable)
		__raw_writel(1 << lut_num, EPDC_IRQ_MASK_SET);
	else
		__raw_writel(1 << lut_num, EPDC_IRQ_MASK_CLEAR);
}

static inline void epdc_working_buf_intr(bool enable)
{
	if (enable)
		__raw_writel(EPDC_IRQ_WB_CMPLT_IRQ, EPDC_IRQ_MASK_SET);
	else
		__raw_writel(EPDC_IRQ_WB_CMPLT_IRQ, EPDC_IRQ_MASK_CLEAR);
}

static inline void epdc_clear_working_buf_irq(void)
{
	__raw_writel(EPDC_IRQ_WB_CMPLT_IRQ | EPDC_IRQ_LUT_COL_IRQ,
		     EPDC_IRQ_CLEAR);
}

static inline void epdc_eof_intr(bool enable)
{
	if (enable)
		__raw_writel(EPDC_IRQ_FRAME_END_IRQ, EPDC_IRQ_MASK_SET);
	else
		__raw_writel(EPDC_IRQ_FRAME_END_IRQ, EPDC_IRQ_MASK_CLEAR);
}

static inline void epdc_clear_eof_irq(void)
{
	__raw_writel(EPDC_IRQ_FRAME_END_IRQ, EPDC_IRQ_CLEAR);
}

static inline bool epdc_signal_eof(void)
{
	return (__raw_readl(EPDC_IRQ_MASK) & __raw_readl(EPDC_IRQ)
		& EPDC_IRQ_FRAME_END_IRQ) ? true : false;
}

static inline void epdc_set_temp(u32 temp)
{
	__raw_writel(temp, EPDC_TEMP);
}

static inline void epdc_set_screen_res(u32 width, u32 height)
{
	u32 val = (height << EPDC_RES_VERTICAL_OFFSET) | width;
	__raw_writel(val, EPDC_RES);
}

static inline void epdc_set_update_addr(u32 addr)
{
	__raw_writel(addr, EPDC_UPD_ADDR);
}

static inline void epdc_set_update_coord(u32 x, u32 y)
{
	u32 val = (y << EPDC_UPD_CORD_YCORD_OFFSET) | x;
	__raw_writel(val, EPDC_UPD_CORD);
}

static inline void epdc_set_update_dimensions(u32 width, u32 height)
{
	u32 val = (height << EPDC_UPD_SIZE_HEIGHT_OFFSET) | width;
	__raw_writel(val, EPDC_UPD_SIZE);
}

static void epdc_submit_update(u32 lut_num, u32 waveform_mode, u32 update_mode,
			       bool use_test_mode, u32 np_val)
{
	volatile static int giLast_waveform_mode=-1;
	u32 reg_val = 0;


	if(giLast_waveform_mode!=waveform_mode) {
		if( g_fb_data->wv_modes.mode_a2==waveform_mode &&
				g_fb_data->wv_modes.mode_du!=giLast_waveform_mode) 
		{
			waveform_mode=g_fb_data->wv_modes.mode_du;
			DBG_MSG("%s():waveform mode has been force chage to DU before A2\n",__FUNCTION__);
		}
		DBG_MSG("%s(%d):%s(),lut=%d,wf_mode=%d,last_wf_mode=%d,upd_mode=%d,test=%d,np_val=%d\n",
			__FILE__,__LINE__,__FUNCTION__,lut_num,waveform_mode,giLast_waveform_mode,update_mode,use_test_mode,np_val);
#ifndef USE_QUEUESPIN_LOCK //[
		schedule_timeout(10);
#endif //] !USE_QUEUESPIN_LOCK
	}
	
	//DBG_MSG("%s(%d):%s(),lut=%d,last_wf_mode=%d,wf_mode=%d,upd_mode=%d,test=%d,np_val=%d\n",
	//	__FILE__,__LINE__,__FUNCTION__,lut_num,giLast_waveform_mode,waveform_mode,update_mode,use_test_mode,np_val);

	if (use_test_mode) {
		reg_val |=
		    ((np_val << EPDC_UPD_FIXED_FIXNP_OFFSET) &
		     EPDC_UPD_FIXED_FIXNP_MASK) | EPDC_UPD_FIXED_FIXNP_EN;

		__raw_writel(reg_val, EPDC_UPD_FIXED);

		reg_val = EPDC_UPD_CTRL_USE_FIXED;
	} else {
		__raw_writel(reg_val, EPDC_UPD_FIXED);
	}

	reg_val |=
	    ((lut_num << EPDC_UPD_CTRL_LUT_SEL_OFFSET) &
	     EPDC_UPD_CTRL_LUT_SEL_MASK) |
	    ((waveform_mode << EPDC_UPD_CTRL_WAVEFORM_MODE_OFFSET) &
	     EPDC_UPD_CTRL_WAVEFORM_MODE_MASK) |
	    update_mode;

#if (GDEBUG>=1)
	if( (__raw_readl(EPDC_IRQ_MASK) & (1<<lut_num) )==0) {
		dump_stack();
	}
#endif
	__raw_writel(reg_val, EPDC_UPD_CTRL);

	giLast_waveform_mode = waveform_mode;

}

static inline bool epdc_is_lut_complete(u32 lut_num)
{
	u32 val = __raw_readl(EPDC_IRQ);
	bool is_compl = val & (1 << lut_num) ? true : false;

	return is_compl;
}

static inline void epdc_clear_lut_complete_irq(u32 lut_num)
{
	__raw_writel(1 << lut_num, EPDC_IRQ_CLEAR);
}

static inline bool epdc_is_lut_active(u32 lut_num)
{
	u32 val = __raw_readl(EPDC_STATUS_LUTS);
	bool is_active = val & (1 << lut_num) ? true : false;

	return is_active;
}

static inline bool epdc_any_luts_active(void)
{
	bool any_active = __raw_readl(EPDC_STATUS_LUTS) ? true : false;

	return any_active;
}

static inline bool epdc_any_luts_available(void)
{
	bool luts_available =
	    (__raw_readl(EPDC_STATUS_NEXTLUT) &
	     EPDC_STATUS_NEXTLUT_NEXT_LUT_VALID) ? true : false;
	return luts_available;
}

static inline int epdc_get_next_lut(void)
{
	u32 val =
	    __raw_readl(EPDC_STATUS_NEXTLUT) &
	    EPDC_STATUS_NEXTLUT_NEXT_LUT_MASK;
	return val;
}

static int epdc_choose_next_lut(int *next_lut)
{
#if 1
	u32 luts_status = (__raw_readl(EPDC_STATUS_LUTS)|__raw_readl(EPDC_IRQ))&0xFFFF;
#else
	u32 luts_status = __raw_readl(EPDC_STATUS_LUTS);
#endif

	*next_lut = fls(luts_status & 0xFFFF);

	if (*next_lut > 15)
		*next_lut = ffz(luts_status & 0xFFFF);

	if (luts_status & 0x8000) {
#if (GDEBUG>=1)//[
		dump_stack();
#endif//]
		return 1;
	}
	else
		return 0;
}

static inline bool epdc_is_working_buffer_busy(void)
{
	u32 val = __raw_readl(EPDC_STATUS);
	bool is_busy = (val & EPDC_STATUS_WB_BUSY) ? true : false;

	return is_busy;
}

static inline bool epdc_is_working_buffer_complete(void)
{
	u32 val = __raw_readl(EPDC_IRQ);
	bool is_compl = (val & EPDC_IRQ_WB_CMPLT_IRQ) ? true : false;

	return is_compl;
}

static inline bool epdc_is_collision(void)
{
	u32 val = __raw_readl(EPDC_IRQ);
	return (val & EPDC_IRQ_LUT_COL_IRQ) ? true : false;
}

static inline int epdc_get_colliding_luts(void)
{
	u32 val = __raw_readl(EPDC_STATUS_COL);
	return val;
}

static void epdc_set_horizontal_timing(u32 horiz_start, u32 horiz_end,
				       u32 hsync_width, u32 hsync_line_length)
{
	u32 reg_val =
	    ((hsync_width << EPDC_TCE_HSCAN1_LINE_SYNC_WIDTH_OFFSET) &
	     EPDC_TCE_HSCAN1_LINE_SYNC_WIDTH_MASK)
	    | ((hsync_line_length << EPDC_TCE_HSCAN1_LINE_SYNC_OFFSET) &
	       EPDC_TCE_HSCAN1_LINE_SYNC_MASK);
	__raw_writel(reg_val, EPDC_TCE_HSCAN1);

	reg_val =
	    ((horiz_start << EPDC_TCE_HSCAN2_LINE_BEGIN_OFFSET) &
	     EPDC_TCE_HSCAN2_LINE_BEGIN_MASK)
	    | ((horiz_end << EPDC_TCE_HSCAN2_LINE_END_OFFSET) &
	       EPDC_TCE_HSCAN2_LINE_END_MASK);
	__raw_writel(reg_val, EPDC_TCE_HSCAN2);
}

static void epdc_set_vertical_timing(u32 vert_start, u32 vert_end,
				     u32 vsync_width)
{
	u32 reg_val =
	    ((vert_start << EPDC_TCE_VSCAN_FRAME_BEGIN_OFFSET) &
	     EPDC_TCE_VSCAN_FRAME_BEGIN_MASK)
	    | ((vert_end << EPDC_TCE_VSCAN_FRAME_END_OFFSET) &
	       EPDC_TCE_VSCAN_FRAME_END_MASK)
	    | ((vsync_width << EPDC_TCE_VSCAN_FRAME_SYNC_OFFSET) &
	       EPDC_TCE_VSCAN_FRAME_SYNC_MASK);
	__raw_writel(reg_val, EPDC_TCE_VSCAN);
}

void epdc_init_settings(struct mxc_epdc_fb_data *fb_data)
{
	struct mxc_epdc_fb_mode *epdc_mode = fb_data->cur_mode;
	struct fb_var_screeninfo *screeninfo = &fb_data->epdc_fb_var;
	u32 reg_val;
	int num_ce;

	/* Reset */
	__raw_writel(EPDC_CTRL_SFTRST, EPDC_CTRL_SET);
	while (!(__raw_readl(EPDC_CTRL) & EPDC_CTRL_CLKGATE))
		;
	__raw_writel(EPDC_CTRL_SFTRST, EPDC_CTRL_CLEAR);

	/* Enable clock gating (clear to enable) */
	__raw_writel(EPDC_CTRL_CLKGATE, EPDC_CTRL_CLEAR);
	while (__raw_readl(EPDC_CTRL) & (EPDC_CTRL_SFTRST | EPDC_CTRL_CLKGATE))
		;

	/* EPDC_CTRL */
	reg_val = __raw_readl(EPDC_CTRL);
	reg_val &= ~EPDC_CTRL_UPD_DATA_SWIZZLE_MASK;
	reg_val |= EPDC_CTRL_UPD_DATA_SWIZZLE_NO_SWAP;
	reg_val &= ~EPDC_CTRL_LUT_DATA_SWIZZLE_MASK;
	reg_val |= EPDC_CTRL_LUT_DATA_SWIZZLE_NO_SWAP;
	__raw_writel(reg_val, EPDC_CTRL_SET);

	/* EPDC_FORMAT - 2bit TFT and bufPixFormat Buf pixel format */
	reg_val = EPDC_FORMAT_TFT_PIXEL_FORMAT_2BIT
	    | bufPixFormat
	    | ((0x0 << EPDC_FORMAT_DEFAULT_TFT_PIXEL_OFFSET) &
	       EPDC_FORMAT_DEFAULT_TFT_PIXEL_MASK);
	__raw_writel(reg_val, EPDC_FORMAT);

	/* EPDC_FIFOCTRL (disabled) */
	reg_val =
	    ((100 << EPDC_FIFOCTRL_FIFO_INIT_LEVEL_OFFSET) &
	     EPDC_FIFOCTRL_FIFO_INIT_LEVEL_MASK)
	    | ((200 << EPDC_FIFOCTRL_FIFO_H_LEVEL_OFFSET) &
	       EPDC_FIFOCTRL_FIFO_H_LEVEL_MASK)
	    | ((100 << EPDC_FIFOCTRL_FIFO_L_LEVEL_OFFSET) &
	       EPDC_FIFOCTRL_FIFO_L_LEVEL_MASK);
	__raw_writel(reg_val, EPDC_FIFOCTRL);

	/* EPDC_TEMP - Use default temp to get index */
	epdc_set_temp(mxc_epdc_fb_get_temp_index(fb_data, DEFAULT_TEMP));

	/* EPDC_RES */
	epdc_set_screen_res(epdc_mode->vmode->xres, epdc_mode->vmode->yres);


	DBG_MSG("hwcfg@(%p),display bus width=0x%02x\n",gptHWCFG,gptHWCFG->m_val.bDisplayBusWidth);

	switch(gptHWCFG->m_val.bDisplayBusWidth) 
	{
		case 1:
		// 16 bits .
		//
		
		/* 
		* EPDC_TCE_CTRL
		* VSCAN_HOLDOFF = 4
		* VCOM_MODE = MANUAL
		* VCOM_VAL = 0
		* DDR_MODE = DISABLED
		* LVDS_MODE_CE = DISABLED
		* LVDS_MODE = DISABLED
		* DUAL_SCAN = DISABLED
		* SDDO_WIDTH = 16bit
		* PIXELS_PER_SDCLK = 8
		*/

		DBG_MSG("16bits display bus width \n");
		reg_val =
			((epdc_mode->vscan_holdoff << EPDC_TCE_CTRL_VSCAN_HOLDOFF_OFFSET) &
			EPDC_TCE_CTRL_VSCAN_HOLDOFF_MASK)
			| EPDC_TCE_CTRL_PIXELS_PER_SDCLK_8
			| EPDC_TCE_CTRL_SDDO_WIDTH_16BIT 
			;
		break;

		case 2:
		// 8 bits ,mirror .
		//
		
		DBG_MSG("8bits mirror display bus width \n");

		reg_val =
			((epdc_mode->vscan_holdoff << EPDC_TCE_CTRL_VSCAN_HOLDOFF_OFFSET) &
			EPDC_TCE_CTRL_VSCAN_HOLDOFF_MASK)
			| EPDC_TCE_CTRL_SCAN_DIR_0_UP
			| EPDC_TCE_CTRL_SCAN_DIR_1_UP
			| EPDC_TCE_CTRL_PIXELS_PER_SDCLK_4
			//| EPDC_TCE_CTRL_SDDO_WIDTH_8BIT 
			;
		break;
	
		case 3:
		// 16 bits ,mirror .
		//
		
		DBG_MSG("16bits mirror display bus width \n");

		reg_val =
			((epdc_mode->vscan_holdoff << EPDC_TCE_CTRL_VSCAN_HOLDOFF_OFFSET) &
			EPDC_TCE_CTRL_VSCAN_HOLDOFF_MASK)
			| EPDC_TCE_CTRL_SCAN_DIR_0_UP
			| EPDC_TCE_CTRL_SCAN_DIR_1_UP
			| EPDC_TCE_CTRL_PIXELS_PER_SDCLK_8
			| EPDC_TCE_CTRL_SDDO_WIDTH_16BIT 
			;
		break;

	default:
		// normal 8 bits .
		
		DBG_MSG("8bits normal display bus width \n");
		/*
		 * EPDC_TCE_CTRL
		 * VSCAN_HOLDOFF = 4
		 * VCOM_MODE = MANUAL
		 * VCOM_VAL = 0
		 * DDR_MODE = DISABLED
		 * LVDS_MODE_CE = DISABLED
		 * LVDS_MODE = DISABLED
		 * DUAL_SCAN = DISABLED
		 * SDDO_WIDTH = 8bit
		 * PIXELS_PER_SDCLK = 4
		 */
		reg_val =
				((epdc_mode->vscan_holdoff << EPDC_TCE_CTRL_VSCAN_HOLDOFF_OFFSET) &
				 EPDC_TCE_CTRL_VSCAN_HOLDOFF_MASK)
				| EPDC_TCE_CTRL_PIXELS_PER_SDCLK_4;
		break;
	}
	__raw_writel(reg_val, EPDC_TCE_CTRL);



	/* EPDC_TCE_HSCAN */
	epdc_set_horizontal_timing(screeninfo->left_margin,
				   screeninfo->right_margin,
				   screeninfo->hsync_len,
				   screeninfo->hsync_len);

	/* EPDC_TCE_VSCAN */
	epdc_set_vertical_timing(screeninfo->upper_margin,
				 screeninfo->lower_margin,
				 screeninfo->vsync_len);

	/* EPDC_TCE_OE */
	reg_val =
	    ((epdc_mode->sdoed_width << EPDC_TCE_OE_SDOED_WIDTH_OFFSET) &
	     EPDC_TCE_OE_SDOED_WIDTH_MASK)
	    | ((epdc_mode->sdoed_delay << EPDC_TCE_OE_SDOED_DLY_OFFSET) &
	       EPDC_TCE_OE_SDOED_DLY_MASK)
	    | ((epdc_mode->sdoez_width << EPDC_TCE_OE_SDOEZ_WIDTH_OFFSET) &
	       EPDC_TCE_OE_SDOEZ_WIDTH_MASK)
	    | ((epdc_mode->sdoez_delay << EPDC_TCE_OE_SDOEZ_DLY_OFFSET) &
	       EPDC_TCE_OE_SDOEZ_DLY_MASK);
	__raw_writel(reg_val, EPDC_TCE_OE);

	/* EPDC_TCE_TIMING1 */
	__raw_writel(0x0, EPDC_TCE_TIMING1);

	/* EPDC_TCE_TIMING2 */
	reg_val =
	    ((epdc_mode->gdclk_hp_offs << EPDC_TCE_TIMING2_GDCLK_HP_OFFSET) &
	     EPDC_TCE_TIMING2_GDCLK_HP_MASK)
	    | ((epdc_mode->gdsp_offs << EPDC_TCE_TIMING2_GDSP_OFFSET_OFFSET) &
	       EPDC_TCE_TIMING2_GDSP_OFFSET_MASK);
	__raw_writel(reg_val, EPDC_TCE_TIMING2);

	/* EPDC_TCE_TIMING3 */
	reg_val =
	    ((epdc_mode->gdoe_offs << EPDC_TCE_TIMING3_GDOE_OFFSET_OFFSET) &
	     EPDC_TCE_TIMING3_GDOE_OFFSET_MASK)
	    | ((epdc_mode->gdclk_offs << EPDC_TCE_TIMING3_GDCLK_OFFSET_OFFSET) &
	       EPDC_TCE_TIMING3_GDCLK_OFFSET_MASK);
	__raw_writel(reg_val, EPDC_TCE_TIMING3);

	/*
	 * EPDC_TCE_SDCFG
	 * SDCLK_HOLD = 1
	 * SDSHR = 1
	 * NUM_CE = 1
	 * SDDO_REFORMAT = FLIP_PIXELS
	 * SDDO_INVERT = DISABLED
	 * PIXELS_PER_CE = display horizontal resolution
	 */
	num_ce = epdc_mode->num_ce;
	if (num_ce == 0)
		num_ce = 1;
	reg_val = EPDC_TCE_SDCFG_SDCLK_HOLD | EPDC_TCE_SDCFG_SDSHR
	    | ((num_ce << EPDC_TCE_SDCFG_NUM_CE_OFFSET) &
	       EPDC_TCE_SDCFG_NUM_CE_MASK)
	    | EPDC_TCE_SDCFG_SDDO_REFORMAT_FLIP_PIXELS
	    | ((epdc_mode->vmode->xres/num_ce << EPDC_TCE_SDCFG_PIXELS_PER_CE_OFFSET) &
	       EPDC_TCE_SDCFG_PIXELS_PER_CE_MASK);
	__raw_writel(reg_val, EPDC_TCE_SDCFG);

	/*
	 * EPDC_TCE_GDCFG
	 * GDRL = 1
	 * GDOE_MODE = 0;
	 * GDSP_MODE = 0;
	 */
	reg_val = EPDC_TCE_SDCFG_GDRL;
	__raw_writel(reg_val, EPDC_TCE_GDCFG);

	/*
	 * EPDC_TCE_POLARITY
	 * SDCE_POL = ACTIVE LOW
	 * SDLE_POL = ACTIVE HIGH
	 * SDOE_POL = ACTIVE HIGH
	 * GDOE_POL = ACTIVE HIGH
	 * GDSP_POL = ACTIVE LOW
	 */
	reg_val = EPDC_TCE_POLARITY_SDLE_POL_ACTIVE_HIGH
	    | EPDC_TCE_POLARITY_SDOE_POL_ACTIVE_HIGH
	    | EPDC_TCE_POLARITY_GDOE_POL_ACTIVE_HIGH;
	__raw_writel(reg_val, EPDC_TCE_POLARITY);

	/* EPDC_IRQ_MASK */
	__raw_writel(EPDC_IRQ_TCE_UNDERRUN_IRQ, EPDC_IRQ_MASK);

	/*
	 * EPDC_GPIO
	 * PWRCOM = ?
	 * PWRCTRL = ?
	 * BDR = ?
	 */
	reg_val = ((0 << EPDC_GPIO_PWRCTRL_OFFSET) & EPDC_GPIO_PWRCTRL_MASK)
	    | ((0 << EPDC_GPIO_BDR_OFFSET) & EPDC_GPIO_BDR_MASK);
	__raw_writel(reg_val, EPDC_GPIO);
}

static void epdc_powerup(struct mxc_epdc_fb_data *fb_data)
{
	struct epd_vc_data vcd;
	int ret = 0;
	int iChk;
	mutex_lock(&fb_data->power_mutex);

	/*
	 * If power down request is pending, clear
	 * powering_down to cancel the request.
	 */
	if (fb_data->powering_down)
		fb_data->powering_down = false;

	if (fb_data->power_state == POWER_STATE_ON) {
		mutex_unlock(&fb_data->power_mutex);
		return;
	}

	dev_dbg(fb_data->dev, "EPDC Powerup\n");

	if (fb_data->wfm < 256) {
		/* fetch and display the voltage control data for waveform mode 0, temp range 0 */
		fetch_Epdc_Pmic_Voltages(&vcd, fb_data, fb_data->wfm, fb_data->temp_index);
	}
	else
		vcd.v5 = 0;
		

    fb_data->updates_active = true;
    


#ifdef USE_PMIC //[
#else //][!USE_PMIC

	if(6==gptHWCFG->m_val.bDisplayCtrl) {
		// Only PMIC can support VCD data .

	if(fb_data->waveform_vcd_buffer) {
		DBG_MSG(" *** enabling the VCOM (%d,%d) ***\n", fb_data->wfm, fb_data->temp_index);
		if (fb_data->wfm < 256) {
			int vcom_uV, new_vcom_uV;
			int v5_sign = 1;
			int v5_offset = vcd.v5 & 0x7fff;
			int vcom_mV;
			int vcom_mV_offset;
			int vcd_offset_uV;
	

			if(0x09==gbFPL_Platform){
				// V320 waveform .
				vcom_mV_offset=-250;
			}
			else {
				vcom_mV_offset=0;
			}
			
			/* get vcom offset value */
			if (vcd.v5 & 0x8000) {
				v5_sign = -1;
			}
/*
			if(8==gptHWCFG->m_val.bDisplayCtrl) {
				fp9928_vcom_get_cached(&vcom_mV);
			}
			else 
*/
			{
				tps65185_vcom_get_cached(&vcom_mV);
			}
			vcom_uV=vcom_mV*1000;

			DBG_MSG("**** PMIC VCOM mV=%d,vcom_nominal=%d,v5_offset=%d,v5_sign=%d,vcom_mv_offset=%d *****\n",
					vcom_mV,vcom_nominal,v5_offset,v5_sign,vcom_mV_offset);

			vcd_offset_uV = v5_offset * 3125 * v5_sign;
			new_vcom_uV = vcom_nominal + vcd_offset_uV+(vcom_mV_offset*1000);
			if(vcom_uV!=new_vcom_uV) {
				//if ((new_vcom_uV >= -3050000) && ( new_vcom_uV <= -500000)) 
				if ((new_vcom_uV <= 0) && ( new_vcom_uV >= -5000000)) 
				{
					printk("vcom %d000 ==> %d uV (vcd offset:0x%04x=%duV,force %d000 uV)\n", 
							vcom_mV, new_vcom_uV, vcd.v5,vcd_offset_uV,vcom_mV_offset);
					
					vcom_mV=new_vcom_uV/1000;
/*
					if(8==gptHWCFG->m_val.bDisplayCtrl) {
						fp9928_vcom_set(vcom_mV,0);
					}
					else 
*/
					{
						tps65185_vcom_set(vcom_mV,0);
					}
				}
				else {
					printk(" adjusted VCOM is out of range, %d uV (offset:0x%04x)\n", new_vcom_uV, vcd.v5);
				}
			}

		}
	}

	}

		

	if(6==gptHWCFG->m_val.bDisplayCtrl) {
#if 1
		// imx508 + tps16585 .
		unsigned long dwTPS65185_mode = TPS65185_MODE_ACTIVE;

		iChk = tps65185_chg_mode(&dwTPS65185_mode,1);
		if(iChk<0) {
			printk(KERN_ERR "%s(%d):[warning] change to power active fail,errno=%d !\n",
				__FILE__,__LINE__,iChk);
		}
		/*	
		iChk = tps65185_wait_panel_poweron();
		if(iChk<0) {
			printk(KERN_ERR "%s(%d):[warning] wait power on fail,errno=%d !\n",
				__FILE__,__LINE__,iChk);
		}
		*/

		//gpio_direction_output(EPDC_VCOM, 1);
		//msleep(10);
		
#endif
	}
	else 
	{
		gpio_direction_output(GPIO_PWRALL, 1);msleep(50);
		gpio_direction_output(EPDC_VCOM, 1);msleep(15);
	}
#endif //] USE_PMIC

	/* Enable pins used by EPDC */
	if (fb_data->pdata->enable_pins)
		fb_data->pdata->enable_pins();

	
	/* Enable clocks to EPDC */
	clk_enable(fb_data->epdc_clk_axi);
	clk_enable(fb_data->epdc_clk_pix);
	__raw_writel(EPDC_CTRL_CLKGATE, EPDC_CTRL_CLEAR);

#ifdef USE_PMIC
	printk(" *** enabling the EPDC PMIC (%d,%d) ***\n",fb_data->wfm, fb_data->temp_index);
	/* Enable power to the EPD panel */
	ret = regulator_enable(fb_data->display_regulator);
	if (IS_ERR((void *)ret)) {
		dev_err(fb_data->dev, "Unable to enable DISPLAY regulator."
			"err = 0x%x\n", ret);
		mutex_unlock(&fb_data->power_mutex);
		return;
	}

	printk(" *** enabling the VCOM (%d,%d) ***\n", fb_data->wfm, fb_data->temp_index);
	if (fb_data->wfm < 256) {
		int vcom_uV, new_vcom_uV;
		int v5_sign = 1;
		int v5_offset = vcd.v5 & 0x7fff;
		
		/* get vcom offset value */
		if (vcd.v5 & 0x8000) {
			v5_sign = -1;
		}
		vcom_uV = regulator_get_voltage(fb_data->vcom_regulator);
		new_vcom_uV = vcom_nominal + v5_offset * 3125 * v5_sign;
		if ((new_vcom_uV >= -3050000) && ( new_vcom_uV <= -500000)) {
			printk(" current vcom is %d uV, adjusted VCOM will be %d uV (offset:0x%04x)\n", vcom_uV, new_vcom_uV, vcd.v5);
			regulator_set_voltage(fb_data->vcom_regulator, new_vcom_uV, new_vcom_uV);
		}
			printk(" adjusted VCOM is out of range, %d uV (offset:0x%04x)\n", new_vcom_uV, vcd.v5);
	}	

	ret = regulator_enable(fb_data->vcom_regulator);
	if (IS_ERR((void *)ret)) {
		dev_err(fb_data->dev, "Unable to enable VCOM regulator."
			"err = 0x%x\n", ret);
		mutex_unlock(&fb_data->power_mutex);
		return;
	}
#else
#endif
	
	fb_data->power_state = POWER_STATE_ON;

	mutex_unlock(&fb_data->power_mutex);
}

static void epdc_powerdown(struct mxc_epdc_fb_data *fb_data)
{
	int iChk;
	
	GALLEN_DBGLOCAL_BEGIN();
	mutex_lock(&fb_data->power_mutex);

	/* If powering_down has been cleared, a powerup
	 * request is pre-empting this powerdown request.
	 */
	if (!fb_data->powering_down
		|| (fb_data->power_state == POWER_STATE_OFF)) {
		mutex_unlock(&fb_data->power_mutex);
		GALLEN_DBGLOCAL_ESC();
		return;
	}

	dev_dbg(fb_data->dev, "EPDC Powerdown\n");

#ifdef USE_PMIC
	/* Disable power to the EPD panel */
	regulator_disable(fb_data->vcom_regulator);
	regulator_disable(fb_data->display_regulator);
#else
#endif

	/* Disable clocks to EPDC */
	__raw_writel(EPDC_CTRL_CLKGATE, EPDC_CTRL_SET);
	clk_disable(fb_data->epdc_clk_pix);
	clk_disable(fb_data->epdc_clk_axi);

	/* Disable pins used by EPDC (to prevent leakage current) */
	if (fb_data->pdata->disable_pins)
	{
		GALLEN_DBGLOCAL_RUNLOG(0);
		fb_data->pdata->disable_pins();
	}

#ifndef DO_NOT_POWEROFF //[
	if(6==gptHWCFG->m_val.bDisplayCtrl) 
	{
		// imx508 + tps16585 .
		//msleep(10);
		//gpio_direction_output(EPDC_VCOM, 0);
		
		//if(!(0==giRootDevNum&&2==giRootPartNum)) 
		{
			// imx508 + tps16585 .
			//unsigned long dwTPS65185_mode = TPS65185_MODE_SLEEP;
			unsigned long dwTPS65185_mode = TPS65185_MODE_STANDBY;
			iChk = tps65185_chg_mode(&dwTPS65185_mode,1);
			if(iChk<0) {
				printk(KERN_ERR "%s(%d):[warning] change to power sleep fail ,errno=%d !\n",
					__FILE__,__LINE__,iChk);
			}	
			/*	
			iChk = tps65185_wait_panel_poweroff();
			if(iChk<0) {
				printk(KERN_ERR "%s(%d):[warning] wait power off fail ,errno=%d !\n",
					__FILE__,__LINE__,iChk);
			}	
			*/
		}
		//else {
			//printk(KERN_ERR "root=/dev/mmcblk0p2 skip PMIC power down !\n");
		//}

	}
	else 
	{
		msleep(5);
		gpio_direction_output(EPDC_VCOM, 0);
	
		msleep(5);// gallen add 2011/06/17 .
		gpio_direction_output(GPIO_PWRALL, 0);
	}
#endif //]DO_NOT_POWEROFF


	

	fb_data->power_state = POWER_STATE_OFF;
	fb_data->powering_down = false;

	if (fb_data->wait_for_powerdown) {
		GALLEN_DBGLOCAL_RUNLOG(1);
		fb_data->wait_for_powerdown = false;
		complete(&fb_data->powerdown_compl);
	}

	mutex_unlock(&fb_data->power_mutex);
	GALLEN_DBGLOCAL_END();
}


#include "mxc_epdc_fake_s1d13522.c"


static void epdc_init_sequence(struct mxc_epdc_fb_data *fb_data)
{
	/* Initialize EPDC, passing pointer to EPDC registers */

	// gallen add : gpio request 
	mxc_iomux_v3_setup_pad(MX50_PAD_EIM_CRE__GPIO_1_27);
	if(0!=gpio_request(GPIO_PWRALL, "epd_power_on")) {
		//printk(KERN_ERR "%s(%d) : epd_power_on request fail !\n",__FUNCTION__,__LINE__);
	}

	epdc_init_settings(fb_data);
	__raw_writel(fb_data->waveform_buffer_phys, EPDC_WVADDR);
	__raw_writel(fb_data->working_buffer_phys, EPDC_WB_ADDR);
	fb_data->in_init = true;
	//if (epdc_powerup(fb_data) == 0)
	//	draw_mode0(fb_data);
	//epdc_powerdown(fb_data);
	fb_data->updates_active = false;	
	
}


static int mxc_epdc_fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	u32 len;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	
	u32 mem_start,mem_len;

	GALLEN_DBGLOCAL_BEGIN();
	
#ifdef CONFIG_ANDROID //[
#else //][!CONFIG_ANDROID
	fake_s1d13522_progress_stop();
	k_fake_s1d13522_wait_inited();
#endif //]CONFIG_ANDROID
	
	if(0==gptHWCFG->m_val.bUIStyle) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		//mem_start = (u32)gpbLOGO_paddr;
		//mem_len = (u32)((2048*1024*1024)-1024);
		mem_start = info->fix.smem_start+info->fix.smem_len;
		mem_len = info->fix.smem_len;

	}
	else {
		GALLEN_DBGLOCAL_RUNLOG(1);
		mem_start = info->fix.smem_start;
		mem_len = info->fix.smem_len;
	}
	
	GALLEN_DBGLOCAL_PRINTMSG("mem_start=0x%x,mem_len=0x%x\n",\
		mem_start,mem_len);
	GALLEN_DBGLOCAL_PRINTMSG("vma.vm_start=0x%x,vma.vm_end=0x%x\n",\
		vma->vm_start,vma->vm_end);
	GALLEN_DBGLOCAL_PRINTMSG("offset=0x%x\n",\
		offset);

	if (offset < mem_len) {
		/* mapping framebuffer memory */
		GALLEN_DBGLOCAL_RUNLOG(2);
		len = mem_len - offset;
		vma->vm_pgoff = (mem_start + offset) >> PAGE_SHIFT;
	} else
	{
		GALLEN_DBGLOCAL_ESC();
		return -EINVAL;
	}

	len = PAGE_ALIGN(len);
	if (vma->vm_end - vma->vm_start > len)
	{
		GALLEN_DBGLOCAL_ESC();
		return -EINVAL;
		//WARNING_MSG("[warning] %s:request mmap size too large !!\n",__FUNCTION__);
	}

	/* make buffers bufferable */
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	vma->vm_flags |= VM_IO | VM_RESERVED;

	if (remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
			    vma->vm_end - vma->vm_start, vma->vm_page_prot)) {
		GALLEN_DBGLOCAL_ESC();
		dev_dbg(info->device, "mmap remap_pfn_range failed\n");
		return -ENOBUFS;
	}

	GALLEN_DBGLOCAL_END();
	return 0;
}

static inline u_int _chan_to_field(u_int chan, struct fb_bitfield *bf)
{
	chan &= 0xffff;
	chan >>= 16 - bf->length;
	return chan << bf->offset;
}

/* Note to kobo - netronix
   please resolve this function - manot
 */
#if 0//kobo
static int mxc_epdc_fb_setcolreg(u_int regno, u_int red, u_int green,
				 u_int blue, u_int transp, struct fb_info *info)
{
	GALLEN_DBGLOCAL_MUTEBEGIN();
	if (regno >= 256)	/* no. of hw registers */
	{
		GALLEN_DBGLOCAL_ESC();
		return 1;
	}
	/*
	 * Program hardware... do anything you want with transp
	 */

	/* grayscale works only partially under directcolor */
	if (info->var.grayscale) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		/* grayscale = 0.30*R + 0.59*G + 0.11*B */
		red = green = blue = (red * 77 + green * 151 + blue * 28) >> 8;
	}

#define CNVT_TOHW(val, width) ((((val)<<(width))+0x7FFF-(val))>>16)
	switch (info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:GALLEN_DBGLOCAL_RUNLOG(1);
	case FB_VISUAL_PSEUDOCOLOR:GALLEN_DBGLOCAL_RUNLOG(2);
		red = CNVT_TOHW(red, info->var.red.length);
		green = CNVT_TOHW(green, info->var.green.length);
		blue = CNVT_TOHW(blue, info->var.blue.length);
		transp = CNVT_TOHW(transp, info->var.transp.length);
		break;
	case FB_VISUAL_DIRECTCOLOR:GALLEN_DBGLOCAL_RUNLOG(3);
		red = CNVT_TOHW(red, 8);	/* expect 8 bit DAC */
		green = CNVT_TOHW(green, 8);
		blue = CNVT_TOHW(blue, 8);
		/* hey, there is bug in transp handling... */
		transp = CNVT_TOHW(transp, 8);
		break;
	}
#undef CNVT_TOHW
	/* Truecolor has hardware independent palette */
	if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
		GALLEN_DBGLOCAL_RUNLOG(4);

		if (regno >= 16) {
			GALLEN_DBGLOCAL_ESC();
			return 1;
		}

		((u32 *) (info->pseudo_palette))[regno] =
		    (red << info->var.red.offset) |
		    (green << info->var.green.offset) |
		    (blue << info->var.blue.offset) |
		    (transp << info->var.transp.offset);
	}
	GALLEN_DBGLOCAL_END();
	return 0;
}
#else
static int mxc_epdc_fb_setcolreg(u_int regno, u_int red, u_int green,
				 u_int blue, u_int transp, struct fb_info *info)
{
	unsigned int val;
	int ret = 1;

	GALLEN_DBGLOCAL_MUTEBEGIN();
	/*
	 * If greyscale is true, then we convert the RGB value
	 * to greyscale no matter what visual we are using.
	 */
	if (info->var.grayscale) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		red = green = blue = (19595 * red + 38470 * green +
				      7471 * blue) >> 16;

	}
	switch (info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:
		GALLEN_DBGLOCAL_RUNLOG(1);
		/*
		 * 16-bit True Colour.  We encode the RGB value
		 * according to the RGB bitfield information.
		 */
		if (regno < 16) {
			GALLEN_DBGLOCAL_RUNLOG(2);
			u32 *pal = info->pseudo_palette;

			val = _chan_to_field(red, &info->var.red);
			val |= _chan_to_field(green, &info->var.green);
			val |= _chan_to_field(blue, &info->var.blue);

			GALLEN_DBGLOCAL_PRINTMSG("color[%d]=0x%08x,RGB=(%d,%d,%d)\n",
					regno,val,(int)red,(int)green,(int)blue);
			pal[regno] = val;
			ret = 0;
		}
		break;

	case FB_VISUAL_STATIC_PSEUDOCOLOR:
		GALLEN_DBGLOCAL_RUNLOG(3);
	case FB_VISUAL_PSEUDOCOLOR:
		GALLEN_DBGLOCAL_RUNLOG(4);
		break;
	}

	GALLEN_DBGLOCAL_MUTEEND();
	return ret;
}
#endif

static int mxc_epdc_fb_setcmap(struct fb_cmap *cmap, struct fb_info *info)
{
	int count, index, r;
	u16 *red, *green, *blue, *transp;
	u16 trans = 0xffff;
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;
	int i;

	GALLEN_DBGLOCAL_BEGIN();
	dev_dbg(fb_data->dev, "setcmap\n");

	if (info->fix.visual == FB_VISUAL_STATIC_PSEUDOCOLOR) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		/* Only support an 8-bit, 256 entry lookup */
		if (cmap->len != 256) {
			GALLEN_DBGLOCAL_ESC();
			return 1;
		}

		mxc_epdc_fb_flush_updates(fb_data);

		mutex_lock(&fb_data->pxp_mutex);
		/*
		 * Store colormap in pxp_conf structure for later transmit
		 * to PxP during update process to convert gray pixels.
		 *
		 * Since red=blue=green for pseudocolor visuals, we can
		 * just use red values.
		 */
		for (i = 0; i < 256; i++)
			fb_data->pxp_conf.proc_data.lut_map[i] = cmap->red[i] & 0xFF;

		fb_data->pxp_conf.proc_data.lut_map_updated = true;

		mutex_unlock(&fb_data->pxp_mutex);
	} else {

		GALLEN_DBGLOCAL_RUNLOG(1);

		red     = cmap->red;
		green   = cmap->green;
		blue    = cmap->blue;
		transp  = cmap->transp;
		index   = cmap->start;

		for (count = 0; count < cmap->len; count++) {
			if (transp) {
				GALLEN_DBGLOCAL_RUNLOG(2);
				trans = *transp++;
			}
			r = mxc_epdc_fb_setcolreg(index++, *red++, *green++, *blue++,
						trans, info);
			if (r != 0) {
				GALLEN_DBGLOCAL_ESC();
				return r;
			}
		}
	}

	GALLEN_DBGLOCAL_END();
	return 0;
}

static void adjust_coordinates(struct mxc_epdc_fb_data *fb_data,
	struct mxcfb_rect *update_region, struct mxcfb_rect *adj_update_region)
{
	struct fb_var_screeninfo *screeninfo = &fb_data->epdc_fb_var;
	u32 rotation = fb_data->epdc_fb_var.rotate;
	u32 temp;

#if 0
	if(3==gptHWCFG->m_val.bDisplayResolution) {
		// ED068OG1 @ 1440x1080 .
		if(FB_ROTATE_UR==rotation) {
			rotation = FB_ROTATE_UD;
		}
		else if(FB_ROTATE_UD==rotation) {
			rotation = FB_ROTATE_UR;
		}
		else if(FB_ROTATE_CW==rotation) {
			rotation = FB_ROTATE_CCW;
		}
		else if(FB_ROTATE_CCW==rotation) {
			rotation = FB_ROTATE_CW;
		}
	}
#endif

	/* If adj_update_region == NULL, pass result back in update_region */
	/* If adj_update_region == valid, use it to pass back result */
	if (adj_update_region)
		switch (rotation) {
		case FB_ROTATE_UR:
			adj_update_region->top = update_region->top;
			adj_update_region->left = update_region->left;
			adj_update_region->width = update_region->width;
			adj_update_region->height = update_region->height;
			break;
		case FB_ROTATE_CW:
			adj_update_region->top = update_region->left;
			adj_update_region->left = screeninfo->yres -
				(update_region->top + update_region->height);
			adj_update_region->width = update_region->height;
			adj_update_region->height = update_region->width;
			break;
		case FB_ROTATE_UD:
			adj_update_region->width = update_region->width;
			adj_update_region->height = update_region->height;
			adj_update_region->top = screeninfo->yres -
				(update_region->top + update_region->height);
			adj_update_region->left = screeninfo->xres -
				(update_region->left + update_region->width);
			break;
		case FB_ROTATE_CCW:
			adj_update_region->left = update_region->top;
			adj_update_region->top = screeninfo->xres -
				(update_region->left + update_region->width);
			adj_update_region->width = update_region->height;
			adj_update_region->height = update_region->width;
			break;
		}
	else
		switch (rotation) {
		case FB_ROTATE_UR:
			/* No adjustment needed */
			break;
		case FB_ROTATE_CW:
			temp = update_region->top;
			update_region->top = update_region->left;
			update_region->left = screeninfo->yres -
				(temp + update_region->height);
			temp = update_region->width;
			update_region->width = update_region->height;
			update_region->height = temp;
			break;
		case FB_ROTATE_UD:
			update_region->top = screeninfo->yres -
				(update_region->top + update_region->height);
			update_region->left = screeninfo->xres -
				(update_region->left + update_region->width);
			break;
		case FB_ROTATE_CCW:
			temp = update_region->left;
			update_region->left = update_region->top;
			update_region->top = screeninfo->xres -
				(temp + update_region->width);
			temp = update_region->width;
			update_region->width = update_region->height;
			update_region->height = temp;
			break;
		}
}

/*
 * Set fixed framebuffer parameters based on variable settings.
 *
 * @param       info     framebuffer information pointer
 */
static int mxc_epdc_fb_set_fix(struct fb_info *info)
{
	struct fb_fix_screeninfo *fix = &info->fix;
	struct fb_var_screeninfo *var = &info->var;

	GALLEN_DBGLOCAL_BEGIN();
	fix->line_length = var->xres_virtual * var->bits_per_pixel / 8;

	fix->type = FB_TYPE_PACKED_PIXELS;
	fix->accel = FB_ACCEL_NONE;
	if (var->grayscale)
		fix->visual = FB_VISUAL_STATIC_PSEUDOCOLOR;
	else
		fix->visual = FB_VISUAL_TRUECOLOR;
	fix->xpanstep = 1;
	fix->ypanstep = 1;

	GALLEN_DBGLOCAL_END();
	return 0;
}

/*
 * This routine actually sets the video mode. It's in here where we
 * the hardware state info->par and fix which can be affected by the
 * change in par. For this driver it doesn't do much.
 *
 */
static int mxc_epdc_fb_set_par(struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;
	struct pxp_config_data *pxp_conf = &fb_data->pxp_conf;
	struct pxp_proc_data *proc_data = &pxp_conf->proc_data;
	struct fb_var_screeninfo *screeninfo = &fb_data->info.var;
	struct mxc_epdc_fb_mode *epdc_modes = fb_data->pdata->epdc_mode;
	int i;
	int ret;
#ifdef USE_QUEUESPIN_LOCK //[
	unsigned long flags;
#endif //] USE_QUEUESPIN_LOCK

	__u32 xoffset_old, yoffset_old;

	GALLEN_DBGLOCAL_BEGIN();

	/*
	 * Can't change the FB parameters until current updates have completed.
	 * This function returns when all active updates are done.
	 */
	mxc_epdc_fb_flush_updates(fb_data);

#ifdef USE_QUEUESPIN_LOCK //[
	spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
	/*
	 * Set all screeninfo except for xoffset/yoffset
	 * Subsequent call to pan_display will handle those.
	 */
	xoffset_old = fb_data->epdc_fb_var.xoffset;
	yoffset_old = fb_data->epdc_fb_var.yoffset;
	fb_data->epdc_fb_var = *screeninfo;
	fb_data->epdc_fb_var.xoffset = xoffset_old;
	fb_data->epdc_fb_var.yoffset = yoffset_old;
#ifdef USE_QUEUESPIN_LOCK //[
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK


	mutex_lock(&fb_data->pxp_mutex);

	/*
	 * Update PxP config data (used to process FB regions for updates)
	 * based on FB info and processing tasks required
	 */

	/* Initialize non-channel-specific PxP parameters */
	proc_data->drect.left = proc_data->srect.left = 0;
	proc_data->drect.top = proc_data->srect.top = 0;
	proc_data->drect.width = proc_data->srect.width = screeninfo->xres;
	proc_data->drect.height = proc_data->srect.height = screeninfo->yres;
	proc_data->scaling = 0;
	proc_data->hflip = 0;
	proc_data->vflip = 0;
	proc_data->rotate = screeninfo->rotate;
	proc_data->bgcolor = 0;
	proc_data->overlay_state = 0;
	proc_data->lut_transform = PXP_LUT_NONE;

	/*
	 * configure S0 channel parameters
	 * Parameters should match FB format/width/height
	 */
	if (screeninfo->grayscale) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		pxp_conf->s0_param.pixel_fmt = PXP_PIX_FMT_GREY;
	}
	else {
		GALLEN_DBGLOCAL_RUNLOG(1);
		switch (screeninfo->bits_per_pixel) {
		case 16:GALLEN_DBGLOCAL_RUNLOG(2);
			pxp_conf->s0_param.pixel_fmt = PXP_PIX_FMT_RGB565;
			break;
		case 24:GALLEN_DBGLOCAL_RUNLOG(3);
			pxp_conf->s0_param.pixel_fmt = PXP_PIX_FMT_RGB24;
			break;
		case 32:GALLEN_DBGLOCAL_RUNLOG(4);
			pxp_conf->s0_param.pixel_fmt = PXP_PIX_FMT_RGB32;
			break;
		default:GALLEN_DBGLOCAL_RUNLOG(5);
			pxp_conf->s0_param.pixel_fmt = PXP_PIX_FMT_RGB565;
			break;
		}
	}
	pxp_conf->s0_param.width = screeninfo->xres_virtual;
	pxp_conf->s0_param.height = screeninfo->yres;
	pxp_conf->s0_param.color_key = -1;
	pxp_conf->s0_param.color_key_enable = false;

	/*
	 * Initialize Output channel parameters
	 * Output is Y-only greyscale
	 * Output width/height will vary based on update region size
	 */
	pxp_conf->out_param.width = screeninfo->xres;
	pxp_conf->out_param.height = screeninfo->yres;
	pxp_conf->out_param.pixel_fmt = PXP_PIX_FMT_GREY;

	mutex_unlock(&fb_data->pxp_mutex);

	/*
	 * If HW not yet initialized, check to see if we are being sent
	 * an initialization request.
	 */
	if (!fb_data->hw_ready) {
		struct fb_videomode mode;
		bool found_match = false;
		u32 xres_temp;
		
		GALLEN_DBGLOCAL_RUNLOG(6);

		fb_var_to_videomode(&mode, screeninfo);

		/* When comparing requested fb mode,
		   we need to use unrotated dimensions */
		if ((screeninfo->rotate == FB_ROTATE_CW) ||
			(screeninfo->rotate == FB_ROTATE_CCW)) {
			GALLEN_DBGLOCAL_RUNLOG(7);
			xres_temp = mode.xres;
			mode.xres = mode.yres;
			mode.yres = xres_temp;
		}

		/* Match videomode against epdc modes */
		DBG_MSG("%s(%d):find epd modes ...\n",__FILE__,__LINE__);
		for (i = 0; i < fb_data->pdata->num_modes; i++) {
			DBG_MSG("mode[%d]=%s\n",i,epdc_modes[i].vmode->name);
			GALLEN_DBGLOCAL_RUNLOG(8);
			if (!fb_mode_is_equal(epdc_modes[i].vmode, &mode)) {
				GALLEN_DBGLOCAL_RUNLOG(9);
				continue;
			}
			fb_data->cur_mode = &epdc_modes[i];
			found_match = true;
			break;
		}

		if (!found_match) {
			GALLEN_DBGLOCAL_RUNLOG(10);
			dev_err(fb_data->dev,
				"Failed to match requested video mode\n");
				
			GALLEN_DBGLOCAL_ESC();
			return EINVAL;
		}

		/* Found a match - Grab timing params */
		screeninfo->left_margin = mode.left_margin;
		screeninfo->right_margin = mode.right_margin;
		screeninfo->upper_margin = mode.upper_margin;
		screeninfo->lower_margin = mode.lower_margin;
		screeninfo->hsync_len = mode.hsync_len;
		screeninfo->vsync_len = mode.vsync_len;

		/* Initialize EPDC settings and init panel */
		ret =
		    mxc_epdc_fb_init_hw((struct fb_info *)fb_data);
		if (ret) {
			dev_err(fb_data->dev,
				"Failed to load panel waveform data\n");
			GALLEN_DBGLOCAL_ESC();
			return ret;
		}
	}

	/*
	 * EOF sync delay (in us) should be equal to the vscan holdoff time
	 * VSCAN_HOLDOFF time = (VSCAN_HOLDOFF value + 1) * Vertical lines
	 * Add 25us for additional margin
	 */
	fb_data->eof_sync_period = (fb_data->cur_mode->vscan_holdoff + 1) *
		1000000/(fb_data->cur_mode->vmode->refresh *
		(fb_data->cur_mode->vmode->upper_margin +
		fb_data->cur_mode->vmode->yres +
		fb_data->cur_mode->vmode->lower_margin +
		fb_data->cur_mode->vmode->vsync_len)) + 25;

	mxc_epdc_fb_set_fix(info);

	GALLEN_DBGLOCAL_END();
	return 0;
}

static int mxc_epdc_fb_check_var(struct fb_var_screeninfo *var,
				 struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;
	
	GALLEN_DBGLOCAL_BEGIN();

	if (!var->xres) 
	{
		GALLEN_DBGLOCAL_RUNLOG(10);
		var->xres = 1;
	}
		
	if (!var->yres)
	{
		GALLEN_DBGLOCAL_RUNLOG(11);
		var->yres = 1;
	}

	if (var->xres_virtual < var->xoffset + var->xres)
	{
		GALLEN_DBGLOCAL_RUNLOG(12);
		var->xres_virtual = var->xoffset + var->xres;
	}
	if (var->yres_virtual < var->yoffset + var->yres)
	{
		GALLEN_DBGLOCAL_RUNLOG(13);
		var->yres_virtual = var->yoffset + var->yres;
	}

	if ((var->bits_per_pixel != 32) && (var->bits_per_pixel != 24) &&
	    (var->bits_per_pixel != 16) && (var->bits_per_pixel != 8))
	{
		GALLEN_DBGLOCAL_RUNLOG(0);
		var->bits_per_pixel = default_bpp;
	}

	switch (var->bits_per_pixel) {
	case 8:GALLEN_DBGLOCAL_RUNLOG(0);
		if (var->grayscale != 0) {
			GALLEN_DBGLOCAL_RUNLOG(1);
			/*
			 * For 8-bit grayscale, R, G, and B offset are equal.
			 *
			 */
			var->red.length = 8;
			var->red.offset = 0;
			var->red.msb_right = 0;

			var->green.length = 8;
			var->green.offset = 0;
			var->green.msb_right = 0;

			var->blue.length = 8;
			var->blue.offset = 0;
			var->blue.msb_right = 0;

			var->transp.length = 0;
			var->transp.offset = 0;
			var->transp.msb_right = 0;
		} else {GALLEN_DBGLOCAL_RUNLOG(2);
			var->red.length = 3;
			var->red.offset = 5;
			var->red.msb_right = 0;

			var->green.length = 3;
			var->green.offset = 2;
			var->green.msb_right = 0;

			var->blue.length = 2;
			var->blue.offset = 0;
			var->blue.msb_right = 0;

			var->transp.length = 0;
			var->transp.offset = 0;
			var->transp.msb_right = 0;
		}
		break;
	case 16:GALLEN_DBGLOCAL_RUNLOG(3);
		var->red.length = 5;
		var->red.offset = 11;
		var->red.msb_right = 0;

		var->green.length = 6;
		var->green.offset = 5;
		var->green.msb_right = 0;

		var->blue.length = 5;
		var->blue.offset = 0;
		var->blue.msb_right = 0;

		var->transp.length = 0;
		var->transp.offset = 0;
		var->transp.msb_right = 0;
		break;
	case 24:GALLEN_DBGLOCAL_RUNLOG(4);
		var->red.length = 8;
		var->red.offset = 16;
		var->red.msb_right = 0;

		var->green.length = 8;
		var->green.offset = 8;
		var->green.msb_right = 0;

		var->blue.length = 8;
		var->blue.offset = 0;
		var->blue.msb_right = 0;

		var->transp.length = 0;
		var->transp.offset = 0;
		var->transp.msb_right = 0;
		break;
	case 32:GALLEN_DBGLOCAL_RUNLOG(5);
		var->red.length = 8;
		var->red.offset = 16;
		var->red.msb_right = 0;

		var->green.length = 8;
		var->green.offset = 8;
		var->green.msb_right = 0;

		var->blue.length = 8;
		var->blue.offset = 0;
		var->blue.msb_right = 0;

		var->transp.length = 8;
		var->transp.offset = 24;
		var->transp.msb_right = 0;
		break;
	}

	if(3==gptHWCFG->m_val.bDisplayResolution) {
		GALLEN_DBGLOCAL_RUNLOG(11);
		// ED068OG1 @ 1440x1080 .
		if(FB_ROTATE_UR==var->rotate) {
			GALLEN_DBGLOCAL_RUNLOG(12);
			var->rotate = FB_ROTATE_UD;
		}
		else if(FB_ROTATE_UD==var->rotate) {
			GALLEN_DBGLOCAL_RUNLOG(13);
			var->rotate = FB_ROTATE_UR;
		}
		else if(FB_ROTATE_CW==var->rotate) {
			GALLEN_DBGLOCAL_RUNLOG(14);
			var->rotate = FB_ROTATE_CCW;
		}
		else if(FB_ROTATE_CCW==var->rotate) {
			GALLEN_DBGLOCAL_RUNLOG(15);
			var->rotate = FB_ROTATE_CW;
		}
	}

	switch (var->rotate) {
	case FB_ROTATE_UR:GALLEN_DBGLOCAL_RUNLOG(6);
	case FB_ROTATE_UD:GALLEN_DBGLOCAL_RUNLOG(7);
		var->xres = fb_data->native_width;
		var->yres = fb_data->native_height;
		break;
	case FB_ROTATE_CW:GALLEN_DBGLOCAL_RUNLOG(8);
	case FB_ROTATE_CCW:GALLEN_DBGLOCAL_RUNLOG(9);
		var->xres = fb_data->native_height;
		var->yres = fb_data->native_width;
		break;
	default:GALLEN_DBGLOCAL_RUNLOG(10);
		/* Invalid rotation value */
		var->rotate = 0;
		dev_dbg(fb_data->dev, "Invalid rotation request\n");
		return -EINVAL;
	}

	var->xres_virtual = ALIGN(var->xres, 32);
	var->yres_virtual = ALIGN(var->yres, 128) * fb_data->num_screens;

	epdfbdc_set_width_height(gptDC,ALIGN(var->xres, 32),ALIGN(var->yres, 128),var->xres,var->yres);

	var->height = -1;
	var->width = -1;

	GALLEN_DBGLOCAL_END();
	return 0;
}

void mxc_epdc_fb_set_waveform_modes(struct mxcfb_waveform_modes *modes,
	struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;

	WARNING_MSG("%s(%d):%s() skip \n",__FILE__,__LINE__,__FUNCTION__);
	return ;

	memcpy(&fb_data->wv_modes, modes, sizeof(struct mxcfb_waveform_modes));
}
EXPORT_SYMBOL(mxc_epdc_fb_set_waveform_modes);

static int mxc_epdc_fb_get_temp_index(struct mxc_epdc_fb_data *fb_data, int temp)
{
	int i;
	int index = -1;
	int iTotallyHit=0;

	if (fb_data->trt_entries == 0) {
		dev_err(fb_data->dev,
			"No TRT exists...using default temp index,temp=%d\n",fb_data->temp_range_bounds[0]);
		return DEFAULT_TEMP_INDEX;
	}

#if 1 //[
	/* Search temperature ranges for a match */
	for (i = 0; i < fb_data->trt_entries; i++) {
		if (temp >= fb_data->temp_range_bounds[i]) 
		{
			index = i;
			if(temp < fb_data->temp_range_bounds[i+1]) {
				iTotallyHit = 1;
				break;
			}
		}
	}
#else //][!
	/* Search temperature ranges for a match */
	for (i = 0; i < fb_data->trt_entries; i++) {
		if ((temp >= fb_data->temp_range_bounds[i])
			&& (temp < fb_data->temp_range_bounds[i+1])) {
			index = i;
			iTotallyHit = 1;
			break;
		}
	}
#endif //]

	if (index < 0) {
		dev_err(fb_data->dev,
			"No TRT index (temp=%d) match,trt entries=%d...using default temp index,temp=%d\n",
			temp,fb_data->trt_entries,fb_data->temp_range_bounds[0]);
		return DEFAULT_TEMP_INDEX;
	}
	else {
		if(1!=iTotallyHit) {
			dev_err(fb_data->dev,
				"closer TRT index (temp=%d) match,trt entries=%d...using temp index=%d,temp=%d~%d\n",
				temp,fb_data->trt_entries,index,fb_data->temp_range_bounds[index],fb_data->temp_range_bounds[index+1]);
		}
	}


	dev_dbg(fb_data->dev, "Using temperature index %d\n", index);

	return index;
}

int mxc_epdc_fb_set_temperature(int temperature, struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;
#ifdef USE_QUEUESPIN_LOCK //[
	unsigned long flags;
#endif //] USE_QUEUESPIN_LOCK

	/* Store temp index. Used later when configuring updates. */
#ifdef USE_QUEUESPIN_LOCK //[
	spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
	
	DBG_MSG("%s() temp=%d\n",__FUNCTION__,temperature);
	fb_data->temp_index = mxc_epdc_fb_get_temp_index(fb_data, temperature);

#ifdef USE_QUEUESPIN_LOCK //[
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

	return 0;
}
EXPORT_SYMBOL(mxc_epdc_fb_set_temperature);

int mxc_epdc_fb_set_auto_update(u32 auto_mode, struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;

	dev_dbg(fb_data->dev, "Setting auto update mode to %d\n", auto_mode);

	if ((auto_mode == AUTO_UPDATE_MODE_AUTOMATIC_MODE)
		|| (auto_mode == AUTO_UPDATE_MODE_REGION_MODE))
		fb_data->auto_mode = auto_mode;
	else {
		dev_err(fb_data->dev, "Invalid auto update mode parameter.\n");
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(mxc_epdc_fb_set_auto_update);

int mxc_epdc_fb_set_merge_on_waveform_mismatch(int merge, struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;

	fb_data->merge_on_waveform_mismatch = (merge) ? 1 : 0;

	return 0;
}

int mxc_epdc_fb_set_upd_scheme(u32 upd_scheme, struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;

	dev_dbg(fb_data->dev, "Setting optimization level to %d\n", upd_scheme);

	/*
	 * Can't change the scheme until current updates have completed.
	 * This function returns when all active updates are done.
	 */
	mxc_epdc_fb_flush_updates(fb_data);

	if ((upd_scheme == UPDATE_SCHEME_SNAPSHOT)
		|| (upd_scheme == UPDATE_SCHEME_QUEUE)
		|| (upd_scheme == UPDATE_SCHEME_QUEUE_AND_MERGE))
		fb_data->upd_scheme = upd_scheme;
	else {
		dev_err(fb_data->dev, "Invalid update scheme specified.\n");
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(mxc_epdc_fb_set_upd_scheme);

static void copy_before_process(struct mxc_epdc_fb_data *fb_data,
	struct update_data_list *upd_data_list)
{
	struct mxcfb_update_data *upd_data =
		&upd_data_list->update_desc->upd_data;
	int i;
	unsigned char *temp_buf_ptr = fb_data->virt_addr_copybuf;
	unsigned char *src_ptr;
	struct mxcfb_rect *src_upd_region;
	int temp_buf_stride;
	int src_stride;
	int bpp = fb_data->epdc_fb_var.bits_per_pixel;
	int left_offs, right_offs;
	int x_trailing_bytes, y_trailing_bytes;
	int alt_buf_offset;

	/* Set source buf pointer based on input source, panning, etc. */
	if (upd_data->flags & EPDC_FLAG_USE_ALT_BUFFER) {
		src_upd_region = &upd_data->alt_buffer_data.alt_update_region;
		src_stride =
			upd_data->alt_buffer_data.width * bpp/8;
		alt_buf_offset = upd_data->alt_buffer_data.phys_addr -
			fb_data->info.fix.smem_start;
		src_ptr = fb_data->info.screen_base + alt_buf_offset
			+ src_upd_region->top * src_stride;
	} else {
		src_upd_region = &upd_data->update_region;
		src_stride = fb_data->epdc_fb_var.xres_virtual * bpp/8;
		src_ptr = fb_data->info.screen_base + fb_data->fb_offset
			+ src_upd_region->top * src_stride;
	}

	temp_buf_stride = ALIGN(src_upd_region->width, 8) * bpp/8;
	left_offs = src_upd_region->left * bpp/8;
	right_offs = src_upd_region->width * bpp/8;
	x_trailing_bytes = (ALIGN(src_upd_region->width, 8)
		- src_upd_region->width) * bpp/8;

	for (i = 0; i < src_upd_region->height; i++) {
		/* Copy the full line */
		memcpy(temp_buf_ptr, src_ptr + left_offs,
			src_upd_region->width * bpp/8);

		/* Clear any unwanted pixels at the end of each line */
		if (src_upd_region->width & 0x7) {
			memset(temp_buf_ptr + right_offs, 0x0,
				x_trailing_bytes);
		}

		temp_buf_ptr += temp_buf_stride;
		src_ptr += src_stride;
	}

	/* Clear any unwanted pixels at the bottom of the end of each line */
	if (src_upd_region->height & 0x7) {
		y_trailing_bytes = (ALIGN(src_upd_region->height, 8)
			- src_upd_region->height) *
			ALIGN(src_upd_region->width, 8) * bpp/8;
		memset(temp_buf_ptr, 0x0, y_trailing_bytes);
	}
}

static int epdc_process_update(struct update_data_list *upd_data_list,
				   struct mxc_epdc_fb_data *fb_data)
{
	struct mxcfb_rect *src_upd_region; /* Region of src buffer for update */
	struct mxcfb_rect pxp_upd_region;
	u32 src_width, src_height;
	u32 offset_from_4, bytes_per_pixel;
	u32 post_rotation_xcoord, post_rotation_ycoord, width_pxp_blocks;
	u32 pxp_input_offs, pxp_output_offs, pxp_output_shift;
	u32 hist_stat = 0;
	int width_unaligned, height_unaligned;
	bool input_unaligned = false;
	bool line_overflow = false;
	int pix_per_line_added;
	bool use_temp_buf = false;
	struct mxcfb_rect temp_buf_upd_region;
	struct update_desc_list *upd_desc_list = upd_data_list->update_desc;

	int ret;


	GALLEN_DBGLOCAL_BEGIN();
	/*
	 * Gotta do a whole bunch of buffer ptr manipulation to
	 * work around HW restrictions for PxP & EPDC
	 */

	/*
	 * Are we using FB or an alternate (overlay)
	 * buffer for source of update?
	 */
	if (upd_desc_list->upd_data.flags & EPDC_FLAG_USE_ALT_BUFFER) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		src_width = upd_desc_list->upd_data.alt_buffer_data.width;
		src_height = upd_desc_list->upd_data.alt_buffer_data.height;
		src_upd_region = &upd_desc_list->upd_data.alt_buffer_data.alt_update_region;
	} else {
		GALLEN_DBGLOCAL_RUNLOG(1);
		src_width = fb_data->epdc_fb_var.xres_virtual;
		src_height = fb_data->epdc_fb_var.yres;
		src_upd_region = &upd_desc_list->upd_data.update_region;
	}

	bytes_per_pixel = fb_data->epdc_fb_var.bits_per_pixel/8;

	/*
	 * SW workaround for PxP limitation
	 *
	 * There are 3 cases where we cannot process the update data
	 * directly from the input buffer:
	 *
	 * 1) PxP must process 8x8 pixel blocks, and all pixels in each block
	 * are considered for auto-waveform mode selection. If the
	 * update region is not 8x8 aligned, additional unwanted pixels
	 * will be considered in auto-waveform mode selection.
	 *
	 * 2) PxP input must be 32-bit aligned, so any update
	 * address not 32-bit aligned must be shifted to meet the
	 * 32-bit alignment.  The PxP will thus end up processing pixels
	 * outside of the update region to satisfy this alignment restriction,
	 * which can affect auto-waveform mode selection.
	 *
	 * 3) If input fails 32-bit alignment, and the resulting expansion
	 * of the processed region would add at least 8 pixels more per
	 * line than the original update line width, the EPDC would
	 * cause screen artifacts by incorrectly handling the 8+ pixels
	 * at the end of each line.
	 *
	 * Workaround is to copy from source buffer into a temporary
	 * buffer, which we pad with zeros to match the 8x8 alignment
	 * requirement. This temp buffer becomes the input to the PxP.
	 */
	width_unaligned = src_upd_region->width & 0x7;
	height_unaligned = src_upd_region->height & 0x7;

	offset_from_4 = src_upd_region->left & 0x3;
	input_unaligned = ((offset_from_4 * bytes_per_pixel % 4) != 0) ?
				true : false;

	pix_per_line_added = (offset_from_4 * bytes_per_pixel % 4)
					/ bytes_per_pixel;
	if ((((fb_data->epdc_fb_var.rotate == FB_ROTATE_UR) ||
		fb_data->epdc_fb_var.rotate == FB_ROTATE_UD)) &&
		(ALIGN(src_upd_region->width, 8) <
			ALIGN(src_upd_region->width + pix_per_line_added, 8))) 
	{
		GALLEN_DBGLOCAL_RUNLOG(2);
		
		line_overflow = true;
	}

	/* Grab pxp_mutex here so that we protect access
	 * to copybuf in addition to the PxP structures */
	mutex_lock(&fb_data->pxp_mutex);
	if (((width_unaligned || height_unaligned || input_unaligned) &&
		(upd_desc_list->upd_data.waveform_mode == WAVEFORM_MODE_AUTO))
		|| line_overflow) {
		GALLEN_DBGLOCAL_RUNLOG(3);

		dev_dbg(fb_data->dev, "Copying update before processing.\n");

		/* Update to reflect what the new source buffer will be */
		src_width = ALIGN(src_upd_region->width, 8);
		src_height = ALIGN(src_upd_region->height, 8);

		copy_before_process(fb_data, upd_data_list);

		/*
		 * src_upd_region should now describe
		 * the new update buffer attributes.
		 */
		temp_buf_upd_region.left = 0;
		temp_buf_upd_region.top = 0;
		temp_buf_upd_region.width = src_upd_region->width;
		temp_buf_upd_region.height = src_upd_region->height;
		src_upd_region = &temp_buf_upd_region;

		use_temp_buf = true;
	}

	/*
	 * Compute buffer offset to account for
	 * PxP limitation (input must be 32-bit aligned)
	 */
	offset_from_4 = src_upd_region->left & 0x3;
	input_unaligned = ((offset_from_4 * bytes_per_pixel % 4) != 0) ?
				true : false;
	if (input_unaligned) {
		GALLEN_DBGLOCAL_RUNLOG(4);
		/* Leave a gap between PxP input addr and update region pixels */
		pxp_input_offs =
			(src_upd_region->top * src_width + src_upd_region->left)
			* bytes_per_pixel & 0xFFFFFFFC;
		/* Update region should change to reflect relative position to input ptr */
		pxp_upd_region.top = 0;
		pxp_upd_region.left = (offset_from_4 * bytes_per_pixel % 4)
					/ bytes_per_pixel;
	} else {
		GALLEN_DBGLOCAL_RUNLOG(5);
		pxp_input_offs =
			(src_upd_region->top * src_width + src_upd_region->left)
			* bytes_per_pixel;
		/* Update region should change to reflect relative position to input ptr */
		pxp_upd_region.top = 0;
		pxp_upd_region.left = 0;
	}

	/* Update region dimensions to meet 8x8 pixel requirement */
	pxp_upd_region.width =
		ALIGN(src_upd_region->width + pxp_upd_region.left, 8);
	pxp_upd_region.height = ALIGN(src_upd_region->height, 8);

	switch (fb_data->epdc_fb_var.rotate) {
	case FB_ROTATE_UR:GALLEN_DBGLOCAL_RUNLOG(6);
	default:
		post_rotation_xcoord = pxp_upd_region.left;
		post_rotation_ycoord = pxp_upd_region.top;
		width_pxp_blocks = pxp_upd_region.width;
		break;
	case FB_ROTATE_CW:GALLEN_DBGLOCAL_RUNLOG(7);
		width_pxp_blocks = pxp_upd_region.height;
		post_rotation_xcoord = width_pxp_blocks - src_upd_region->height;
		post_rotation_ycoord = pxp_upd_region.left;
		break;
	case FB_ROTATE_UD:GALLEN_DBGLOCAL_RUNLOG(8);
		width_pxp_blocks = pxp_upd_region.width;
		post_rotation_xcoord = width_pxp_blocks - src_upd_region->width - pxp_upd_region.left;
		post_rotation_ycoord = pxp_upd_region.height - src_upd_region->height - pxp_upd_region.top;
		break;
	case FB_ROTATE_CCW:GALLEN_DBGLOCAL_RUNLOG(9);
		width_pxp_blocks = pxp_upd_region.height;
		post_rotation_xcoord = pxp_upd_region.top;
		post_rotation_ycoord = pxp_upd_region.width - src_upd_region->width - pxp_upd_region.left;
		break;
	}

	/* Update region start coord to force PxP to process full 8x8 regions */
	pxp_upd_region.top &= ~0x7;
	pxp_upd_region.left &= ~0x7;

	pxp_output_shift = ALIGN(post_rotation_xcoord, 8)
		- post_rotation_xcoord;

	pxp_output_offs = post_rotation_ycoord * width_pxp_blocks
		+ pxp_output_shift;

	upd_desc_list->epdc_offs = ALIGN(pxp_output_offs, 8);

	//mutex_lock(&fb_data->pxp_mutex);

	/* Source address either comes from alternate buffer
	   provided in update data, or from the framebuffer. */
	if (use_temp_buf) 
	{
		GALLEN_DBGLOCAL_RUNLOG(10);
		sg_dma_address(&fb_data->sg[0]) =
			fb_data->phys_addr_copybuf;
	}
	else if (upd_desc_list->upd_data.flags & EPDC_FLAG_USE_ALT_BUFFER)
	{
		GALLEN_DBGLOCAL_RUNLOG(11);
		sg_dma_address(&fb_data->sg[0]) =
			upd_desc_list->upd_data.alt_buffer_data.phys_addr
				+ pxp_input_offs;
	}
	else {
		GALLEN_DBGLOCAL_RUNLOG(12);
		sg_dma_address(&fb_data->sg[0]) =
			fb_data->info.fix.smem_start + fb_data->fb_offset
			+ pxp_input_offs;
		sg_set_page(&fb_data->sg[0],
			virt_to_page(fb_data->info.screen_base),
			fb_data->info.fix.smem_len,
			offset_in_page(fb_data->info.screen_base));
	}

	/* Update sg[1] to point to output of PxP proc task */
	sg_dma_address(&fb_data->sg[1]) = upd_data_list->phys_addr
						+ pxp_output_shift;
	sg_set_page(&fb_data->sg[1], virt_to_page(upd_data_list->virt_addr),
		    fb_data->max_pix_size,
		    offset_in_page(upd_data_list->virt_addr));

	/*
	 * Set PxP LUT transform type based on update flags.
	 */
	fb_data->pxp_conf.proc_data.lut_transform = 0;
	if (upd_desc_list->upd_data.flags & EPDC_FLAG_ENABLE_INVERSION)
	{
		GALLEN_DBGLOCAL_RUNLOG(13);
		fb_data->pxp_conf.proc_data.lut_transform |= PXP_LUT_INVERT;
	}
	if (upd_desc_list->upd_data.flags & EPDC_FLAG_FORCE_MONOCHROME)
	{
		GALLEN_DBGLOCAL_RUNLOG(14);
		fb_data->pxp_conf.proc_data.lut_transform |=
			PXP_LUT_BLACK_WHITE;
	}
	if (upd_desc_list->upd_data.flags & EPDC_FLAG_USE_CMAP) {
		GALLEN_DBGLOCAL_RUNLOG(23);
		fb_data->pxp_conf.proc_data.lut_transform |=
			PXP_LUT_USE_CMAP;
	}

	/*
	 * Toggle inversion processing if 8-bit
	 * inverted is the current pixel format.
	 */
	if (fb_data->epdc_fb_var.grayscale == GRAYSCALE_8BIT_INVERTED)
	{
		GALLEN_DBGLOCAL_RUNLOG(15);
		fb_data->pxp_conf.proc_data.lut_transform ^= PXP_LUT_INVERT;
	}

	/* TODO: Support REAGL LUT lookup in conjunction with other LUT
	 * transformations.  This code just overwrites other LUT options. */
	if (bufPixFormat==EPDC_FORMAT_BUF_PIXEL_FORMAT_P5N) {
		GALLEN_DBGLOCAL_RUNLOG(24);
		fb_data->pxp_conf.proc_data.lut_transform |= PXP_LUT_AA;
	}

	/* This is a blocking call, so upon return PxP tx should be done */
	ret = pxp_process_update(fb_data, src_width, src_height,
		&pxp_upd_region);
	if (ret) {
		dev_err(fb_data->dev, "Unable to submit PxP update task.\n");
		mutex_unlock(&fb_data->pxp_mutex);
		GALLEN_DBGLOCAL_ESC();
		return ret;
	}

	/* If needed, enable EPDC HW while ePxP is processing */
	if ((fb_data->power_state == POWER_STATE_OFF)
		|| fb_data->powering_down) {
		epdc_powerup(fb_data);
		GALLEN_DBGLOCAL_RUNLOG(16);
	}

	/* This is a blocking call, so upon return PxP tx should be done */
	ret = pxp_complete_update(fb_data, &hist_stat);
	if (ret) {
		dev_err(fb_data->dev, "Unable to complete PxP update task.\n");
		mutex_unlock(&fb_data->pxp_mutex);
		GALLEN_DBGLOCAL_ESC();
		return ret;
	}

	mutex_unlock(&fb_data->pxp_mutex);

	/* Update waveform mode from PxP histogram results */
	if (upd_desc_list->upd_data.waveform_mode == WAVEFORM_MODE_AUTO) {
		GALLEN_DBGLOCAL_RUNLOG(17);
		if (hist_stat & 0x1)
		{
			GALLEN_DBGLOCAL_RUNLOG(18);
			upd_desc_list->upd_data.waveform_mode =
				fb_data->wv_modes.mode_du;
		}
		else if (hist_stat & 0x2)
		{
			GALLEN_DBGLOCAL_RUNLOG(19);
			upd_desc_list->upd_data.waveform_mode =
				fb_data->wv_modes.mode_gc4;
		}
		else if (hist_stat & 0x4)
		{
			GALLEN_DBGLOCAL_RUNLOG(20);
			upd_desc_list->upd_data.waveform_mode =
				fb_data->wv_modes.mode_gc8;
		}
		else if (hist_stat & 0x8)
		{
			GALLEN_DBGLOCAL_RUNLOG(21);
#ifdef NTX_AUTOMODE_PATCH //[ waveform mode change automatically .
	#if !defined(NO_AUTO_REAGL_MODE)//[
			if (bufPixFormat==EPDC_FORMAT_BUF_PIXEL_FORMAT_P5N)
			{
				GALLEN_DBGLOCAL_RUNLOG(24);
				if(UPDATE_MODE_FULL==upd_desc_list->upd_data.update_mode) {
					upd_data_list->update_desc->upd_data.flags |= EPDC_FLAG_USE_AAD;
					upd_desc_list->upd_data.waveform_mode =
						fb_data->wv_modes.mode_aad;
				}
				else {
					upd_data_list->update_desc->upd_data.flags &= ~EPDC_FLAG_USE_AAD;
					upd_desc_list->upd_data.waveform_mode =
						fb_data->wv_modes.mode_aa;
				}
				/*
				printk("auto mode selection ,aa=%d,aad=%d,mode %d selected !\n",
						fb_data->wv_modes.mode_aa,fb_data->wv_modes.mode_aad,
						upd_desc_list->upd_data.waveform_mode);
				*/
			}
			else 
	#endif //]NO_AUTO_REAGL_MODE
			{
				if(UPDATE_MODE_FULL==upd_desc_list->upd_data.update_mode) {
					upd_desc_list->upd_data.waveform_mode =
						fb_data->wv_modes.mode_gc16;
				}
				else {
					upd_desc_list->upd_data.waveform_mode =
						fb_data->wv_modes.mode_gl16;
				}
			}
#else //][!NTX_AUTOMODE_PATCH
			upd_desc_list->upd_data.waveform_mode =
				fb_data->wv_modes.mode_gc16;
#endif//] NTX_AUTOMODE_PATCH
		}
		else
		{
			GALLEN_DBGLOCAL_RUNLOG(22);
			upd_desc_list->upd_data.waveform_mode =
				fb_data->wv_modes.mode_gc32;
		}

		dev_dbg(fb_data->dev, "hist_stat = 0x%x, new waveform = 0x%x\n",
			hist_stat, upd_desc_list->upd_data.waveform_mode);
	}

	GALLEN_DBGLOCAL_END();
	return 0;

}

static int epdc_submit_merge(struct update_desc_list *upd_desc_list,
				struct update_desc_list *update_to_merge,
				int merge_on_waveform_mismatch)
{
	struct mxcfb_update_data *a, *b;
	struct mxcfb_rect *arect, *brect;
	struct mxcfb_rect combine;
	bool use_flags = false;
	int waveform_mismatch;

	GALLEN_DBGLOCAL_BEGIN();

	a = &upd_desc_list->upd_data;
	b = &update_to_merge->upd_data;
	arect = &upd_desc_list->upd_data.update_region;
	brect = &update_to_merge->upd_data.update_region;

	/*
	 * Updates with different flags must be executed sequentially.
	 * Halt the merge process to ensure this.
	 */
	if (a->flags != b->flags) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		/*
		 * Special exception: if update regions are identical,
		 * we may be able to merge them.
		 */
		if ((arect->left != brect->left) ||
			(arect->top != brect->top) ||
			(arect->width != brect->width) ||
			(arect->height != brect->height)) 
		{
			GALLEN_DBGLOCAL_ESC();
			return MERGE_BLOCK;
		}

		use_flags = true;
	}

	if (merge_on_waveform_mismatch) {
		GALLEN_DBGLOCAL_RUNLOG(2);
		if (a->update_mode != b->update_mode) {
			GALLEN_DBGLOCAL_RUNLOG(3);
			a->update_mode = UPDATE_MODE_FULL;
		}

		if (a->waveform_mode != b->waveform_mode) {
			GALLEN_DBGLOCAL_RUNLOG(4);
			a->waveform_mode = WAVEFORM_MODE_AUTO;
		}

		waveform_mismatch = 0;
	} else {
		GALLEN_DBGLOCAL_RUNLOG(5);
		waveform_mismatch = (a->waveform_mode != b->waveform_mode &&
		                     a->waveform_mode != WAVEFORM_MODE_AUTO) ||
		                    a->update_mode != b->update_mode;
	}

	if (waveform_mismatch ||
		arect->left > (brect->left + brect->width) ||
		brect->left > (arect->left + arect->width) ||
		arect->top > (brect->top + brect->height) ||
		brect->top > (arect->top + arect->height)) 
	{
		GALLEN_DBGLOCAL_ESC();
		return MERGE_FAIL;
	}

	combine.left = arect->left < brect->left ? arect->left : brect->left;
	combine.top = arect->top < brect->top ? arect->top : brect->top;
	combine.width = (arect->left + arect->width) >
			(brect->left + brect->width) ?
			(arect->left + arect->width - combine.left) :
			(brect->left + brect->width - combine.left);
	combine.height = (arect->top + arect->height) >
			(brect->top + brect->height) ?
			(arect->top + arect->height - combine.top) :
			(brect->top + brect->height - combine.top);

	*arect = combine;

	/* Use flags of the later update */
	if (use_flags) {
		GALLEN_DBGLOCAL_RUNLOG(6);
		a->flags = b->flags;
	}

	/* Merge markers */
	list_splice_tail(&update_to_merge->upd_marker_list,
		&upd_desc_list->upd_marker_list);

	/* Merged update should take on the earliest order */
	upd_desc_list->update_order =
		(upd_desc_list->update_order > update_to_merge->update_order) ?
		upd_desc_list->update_order : update_to_merge->update_order;

	GALLEN_DBGLOCAL_END();
	return MERGE_OK;
}

#ifdef FW_IN_RAM //[

static void epdc_firmware_func(struct work_struct *work)
{
	struct mxc_epdc_fb_data *fb_data =
		container_of(work, struct mxc_epdc_fb_data, epdc_firmware_work);
	struct firmware fw;

	GALLEN_DBGLOCAL_BEGIN();
	fw.size = gdwWF_size;
	fw.data = (u8*)gpbWF_vaddr;

	printk("[%s]:fw p=%p,size=%u\n",__FUNCTION__,fw.data,fw.size);
	mxc_epdc_fb_fw_handler(&fw,fb_data);
	GALLEN_DBGLOCAL_END();
}
	
#endif //]FW_IN_RAM

static void epdc_submit_work_func(struct work_struct *work)
{
	int temp_index;
	struct update_data_list *next_update, *temp_update;
	struct update_desc_list *next_desc, *temp_desc;
	struct update_marker_data *next_marker, *temp_marker;
	struct mxc_epdc_fb_data *fb_data =
		container_of(work, struct mxc_epdc_fb_data, epdc_submit_work);
	struct update_data_list *upd_data_list = NULL;
	struct mxcfb_rect adj_update_region;
	
	
	bool end_merge = false;
	bool is_aa = false;
	int algorithmVersion ;
	int ret;
#ifdef USE_QUEUESPIN_LOCK //[
	unsigned long flags;
#endif //] USE_QUEUESPIN_LOCK


	GALLEN_DBGLOCAL_BEGIN_EX(64);
	/* Protect access to buffer queues and to update HW */
#ifdef USE_QUEUESPIN_LOCK //[
	spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

	/*
	 * Are any of our collision updates able to go now?
	 * Go through all updates in the collision list and check to see
	 * if the collision mask has been fully cleared
	 */
	list_for_each_entry_safe(next_update, temp_update,
				&fb_data->upd_buf_collision_list, list) {
		GALLEN_DBGLOCAL_RUNLOG(0);

		if (next_update->collision_mask != 0) {
			GALLEN_DBGLOCAL_RUNLOG(1);
			continue;
		}

		dev_dbg(fb_data->dev, "A collision update is ready to go!\n");

		if (fb_data->merge_on_waveform_mismatch) {
			GALLEN_DBGLOCAL_RUNLOG(23);
			/* Force waveform mode to auto for resubmitted collisions */
			next_update->update_desc->upd_data.waveform_mode =
				WAVEFORM_MODE_AUTO;
		}

		/*
		 * We have a collision cleared, so select it for resubmission.
		 * If an update is already selected, attempt to merge.
		 */
		if (!upd_data_list) {
			GALLEN_DBGLOCAL_RUNLOG(2);
			upd_data_list = next_update;
			list_del_init(&next_update->list);
			if (fb_data->upd_scheme == UPDATE_SCHEME_QUEUE) {
				GALLEN_DBGLOCAL_RUNLOG(3);
				/* If not merging, we have our update */
				break;
			}
		} else {
			GALLEN_DBGLOCAL_RUNLOG(4);
			switch (epdc_submit_merge(upd_data_list->update_desc,
						next_update->update_desc,
						fb_data->merge_on_waveform_mismatch)) {
							
			case MERGE_OK:GALLEN_DBGLOCAL_RUNLOG(5);
				dev_dbg(fb_data->dev,
					"Update merged [collision]\n");
				list_del_init(&next_update->update_desc->list);
				kfree(next_update->update_desc);
				next_update->update_desc = NULL;
				list_del_init(&next_update->list);
				/* Add to free buffer list */
				list_add_tail(&next_update->list,
					 &fb_data->upd_buf_free_list);
				break;
			case MERGE_FAIL:GALLEN_DBGLOCAL_RUNLOG(6);
				dev_dbg(fb_data->dev,
					"Update not merged [collision]\n");
				break;
			case MERGE_BLOCK:GALLEN_DBGLOCAL_RUNLOG(7);
				dev_dbg(fb_data->dev,
					"Merge blocked [collision]\n");
				end_merge = true;
				break;
			}

			if (end_merge) {
				GALLEN_DBGLOCAL_RUNLOG(8);
				end_merge = false;
				break;
			}
		}
	}

	/*
	 * Skip pending update list only if we found a collision
	 * update and we are not merging
	 */
	if (!((fb_data->upd_scheme == UPDATE_SCHEME_QUEUE) &&
		upd_data_list)) {
		GALLEN_DBGLOCAL_RUNLOG(9);
		/*
		 * If we didn't find a collision update ready to go, we
		 * need to get a free buffer and match it to a pending update.
		 */

		/*
		 * Can't proceed if there are no free buffers (and we don't
		 * already have a collision update selected)
		*/
		if (!upd_data_list &&
			list_empty(&fb_data->upd_buf_free_list)) {
			GALLEN_DBGLOCAL_RUNLOG(10);
#ifdef USE_QUEUESPIN_LOCK //[
			spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
			mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

			
			GALLEN_DBGLOCAL_ESC();
			return;
		}

		list_for_each_entry_safe(next_desc, temp_desc,
				&fb_data->upd_pending_list, list) {
			GALLEN_DBGLOCAL_RUNLOG(11);		

			dev_dbg(fb_data->dev, "Found a pending update!\n");

			if (!upd_data_list) {
				GALLEN_DBGLOCAL_RUNLOG(12);
				if (list_empty(&fb_data->upd_buf_free_list)) {
					GALLEN_DBGLOCAL_RUNLOG(13);
					break;
				}
				upd_data_list =
					list_entry(fb_data->upd_buf_free_list.next,
						struct update_data_list, list);
				list_del_init(&upd_data_list->list);
				upd_data_list->update_desc = next_desc;
				list_del_init(&next_desc->list);
				if (fb_data->upd_scheme == UPDATE_SCHEME_QUEUE) {
					GALLEN_DBGLOCAL_RUNLOG(14);
					/* If not merging, we have an update */
					break;
				}
			} else {
				GALLEN_DBGLOCAL_RUNLOG(15);
				switch (epdc_submit_merge(upd_data_list->update_desc,
						next_desc, fb_data->merge_on_waveform_mismatch)) {
				case MERGE_OK:GALLEN_DBGLOCAL_RUNLOG(16);
					dev_dbg(fb_data->dev,
						"Update merged [queue]\n");
					list_del_init(&next_desc->list);
					kfree(next_desc);
					break;
				case MERGE_FAIL:GALLEN_DBGLOCAL_RUNLOG(17);
					dev_dbg(fb_data->dev,
						"Update not merged [queue]\n");
					break;
				case MERGE_BLOCK:GALLEN_DBGLOCAL_RUNLOG(18);
					dev_dbg(fb_data->dev,
						"Merge blocked [collision]\n");
					end_merge = true;
					break;
				}

				if (end_merge) {
					GALLEN_DBGLOCAL_RUNLOG(19);
					break;
				}
			}
		}
	}


	/* Is update list empty? */
	if (!upd_data_list) {
#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
		GALLEN_DBGLOCAL_ESC();
		return;
	}

	/* Select from PxP output buffers */
	upd_data_list->phys_addr =
		fb_data->phys_addr_updbuf[fb_data->upd_buffer_num];
	upd_data_list->virt_addr =
		fb_data->virt_addr_updbuf[fb_data->upd_buffer_num];
	fb_data->upd_buffer_num++;
	if (fb_data->upd_buffer_num > fb_data->max_num_buffers-1)
		fb_data->upd_buffer_num = 0;

	/* Release buffer queues */
#ifdef USE_QUEUESPIN_LOCK //[
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK


	/* Perform PXP processing - EPDC power will also be enabled */
	if (epdc_process_update(upd_data_list, fb_data)) {
		dev_dbg(fb_data->dev, "PXP processing error.\n");
		/* Protect access to buffer queues and to update HW */

#ifdef USE_QUEUESPIN_LOCK //[
		spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
		list_del_init(&upd_data_list->update_desc->list);
		kfree(upd_data_list->update_desc);
		upd_data_list->update_desc = NULL;
		/* Add to free buffer list */
		list_add_tail(&upd_data_list->list,
			&fb_data->upd_buf_free_list);
		/* Release buffer queues */
#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

		
		GALLEN_DBGLOCAL_ESC();
		return;
	}

	/* Protect access to buffer queues and to update HW */
#ifdef USE_QUEUESPIN_LOCK //[
	spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
	

	/* Get rotation-adjusted coordinates */
	adjust_coordinates(fb_data,
		&upd_data_list->update_desc->upd_data.update_region,
		&adj_update_region);

	/*
	 * Is the working buffer idle?
	 * If the working buffer is busy, we must wait for the resource
	 * to become free. The IST will signal this event.
	 */
	if (fb_data->cur_update != NULL) {
		GALLEN_DBGLOCAL_RUNLOG(20);
		dev_dbg(fb_data->dev, "working buf busy!\n");

		/* Initialize event signalling an update resource is free */
		init_completion(&fb_data->update_res_free);

		fb_data->waiting_for_wb = true;

		/* Leave spinlock while waiting for WB to complete */
#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

		
		wait_for_completion(&fb_data->update_res_free);
#ifdef USE_QUEUESPIN_LOCK //[
		spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
	}


	if (upd_data_list->update_desc->upd_data.temp != TEMP_USE_AMBIENT) {
		GALLEN_DBGLOCAL_RUNLOG(22);
		DBG_MSG("%s(%d) set temp=%d into EPDC by manual\n",__FILE__,__LINE__,temp_index);
		temp_index = mxc_epdc_fb_get_temp_index(fb_data,
			upd_data_list->update_desc->upd_data.temp);
	} 
	else {
		DBG_MSG("%s(%d) set temp=%d into EPDC by AMBIENT\n",__FILE__,__LINE__,fb_data->temp_index);
		temp_index = fb_data->temp_index;
	}

	/*
	 * check if the waveform has the advance data
	 */
	if (fb_data->waveform_acd_buffer) {

		/*
		 * GALLEN : 似乎在waveform_mode不為regal/regald時algorithmVersion會為0 。
		 *
		 */ 
			
		algorithmVersion = mxc_epdc_fb_prep_algorithm_data( fb_data->waveform_acd_buffer, 
			upd_data_list->update_desc->upd_data.waveform_mode,
			temp_index,
			fb_data->waveform_mc,
			fb_data->waveform_trc,
			fb_data->waveform_magic_number,
			0x3);	
		GALLEN_DBGLOCAL_RUNLOG(24);

		if (0==algorithmVersion) {
			// regular waveform .
		}
		else if (1==algorithmVersion) {
			// REGAL mode .
			GALLEN_DBGLOCAL_RUNLOG(26);
			is_aa = true;
		}
		else if(2==algorithmVersion) {
			// REGALD mode .
			GALLEN_DBGLOCAL_RUNLOG(32);
			is_aa = true;
		}
		else if(-1==algorithmVersion) {
			GALLEN_DBGLOCAL_RUNLOG(33);
			dev_err(fb_data->dev, "invalid waveform mode or invalid temperature range !\n");
		}
		else if(-1>algorithmVersion) {
			GALLEN_DBGLOCAL_RUNLOG(34);
			dev_err(fb_data->dev, "acd buffer data checksum errors !\n");
		}
		else {
			dev_err(fb_data->dev, "unkown error of acd buffer algorithm (%d) !\n",algorithmVersion);
		}
	}
	else {
#if 0 // 20130528 Gallen disabled assigned regal mode from user space .
		if (upd_data_list->update_desc->upd_data.waveform_mode == fb_data->wv_modes.mode_aa ||
				upd_data_list->update_desc->upd_data.waveform_mode == fb_data->wv_modes.mode_aad) {
			GALLEN_DBGLOCAL_RUNLOG(27);
			is_aa = true;
		}
#endif
	}

	if (is_aa) {
		GALLEN_DBGLOCAL_RUNLOG(28);
		/*
		 * reagl_flow
		 * Working buffer is now ensured to not be busy,
		 * so we can begin special waveform processing.
		 */
		/*
               * Invoke AA algorithm processing here:
               * Function should need these parameters:
               *            1) Update region pointer (Y8 format)
               *            2) Update region bounds (top, left, width, height)
               *            3) Update region stride
               *            4) Working Buffer pointer
               *            5) Working Buffer width
	        *            6) Working Buffer height
               *            7) Output Buffer pointer
               */
		/* Warn against AA updates w/o AA waveform */
		if (bufPixFormat != EPDC_FORMAT_BUF_PIXEL_FORMAT_P5N) 
		{
			dev_err(fb_data->dev, "AA waveform update specified "
					"without valid AA waveform file.\n");
		}
		else 
		{
			GALLEN_DBGLOCAL_RUNLOG(29);
			if (upd_data_list->update_desc->upd_data.flags & EPDC_FLAG_USE_AAD) {
				GALLEN_DBGLOCAL_RUNLOG(30);

				flush_cache_all();
				if(do_aad_processing_v2_1_0((uint8_t *)(upd_data_list->virt_addr + upd_data_list->update_desc->epdc_offs),
	                                &adj_update_region,
	                                ALIGN(adj_update_region.width, 8),
	                                (uint16_t *)fb_data->working_buffer_virt,
	                                fb_data->native_width,
	                                fb_data->native_height))
					dev_info(fb_data->dev,"AAD processing is not supported!\n");
				flush_cache_all();

			}
			else { 
				GALLEN_DBGLOCAL_RUNLOG(31);

				flush_cache_all();
				if(do_aa_processing_v2_2_0((uint8_t *)(upd_data_list->virt_addr + upd_data_list->update_desc->epdc_offs),
	                                &adj_update_region,
	                                ALIGN(adj_update_region.width, 8),
	                                (uint16_t *)fb_data->working_buffer_virt,
	                                fb_data->native_width,
	                                fb_data->native_height))
					dev_info(fb_data->dev,"AA processing is not supported!\n");
				flush_cache_all();

			}
		}
	}

	/*
	 * If there are no LUTs available,
	 * then we must wait for the resource to become free.
	 * The IST will signal this event.
	 */
	if (!epdc_any_luts_available()) {
		GALLEN_DBGLOCAL_RUNLOG(21);
		dev_dbg(fb_data->dev, "no luts available!\n");

		/* Initialize event signalling an update resource is free */
		init_completion(&fb_data->update_res_free);

		fb_data->waiting_for_lut = true;

		/* Leave spinlock while waiting for LUT to free up */
#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

		
		wait_for_completion(&fb_data->update_res_free);
#ifdef USE_QUEUESPIN_LOCK //[
		spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
		
	}

	ret = epdc_choose_next_lut(&upd_data_list->lut_num);
	/*
	 * If LUT15 is in use:
	 *   - Wait for LUT15 to complete is if TCE underrun prevent is enabled
	 *   - If we go ahead with update, sync update submission with EOF
	 */
	if (ret && fb_data->tce_prevent) {
		dev_dbg(fb_data->dev, "Waiting for LUT15\n");

		/* Initialize event signalling that lut15 is free */
		init_completion(&fb_data->lut15_free);

		fb_data->waiting_for_lut15 = true;

		/* Leave spinlock while waiting for LUT to free up */
#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

		
		wait_for_completion(&fb_data->lut15_free);

#ifdef USE_QUEUESPIN_LOCK //[
		spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
		

		epdc_choose_next_lut(&upd_data_list->lut_num);
	} else if (ret) {
		/* Synchronize update submission time to reduce
		   chances of TCE underrun */
		init_completion(&fb_data->eof_event);

		epdc_eof_intr(true);

		/* Leave spinlock while waiting for EOF event */
#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

		
		ret = wait_for_completion_timeout(&fb_data->eof_event,
			msecs_to_jiffies(1000));
		if (!ret) {
			dev_err(fb_data->dev, "Missed EOF event!\n");
			epdc_eof_intr(false);
		}
		udelay(fb_data->eof_sync_period);
#ifdef USE_QUEUESPIN_LOCK //[
		spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
		

	}

	/* LUTs are available, so we get one here */
	fb_data->cur_update = upd_data_list;

	/* Reset mask for LUTS that have completed during WB processing */
	fb_data->luts_complete_wb = 0;
	ASSERT(upd_data_list->update_desc);
	ASSERT(fb_data->cur_update);

	/* Associate LUT with update marker */
	list_for_each_entry_safe(next_marker, temp_marker,
		&upd_data_list->update_desc->upd_marker_list, upd_list)
		next_marker->lut_num = fb_data->cur_update->lut_num;

	/* Mark LUT with order */
	fb_data->lut_update_order[upd_data_list->lut_num] =
		upd_data_list->update_desc->update_order;

	/* Enable Collision and WB complete IRQs */
	epdc_working_buf_intr(true);
	epdc_lut_complete_intr(upd_data_list->lut_num, true);

	/* Program EPDC update to process buffer */
	epdc_set_temp(temp_index);
	
	epdc_set_update_addr(upd_data_list->phys_addr
				+ upd_data_list->update_desc->epdc_offs);
	epdc_set_update_coord(adj_update_region.left, adj_update_region.top);
	epdc_set_update_dimensions(adj_update_region.width,
				   adj_update_region.height);
	epdc_submit_update(upd_data_list->lut_num,
			   upd_data_list->update_desc->upd_data.waveform_mode,
			   upd_data_list->update_desc->upd_data.update_mode,
			   false, 0);

	/* Release buffer queues */
#ifdef USE_QUEUESPIN_LOCK //[
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

	
	
	GALLEN_DBGLOCAL_END();
}


int mxc_epdc_fb_send_update(struct mxcfb_update_data *upd_data,
				   struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;
	struct update_data_list *upd_data_list = NULL;
	struct mxcfb_rect *screen_upd_region; /* Region on screen to update */
	int temp_index;
	int ret;
	struct update_desc_list *upd_desc;
	struct update_marker_data *marker_data, *next_marker, *temp_marker;

#ifdef USE_QUEUESPIN_LOCK //[
	unsigned long flags;
#endif //] USE_QUEUESPIN_LOCK


	GALLEN_DBGLOCAL_MUTEBEGIN();


#ifdef OUTPUT_SNAPSHOT_IMGFILE //[
	fb_capture(gptDC,8,EPDFB_R_0,gcFB_snapshot_pathA);
#endif //]OUTPUT_SNAPSHOT_IMGFILE


#if 0 //test REGALD
	if( (0x19==gbModeVersion && 0==gbWFM_REV) || (0x20==gbModeVersion) )  
	{
		if(4==upd_data->waveform_mode) {
			// A2 mode .
			printk("[skip force REGAL test] @ A2 mode");
		}
		if(1==upd_data->waveform_mode) {
			// DU mode .
			printk("[skip force REGAL test] @ DU mode");
		}
		else {
			upd_data->waveform_mode=fb_data->wv_modes.mode_aad;
			upd_data->update_mode=0;
			upd_data->flags |= EPDC_FLAG_USE_AAD;
			printk("[force REGALD mode test]");
		}
	}
#endif

	DBG_MSG("%s:upd rect={%u,%u,%u,%u},\n\twfmode=%u,updmode=%u,updmarker=%u,flags=0x%x,temp=%d\n",__FUNCTION__,
			upd_data->update_region.left,upd_data->update_region.top,
			upd_data->update_region.width,upd_data->update_region.height,
			upd_data->waveform_mode,upd_data->update_mode,upd_data->update_marker,
			upd_data->flags,upd_data->temp);

	if(TEMP_USE_AMBIENT!=upd_data->temp) {
		WARNING_MSG(KERN_ERR"use custom temperature %d\n",upd_data->temp);
	}

	
	/* Has EPDC HW been initialized? */
	if (!fb_data->hw_ready) {
		dev_err(fb_data->dev, "Display HW not properly initialized."
			"  Aborting update.\n");
		GALLEN_DBGLOCAL_ESC();
		return -EPERM;
	}

	/* Check validity of update params */
	if ((upd_data->update_mode != UPDATE_MODE_PARTIAL) &&
		(upd_data->update_mode != UPDATE_MODE_FULL)) {
		dev_err(fb_data->dev,
			"Update mode 0x%x is invalid.  Aborting update.\n",
			upd_data->update_mode);
		GALLEN_DBGLOCAL_ESC();
		return -EINVAL;
	}
#if (NTX_WFM_MODE_TOTAL>0) //[
	if ( (upd_data->waveform_mode < 0) || 
			((upd_data->waveform_mode >= NTX_WFM_MODE_TOTAL) &&
		(upd_data->waveform_mode != WAVEFORM_MODE_AUTO)) ) 
#else
	if ((upd_data->waveform_mode > 255) &&
		(upd_data->waveform_mode != WAVEFORM_MODE_AUTO)) 
#endif
	{
		dev_err(fb_data->dev,
			"Update waveform mode %d is invalid."
			"  Aborting update.\n",
			upd_data->waveform_mode);
		GALLEN_DBGLOCAL_ESC();
		return -EINVAL;
	}

	if ((upd_data->update_region.left + upd_data->update_region.width > fb_data->epdc_fb_var.xres) ||
		(upd_data->update_region.top + upd_data->update_region.height > fb_data->epdc_fb_var.yres)) {
		dev_err(fb_data->dev,
			"Update region is outside bounds of framebuffer.(left,top=(%d,%d),w=%d,h=%d,panelw=%d,panelh=%d)"
			"Aborting update.\n",upd_data->update_region.left,upd_data->update_region.top,\
			upd_data->update_region.width,upd_data->update_region.height,\
			fb_data->epdc_fb_var.xres,fb_data->epdc_fb_var.yres);
		GALLEN_DBGLOCAL_ESC();
		return -EINVAL;
	}
	

	k_set_temperature(info);
	
	
	if (upd_data->flags & EPDC_FLAG_USE_ALT_BUFFER) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		if ((upd_data->update_region.width !=
			upd_data->alt_buffer_data.alt_update_region.width) ||
			(upd_data->update_region.height !=
			upd_data->alt_buffer_data.alt_update_region.height)) {
			dev_err(fb_data->dev,
				"Alternate update region dimensions must "
				"match screen update region dimensions.\n");
			GALLEN_DBGLOCAL_ESC();
			return -EINVAL;
		}
		/* Validate physical address parameter */
		if ((upd_data->alt_buffer_data.phys_addr <
			fb_data->info.fix.smem_start) ||
			(upd_data->alt_buffer_data.phys_addr >
			fb_data->info.fix.smem_start + fb_data->map_size)) {
			dev_err(fb_data->dev,
				"Invalid physical address for alternate "
				"buffer.  Aborting update...\n");
			GALLEN_DBGLOCAL_ESC();
			return -EINVAL;
		}
	}

#ifdef USE_QUEUESPIN_LOCK //[
	spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

	/*
	 * If we are waiting to go into suspend, or the FB is blanked,
	 * we do not accept new updates
	 */
	if ((fb_data->waiting_for_idle) ||
		((fb_data->blank != FB_BLANK_UNBLANK) && (fb_data->blank != FB_BLANK_NORMAL))) {
		dev_err(fb_data->dev, "EPDC not active."
			"Update request abort.\n");
#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

		
		GALLEN_DBGLOCAL_ESC();
		return -EPERM;
	}

	if (fb_data->upd_scheme == UPDATE_SCHEME_SNAPSHOT) {
		int count = 0;
		struct update_data_list *plist;
		GALLEN_DBGLOCAL_RUNLOG(1);
		/*
		 * Get available intermediate (PxP output) buffer to hold
		 * processed update region
		 */
		if (upd_data->update_mode == UPDATE_MODE_FULL) {
#ifdef USE_QUEUESPIN_LOCK //[
			spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
			mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

			
			mxc_epdc_fb_flush_updates(fb_data);
#ifdef USE_QUEUESPIN_LOCK //[
			spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
			mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
			
		}

		/* Count buffers in free buffer list */
		list_for_each_entry(plist, &fb_data->upd_buf_free_list, list)
			count++;

		/* Use count to determine if we have enough
		 * free buffers to handle this update request */
		if (count + fb_data->max_num_buffers
			<= EPDC_MAX_NUM_UPDATES) {
			dev_err(fb_data->dev,
				"No free intermediate buffers available.\n");
#ifdef USE_QUEUESPIN_LOCK //[
			spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
			mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

			
			GALLEN_DBGLOCAL_ESC();
			return -ENOMEM;
		}

		/* Grab first available buffer and delete from the free list */
		upd_data_list =
		    list_entry(fb_data->upd_buf_free_list.next,
			       struct update_data_list, list);

		list_del_init(&upd_data_list->list);
	}

	/*
	 * Create new update data structure, fill it with new update
	 * data and add it to the list of pending updates
	 */
	upd_desc = kzalloc(sizeof(struct update_desc_list), GFP_KERNEL);
	if (!upd_desc) {
		GALLEN_DBGLOCAL_RUNLOG(2);
		dev_err(fb_data->dev,
			"Insufficient system memory for update! Aborting.\n");
		if (fb_data->upd_scheme == UPDATE_SCHEME_SNAPSHOT) {
			GALLEN_DBGLOCAL_RUNLOG(3);
			list_add(&upd_data_list->list,
				&fb_data->upd_buf_free_list);
		}
#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

		
		GALLEN_DBGLOCAL_ESC();
		return -EPERM;
	}
	
	
		
	/* Initialize per-update marker list */
	INIT_LIST_HEAD(&upd_desc->upd_marker_list);
	upd_desc->upd_data = *upd_data;
	upd_desc->update_order = fb_data->order_cnt++;
	list_add_tail(&upd_desc->list, &fb_data->upd_pending_list);

	// LV panel & LV waveform test
	{ 


		if(WAVEFORM_MODE_AUTO!=upd_desc->upd_data.waveform_mode) {

#ifdef NTX_WFM_MODE_OPTIMIZED //[
			//if(0==gptHWCFG->m_val.bUIStyle) 
			{

				if(NTX_WFM_MODE_GC16==upd_desc->upd_data.waveform_mode) {
					if(upd_desc->upd_data.update_mode == UPDATE_MODE_FULL) {
						#ifdef NTX_WFM_MODE_OPTIMIZED_REAGL//[
						if(giNTX_waveform_modeA[NTX_WFM_MODE_GLD16]!=giNTX_waveform_modeA[NTX_WFM_MODE_GC16]) {
							// has GLD16 mode .
							DBG_MSG("WF Mode version=0x%02x,chg W.F Mode GC16(%d)->GLD16(%d) @ full\n",
								gbModeVersion,NTX_WFM_MODE_GC16,NTX_WFM_MODE_GLD16);
							upd_desc->upd_data.waveform_mode = NTX_WFM_MODE_GLD16;
						}
						#endif //]NTX_WFM_MODE_OPTIMIZED_REAGL
					}
					else if(upd_desc->upd_data.update_mode == UPDATE_MODE_PARTIAL){
						#ifdef NTX_WFM_MODE_OPTIMIZED_REAGL//[
						if(giNTX_waveform_modeA[NTX_WFM_MODE_GLR16]!=giNTX_waveform_modeA[NTX_WFM_MODE_GC16]) {
							DBG_MSG("WF Mode version=0x%02x,chg W.F Mode GC16(%d)->GLR16(%d) @ partial\n",
								gbModeVersion,NTX_WFM_MODE_GC16,NTX_WFM_MODE_GLR16);
							upd_desc->upd_data.waveform_mode = NTX_WFM_MODE_GLR16;
						}
						else
						#endif //]NTX_WFM_MODE_OPTIMIZED_REAGL
						if (giNTX_waveform_modeA[NTX_WFM_MODE_GL16]!=giNTX_waveform_modeA[NTX_WFM_MODE_GC16]) {
							DBG_MSG("chg W.F Mode GC16(%d)->GL16(%d) @ partial\n",
								NTX_WFM_MODE_GC16,NTX_WFM_MODE_GL16);
							upd_desc->upd_data.waveform_mode = NTX_WFM_MODE_GL16;
						}
					}	
				}

			}
#endif //] NTX_WFM_MODE_OPTIMIZED

			if(NTX_WFM_MODE_GLD16==upd_desc->upd_data.waveform_mode) {
				upd_desc->upd_data.flags |= EPDC_FLAG_USE_AAD;
			}
			else if(NTX_WFM_MODE_GLR16==upd_desc->upd_data.waveform_mode) {
				upd_desc->upd_data.flags &= ~EPDC_FLAG_USE_AAD;
			}

			DBG_MSG("ntx wfm mode %d->eink wfm mode %d @ mode 0x%02x\n",
					upd_desc->upd_data.waveform_mode,giNTX_waveform_modeA[upd_desc->upd_data.waveform_mode],
					gbModeVersion);
			upd_desc->upd_data.waveform_mode = giNTX_waveform_modeA[upd_desc->upd_data.waveform_mode];
		}

		
	}
	
	/* If marker specified, associate it with a completion */
	if (upd_data->update_marker != 0) {
		GALLEN_DBGLOCAL_RUNLOG(4);
		/* Allocate new update marker and set it up */
		marker_data = kzalloc(sizeof(struct update_marker_data),
				GFP_KERNEL);
		if (!marker_data) {
			dev_err(fb_data->dev, "No memory for marker!\n");
#ifdef USE_QUEUESPIN_LOCK //[
			spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
			mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

			
			GALLEN_DBGLOCAL_ESC();
			return -ENOMEM;
		}
		DBG_MSG("[%s] alloc update_marker_data size=%d\n",
			__FUNCTION__,sizeof(struct update_marker_data));
			
		list_add_tail(&marker_data->upd_list,
			&upd_desc->upd_marker_list);
		marker_data->update_marker = upd_data->update_marker;
		marker_data->lut_num = INVALID_LUT;
		init_completion(&marker_data->update_completion);
		/* Add marker to master marker list */
		list_add_tail(&marker_data->full_list,
			&fb_data->full_marker_list);
	}

	if (fb_data->upd_scheme != UPDATE_SCHEME_SNAPSHOT) {
		/* Queued update scheme processing */

#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

		

		/* Signal workqueue to handle new update */
		queue_work(fb_data->epdc_submit_workqueue,
			&fb_data->epdc_submit_work);


		GALLEN_DBGLOCAL_ESC();
		return 0;
	}

	/* Snapshot update scheme processing */

	/* Set descriptor for current update, delete from pending list */
	upd_data_list->update_desc = upd_desc;
	list_del_init(&upd_desc->list);

#ifdef USE_QUEUESPIN_LOCK //[
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

	

	/*
	 * Hold on to original screen update region, which we
	 * will ultimately use when telling EPDC where to update on panel
	 */
	screen_upd_region = &upd_desc->upd_data.update_region;

	/* Select from PxP output buffers */
	upd_data_list->phys_addr =
		fb_data->phys_addr_updbuf[fb_data->upd_buffer_num];
	upd_data_list->virt_addr =
		fb_data->virt_addr_updbuf[fb_data->upd_buffer_num];
	fb_data->upd_buffer_num++;
	if (fb_data->upd_buffer_num > fb_data->max_num_buffers-1)
		fb_data->upd_buffer_num = 0;

	ret = epdc_process_update(upd_data_list, fb_data);
	if (ret) {
		mutex_unlock(&fb_data->pxp_mutex);
		GALLEN_DBGLOCAL_ESC();
		return ret;
	}

	/* Pass selected waveform mode back to user */
	upd_data->waveform_mode = upd_desc->upd_data.waveform_mode;


	/* Get rotation-adjusted coordinates */
	adjust_coordinates(fb_data, &upd_desc->upd_data.update_region,
		NULL);

	/* Grab lock for queue manipulation and update submission */
#ifdef USE_QUEUESPIN_LOCK //[
	spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

	/*
	 * Is the working buffer idle?
	 * If either the working buffer is busy, or there are no LUTs available,
	 * then we return and let the ISR handle the update later
	 */
	if ((fb_data->cur_update != NULL) || !epdc_any_luts_available()) {
		/* Add processed Y buffer to update list */
		list_add_tail(&upd_data_list->list, &fb_data->upd_buf_queue);

		/* Return and allow the update to be submitted by the ISR. */
#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

		
		GALLEN_DBGLOCAL_ESC();
		return 0;
	}

	/* LUTs are available, so we get one here */
	ret = epdc_choose_next_lut(&upd_data_list->lut_num);
	if (ret && fb_data->tce_prevent) {
		dev_dbg(fb_data->dev, "Must wait for LUT15\n");
		/* Add processed Y buffer to update list */
		list_add_tail(&upd_data_list->list, &fb_data->upd_buf_queue);

		/* Return and allow the update to be submitted by the ISR. */
#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

		
		GALLEN_DBGLOCAL_ESC();
		return 0;
	}

	/* Save current update */
	fb_data->cur_update = upd_data_list;

	/* Reset mask for LUTS that have completed during WB processing */
	fb_data->luts_complete_wb = 0;

	/* Associate LUT with update marker */
	list_for_each_entry_safe(next_marker, temp_marker,
		&upd_data_list->update_desc->upd_marker_list, upd_list)
		next_marker->lut_num = upd_data_list->lut_num;

	/* Mark LUT as containing new update */
	fb_data->lut_update_order[upd_data_list->lut_num] =
		upd_desc->update_order;

	/* Clear status and Enable LUT complete and WB complete IRQs */
	epdc_working_buf_intr(true);
	epdc_lut_complete_intr(upd_data_list->lut_num, true);

	/* Program EPDC update to process buffer */
	epdc_set_update_addr(upd_data_list->phys_addr + upd_desc->epdc_offs);
	epdc_set_update_coord(screen_upd_region->left, screen_upd_region->top);
	epdc_set_update_dimensions(screen_upd_region->width,
		screen_upd_region->height);
	if (upd_desc->upd_data.temp != TEMP_USE_AMBIENT) {
		GALLEN_DBGLOCAL_RUNLOG(6);
		temp_index = mxc_epdc_fb_get_temp_index(fb_data,
			upd_desc->upd_data.temp);
		DBG_MSG("%s(%d) set temp=%d into EPDC by manual\n",__FILE__,__LINE__,temp_index);
		epdc_set_temp(temp_index);
	} else {
		GALLEN_DBGLOCAL_RUNLOG(7);
		DBG_MSG("%s(%d) set temp=%d into EPDC by AMBIENT\n",__FILE__,__LINE__,fb_data->temp_index);
		epdc_set_temp(fb_data->temp_index);
	}

	epdc_submit_update(upd_data_list->lut_num,
			   upd_desc->upd_data.waveform_mode,
			   upd_desc->upd_data.update_mode, false, 0);

#ifdef USE_QUEUESPIN_LOCK //[
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

	
	GALLEN_DBGLOCAL_END();
	return 0;
}
EXPORT_SYMBOL(mxc_epdc_fb_send_update);

int mxc_epdc_fb_wait_update_complete(u32 update_marker, struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;
	struct update_marker_data *next_marker;
	struct update_marker_data *temp;
	bool marker_found = false;
	int ret = 0;
#ifdef USE_QUEUESPIN_LOCK //[
	unsigned long flags;
#endif //] USE_QUEUESPIN_LOCK

	
	GALLEN_DBGLOCAL_BEGIN();

	/* 0 is an invalid update_marker value */
	if (update_marker == 0) {
		GALLEN_DBGLOCAL_ESC();
		return -EINVAL;
	}

	/*
	 * Find completion associated with update_marker requested.
	 * Note: If update completed already, marker will have been
	 * cleared, it won't be found, and function will just return.
	 */

	/* Grab queue lock to protect access to marker list */
#ifdef USE_QUEUESPIN_LOCK //[
	spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

	list_for_each_entry_safe(next_marker, temp,
		&fb_data->full_marker_list, full_list) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		if (next_marker->update_marker == update_marker) {
			GALLEN_DBGLOCAL_RUNLOG(1);
			dev_dbg(fb_data->dev, "Waiting for marker %d\n",
				update_marker);
			next_marker->waiting = true;
			marker_found = true;
			break;
		}
	}

#ifdef USE_QUEUESPIN_LOCK //[
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

	

	/*
	 * If marker not found, it has either been signalled already
	 * or the update request failed.  In either case, just return.
	 */
	if (!marker_found) {
		GALLEN_DBGLOCAL_ESC();
		return ret;
	}

	DBG_MSG("%s(%d) update marker %d @ %p waiting for completion !\n",\
		__FUNCTION__,__LINE__,next_marker->update_marker,next_marker);
	ret = wait_for_completion_timeout(&next_marker->update_completion,
						msecs_to_jiffies(10000));
	if (!ret) {
		GALLEN_DBGLOCAL_RUNLOG(2);
		dev_err(fb_data->dev,
			"Timed out waiting for update completion\n");
		list_del_init(&next_marker->full_list);
		ret = -ETIMEDOUT;
	}
	else {
		DBG_MSG("%s(%d) update marker %d @ %p waiting for completion ! OK\n",\
			__FUNCTION__,__LINE__,next_marker->update_marker,next_marker);
	}

	/* Free update marker object */
	kfree(next_marker);

	GALLEN_DBGLOCAL_END();
	return ret;
}
EXPORT_SYMBOL(mxc_epdc_fb_wait_update_complete);

int mxc_epdc_fb_set_pwrdown_delay(u32 pwrdown_delay,
					    struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;

	fb_data->pwrdown_delay = pwrdown_delay;

	return 0;
}
EXPORT_SYMBOL(mxc_epdc_fb_set_pwrdown_delay);

int mxc_epdc_get_pwrdown_delay(struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;

	return fb_data->pwrdown_delay;
}
EXPORT_SYMBOL(mxc_epdc_get_pwrdown_delay);

static int mxc_epdc_fb_open (struct fb_info *info, int user)
{
	GALLEN_DBGLOCAL_BEGIN();
#ifdef CONFIG_ANDROID//[
	fake_s1d13522_progress_stop();
	k_fake_s1d13522_wait_inited();
#endif //] CONFIG_ANDROID 

	//k_create_sys_attr();

	GALLEN_DBGLOCAL_END();
	return 0;
}

static int mxc_epdc_fb_ioctl(struct fb_info *info, unsigned int cmd,
			     unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int ret = -EINVAL;
	GALLEN_DBGLOCAL_BEGIN();
#ifdef CONFIG_ANDROID //[
#else //][!CONFIG_ANDROID
	fake_s1d13522_progress_stop();
	k_fake_s1d13522_wait_inited();
#endif //]CONFIG_ANDROID

	switch (cmd) {
	case MXCFB_SET_WAVEFORM_MODES: GALLEN_DBGLOCAL_RUNLOG(0);
		{
			struct mxcfb_waveform_modes modes;
			if (!copy_from_user(&modes, argp, sizeof(modes))) {
				GALLEN_DBGLOCAL_RUNLOG(1);
				mxc_epdc_fb_set_waveform_modes(&modes, info);
				ret = 0;
			}
			break;
		}
	case MXCFB_SET_TEMPERATURE: GALLEN_DBGLOCAL_RUNLOG(2);
		{
			int temperature;
			if (!get_user(temperature, (int32_t __user *) arg)) {
				GALLEN_DBGLOCAL_RUNLOG(3);
				ret = mxc_epdc_fb_set_temperature(temperature,
					info);
			}
			break;
		}
	case MXCFB_SET_AUTO_UPDATE_MODE:GALLEN_DBGLOCAL_RUNLOG(4);
		{
			u32 auto_mode = 0;
			if (!get_user(auto_mode, (__u32 __user *) arg)) {
				GALLEN_DBGLOCAL_RUNLOG(5);
				ret = mxc_epdc_fb_set_auto_update(auto_mode,
					info);
			}
			break;
		}
	case MXCFB_SET_UPDATE_SCHEME:GALLEN_DBGLOCAL_RUNLOG(6);
		{
			u32 upd_scheme = 0;
			if (!get_user(upd_scheme, (__u32 __user *) arg)) {
				GALLEN_DBGLOCAL_RUNLOG(7);
				ret = mxc_epdc_fb_set_upd_scheme(upd_scheme,
					info);
			}
			break;
		}
	case MXCFB_SEND_UPDATE:GALLEN_DBGLOCAL_RUNLOG(8);
		{
			struct mxcfb_update_data upd_data;
			//printk("MXCFB_SEND_UPDATE:0x%x\n",MXCFB_SEND_UPDATE);
			if (!copy_from_user(&upd_data, argp,
				sizeof(upd_data))) {
				GALLEN_DBGLOCAL_RUNLOG(9);	
				ret = mxc_epdc_fb_send_update(&upd_data, info);
				if (ret == 0 && copy_to_user(argp, &upd_data,
					sizeof(upd_data))) {
					GALLEN_DBGLOCAL_RUNLOG(10);
					ret = -EFAULT;
				}
			} else {
				GALLEN_DBGLOCAL_RUNLOG(11);
				ret = -EFAULT;
			}

			break;
		}
#ifndef CONFIG_ANDROID  //[
	case MXCFB_SEND_UPDATE_ORG:GALLEN_DBGLOCAL_RUNLOG(20);
		{
			struct mxcfb_update_data upd_data;
			struct mxcfb_update_data_org upd_data_org;
			//printk("MXCFB_SEND_UPDATE_ORG:0x%x\n",MXCFB_SEND_UPDATE_ORG);
			if (!copy_from_user(&upd_data_org, argp,
				sizeof(upd_data))) {
				GALLEN_DBGLOCAL_RUNLOG(21);	

				memcpy(&upd_data.update_region,&upd_data_org.update_region,sizeof(struct mxcfb_rect));
				upd_data.waveform_mode = upd_data_org.waveform_mode;
				upd_data.update_mode = upd_data_org.update_mode;
				upd_data.update_marker = upd_data_org.update_marker;
				upd_data.temp = upd_data_org.temp;
				upd_data.flags = upd_data_org.flags;
				upd_data.alt_buffer_data.phys_addr = upd_data_org.alt_buffer_data.phys_addr;
				upd_data.alt_buffer_data.width = upd_data_org.alt_buffer_data.width;
				upd_data.alt_buffer_data.height = upd_data_org.alt_buffer_data.height;
				memcpy(&upd_data.alt_buffer_data.alt_update_region,&upd_data_org.alt_buffer_data.alt_update_region,sizeof(struct mxcfb_rect));

				ret = mxc_epdc_fb_send_update(&upd_data, info);

				memcpy(&upd_data_org.update_region,&upd_data.update_region,sizeof(struct mxcfb_rect));
				upd_data_org.waveform_mode = upd_data.waveform_mode;
				upd_data_org.update_mode = upd_data.update_mode;
				upd_data_org.update_marker = upd_data.update_marker;
				upd_data_org.temp = upd_data.temp;
				upd_data_org.flags = upd_data.flags;
				upd_data_org.alt_buffer_data.phys_addr = upd_data.alt_buffer_data.phys_addr;
				upd_data_org.alt_buffer_data.width = upd_data.alt_buffer_data.width;
				upd_data_org.alt_buffer_data.height = upd_data.alt_buffer_data.height;
				memcpy(&upd_data_org.alt_buffer_data.alt_update_region,&upd_data.alt_buffer_data.alt_update_region,sizeof(struct mxcfb_rect));

				
				if (ret == 0 && copy_to_user(argp, &upd_data_org,
					sizeof(upd_data))) {
					GALLEN_DBGLOCAL_RUNLOG(22);
					ret = -EFAULT;
				}
			} else {
				GALLEN_DBGLOCAL_RUNLOG(23);
				ret = -EFAULT;
			}

			break;
		}
#endif //] !CONFIG_ANDROID
	case MXCFB_WAIT_FOR_UPDATE_COMPLETE:GALLEN_DBGLOCAL_RUNLOG(12);
		{
			u32 update_marker = 0;
			if (!get_user(update_marker, (__u32 __user *) arg)) {
				GALLEN_DBGLOCAL_RUNLOG(13);
				ret =
				    mxc_epdc_fb_wait_update_complete(update_marker,
					info);
			}
			break;
		}

	case MXCFB_SET_PWRDOWN_DELAY:GALLEN_DBGLOCAL_RUNLOG(14);
		{
			int delay = 0;
			if (!get_user(delay, (__u32 __user *) arg))
			{
				GALLEN_DBGLOCAL_RUNLOG(15);
				ret =
				    mxc_epdc_fb_set_pwrdown_delay(delay, info);
			}
			break;
		}

	case MXCFB_GET_PWRDOWN_DELAY:GALLEN_DBGLOCAL_RUNLOG(16);
		{
			int pwrdown_delay = mxc_epdc_get_pwrdown_delay(info);
			if (put_user(pwrdown_delay,
				(int __user *)argp))
			{
				GALLEN_DBGLOCAL_RUNLOG(17);
					
				ret = -EFAULT;
			}
			ret = 0;
			break;
		}

	case MXCFB_SET_MERGE_ON_WAVEFORM_MISMATCH:
		{
			int merge = 1;
			if (!get_user(merge, (int __user *) arg))
				ret = mxc_epdc_fb_set_merge_on_waveform_mismatch(merge, info);
			break;
		}

	default:GALLEN_DBGLOCAL_RUNLOG(18);
		{
			ret = k_fake_s1d13522_ioctl(cmd,arg);
		}
		break;
	}
	GALLEN_DBGLOCAL_END();
	return ret;
}

static void mxc_epdc_fb_update_pages(struct mxc_epdc_fb_data *fb_data,
				     u16 y1, u16 y2)
{
	struct mxcfb_update_data update;
	GALLEN_DBGLOCAL_BEGIN();

	/* Do partial screen update, Update full horizontal lines */
	update.update_region.left = 0;
	update.update_region.width = fb_data->epdc_fb_var.xres;
	update.update_region.top = y1;
	update.update_region.height = y2 - y1;
	update.waveform_mode = WAVEFORM_MODE_AUTO;
	update.update_mode = UPDATE_MODE_FULL;
	update.update_marker = 0;
	update.temp = TEMP_USE_AMBIENT;
	update.flags = 0;

	mxc_epdc_fb_send_update(&update, &fb_data->info);
	
	GALLEN_DBGLOCAL_END();
}

/* this is called back from the deferred io workqueue */
static void mxc_epdc_fb_deferred_io(struct fb_info *info,
				    struct list_head *pagelist)
{
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;
	struct page *page;
	unsigned long beg, end;
	int y1, y2, miny, maxy;


	GALLEN_DBGLOCAL_BEGIN();

	if (fb_data->auto_mode != AUTO_UPDATE_MODE_AUTOMATIC_MODE) {
		GALLEN_DBGLOCAL_ESC();
		return;
	}

	miny = INT_MAX;
	maxy = 0;
	list_for_each_entry(page, pagelist, lru) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		beg = page->index << PAGE_SHIFT;
		end = beg + PAGE_SIZE - 1;
		y1 = beg / info->fix.line_length;
		y2 = end / info->fix.line_length;
		if (y2 >= fb_data->epdc_fb_var.yres) {
			y2 = fb_data->epdc_fb_var.yres - 1;
			GALLEN_DBGLOCAL_RUNLOG(1);
		}
		if (miny > y1) {
			miny = y1;
			GALLEN_DBGLOCAL_RUNLOG(2);
		}
		if (maxy < y2) {
			maxy = y2;
			GALLEN_DBGLOCAL_RUNLOG(3);
		}
	}

	mxc_epdc_fb_update_pages(fb_data, miny, maxy);
	GALLEN_DBGLOCAL_END();
}

void mxc_epdc_fb_flush_updates(struct mxc_epdc_fb_data *fb_data)
{
	int ret;
#ifdef USE_QUEUESPIN_LOCK //[
	unsigned long flags;
#endif //] USE_QUEUESPIN_LOCK

#if 0
	int iIsPendingListEmpty;
	int iIsFreeListFull;
	int iPowerState;
	int iPoweringDown;
	int updates_active;
#endif
	
	GALLEN_DBGLOCAL_BEGIN();
	/* Grab queue lock to prevent any new updates from being submitted */
#ifdef USE_QUEUESPIN_LOCK //[
	spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
	

		
	/*
	 * 3 places to check for updates that are active or pending:
	 *   1) Updates in the pending list
	 *   2) Update buffers in use (e.g., PxP processing)
	 *   3) Active updates to panel - We can key off of EPDC
	 *      power state to know if we have active updates.
	 */
	#if 1 
	/* gallen modify 20110704 : if there is no update list in queue 
		,this condition will cause update timeout 5 secs .
	*/
	if ((fb_data->power_state == POWER_STATE_ON) &&
			(!list_empty(&fb_data->upd_pending_list) ||
		!is_free_list_full(fb_data))) 
	#else
	if (!list_empty(&fb_data->upd_pending_list) ||
		!is_free_list_full(fb_data) ||
		(fb_data->updates_active == true))
	#endif
	{
		GALLEN_DBGLOCAL_RUNLOG(0);
		
		/* Initialize event signalling updates are done */
		init_completion(&fb_data->updates_done);
		fb_data->waiting_for_idle = true;

#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
		
		/* Wait for any currently active updates to complete */
		//DBG_MSG(KERN_ERR "%s(%d) waiting for update done !\n",__FUNCTION__,__LINE__);
		schedule_timeout(1);
		ret = wait_for_completion_timeout(&fb_data->updates_done,
						msecs_to_jiffies(5000));
		schedule_timeout(1);
		if (!ret) 
		{
			GALLEN_DBGLOCAL_RUNLOG(1);
			dev_err(fb_data->dev,
				"Flush updates timeout! ret = 0x%x,is_free_list_full()=%d,list_empty=%d,power_state=%d,powering_down=%d,update_active=%d\n", ret,
				is_free_list_full(fb_data),
				list_empty(&fb_data->upd_pending_list),
				fb_data->power_state,
				fb_data->powering_down,
				fb_data->updates_active);
			dump_all_updates(fb_data);
		}
		else {
			//DBG_MSG(KERN_ERR "%s(%d) waiting for update done ! OK \n",__FUNCTION__,__LINE__);
		}

#ifdef USE_QUEUESPIN_LOCK //[
		spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
		
		fb_data->waiting_for_idle = false;
	}

#ifdef USE_QUEUESPIN_LOCK //[
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

	GALLEN_DBGLOCAL_END();
}

static int mxc_epdc_fb_blank(int blank, struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;
	int ret;

	GALLEN_DBGLOCAL_BEGIN();
	dev_dbg(fb_data->dev, "blank = %d\n", blank);

	if (fb_data->blank == blank)
	{
		GALLEN_DBGLOCAL_ESC();
		return 0;
	}

	//fb_data->blank = blank;

	switch (blank) {
	case FB_BLANK_POWERDOWN:GALLEN_DBGLOCAL_RUNLOG(0);
		mxc_epdc_fb_flush_updates(fb_data);
		/* Wait for powerdown */
		mutex_lock(&fb_data->power_mutex);
#if 1 //[

		if (fb_data->power_state == POWER_STATE_ON) {
#else //][!

		if ((fb_data->power_state == POWER_STATE_ON) &&
			(fb_data->pwrdown_delay == FB_POWERDOWN_DISABLE)) {

			/* Powerdown disabled, so we disable EPDC manually */
			int count = 0;
			int sleep_ms = 10;

			mutex_unlock(&fb_data->power_mutex);

			/* If any active updates, wait for them to complete */
			while (fb_data->updates_active) {
				/* Timeout after 1 sec */
				if ((count * sleep_ms) > 1000)
					break;
				msleep(sleep_ms);
				count++;
			}

			fb_data->powering_down = true;
			epdc_powerdown(fb_data);
		} else if (fb_data->power_state != POWER_STATE_OFF) {
#endif
			GALLEN_DBGLOCAL_RUNLOG(1);
			fb_data->wait_for_powerdown = true;
			init_completion(&fb_data->powerdown_compl);
			mutex_unlock(&fb_data->power_mutex);
			ret = wait_for_completion_timeout(&fb_data->powerdown_compl,
				msecs_to_jiffies(5000));
			if (!ret) {
				dev_err(fb_data->dev,
					"No powerdown received!\n");
				GALLEN_DBGLOCAL_ESC();
				return -ETIMEDOUT;
			}
		} else
			mutex_unlock(&fb_data->power_mutex);
		break;
	case FB_BLANK_VSYNC_SUSPEND:GALLEN_DBGLOCAL_RUNLOG(1);
	case FB_BLANK_HSYNC_SUSPEND:GALLEN_DBGLOCAL_RUNLOG(2);
	case FB_BLANK_NORMAL:GALLEN_DBGLOCAL_RUNLOG(3);
		mxc_epdc_fb_flush_updates(fb_data);
		break;
	}
	
	GALLEN_DBGLOCAL_END();

	return 0;
}

static int mxc_epdc_fb_pan_display(struct fb_var_screeninfo *var,
				   struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;
	u_int y_bottom;
#ifdef USE_QUEUESPIN_LOCK //[
	unsigned long flags;
#endif //] USE_QUEUESPIN_LOCK

	
	GALLEN_DBGLOCAL_BEGIN();

	dev_dbg(info->device, "%s: var->yoffset %d, info->var.yoffset %d\n",
		 __func__, var->yoffset, info->var.yoffset);
	/* check if var is valid; also, xpan is not supported */
	if (!var || (var->xoffset != info->var.xoffset) ||
	    (var->yoffset + var->yres > var->yres_virtual)) {
		dev_dbg(info->device, "x panning not supported\n");
		GALLEN_DBGLOCAL_ESC();
		return -EINVAL;
	}

	if ((fb_data->epdc_fb_var.xoffset == var->xoffset) &&
		(fb_data->epdc_fb_var.yoffset == var->yoffset)) {
		GALLEN_DBGLOCAL_ESC();
		return 0;	/* No change, do nothing */
	}

	y_bottom = var->yoffset;

	if (!(var->vmode & FB_VMODE_YWRAP))
		y_bottom += var->yres;

	if (y_bottom > info->var.yres_virtual) {
		GALLEN_DBGLOCAL_ESC();
		return -EINVAL;
	}

#ifdef USE_QUEUESPIN_LOCK //[
	spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

	fb_data->fb_offset = (var->yoffset * var->xres_virtual + var->xoffset)
		* (var->bits_per_pixel) / 8;

	fb_data->epdc_fb_var.xoffset = var->xoffset;
	fb_data->epdc_fb_var.yoffset = var->yoffset;

	if (var->vmode & FB_VMODE_YWRAP) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		info->var.vmode |= FB_VMODE_YWRAP;
	}
	else {
		GALLEN_DBGLOCAL_RUNLOG(1);
		info->var.vmode &= ~FB_VMODE_YWRAP;
	}

#ifdef USE_QUEUESPIN_LOCK //[
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

	GALLEN_DBGLOCAL_END();
	return 0;
}

static struct fb_ops mxc_epdc_fb_ops = {
	.owner = THIS_MODULE,
	.fb_check_var = mxc_epdc_fb_check_var,
	.fb_set_par = mxc_epdc_fb_set_par,
//	.fb_setcmap = mxc_epdc_fb_setcmap,
	.fb_setcolreg = mxc_epdc_fb_setcolreg,
	.fb_pan_display = mxc_epdc_fb_pan_display,
#ifdef CONFIG_ANDROID //[
	.fb_open = mxc_epdc_fb_open,
#endif //]CONFIG_ANDROID
	.fb_ioctl = mxc_epdc_fb_ioctl,
	.fb_mmap = mxc_epdc_fb_mmap,
	.fb_blank = mxc_epdc_fb_blank,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
};

static struct fb_deferred_io mxc_epdc_fb_defio = {
	.delay = HZ,
	.deferred_io = mxc_epdc_fb_deferred_io,
};

static void epdc_done_work_func(struct work_struct *work)
{
	struct mxc_epdc_fb_data *fb_data =
		container_of(work, struct mxc_epdc_fb_data,
			epdc_done_work.work);
	epdc_powerdown(fb_data);
}

static bool is_free_list_full(struct mxc_epdc_fb_data *fb_data)
{
	int count = 0;
	struct update_data_list *plist;

	/* Count buffers in free buffer list */
	list_for_each_entry(plist, &fb_data->upd_buf_free_list, list)
		count++;

	/* Check to see if all buffers are in this list */
	if (count == EPDC_MAX_NUM_UPDATES)
		return true;
	else
		return false;
}

static irqreturn_t mxc_epdc_irq_handler(int irq, void *dev_id)
{
	struct mxc_epdc_fb_data *fb_data = dev_id;
	u32 ints_fired;
	int iChk;
#if 1
	u32 irq_mask, irq_regs;
#endif

	GALLEN_DBGLOCAL_BEGIN();

	if(0==g_fb_data) {
		printk(KERN_ERR "%s(%d):interrupt occured before probe complete .\n",__FUNCTION__,__LINE__);
	}

	/*
	 * If we just completed one-time panel init, bypass
	 * queue handling, clear interrupt and return
	 */
	if (fb_data->in_init) {
		if (epdc_is_working_buffer_complete()) {
			epdc_working_buf_intr(false);
			epdc_clear_working_buf_irq();
			dev_dbg(fb_data->dev, "Cleared WB for init update\n");
		}

		if (epdc_is_lut_complete(0)) {
			epdc_lut_complete_intr(0, false);
			epdc_clear_lut_complete_irq(0);
			fb_data->in_init = false;
			dev_dbg(fb_data->dev, "Cleared LUT complete for init update\n");
		}
		GALLEN_DBGLOCAL_ESC();
		return IRQ_HANDLED;
	}

#if 1
	irq_mask = __raw_readl(EPDC_IRQ_MASK);
	irq_regs = __raw_readl(EPDC_IRQ);
	if (irq_regs & EPDC_IRQ_TCE_UNDERRUN_IRQ) {
#else
	if (!(__raw_readl(EPDC_IRQ_MASK) & __raw_readl(EPDC_IRQ))) {
		GALLEN_DBGLOCAL_ESC();
		return IRQ_HANDLED;
	}

	if (__raw_readl(EPDC_IRQ) & EPDC_IRQ_TCE_UNDERRUN_IRQ) {
#endif
		GALLEN_DBGLOCAL_RUNLOG(4);
		dev_err(fb_data->dev,
			"TCE underrun! Will continue to update panel\n");
		/* Clear TCE underrun IRQ */
		__raw_writel(EPDC_IRQ_TCE_UNDERRUN_IRQ, EPDC_IRQ_CLEAR);
	}

	/* Check if we are waiting on EOF to sync a new update submission */
	if (epdc_signal_eof()) {
		GALLEN_DBGLOCAL_RUNLOG(5);
		epdc_eof_intr(false);
		epdc_clear_eof_irq();
		complete(&fb_data->eof_event);
	}

	/* Clear the interrupt mask for any interrupts signalled */
#if 1
	GALLEN_DBGLOCAL_RUNLOG(6);
	ints_fired = irq_mask & irq_regs;
#else
	ints_fired = __raw_readl(EPDC_IRQ_MASK) & __raw_readl(EPDC_IRQ);
#endif
#if 1
		if((__raw_readl(EPDC_STATUS_LUTS)&0xFFFF) & ints_fired) {
			GALLEN_DBGLOCAL_RUNLOG(7);
			dump_stack();
		}
#endif
	__raw_writel(ints_fired, EPDC_IRQ_MASK_CLEAR);

	queue_work(fb_data->epdc_intr_workqueue,
		&fb_data->epdc_intr_work);

	GALLEN_DBGLOCAL_END();
	return IRQ_HANDLED;
}

static void epdc_intr_work_func(struct work_struct *work)
{
	struct mxc_epdc_fb_data *fb_data =
		container_of(work, struct mxc_epdc_fb_data, epdc_intr_work);
	struct update_data_list *collision_update;
	struct mxcfb_rect *next_upd_region;
	struct update_marker_data *next_marker;
	struct update_marker_data *temp;
	int temp_index;
	u32 temp_mask;
	u32 lut;
	bool ignore_collision = false;
	int i;
	bool wb_lut_done = false;
	bool free_update = true;
	int next_lut;
	u32 epdc_irq_stat, epdc_luts_active, epdc_wb_busy, epdc_luts_avail;
	u32 epdc_collision, epdc_colliding_luts, epdc_next_lut_15;
	bool epdc_waiting_on_wb;
#ifdef USE_QUEUESPIN_LOCK //[
	unsigned long flags;
#endif //] USE_QUEUESPIN_LOCK

	int iUpdateCompletionWaked=0 ; // 20130614 gallen add .


	//if(giIsWaitingForUpdateCompleitonTimedout) {
	//	printk("%s(%d):%s() begin\n",__FILE__,__LINE__,__FUNCTION__);
	//}

	GALLEN_DBGLOCAL_BEGIN_EX(64);
	GALLEN_DBGLOCAL_DBGLVL_SET(giDbgLvl);

	/* Protect access to buffer queues and to update HW */
#ifdef USE_QUEUESPIN_LOCK //[
	spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
	

	/* Capture EPDC status one time up front to prevent race conditions */
	epdc_luts_active = epdc_any_luts_active();
	epdc_wb_busy = epdc_is_working_buffer_busy();
	epdc_luts_avail = epdc_any_luts_available();
	epdc_collision = epdc_is_collision();
	epdc_colliding_luts = epdc_get_colliding_luts();
	epdc_irq_stat = __raw_readl(EPDC_IRQ);
	epdc_waiting_on_wb = (fb_data->cur_update != NULL) ? true : false;

	/* Free any LUTs that have completed */
	for (i = 0; i < EPDC_NUM_LUTS; i++) {
		if (!(epdc_irq_stat & (1 << i)))
			continue;

		dev_dbg(fb_data->dev, "\nLUT %d completed\n", i);

		/* Disable IRQ for completed LUT */
		//epdc_lut_complete_intr(i, false);

		/*
		 * Go through all updates in the collision list and
		 * unmask any updates that were colliding with
		 * the completed LUT.
		 */
		list_for_each_entry(collision_update,
				    &fb_data->upd_buf_collision_list, list) {
			collision_update->collision_mask =
			    collision_update->collision_mask & ~(1 << i);
		}

		epdc_clear_lut_complete_irq(i);

		fb_data->luts_complete_wb |= 1 << i;

		fb_data->lut_update_order[i] = 0;

		/* Signal completion if submit workqueue needs a LUT */
		if (fb_data->waiting_for_lut) {
			complete(&fb_data->update_res_free);
			fb_data->waiting_for_lut = false;
		}

		/* Signal completion if LUT15 free and is needed */
		if (fb_data->waiting_for_lut15 && (i == 15)) {
			complete(&fb_data->lut15_free);
			fb_data->waiting_for_lut15 = false;
		}

		/* Detect race condition where WB and its LUT complete
		   (i.e. full update completes) in one swoop */
		if (fb_data->cur_update &&
			(i == fb_data->cur_update->lut_num))
			wb_lut_done = true;

		/* Signal completion if anyone waiting on this LUT */
		if (!wb_lut_done)
		{
			list_for_each_entry_safe(next_marker, temp,
				&fb_data->full_marker_list,
				full_list) {
				if (next_marker->lut_num != i)
					continue;

				/* Found marker to signal - remove from list */
				list_del_init(&next_marker->full_list);

				/* Signal completion of update */
				dev_dbg(fb_data->dev, "Signaling marker %d\n",
					next_marker->update_marker);
				if (next_marker->waiting) {
					DBG_MSG("%s(%d) update marker %d @ %p complete !\n",\
							__FUNCTION__,__LINE__,next_marker->update_marker,next_marker);
					complete(&next_marker->update_completion);
					iUpdateCompletionWaked = 1;
				}
				else {
					DBG_MSG("%s(%d) update marker %d @ %p no thread waiting, free it now !\n",\
							__FUNCTION__,__LINE__,next_marker->update_marker,next_marker);
					kfree(next_marker);
				}
			}
		}
	}

#if 0 //debug : checking any thread is waiting for marker .
	if(!iUpdateCompletionWaked) {
		list_for_each_entry_safe(next_marker, temp,
			&fb_data->full_marker_list,
			full_list) 
		{
			if(next_marker->waiting) {
				printk(KERN_ERR "%s(%d) thread is waiting for update marker %d @ %p \n",\
					__FUNCTION__,__LINE__,next_marker->update_marker,next_marker);
			}
		}
	}
#endif
	



	/* Check to see if all updates have completed */
	if (list_empty(&fb_data->upd_pending_list) &&
		is_free_list_full(fb_data) &&
		(fb_data->cur_update == NULL) &&
		!epdc_luts_active) {

		fb_data->updates_active = false;

		if (fb_data->pwrdown_delay != FB_POWERDOWN_DISABLE) {
			/*
			 * Set variable to prevent overlapping
			 * enable/disable requests
			 */
			fb_data->powering_down = true;

			/* Schedule task to disable EPDC HW until next update */
			schedule_delayed_work(&fb_data->epdc_done_work,
				msecs_to_jiffies(fb_data->pwrdown_delay));

			/* Reset counter to reduce chance of overflow */
			fb_data->order_cnt = 0;
		}

		if (fb_data->waiting_for_idle) 
		{
			//DBG_MSG(KERN_ERR "%s(%d) updating done !\n",__FUNCTION__,__LINE__);
#ifndef USE_QUEUESPIN_LOCK //[
			schedule_timeout(1);
#endif //] !USE_QUEUESPIN_LOCK

			complete(&fb_data->updates_done);

#ifndef USE_QUEUESPIN_LOCK //[
			schedule_timeout(1);
#endif //] !USE_QUEUESPIN_LOCK
		}
		else {
			//DBG_MSG(KERN_ERR "%s(%d) no thread is waiting for updating !\n",__FUNCTION__,__LINE__);
		}
	}
	else 
	{
		if (fb_data->waiting_for_idle) {
			printk(KERN_ERR "%s(%d):some thread is waiting for update !but we won't want to wake it up !\n"
					"\tupd_pending_list empty=%d,upd_buf_free_list full=%d,cur_update@%p,epdc_luts_active=0x%x\n",
					__FUNCTION__,__LINE__,
					list_empty(&fb_data->upd_pending_list),
					is_free_list_full(fb_data),
					fb_data->cur_update,
					epdc_luts_active);
		}
	}

	/* Is Working Buffer busy? */
	if (epdc_wb_busy) {
		/* Can't submit another update until WB is done */
#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

		GALLEN_DBGLOCAL_ESC();
		return;
	}

	/*
	 * Were we waiting on working buffer?
	 * If so, update queues and check for collisions
	 */
	if (epdc_waiting_on_wb) {
		dev_dbg(fb_data->dev, "\nWorking buffer completed\n");

		/* Signal completion if submit workqueue was waiting on WB */
		if (fb_data->waiting_for_wb) {
			complete(&fb_data->update_res_free);
			fb_data->waiting_for_wb = false;
		}

		/* Was there a collision? */
		if (epdc_collision) {
			/* Check list of colliding LUTs, and add to our collision mask */
			fb_data->cur_update->collision_mask =
			    epdc_colliding_luts;

			/* Clear collisions that completed since WB began */
			fb_data->cur_update->collision_mask &=
				~fb_data->luts_complete_wb;

			dev_dbg(fb_data->dev, "Collision mask = 0x%x\n",
			       epdc_colliding_luts);

			/*
			 * If we collide with newer updates, then
			 * we don't need to re-submit the update. The
			 * idea is that the newer updates should take
			 * precedence anyways, so we don't want to
			 * overwrite them.
			 */
			for (temp_mask = fb_data->cur_update->collision_mask, lut = 0;
				temp_mask != 0;
				lut++, temp_mask = temp_mask >> 1) {
				if (!(temp_mask & 0x1))
					continue;

				if (fb_data->lut_update_order[lut] >=
					fb_data->cur_update->update_desc->update_order) {
					dev_dbg(fb_data->dev,
						"Ignoring collision with"
						"newer update.\n");
					ignore_collision = true;
					break;
				}
			}

			if (!ignore_collision) {
				free_update = false;
				/*
				 * If update has markers, clear the LUTs to
				 * avoid signalling that they have completed.
				 */
				list_for_each_entry_safe(next_marker, temp,
					&fb_data->cur_update->update_desc->upd_marker_list,
					upd_list)
					next_marker->lut_num = INVALID_LUT;

				/* Move to collision list */
				list_add_tail(&fb_data->cur_update->list,
					 &fb_data->upd_buf_collision_list);
			}
		}

		if (free_update) {
			/* Handle condition where WB & LUT are both complete */
			if (wb_lut_done)
				list_for_each_entry_safe(next_marker, temp,
					&fb_data->cur_update->update_desc->upd_marker_list,
					upd_list) {

					/* Del from per-update & full list */
					list_del_init(&next_marker->upd_list);
					list_del_init(&next_marker->full_list);

					/* Signal completion of update */
					dev_dbg(fb_data->dev,
						"Signaling marker %d\n",
						next_marker->update_marker);
					if (next_marker->waiting) {
						DBG_MSG("%s(%d) update marker %d @ %p complete !\n",\
								__FUNCTION__,__LINE__,next_marker->update_marker,next_marker);
						complete(&next_marker->update_completion);
					}
					else {
						DBG_MSG("%s(%d) update marker %d @ %p no thread waiting, free it now !\n",\
								__FUNCTION__,__LINE__,next_marker->update_marker,next_marker);
						kfree(next_marker);
					}
				}

			/* Free marker list and update descriptor */
			kfree(fb_data->cur_update->update_desc);

			/* Add to free buffer list */
			list_add_tail(&fb_data->cur_update->list,
				 &fb_data->upd_buf_free_list);
		}

		/* Clear current update */
		fb_data->cur_update = NULL;

		/* Clear IRQ for working buffer */
		epdc_working_buf_intr(false);
		epdc_clear_working_buf_irq();
	}

	if (fb_data->upd_scheme != UPDATE_SCHEME_SNAPSHOT) {
		/* Queued update scheme processing */

		/* Schedule task to submit collision and pending update */
		if (!fb_data->powering_down)
			queue_work(fb_data->epdc_submit_workqueue,
				&fb_data->epdc_submit_work);

		/* Release buffer queues */
#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

		GALLEN_DBGLOCAL_ESC();
		return;
	}

	/* Snapshot update scheme processing */

	/* Check to see if any LUTs are free */
	if (!epdc_luts_avail) {
		dev_dbg(fb_data->dev, "No luts available.\n");
#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
		
		GALLEN_DBGLOCAL_ESC();
		return;
	}

	epdc_next_lut_15 = epdc_choose_next_lut(&next_lut);
	/* Check to see if there is a valid LUT to use */
	if (epdc_next_lut_15 && fb_data->tce_prevent) {
		dev_dbg(fb_data->dev, "Must wait for LUT15\n");
#ifdef USE_QUEUESPIN_LOCK //[
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
		mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

		

		GALLEN_DBGLOCAL_ESC();
		return;
	}

	/*
	 * Are any of our collision updates able to go now?
	 * Go through all updates in the collision list and check to see
	 * if the collision mask has been fully cleared
	 */
	list_for_each_entry(collision_update,
			    &fb_data->upd_buf_collision_list, list) {

		if (collision_update->collision_mask != 0)
			continue;

		dev_dbg(fb_data->dev, "A collision update is ready to go!\n");
		/*
		 * We have a collision cleared, so select it
		 * and we will retry the update
		 */
		fb_data->cur_update = collision_update;
		list_del_init(&fb_data->cur_update->list);
		break;
	}

	/*
	 * If we didn't find a collision update ready to go,
	 * we try to grab one from the update queue
	 */
	if (fb_data->cur_update == NULL) {
		/* Is update list empty? */
		if (list_empty(&fb_data->upd_buf_queue)) {
			dev_dbg(fb_data->dev, "No pending updates.\n");

			/* No updates pending, so we are done */
#ifdef USE_QUEUESPIN_LOCK //[
			spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
			mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
			
			GALLEN_DBGLOCAL_ESC();
			return;
		} else {
			dev_dbg(fb_data->dev, "Found a pending update!\n");

			/* Process next item in update list */
			fb_data->cur_update =
			    list_entry(fb_data->upd_buf_queue.next,
				       struct update_data_list, list);
			list_del_init(&fb_data->cur_update->list);
		}
	}

	/* Use LUT selected above */
	fb_data->cur_update->lut_num = next_lut;

	/* Associate LUT with update markers */
	list_for_each_entry_safe(next_marker, temp,
		&fb_data->cur_update->update_desc->upd_marker_list, upd_list)
		next_marker->lut_num = fb_data->cur_update->lut_num;

	/* Mark LUT as containing new update */
	fb_data->lut_update_order[fb_data->cur_update->lut_num] =
		fb_data->cur_update->update_desc->update_order;

	/* Enable Collision and WB complete IRQs */
	epdc_working_buf_intr(true);
	epdc_lut_complete_intr(fb_data->cur_update->lut_num, true);

	/* Program EPDC update to process buffer */
	next_upd_region =
		&fb_data->cur_update->update_desc->upd_data.update_region;
	if (fb_data->cur_update->update_desc->upd_data.temp
		!= TEMP_USE_AMBIENT) {
		temp_index = mxc_epdc_fb_get_temp_index(fb_data,
			fb_data->cur_update->update_desc->upd_data.temp);
		DBG_MSG("%s(%d) set temp=%d into EPDC by manual\n",__FILE__,__LINE__,temp_index);
		epdc_set_temp(temp_index);
	} else {
		DBG_MSG("%s(%d) set temp=%d into EPDC by AMBIENT\n",__FILE__,__LINE__,fb_data->temp_index);
		epdc_set_temp(fb_data->temp_index);
	}
	epdc_set_update_addr(fb_data->cur_update->phys_addr +
				fb_data->cur_update->update_desc->epdc_offs);
	epdc_set_update_coord(next_upd_region->left, next_upd_region->top);
	epdc_set_update_dimensions(next_upd_region->width,
				   next_upd_region->height);

	epdc_submit_update(fb_data->cur_update->lut_num,
			   fb_data->cur_update->update_desc->upd_data.waveform_mode,
			   fb_data->cur_update->update_desc->upd_data.update_mode,
			   false, 0);

	/* Release buffer queues */
#ifdef USE_QUEUESPIN_LOCK //[
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

	GALLEN_DBGLOCAL_END();
	return;
}

static void draw_mode0(struct mxc_epdc_fb_data *fb_data)
{
	u32 *upd_buf_ptr;
	int i;
	struct fb_var_screeninfo *screeninfo = &fb_data->epdc_fb_var;
	u32 xres, yres;

#ifdef USE_QUEUESPIN_LOCK //[
	spin_lock_irqsave(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_lock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
		

	GALLEN_DBGLOCAL_BEGIN();
	upd_buf_ptr = (u32 *)fb_data->info.screen_base;

	epdc_working_buf_intr(true);
	epdc_lut_complete_intr(0, true);
	fb_data->in_init = true;

	/* Use unrotated (native) width/height */
	if ((screeninfo->rotate == FB_ROTATE_CW) ||
		(screeninfo->rotate == FB_ROTATE_CCW)) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		xres = screeninfo->yres;
		yres = screeninfo->xres;
	} else {
		GALLEN_DBGLOCAL_RUNLOG(1);
		xres = screeninfo->xres;
		yres = screeninfo->yres;
	}

	/* Program EPDC update to process buffer */
	epdc_set_update_addr(fb_data->phys_start);
	epdc_set_update_coord(0, 0);
	epdc_set_update_dimensions(xres, yres);
	epdc_submit_update(0, fb_data->wv_modes.mode_init, UPDATE_MODE_FULL, true, 0xFF);

#ifdef USE_QUEUESPIN_LOCK //[
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_unlock(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK

	dev_dbg(fb_data->dev, "Mode0 update - Waiting for LUT to complete...\n");

	/* Will timeout after ~5 seconds */

	for (i = 0; i < 50; i++) {
		if (!epdc_is_lut_active(0)) {
			dev_dbg(fb_data->dev, "Mode0 init complete\n");
			GALLEN_DBGLOCAL_ESC();
			return;
		}
		msleep(100);
	}

	dev_err(fb_data->dev, "Mode0 init failed!\n");

	GALLEN_DBGLOCAL_END();
	return;
}

int mxc_epdc_fb_enable_reagl(int iIsEnable)
{
	int iRet;
	if(!g_fb_data) {
		return -1;
	}

	if(iIsEnable) {
		if(0x19==gbModeVersion&&0==gbWFM_REV) {
			g_fb_data->wv_modes.mode_aa = 4; /* default REAGL mode */
			g_fb_data->wv_modes.mode_aad = 5; /* default REAGL-D mode */
			giNTX_waveform_modeA[NTX_WFM_MODE_GLD16] = 4;
			giNTX_waveform_modeA[NTX_WFM_MODE_GLR16] = 5;
			iRet = 0;
			
			printk("enable reagl ,aa=%d,aad=%d\n",
					g_fb_data->wv_modes.mode_aa,
					g_fb_data->wv_modes.mode_aad);
			
		}
		else {
			iRet = -1;
		}
	}
	else {
		if(0x19==gbModeVersion&&0==gbWFM_REV) {
			g_fb_data->wv_modes.mode_aa = 3; /* default REAGL mode */
			g_fb_data->wv_modes.mode_aad = 2; /* default REAGL-D mode */
			giNTX_waveform_modeA[NTX_WFM_MODE_GLD16] = 3;
			giNTX_waveform_modeA[NTX_WFM_MODE_GLR16] = 2;
			iRet = 0;
			
			printk("disable reagl ,aa=%d,aad=%d\n",
					g_fb_data->wv_modes.mode_aa,
					g_fb_data->wv_modes.mode_aad);
			
		}
		else {
			iRet = -1;
		}
	}

	return iRet;
}	


static void mxc_epdc_fb_fw_handler(const struct firmware *fw,
						     void *context)
{
	struct mxc_epdc_fb_data *fb_data = context;
	int ret;
	struct mxcfb_waveform_data_file *wv_file;
	int wv_data_offs;
	int i;
	struct mxcfb_update_data update;
	struct fb_var_screeninfo *screeninfo = &fb_data->epdc_fb_var;
	u32 xres, yres;


#ifdef FW_IN_RAM//[
	struct firmware ram_fw;
#endif //]FW_IN_RAM

	GALLEN_DBGLOCAL_BEGIN();
	
	
	if (fw == NULL) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		/* If default FW file load failed, we give up */
		if (fb_data->fw_default_load) {
			GALLEN_DBGLOCAL_ESC();
			return;
		}

		/* Try to load default waveform */
		dev_dbg(fb_data->dev,
			"Can't find firmware. Trying fallback fw\n");
		fb_data->fw_default_load = true;
#ifdef FW_IN_RAM//[ gallen modify 20110609 : waveform pass from bootloader in RAM .
		{
			

			ram_fw.size = gdwWF_size;
			ram_fw.data = (u8*)gpbWF_vaddr;
			//fw.page = 0;
			printk("[%s]:fw p=%p,size=%u\n",__FUNCTION__,ram_fw.data,ram_fw.size);
			//mxc_epdc_fb_fw_handler(&fw,fb_data);
			ret = 0;
			fw=&ram_fw;
		}
#else //][ befrom modify ...
		ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG,
			"imx/epdc.fw", fb_data->dev, GFP_KERNEL, fb_data,
			mxc_epdc_fb_fw_handler);
		if (ret) {
			GALLEN_DBGLOCAL_RUNLOG(1);
			dev_err(fb_data->dev,
				"Failed request_firmware_nowait err %d\n", ret);
		}
#endif //]
		GALLEN_DBGLOCAL_ESC();
		return;
	}
	
	
	gbModeVersion = *(fw->data+0x10);
	gbWFM_REV = *(fw->data+0x16);
	gbFPL_Platform=*(fw->data+0x0d);

	GALLEN_DBGLOCAL_PRINTMSG("---fw data = %p,fw size = %u ---\n",fw->data,fw->size);

	if((0x20==gbModeVersion)||(0x19==gbModeVersion)||(0x18==gbModeVersion)) {
		GALLEN_DBGLOCAL_RUNLOG(6);
		fb_data->wv_modes.mode_gl16 = 3; /* GL16 mode */
		fb_data->wv_modes.mode_a2 = 6; /* A2 mode */
		if(0x18==gbModeVersion) {
			fb_data->wv_modes.mode_gc4 = 2; /* GC4 mode */
		}
		else {
			fb_data->wv_modes.mode_gc4 = 7; /* GC4 mode */
		}
		fb_data->wv_modes.mode_du = 1; /* DU mode */
#if defined(NO_CUS_REAGL_MODE) //[
		fb_data->wv_modes.mode_aa = fb_data->wv_modes.mode_gl16; /* REAGL mode */
		fb_data->wv_modes.mode_aad = fb_data->wv_modes.mode_gc16; /* REAGL-D mode */
#else //][!NO_CUS_REAGL_MODE
		fb_data->wv_modes.mode_aa = 4; /* REAGL mode */
		fb_data->wv_modes.mode_aad = 5; /*REAGL-D mode */
#endif //]NO_CUS_REAGL_MODE
	}
	else if(0x13==gbModeVersion) {
		fb_data->wv_modes.mode_du = 1; /* DU mode */
		fb_data->wv_modes.mode_a2 = 4; /* A2 mode */
		fb_data->wv_modes.mode_gl16 = 3; /* GL16 mode as GLR16*/
		fb_data->wv_modes.mode_aa = 3; /* REAGL mode as GLR16*/
		fb_data->wv_modes.mode_aad = 2; /*REAGL-D mode as GC16*/
	}
	else if(0x23==gbModeVersion) {
		// AD type waveform .
		fb_data->wv_modes.mode_du = 1; /* DU mode */
		fb_data->wv_modes.mode_a2 = 4; /* A2 mode */
		fb_data->wv_modes.mode_gl16 = 3; /* GL16 mode */
		fb_data->wv_modes.mode_aa = 3; /* REAGL mode as GLR16*/
		fb_data->wv_modes.mode_aad = 2; /*REAGL-D mode as GC16*/
	}
	else if(0x4==gbModeVersion) {
		fb_data->wv_modes.mode_gl16 = 5; /* GL16 mode */
		fb_data->wv_modes.mode_aa= 5; /* REAGL mode */
		fb_data->wv_modes.mode_aad = 5; /* REAGL-D mode */
		fb_data->wv_modes.mode_a2 = 4; /* A2 mode */
	}
	else {
		fb_data->wv_modes.mode_gl16 = 2; /* GL16 mode */
		fb_data->wv_modes.mode_aa = 2; /* REAGL mode */
		fb_data->wv_modes.mode_aad = 2; /* REAGL-D mode */
		fb_data->wv_modes.mode_a2 = 1; /* A2 mode */
	}


	giNTX_waveform_modeA[NTX_WFM_MODE_INIT] = fb_data->wv_modes.mode_init;
	giNTX_waveform_modeA[NTX_WFM_MODE_DU] = fb_data->wv_modes.mode_du;
	giNTX_waveform_modeA[NTX_WFM_MODE_GC16] = fb_data->wv_modes.mode_gc16;
	giNTX_waveform_modeA[NTX_WFM_MODE_GC4] = fb_data->wv_modes.mode_gc4;
	giNTX_waveform_modeA[NTX_WFM_MODE_A2] = fb_data->wv_modes.mode_a2;
	giNTX_waveform_modeA[NTX_WFM_MODE_GL16] = fb_data->wv_modes.mode_gl16;
	giNTX_waveform_modeA[NTX_WFM_MODE_GLR16] = fb_data->wv_modes.mode_aa;
	giNTX_waveform_modeA[NTX_WFM_MODE_GLD16] = fb_data->wv_modes.mode_aad;



	wv_file = (struct mxcfb_waveform_data_file *)fw->data;

	if(1==gptHWCFG->m_val.bDisplayResolution) {
		printk("%s(%d):EPD 1024x758 \n",__FILE__,__LINE__);
		fb_data->cur_mode = &fb_data->pdata->epdc_mode[2];//
	}
	else if(3==gptHWCFG->m_val.bDisplayResolution) {
		printk("%s(%d):EPD 1440x1080 \n",__FILE__,__LINE__);
		fb_data->cur_mode = &fb_data->pdata->epdc_mode[5];//
	}
	else if(5==gptHWCFG->m_val.bDisplayResolution) {
		printk("%s(%d):EPD 1448x1072 \n",__FILE__,__LINE__);
		fb_data->cur_mode = &fb_data->pdata->epdc_mode[6];//
	}
	else if(2==gptHWCFG->m_val.bDisplayResolution) {
		printk("%s(%d):EPD 1024x768 \n",__FILE__,__LINE__);
		fb_data->cur_mode = &fb_data->pdata->epdc_mode[3];//
	}
	else
	{
		unsigned char *pbWFHdr = (unsigned char *)(fw->data);
		switch (*(pbWFHdr+0x0d)) {// FPL platform detect .
		case 0x06: // V220 .
			if(27==gptHWCFG->m_val.bPCB) {
				printk("%s(%d):V220 for E50610 FPL platform \n",__FILE__,__LINE__);
				fb_data->cur_mode = &fb_data->pdata->epdc_mode[4];//v220 parameters for E50612 .
			}
			else {
				printk("%s(%d):V220 FPL platform \n",__FILE__,__LINE__);
				fb_data->cur_mode = &fb_data->pdata->epdc_mode[0];//v220 parameters
			}
			break;
		case 0x03: // V110 .
		case 0x04: // V110A .
			printk("%s(%d):V110/V110A FPL platform \n",__FILE__,__LINE__);
			ASSERT(fb_data->pdata->num_modes>1);
			fb_data->cur_mode = &fb_data->pdata->epdc_mode[1];//v110 parameters
			break;
		default:
			ERR_MSG("%s(%d):new FPL platform=0x%x ,please modify source to support this \n",__FILE__,__LINE__,*(pbWFHdr+0x0d));
			break;
		}
	}

	/* Get size and allocate temperature range table */
	fb_data->trt_entries = wv_file->wdh.trc + 1;
	fb_data->temp_range_bounds = kzalloc(fb_data->trt_entries + 1, GFP_KERNEL);

	/* Copy TRT data */
	memcpy(fb_data->temp_range_bounds, &wv_file->data, fb_data->trt_entries + 1);
	for (i = 0; i < fb_data->trt_entries; i++) {
		GALLEN_DBGLOCAL_RUNLOG(2);
		DBG_MSG("trt entry #%d = 0x%x(%d~%d)\n", 
				i, *((u8 *)&wv_file->data + i),fb_data->temp_range_bounds[i],fb_data->temp_range_bounds[i+1]-1);
	}


	/* Set default temperature index using TRT and room temp */
	fb_data->temp_index = mxc_epdc_fb_get_temp_index(fb_data, DEFAULT_TEMP);

	/* Get offset and size for waveform data */
	wv_data_offs = sizeof(wv_file->wdh) + fb_data->trt_entries + 1;
	fb_data->waveform_buffer_size = fw->size - wv_data_offs;

#ifdef REGAL_ENABLE//[
	/* check for 2nd gen waveform firmware file */
	{
		int awv, wmc, wtrc, xwia;
		u64 longOffset;
		u32 bufferSize;
		u8 *fwDataBuffer = (u8 *)(fw->data) + wv_data_offs;
		u8 *waveform_xwi_buffer = NULL;

		wtrc = wv_file->wdh.trc + 1;
		wmc = wv_file->wdh.mc + 1;
		awv = wv_file->wdh.awv;
		xwia = wv_file->wdh.xwia;
		memcpy (&longOffset,fwDataBuffer,8);
//		printk("%s() : awv=0x%02X\n",__FUNCTION__,awv);
		
		if ((unsigned) longOffset > (8*wmc))
		{
			u64 avcOffset, acdOffset, acdMagic, xwiOffset;
			GALLEN_DBGLOCAL_RUNLOG(7);
			avcOffset = acdOffset = acdMagic = xwiOffset = 0l;
			/* look at the advance waveform flags */
			switch ( awv ) {
				case 0 : /* voltage control flag is set */
					GALLEN_DBGLOCAL_RUNLOG(8);
					if (xwia > 0) {
						GALLEN_DBGLOCAL_RUNLOG(9);
						/* extra waveform information */
						memcpy (&xwiOffset,fwDataBuffer + (8*wmc),8);
						bufferSize = (unsigned)(fb_data->waveform_buffer_size - xwiOffset);
						fb_data->waveform_xwi_buffer = kmalloc(bufferSize, GFP_KERNEL);
						memcpy(fb_data->waveform_xwi_buffer, fwDataBuffer+xwiOffset, bufferSize );
						fb_data->waveform_buffer_size = xwiOffset;
					}
					break;
				case 1 : /* voltage control flag is set */
					GALLEN_DBGLOCAL_RUNLOG(10);
					memcpy (&avcOffset,fwDataBuffer + (8*wmc),8);
					if (xwia > 0) {
						GALLEN_DBGLOCAL_RUNLOG(11);
						/* extra waveform information */
						memcpy (&xwiOffset,fwDataBuffer + (8*wmc) +8,8);
						bufferSize = (unsigned)(fb_data->waveform_buffer_size - xwiOffset);
						fb_data->waveform_xwi_buffer = kmalloc(bufferSize, GFP_KERNEL);
						/* voltage control data */
						memcpy(fb_data->waveform_xwi_buffer, fwDataBuffer+xwiOffset, bufferSize );
						bufferSize = (unsigned)(xwiOffset - avcOffset);
						fb_data->waveform_vcd_buffer = kmalloc(bufferSize, GFP_KERNEL);
						memcpy(fb_data->waveform_vcd_buffer, fwDataBuffer+avcOffset, bufferSize );
					}
					else {
						/* voltage control data */
						GALLEN_DBGLOCAL_RUNLOG(12);
						bufferSize = (unsigned)(fb_data->waveform_buffer_size - avcOffset);
						fb_data->waveform_vcd_buffer = kmalloc(bufferSize, GFP_KERNEL);
						memcpy(fb_data->waveform_vcd_buffer, fwDataBuffer+avcOffset, bufferSize );
					}
					fb_data->waveform_buffer_size = avcOffset;
					break;
				case 2 : /* voltage control flag is set */
					GALLEN_DBGLOCAL_RUNLOG(13);
					memcpy (&acdOffset,fwDataBuffer + (8*wmc),8);
					memcpy (&acdMagic,fwDataBuffer + (8*wmc) + 8,8);
					if (xwia > 0) {
						GALLEN_DBGLOCAL_RUNLOG(14);
						/* extra waveform information */
						memcpy (&xwiOffset,fwDataBuffer + (8*wmc) + 16,8);
						bufferSize = (unsigned)(fb_data->waveform_buffer_size - xwiOffset);
						fb_data->waveform_xwi_buffer = kmalloc(bufferSize, GFP_KERNEL);
						memcpy(fb_data->waveform_xwi_buffer, fwDataBuffer+xwiOffset, bufferSize );
						/* algorithm control data */
						bufferSize = (unsigned)(xwiOffset - acdOffset);
						fb_data->waveform_acd_buffer = kmalloc(bufferSize, GFP_KERNEL);
						memcpy(fb_data->waveform_acd_buffer, fwDataBuffer+acdOffset, bufferSize );
					}
					else {
						GALLEN_DBGLOCAL_RUNLOG(15);
						/* algorithm control data */
						bufferSize = (unsigned)(fb_data->waveform_buffer_size - acdOffset);
						fb_data->waveform_acd_buffer = kmalloc(bufferSize, GFP_KERNEL);
						memcpy(fb_data->waveform_acd_buffer, fwDataBuffer+acdOffset, bufferSize );
					}
					fb_data->waveform_buffer_size = acdOffset;
					break;
				case 3 : /* voltage control flag is set */
					GALLEN_DBGLOCAL_RUNLOG(16);
					memcpy (&avcOffset,fwDataBuffer + (8*wmc),8);
					memcpy (&acdOffset,fwDataBuffer + (8*wmc) + 8,8);
					memcpy (&acdMagic,fwDataBuffer + (8*wmc) + 16,8);
					if (xwia > 0) {
						GALLEN_DBGLOCAL_RUNLOG(17);
						/* extra waveform information */
						memcpy (&xwiOffset,fwDataBuffer + (8*wmc) + 24,8);
						bufferSize = (unsigned)(fb_data->waveform_buffer_size - xwiOffset);
						fb_data->waveform_xwi_buffer = kmalloc(bufferSize, GFP_KERNEL);
						memcpy(fb_data->waveform_xwi_buffer, fwDataBuffer+xwiOffset, bufferSize );
						/* algorithm control data */
						bufferSize = (unsigned)(xwiOffset - acdOffset);
						fb_data->waveform_acd_buffer = kmalloc(bufferSize, GFP_KERNEL);
						memcpy(fb_data->waveform_acd_buffer, fwDataBuffer+acdOffset, bufferSize );
						/* voltage control data */
						bufferSize = (unsigned)(acdOffset - avcOffset);
						fb_data->waveform_vcd_buffer = kmalloc(bufferSize, GFP_KERNEL);
						memcpy(fb_data->waveform_vcd_buffer, fwDataBuffer+avcOffset, bufferSize );
					}
					else {
						GALLEN_DBGLOCAL_RUNLOG(18);
						/* algorithm control data */
						bufferSize = (unsigned)(fb_data->waveform_buffer_size - acdOffset);
						fb_data->waveform_acd_buffer = kmalloc(bufferSize, GFP_KERNEL);
						memcpy(fb_data->waveform_acd_buffer, fwDataBuffer+avcOffset, bufferSize );
						/* voltage control data */
						bufferSize = (unsigned)(acdOffset - avcOffset);
						fb_data->waveform_vcd_buffer = kmalloc(bufferSize, GFP_KERNEL);
						memcpy(fb_data->waveform_vcd_buffer, fwDataBuffer+avcOffset, bufferSize );
					}
					fb_data->waveform_buffer_size = avcOffset;
					break;
				}
			if (acdMagic) 
			{
				GALLEN_DBGLOCAL_RUNLOG(19);
				fb_data->waveform_magic_number = acdMagic;
			}
			fb_data->waveform_mc = wmc;
			fb_data->waveform_trc =wtrc;
		}
	}

	/* get the extra waveform info and display it - This can be removed! It is here for illustration only */
	if (fb_data->waveform_xwi_buffer) {
		char *xwiString;
		unsigned strLength = mxc_epdc_fb_fetch_wxi_data(fb_data->waveform_xwi_buffer, NULL);

		GALLEN_DBGLOCAL_RUNLOG(20);

		dev_info(fb_data->dev, " --- Extra Waveform Data length: %d bytes---\n",strLength);
		if (strLength > 0) {
			GALLEN_DBGLOCAL_RUNLOG(21);
			xwiString = (char *) kmalloc(strLength + 1, GFP_KERNEL);
			if (mxc_epdc_fb_fetch_wxi_data(fb_data->waveform_xwi_buffer, xwiString) > 0) {
				GALLEN_DBGLOCAL_RUNLOG(22);
				xwiString[strLength+1] = '\0';
				dev_info(fb_data->dev, "     Extra Waveform Data: %s\n",xwiString);
			}
			else {
				GALLEN_DBGLOCAL_RUNLOG(23);
				dev_err(fb_data->dev, " *** Extra Waveform Data checksum error ***\n");
			}

			kfree(xwiString);
		}	
	}

	/* show the vcd */
	if (fb_data->waveform_vcd_buffer) {
		{
			struct epd_vc_data vcd;
			/* fetch and display the voltage control data for waveform mode 0, temp range 0 */
			fetch_Epdc_Pmic_Voltages(&vcd, fb_data, 0, 0);
		}
	}
#endif //]REGAL_ENABLE

	/* Allocate memory for waveform data */
	fb_data->waveform_buffer_virt = dma_alloc_coherent(fb_data->dev,
						fb_data->waveform_buffer_size,
						&fb_data->waveform_buffer_phys,
						GFP_DMA);
	if (fb_data->waveform_buffer_virt == NULL) {
		dev_err(fb_data->dev, "Can't allocate mem for waveform!\n");
		GALLEN_DBGLOCAL_ESC();
		return;
	}
	DBG_MSG("[%s]waveform:vir_p=%p,phy_p=%p,sz=%d,trt_entries=%d\n",__FUNCTION__,\
		fb_data->waveform_buffer_virt,fb_data->waveform_buffer_phys,\
		fb_data->waveform_buffer_size,fb_data->trt_entries);
	

	memcpy(fb_data->waveform_buffer_virt, (u8 *)(fw->data) + wv_data_offs,
		fb_data->waveform_buffer_size);

	/* check buffer pixel format */
	if ((wv_file->wdh.luts & 0x0c) == 0x04) 
	{
		GALLEN_DBGLOCAL_RUNLOG(27);
		bufPixFormat=EPDC_FORMAT_BUF_PIXEL_FORMAT_P5N;
	}
	else
	{
		GALLEN_DBGLOCAL_RUNLOG(28);
		bufPixFormat=EPDC_FORMAT_BUF_PIXEL_FORMAT_P4N;
	}

#ifdef FW_IN_RAM//[ gallen modify 20110609 : waveform pass from bootloader in RAM .
#else//][!FW_IN_RAM

	release_firmware(fw);
#endif //]FW_IN_RAM

	/* Enable clocks to access EPDC regs */
	clk_enable(fb_data->epdc_clk_axi);

	/* Enable pix clk for EPDC */
	clk_enable(fb_data->epdc_clk_pix);
	clk_set_rate(fb_data->epdc_clk_pix, fb_data->cur_mode->vmode->pixclock);

	epdc_init_sequence(fb_data);

	/* Disable clocks */
	clk_disable(fb_data->epdc_clk_axi);
	clk_disable(fb_data->epdc_clk_pix);

	fb_data->hw_ready = true;

	/* Use unrotated (native) width/height */
	if ((screeninfo->rotate == FB_ROTATE_CW) ||
		(screeninfo->rotate == FB_ROTATE_CCW)) {
		GALLEN_DBGLOCAL_RUNLOG(3);
		xres = screeninfo->yres;
		yres = screeninfo->xres;
	} else {
		GALLEN_DBGLOCAL_RUNLOG(4);
		xres = screeninfo->xres;
		yres = screeninfo->yres;
	}

#if 0 // gallen remove 20110704
	update.update_region.left = 0;
	update.update_region.width = xres;
	update.update_region.top = 0;
	update.update_region.height = yres;
	update.update_mode = UPDATE_MODE_FULL;
	update.waveform_mode = WAVEFORM_MODE_AUTO;
	update.update_marker = INIT_UPDATE_MARKER;
	update.temp = TEMP_USE_AMBIENT;
	update.flags = 0;

	mxc_epdc_fb_send_update(&update, &fb_data->info);

	/* Block on initial update */
	ret = mxc_epdc_fb_wait_update_complete(update.update_marker,
		&fb_data->info);
	if (ret < 0) {
		GALLEN_DBGLOCAL_RUNLOG(5);
		dev_err(fb_data->dev,
			"Wait for update complete failed.  Error = 0x%x", ret);
			
	}
#endif 
	
	k_fake_s1d13522_init((unsigned char *)gpbLOGO_vaddr);

	//printk("%s() end---\n",__FUNCTION__);
	GALLEN_DBGLOCAL_END();
}

static int mxc_epdc_fb_init_hw(struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;
	int ret;
	
	GALLEN_DBGLOCAL_BEGIN();

	/*
	 * Create fw search string based on ID string in selected videomode.
	 * Format is "imx/epdc_[panel string].fw"
	 */
	if (fb_data->cur_mode) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		strcat(fb_data->fw_str, "imx/epdc_");
		strcat(fb_data->fw_str, fb_data->cur_mode->vmode->name);
		strcat(fb_data->fw_str, ".fw");
	}

	fb_data->fw_default_load = false;

#ifdef FW_IN_RAM//[ gallen modify 20110609 : waveform pass from bootloader in RAM .
	#if 1
		queue_work(fb_data->epdc_submit_workqueue,
			&fb_data->epdc_firmware_work);
		ret = 0;
	#else
	{

		struct firmware fw;
		
		fw.size = gdwWF_size;
		fw.data = gpbWF_vaddr;
		//fw.page = 0;
		printk("[%s]:fw p=%p,size=%u\n",__FUNCTION__,fw.data,fw.size);
		mxc_epdc_fb_fw_handler(&fw,fb_data);
		ret = 0;
	}
	#endif
#else	//][ befrom modify ...
	ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG,
				fb_data->fw_str, fb_data->dev, GFP_KERNEL,
				fb_data, mxc_epdc_fb_fw_handler);
	if (ret) 
	{
		GALLEN_DBGLOCAL_RUNLOG(1);
		dev_dbg(fb_data->dev,
			"Failed request_firmware_nowait err %d\n", ret);
	}
#endif //]

	GALLEN_DBGLOCAL_END();
	return ret;
}

static ssize_t store_update(struct device *device,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct mxcfb_update_data update;
	struct fb_info *info = dev_get_drvdata(device);
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;

	GALLEN_DBGLOCAL_BEGIN();
	if (strncmp(buf, "direct", 6) == 0) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		update.waveform_mode = fb_data->wv_modes.mode_du;
	}
	else if (strncmp(buf, "gc16", 4) == 0) {
		GALLEN_DBGLOCAL_RUNLOG(1);
		update.waveform_mode = fb_data->wv_modes.mode_gc16;
	}
	else if (strncmp(buf, "gc4", 3) == 0) {
		GALLEN_DBGLOCAL_RUNLOG(2);
		update.waveform_mode = fb_data->wv_modes.mode_gc4;
	}

	/* Now, request full screen update */
	update.update_region.left = 0;
	update.update_region.width = fb_data->epdc_fb_var.xres;
	update.update_region.top = 0;
	update.update_region.height = fb_data->epdc_fb_var.yres;
	update.update_mode = UPDATE_MODE_FULL;
	update.temp = TEMP_USE_AMBIENT;
	update.update_marker = 0;
	update.flags = 0;

	mxc_epdc_fb_send_update(&update, info);

	GALLEN_DBGLOCAL_END();
	return count;
}

static struct device_attribute fb_attrs[] = {
	__ATTR(update, S_IRUGO|S_IWUSR, NULL, store_update),
};

int __devinit mxc_epdc_fb_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct mxc_epdc_fb_data *fb_data;
	struct resource *res;
	struct fb_info *info;
	char *options, *opt;
	char *panel_str = NULL;
	char name[] = "mxcepdcfb";
	struct fb_videomode *vmode;
	int xres_virt, yres_virt, buf_size;
	int xres_virt_rot, yres_virt_rot, pix_size_rot;
	struct fb_var_screeninfo *var_info;
	struct fb_fix_screeninfo *fix_info;
	struct pxp_config_data *pxp_conf;
	struct pxp_proc_data *proc_data;
	struct scatterlist *sg;
	struct update_data_list *upd_list;
	struct update_data_list *plist, *temp_list;
	int i;
	unsigned long x_mem_size = 0;
#ifdef CONFIG_FRAMEBUFFER_CONSOLE
	struct mxcfb_update_data update;
#endif

	GALLEN_DBGLOCAL_BEGIN_EX(64);
	
	if(4!=gptHWCFG->m_val.bDisplayCtrl&&6!=gptHWCFG->m_val.bDisplayCtrl) {
		// display controller is not mx508 .
		ERR_MSG("[%s] ignore probe : hwcfg dispctrl not mx508 (is %d).\n",\
			__FUNCTION__,gptHWCFG->m_val.bDisplayCtrl);
		return -ENODEV;
	}

	if(0==gptHWCFG->m_val.bRamSize||1==gptHWCFG->m_val.bRamSize) {
		// RAM SIZE < 256MB
		giEPDC_MAX_NUM_UPDATES = 1;
	}
	else {
		giEPDC_MAX_NUM_UPDATES = 10;
	}

	fb_data = (struct mxc_epdc_fb_data *)framebuffer_alloc(
			sizeof(struct mxc_epdc_fb_data), &pdev->dev);
	if (fb_data == NULL) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		ret = -ENOMEM;
		goto out;
	}

#if 1
	init_completion(&fb_data->powerdown_compl);
	init_completion(&fb_data->updates_done);
	init_completion(&fb_data->update_res_free);
	init_completion(&fb_data->lut15_free);
	init_completion(&fb_data->eof_event);
	init_completion(&fb_data->pxp_tx_cmpl);
#endif

	DBG_MSG("[%s]epdc_fb_data size=%d\n",__FUNCTION__,sizeof(struct mxc_epdc_fb_data));

	/* Get platform data and check validity */
	fb_data->pdata = pdev->dev.platform_data;
	if ((fb_data->pdata == NULL) || (fb_data->pdata->num_modes < 1)
		|| (fb_data->pdata->epdc_mode == NULL)
		|| (fb_data->pdata->epdc_mode->vmode == NULL)) {
		GALLEN_DBGLOCAL_RUNLOG(1);
		ret = -EINVAL;
		goto out_fbdata;
	}

	if (fb_get_options(name, &options)) {
		GALLEN_DBGLOCAL_RUNLOG(2);
		ret = -ENODEV;
		goto out_fbdata;
	}

	fb_data->tce_prevent = 1;
	
	if (options) 
	{
		GALLEN_DBGLOCAL_RUNLOG(3);
		while ((opt = strsep(&options, ",")) != NULL) {
			GALLEN_DBGLOCAL_RUNLOG(4);
			if (!*opt) {
				GALLEN_DBGLOCAL_RUNLOG(5);
				continue;
			}

			if (!strncmp(opt, "bpp=", 4)) 
			{
				GALLEN_DBGLOCAL_RUNLOG(6);
				fb_data->default_bpp =
					simple_strtoul(opt + 4, NULL, 0);
			}
			else if (!strncmp(opt, "x_mem=", 6)) {
				GALLEN_DBGLOCAL_RUNLOG(7);
				x_mem_size = memparse(opt + 6, NULL);
			}
			else if (!strncmp(opt,"tce_prevent",11)) 
			{
				fb_data->tce_prevent = 1;
			}
			else {
				GALLEN_DBGLOCAL_RUNLOG(8);
				panel_str = opt;
			}
		}
	}

	fb_data->dev = &pdev->dev;

	if (!fb_data->default_bpp) {
		GALLEN_DBGLOCAL_RUNLOG(9);
		fb_data->default_bpp = default_bpp;
	}

	/* Set default (first defined mode) before searching for a match */
	if(1==gptHWCFG->m_val.bDisplayResolution) {
		printk("%s(%d):EPD 1024x758 \n",__FILE__,__LINE__);
		fb_data->cur_mode = &fb_data->pdata->epdc_mode[2];//
	}
	else if(3==gptHWCFG->m_val.bDisplayResolution) {
		printk("%s(%d):EPD 1440x1080 \n",__FILE__,__LINE__);
		fb_data->cur_mode = &fb_data->pdata->epdc_mode[5];//
	}
	else if(5==gptHWCFG->m_val.bDisplayResolution) {
		printk("%s(%d):EPD 1448x1072 \n",__FILE__,__LINE__);
		fb_data->cur_mode = &fb_data->pdata->epdc_mode[6];//
	}
	else if(2==gptHWCFG->m_val.bDisplayResolution) {
		printk("%s(%d):EPD 1024x768 \n",__FILE__,__LINE__);
		fb_data->cur_mode = &fb_data->pdata->epdc_mode[3];//
	}
	else {
		fb_data->cur_mode = &fb_data->pdata->epdc_mode[0];
	}

	if (panel_str) {
		GALLEN_DBGLOCAL_RUNLOG(10);
		for (i = 0; i < fb_data->pdata->num_modes; i++)
			if (!strcmp(fb_data->pdata->epdc_mode[i].vmode->name,
						panel_str)) {
				GALLEN_DBGLOCAL_RUNLOG(11);
				fb_data->cur_mode =
					&fb_data->pdata->epdc_mode[i];
				break;
			}
	}

	vmode = fb_data->cur_mode->vmode;

	platform_set_drvdata(pdev, fb_data);
	info = &fb_data->info;

	/* Allocate color map for the FB */
	ret = fb_alloc_cmap(&info->cmap, 256, 0);
	if (ret) {
		GALLEN_DBGLOCAL_RUNLOG(12);
		goto out_fbdata;
	}

	dev_dbg(&pdev->dev, "resolution %dx%d, bpp %d\n",
		vmode->xres, vmode->yres, fb_data->default_bpp);

	DBG_MSG("resolution %dx%d,bpp %d\n",vmode->xres, vmode->yres, fb_data->default_bpp);
	/*
	 * GPU alignment restrictions dictate framebuffer parameters:
	 * - 32-byte alignment for buffer width
	 * - 128-byte alignment for buffer height
	 * => 4K buffer alignment for buffer start
	 */
	xres_virt = ALIGN(vmode->xres, 32);
	yres_virt = ALIGN(vmode->yres, 128);
	fb_data->max_pix_size = PAGE_ALIGN(xres_virt * yres_virt);

	/*
	 * Have to check to see if aligned buffer size when rotated
	 * is bigger than when not rotated, and use the max
	 */
	xres_virt_rot = ALIGN(vmode->yres, 32);
	yres_virt_rot = ALIGN(vmode->xres, 128);
	pix_size_rot = PAGE_ALIGN(xres_virt_rot * yres_virt_rot);
	fb_data->max_pix_size = (fb_data->max_pix_size > pix_size_rot) ?
				fb_data->max_pix_size : pix_size_rot;

	buf_size = fb_data->max_pix_size * fb_data->default_bpp/8;

	/* Compute the number of screens needed based on X memory requested */
	if (x_mem_size > 0) {
		GALLEN_DBGLOCAL_RUNLOG(13);
		fb_data->num_screens = DIV_ROUND_UP(x_mem_size, buf_size);
		if (fb_data->num_screens < NUM_SCREENS_MIN) {
			GALLEN_DBGLOCAL_RUNLOG(14);
			fb_data->num_screens = NUM_SCREENS_MIN;
		}
		else if (buf_size * fb_data->num_screens > SZ_16M) {
			GALLEN_DBGLOCAL_RUNLOG(15);
			fb_data->num_screens = SZ_16M / buf_size;
		}
	} else  {
		GALLEN_DBGLOCAL_RUNLOG(16);
		fb_data->num_screens = NUM_SCREENS_MIN;
	}

	fb_data->map_size = buf_size * fb_data->num_screens;
	dev_dbg(&pdev->dev, "memory to allocate: %d\n", fb_data->map_size);

	DBG_MSG("[%s] memory to allocate: %d,num_screens=%d\n",__FUNCTION__, \
		fb_data->map_size,fb_data->num_screens);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL) {
		GALLEN_DBGLOCAL_RUNLOG(17);
		ret = -ENODEV;
		goto out_cmap;
	}

	epdc_base = ioremap(res->start, SZ_4K);
	if (epdc_base == NULL) {
		GALLEN_DBGLOCAL_RUNLOG(18);
		ret = -ENOMEM;
		goto out_cmap;
	}

	/* Allocate FB memory */
	info->screen_base = dma_alloc_writecombine(&pdev->dev,
						  fb_data->map_size<<1,
						  &fb_data->phys_start,
						  GFP_DMA);

	if (info->screen_base == NULL) {
		GALLEN_DBGLOCAL_RUNLOG(19);
		ret = -ENOMEM;
		goto out_mapregs;
	}
	dev_dbg(&pdev->dev, "allocated at %p:0x%x\n", info->screen_base,
		fb_data->phys_start);

	var_info = &info->var;
	var_info->activate = FB_ACTIVATE_TEST;
	var_info->bits_per_pixel = fb_data->default_bpp;
	var_info->xres = vmode->xres;
	var_info->yres = vmode->yres;
	var_info->xres_virtual = xres_virt;
	/* Additional screens allow for panning  and buffer flipping */
	var_info->yres_virtual = yres_virt * fb_data->num_screens;

	var_info->pixclock = vmode->pixclock;
	var_info->left_margin = vmode->left_margin;
	var_info->right_margin = vmode->right_margin;
	var_info->upper_margin = vmode->upper_margin;
	var_info->lower_margin = vmode->lower_margin;
	var_info->hsync_len = vmode->hsync_len;
	var_info->vsync_len = vmode->vsync_len;
	var_info->vmode = FB_VMODE_NONINTERLACED;

	switch (fb_data->default_bpp) {
	case 32:GALLEN_DBGLOCAL_RUNLOG(20);
	case 24:GALLEN_DBGLOCAL_RUNLOG(21);
		var_info->red.offset = 16;
		var_info->red.length = 8;
		var_info->green.offset = 8;
		var_info->green.length = 8;
		var_info->blue.offset = 0;
		var_info->blue.length = 8;
		break;

	case 16:GALLEN_DBGLOCAL_RUNLOG(22);
		var_info->red.offset = 11;
		var_info->red.length = 5;
		var_info->green.offset = 5;
		var_info->green.length = 6;
		var_info->blue.offset = 0;
		var_info->blue.length = 5;
		break;

	case 8:GALLEN_DBGLOCAL_RUNLOG(23);
		/*
		 * For 8-bit grayscale, R, G, and B offset are equal.
		 *
		 */
		var_info->grayscale = GRAYSCALE_8BIT;

		var_info->red.length = 8;
		var_info->red.offset = 0;
		var_info->red.msb_right = 0;
		var_info->green.length = 8;
		var_info->green.offset = 0;
		var_info->green.msb_right = 0;
		var_info->blue.length = 8;
		var_info->blue.offset = 0;
		var_info->blue.msb_right = 0;
		break;

	default:GALLEN_DBGLOCAL_RUNLOG(24);
		dev_err(&pdev->dev, "unsupported bitwidth %d\n",
			fb_data->default_bpp);
		ret = -EINVAL;
		goto out_dma_fb;
	}

	fix_info = &info->fix;

	strcpy(fix_info->id, "mxc_epdc_fb");
	fix_info->type = FB_TYPE_PACKED_PIXELS;
	fix_info->visual = FB_VISUAL_TRUECOLOR;
	fix_info->xpanstep = 0;
	fix_info->ypanstep = 0;
	fix_info->ywrapstep = 0;
	fix_info->accel = FB_ACCEL_NONE;
	fix_info->smem_start = fb_data->phys_start;
	fix_info->smem_len = fb_data->map_size;
	fix_info->ypanstep = 0;

	fb_data->native_width = vmode->xres;
	fb_data->native_height = vmode->yres;

	info->fbops = &mxc_epdc_fb_ops;
	info->var.activate = FB_ACTIVATE_NOW;
	info->pseudo_palette = fb_data->pseudo_palette;
	info->screen_size = info->fix.smem_len;
	info->flags = FBINFO_FLAG_DEFAULT;

	mxc_epdc_fb_set_fix(info);

	fb_data->auto_mode = AUTO_UPDATE_MODE_REGION_MODE;
	fb_data->upd_scheme = UPDATE_SCHEME_QUEUE_AND_MERGE;
	//fb_data->upd_scheme = UPDATE_SCHEME_SNAPSHOT;

	/* Initialize our internal copy of the screeninfo */
	fb_data->epdc_fb_var = *var_info;
	fb_data->fb_offset = 0;
	fb_data->eof_sync_period = 0;

	fb_data->max_num_buffers = EPDC_MAX_NUM_BUFFERS;

	/*
	 * Initialize lists for pending updates,
	 * active update requests, update collisions,
	 * and available update (PxP output) buffers
	 */
	INIT_LIST_HEAD(&fb_data->upd_pending_list);
	INIT_LIST_HEAD(&fb_data->upd_buf_queue);
	INIT_LIST_HEAD(&fb_data->upd_buf_free_list);
	INIT_LIST_HEAD(&fb_data->upd_buf_collision_list);

	/* Allocate update buffers and add them to the list */
	for (i = 0; i < EPDC_MAX_NUM_UPDATES; i++) {
		upd_list = kzalloc(sizeof(*upd_list), GFP_KERNEL);
		if (upd_list == NULL) {
			GALLEN_DBGLOCAL_RUNLOG(25);
			ret = -ENOMEM;
			goto out_upd_lists;
		}

		/* Add newly allocated buffer to free list */
		list_add(&upd_list->list, &fb_data->upd_buf_free_list);
	}

	//fb_data->virt_addr_updbuf =	kzalloc(sizeof(void *) * fb_data->max_num_buffers, GFP_KERNEL);
	fb_data->virt_addr_updbuf =	kzalloc(sizeof(void *) * fb_data->max_num_buffers, GFP_DMA);
	//fb_data->phys_addr_updbuf =	kzalloc(sizeof(dma_addr_t) * fb_data->max_num_buffers,GFP_KERNEL);
	fb_data->phys_addr_updbuf =	kzalloc(sizeof(dma_addr_t) * fb_data->max_num_buffers,GFP_DMA);
	for (i = 0; i < fb_data->max_num_buffers; i++) {
		/*
		 * Allocate memory for PxP output buffer.
		 * Each update buffer is 1 byte per pixel, and can
		 * be as big as the full-screen frame buffer
		 */
		//fb_data->virt_addr_updbuf[i] = kmalloc(fb_data->max_pix_size, GFP_KERNEL);
		fb_data->virt_addr_updbuf[i] = kmalloc(fb_data->max_pix_size, GFP_DMA);
		fb_data->phys_addr_updbuf[i] = virt_to_phys(fb_data->virt_addr_updbuf[i]);
		if (fb_data->virt_addr_updbuf[i] == NULL) {
			ret = -ENOMEM;
			goto out_upd_buffers;
		}

		dev_dbg(fb_data->info.device, "allocated %d bytes @ 0x%08X\n",
			fb_data->max_pix_size, fb_data->phys_addr_updbuf[i]);
		DBG_MSG("allocated %d bytes @ 0x%08X\n",
			fb_data->max_pix_size, fb_data->phys_addr_updbuf[i]);
	}

	/* Counter indicating which update buffer should be used next. */
	fb_data->upd_buffer_num = 0;

	/*
	 * Allocate memory for PxP SW workaround buffer
	 * These buffers are used to hold copy of the update region,
	 * before sending it to PxP for processing.
	 */
	//fb_data->virt_addr_copybuf = kmalloc(fb_data->max_pix_size*2, GFP_KERNEL);
	fb_data->virt_addr_copybuf = kmalloc(fb_data->max_pix_size*2, GFP_DMA);
	fb_data->phys_addr_copybuf = virt_to_phys(fb_data->virt_addr_copybuf);
	if (fb_data->virt_addr_copybuf == NULL) {
		ret = -ENOMEM;
		goto out_upd_buffers;
	}

	fb_data->working_buffer_size = vmode->yres * vmode->xres * 2;
	/* Allocate memory for EPDC working buffer */
	//fb_data->working_buffer_virt = kmalloc(fb_data->working_buffer_size, GFP_KERNEL);
	fb_data->working_buffer_virt = kmalloc(fb_data->working_buffer_size, GFP_DMA);
       fb_data->working_buffer_phys = virt_to_phys(fb_data->working_buffer_virt);
	if (fb_data->working_buffer_virt == NULL) {
		GALLEN_DBGLOCAL_RUNLOG(28);
		dev_err(&pdev->dev, "Can't allocate mem for working buf!\n");
		ret = -ENOMEM;
		goto out_copybuffer;
	}
	DBG_MSG("[%s]:working buffer size=%d\n",__FUNCTION__,fb_data->working_buffer_size);
	
	/* Initialize EPDC pins */
	if (fb_data->pdata->get_pins) {
		GALLEN_DBGLOCAL_RUNLOG(29);
		fb_data->pdata->get_pins();
	}

	fb_data->epdc_clk_axi = clk_get(fb_data->dev, "epdc_axi");
	if (IS_ERR(fb_data->epdc_clk_axi)) {
		GALLEN_DBGLOCAL_RUNLOG(30);
		dev_err(&pdev->dev, "Unable to get EPDC AXI clk."
			"err = 0x%x\n", (int)fb_data->epdc_clk_axi);
		ret = -ENODEV;
		goto out_copybuffer;
	}
	fb_data->epdc_clk_pix = clk_get(fb_data->dev, "epdc_pix");
	if (IS_ERR(fb_data->epdc_clk_pix)) {
		GALLEN_DBGLOCAL_RUNLOG(31);
		dev_err(&pdev->dev, "Unable to get EPDC pix clk."
			"err = 0x%x\n", (int)fb_data->epdc_clk_pix);
		ret = -ENODEV;
		goto out_copybuffer;
	}

	fb_data->in_init = false;

	fb_data->hw_ready = false;

	/*
	 * Set default waveform mode values.
	 * Should be overwritten via ioctl.
	 */
	fb_data->wv_modes.mode_init = 0;
	fb_data->wv_modes.mode_du = 1;
	fb_data->wv_modes.mode_gc4 = 3;
	fb_data->wv_modes.mode_gc8 = 2;
	fb_data->wv_modes.mode_gc16 = 2;
	fb_data->wv_modes.mode_gc32 = 2;

	/*
	 * reagl_flow
	 */
	fb_data->wv_modes.mode_aa = 2; /* default REAGL mode */
	fb_data->wv_modes.mode_aad = 2; /* default REAGL-D mode */
	fb_data->wv_modes.mode_gl16 = 2; /* default GL16 mode */
	fb_data->wv_modes.mode_a2 = 1; /* default A2 mode */


	fb_data->merge_on_waveform_mismatch = 1;

	/* Initialize marker list */
	INIT_LIST_HEAD(&fb_data->full_marker_list);

	/* Initialize all LUTs to inactive */
	for (i = 0; i < EPDC_NUM_LUTS; i++) {
		GALLEN_DBGLOCAL_RUNLOG(32);
		fb_data->lut_update_order[i] = 0;
	}

	INIT_DELAYED_WORK(&fb_data->epdc_done_work, epdc_done_work_func);
	fb_data->epdc_submit_workqueue =
		__create_workqueue(("EPDC Submit"), 1, 0, 1);
	INIT_WORK(&fb_data->epdc_submit_work, epdc_submit_work_func);
	fb_data->epdc_intr_workqueue =
		__create_workqueue(("EPDC Interrupt"), 1, 0, 1);
	INIT_WORK(&fb_data->epdc_intr_work, epdc_intr_work_func);
	INIT_WORK(&fb_data->epdc_firmware_work, epdc_firmware_func);

	/* Retrieve EPDC IRQ num */
	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (res == NULL) {
		GALLEN_DBGLOCAL_RUNLOG(33);
		dev_err(&pdev->dev, "cannot get IRQ resource\n");
		ret = -ENODEV;
		goto out_dma_work_buf;
	}
	fb_data->epdc_irq = res->start;

	/* Register IRQ handler */
	ret = request_irq(fb_data->epdc_irq, mxc_epdc_irq_handler, 0,
			"fb_dma", fb_data);
	if (ret) {
		GALLEN_DBGLOCAL_RUNLOG(34);
		dev_err(&pdev->dev, "request_irq (%d) failed with error %d\n",
			fb_data->epdc_irq, ret);
		ret = -ENODEV;
		goto out_dma_work_buf;
	}

	info->fbdefio = &mxc_epdc_fb_defio;
#ifdef CONFIG_FB_MXC_EINK_AUTO_UPDATE_MODE
	GALLEN_DBGLOCAL_RUNLOG(45);
	fb_deferred_io_init(info);
#endif

#ifdef USE_PMIC
	/* get pmic regulators */
	GALLEN_DBGLOCAL_RUNLOG(46);
	fb_data->display_regulator = regulator_get(NULL, "DISPLAY");
	if (IS_ERR(fb_data->display_regulator)) {
		GALLEN_DBGLOCAL_RUNLOG(35);
		dev_err(&pdev->dev, "Unable to get display PMIC regulator."
			"err = 0x%x\n", (int)fb_data->display_regulator);
		ret = -ENODEV;
		goto out_irq;
	}
	fb_data->vcom_regulator = regulator_get(NULL, "VCOM");
	if (IS_ERR(fb_data->vcom_regulator)) {
		GALLEN_DBGLOCAL_RUNLOG(36);
		regulator_put(fb_data->display_regulator);
		dev_err(&pdev->dev, "Unable to get VCOM regulator."
			"err = 0x%x\n", (int)fb_data->vcom_regulator);
		ret = -ENODEV;
		goto out_irq;
	}

	/* save the nominal vcom value */
	fb_data->wfm = 0; /* initial waveform mode should be INIT */
	vcom_nominal = regulator_get_voltage(fb_data->vcom_regulator); /* save the vcom_nominal value in uV */
	
	
#else
	GALLEN_DBGLOCAL_RUNLOG(47);
    mxc_iomux_v3_setup_pad(MX50_PAD_EIM_CRE__GPIO_1_27);
    gpio_request(GPIO_PWRALL, "epd_power_on");
    gpio_direction_output(GPIO_PWRALL, 1);
#endif
	
	if (device_create_file(info->dev, &fb_attrs[0])) {
		GALLEN_DBGLOCAL_RUNLOG(37);
		dev_err(&pdev->dev, "Unable to create file from fb_attrs\n");
	}

	fb_data->cur_update = NULL;

#ifdef USE_QUEUESPIN_LOCK //[
	spin_lock_init(&fb_data->queue_lock);
#else //][ !USE_QUEUESPIN_LOCK
	mutex_init(&fb_data->queue_mutex);
#endif //] USE_QUEUESPIN_LOCK
	mutex_init(&fb_data->pxp_mutex);

	mutex_init(&fb_data->power_mutex);

	/* PxP DMA interface */
	dmaengine_get();

	/*
	 * Fill out PxP config data structure based on FB info and
	 * processing tasks required
	 */
	pxp_conf = &fb_data->pxp_conf;
	proc_data = &pxp_conf->proc_data;

	/* Initialize non-channel-specific PxP parameters */
	proc_data->drect.left = proc_data->srect.left = 0;
	proc_data->drect.top = proc_data->srect.top = 0;
	proc_data->drect.width = proc_data->srect.width = fb_data->info.var.xres;
	proc_data->drect.height = proc_data->srect.height = fb_data->info.var.yres;
	proc_data->scaling = 0;
	proc_data->hflip = 0;
	proc_data->vflip = 0;
	proc_data->rotate = 0;
	proc_data->bgcolor = 0;
	proc_data->overlay_state = 0;
	proc_data->lut_transform = PXP_LUT_NONE;
	proc_data->lut_map = NULL;

	/*
	 * We initially configure PxP for RGB->YUV conversion,
	 * and only write out Y component of the result.
	 */

	/*
	 * Initialize S0 channel parameters
	 * Parameters should match FB format/width/height
	 */
	pxp_conf->s0_param.pixel_fmt = PXP_PIX_FMT_RGB565;
	pxp_conf->s0_param.width = fb_data->info.var.xres_virtual;
	pxp_conf->s0_param.height = fb_data->info.var.yres;
	pxp_conf->s0_param.color_key = -1;
	pxp_conf->s0_param.color_key_enable = false;

	/*
	 * Initialize OL0 channel parameters
	 * No overlay will be used for PxP operation
	 */
	for (i = 0; i < 8; i++) {
		pxp_conf->ol_param[i].combine_enable = false;
		pxp_conf->ol_param[i].width = 0;
		pxp_conf->ol_param[i].height = 0;
		pxp_conf->ol_param[i].pixel_fmt = PXP_PIX_FMT_RGB565;
		pxp_conf->ol_param[i].color_key_enable = false;
		pxp_conf->ol_param[i].color_key = -1;
		pxp_conf->ol_param[i].global_alpha_enable = false;
		pxp_conf->ol_param[i].global_alpha = 0;
		pxp_conf->ol_param[i].local_alpha_enable = false;
	}

	/*
	 * Initialize Output channel parameters
	 * Output is Y-only greyscale
	 * Output width/height will vary based on update region size
	 */
	pxp_conf->out_param.width = fb_data->info.var.xres;
	pxp_conf->out_param.height = fb_data->info.var.yres;
	pxp_conf->out_param.pixel_fmt = PXP_PIX_FMT_GREY;

	/* Initialize color map for conversion of 8-bit gray pixels */
	fb_data->pxp_conf.proc_data.lut_map = kmalloc(256, GFP_KERNEL);
	if (fb_data->pxp_conf.proc_data.lut_map == NULL) {
		dev_err(&pdev->dev, "Can't allocate mem for lut map!\n");
		ret = -ENOMEM;
		goto out_dmaengine;
	}
	for (i = 0; i < 256; i++)
		fb_data->pxp_conf.proc_data.lut_map[i] = i;

	fb_data->pxp_conf.proc_data.lut_map_updated = true;

	/*
	 * Ensure this is set to NULL here...we will initialize pxp_chan
	 * later in our thread.
	 */
	fb_data->pxp_chan = NULL;

	/* Initialize Scatter-gather list containing 2 buffer addresses. */
	sg = fb_data->sg;
	sg_init_table(sg, 2);

	/*
	 * For use in PxP transfers:
	 * sg[0] holds the FB buffer pointer
	 * sg[1] holds the Output buffer pointer (configured before TX request)
	 */
	sg_dma_address(&sg[0]) = info->fix.smem_start;
	sg_set_page(&sg[0], virt_to_page(info->screen_base),
		    info->fix.smem_len, offset_in_page(info->screen_base));

	fb_data->order_cnt = 0;
	fb_data->waiting_for_wb = false;
	fb_data->waiting_for_lut = false;
	fb_data->waiting_for_lut15 = false;
	fb_data->waiting_for_idle = false;
	fb_data->blank = FB_BLANK_UNBLANK;
	fb_data->power_state = POWER_STATE_OFF;
	fb_data->powering_down = false;
	fb_data->wait_for_powerdown = false;
	fb_data->updates_active = false;
	fb_data->pwrdown_delay = 0;

	
	set_aad_update_counter(0);

	fake_s1d13522_parse_epd_cmdline();

	/* Register FB */
	ret = register_framebuffer(info);
	if (ret) {
		GALLEN_DBGLOCAL_RUNLOG(38);
		dev_err(&pdev->dev,
			"register_framebuffer failed with error %d\n", ret);
		goto out_dmaengine;
	}

	g_fb_data = fb_data;

#ifdef DEFAULT_PANEL_HW_INIT
	GALLEN_DBGLOCAL_RUNLOG(48);
	ret = mxc_epdc_fb_init_hw((struct fb_info *)fb_data);
	if (ret) {
		GALLEN_DBGLOCAL_RUNLOG(39);
		dev_err(&pdev->dev, "Failed to initialize HW!\n");
	}
#endif

#if 0
#ifdef CONFIG_FRAMEBUFFER_CONSOLE
	GALLEN_DBGLOCAL_RUNLOG(49);
	/* If FB console included, update display to show logo */
	update.update_region.left = 0;
	update.update_region.width = info->var.xres;
	update.update_region.top = 0;
	update.update_region.height = info->var.yres;
	update.update_mode = UPDATE_MODE_PARTIAL;
	update.waveform_mode = WAVEFORM_MODE_AUTO;
	update.update_marker = INIT_UPDATE_MARKER;
	update.temp = TEMP_USE_AMBIENT;
	update.flags = 0;

	mxc_epdc_fb_send_update(&update, info);

	ret = mxc_epdc_fb_wait_update_complete(update.update_marker, info);
	if (ret < 0) {
		GALLEN_DBGLOCAL_RUNLOG(40);
		dev_err(fb_data->dev,
			"Wait for update complete failed.  Error = 0x%x", ret);
	}
#endif
#endif

	goto out;

out_dmaengine:
	dmaengine_put();
out_irq:
	free_irq(fb_data->epdc_irq, fb_data);
out_dma_work_buf:
	kfree(fb_data->working_buffer_virt);
	if (fb_data->pdata->put_pins) {
		GALLEN_DBGLOCAL_RUNLOG(41);
		fb_data->pdata->put_pins();
	}
out_copybuffer:
	kfree(fb_data->virt_addr_copybuf);
out_upd_buffers:
	for (i = 0; i < fb_data->max_num_buffers; i++)
		if (fb_data->virt_addr_updbuf[i] != NULL)
			kfree(fb_data->virt_addr_updbuf[i]);
	if (fb_data->virt_addr_updbuf != NULL)
		kfree(fb_data->virt_addr_updbuf);
	if (fb_data->phys_addr_updbuf != NULL)
		kfree(fb_data->phys_addr_updbuf);
out_upd_lists:
	list_for_each_entry_safe(plist, temp_list, &fb_data->upd_buf_free_list,
			list) {
		GALLEN_DBGLOCAL_RUNLOG(42);
		list_del(&plist->list);
		kfree(plist);
	}
out_dma_fb:
	dma_free_writecombine(&pdev->dev, fb_data->map_size<<1, info->screen_base,
			      fb_data->phys_start);

out_mapregs:
	iounmap(epdc_base);
out_cmap:
	fb_dealloc_cmap(&info->cmap);
out_fbdata:
	kfree(fb_data);
out:
	GALLEN_DBGLOCAL_END();
	return ret;
}

static int mxc_epdc_fb_remove(struct platform_device *pdev)
{
	struct update_data_list *plist, *temp_list;
	struct mxc_epdc_fb_data *fb_data = platform_get_drvdata(pdev);
	int i;
	
	GALLEN_DBGLOCAL_BEGIN();
	/* restore the vcom_nominal value */
#ifdef USE_BSP_PMIC //[
	regulator_set_voltage(fb_data->vcom_regulator, vcom_nominal, vcom_nominal);
#else //][!USE_BSP_PMIC
	if(6==gptHWCFG->m_val.bDisplayCtrl) 
	{
		int vcom_mV=vcom_nominal/1000;
		tps65185_vcom_set(vcom_mV,0);
	}
#endif //] USB_BSP_PMIC
	mxc_epdc_fb_blank(FB_BLANK_POWERDOWN, &fb_data->info);

	flush_workqueue(fb_data->epdc_submit_workqueue);
	destroy_workqueue(fb_data->epdc_submit_workqueue);

#ifdef USE_PMIC
	GALLEN_DBGLOCAL_RUNLOG(0);
	regulator_put(fb_data->display_regulator);
	regulator_put(fb_data->vcom_regulator);
#endif

	unregister_framebuffer(&fb_data->info);
	free_irq(fb_data->epdc_irq, fb_data);

	for (i = 0; i < fb_data->max_num_buffers; i++)
		if (fb_data->virt_addr_updbuf[i] != NULL)
			kfree(fb_data->virt_addr_updbuf[i]);
	if (fb_data->virt_addr_updbuf != NULL)
		kfree(fb_data->virt_addr_updbuf);
	if (fb_data->phys_addr_updbuf != NULL)
		kfree(fb_data->phys_addr_updbuf);

	if (fb_data->working_buffer_virt) kfree(fb_data->working_buffer_virt);
	if (fb_data->waveform_buffer_virt) dma_free_coherent(fb_data->dev,
						fb_data->waveform_buffer_size,
						fb_data->waveform_buffer_virt,
						fb_data->waveform_buffer_phys);
	if (fb_data->virt_addr_copybuf != NULL)
		kfree(fb_data->virt_addr_copybuf);

	if (fb_data->temp_range_bounds) kfree(fb_data->temp_range_bounds);
	kfree(fb_data->pxp_conf.proc_data.lut_map);
	if (fb_data->waveform_vcd_buffer) kfree (fb_data->waveform_vcd_buffer);
	if (fb_data->waveform_acd_buffer) kfree (fb_data->waveform_acd_buffer);
	if (fb_data->waveform_xwi_buffer) kfree (fb_data->waveform_xwi_buffer);

	list_for_each_entry_safe(plist, temp_list, &fb_data->upd_buf_free_list,
			list) {
		GALLEN_DBGLOCAL_RUNLOG(2);		
		list_del(&plist->list);
		kfree(plist);
	}
#ifdef CONFIG_FB_MXC_EINK_AUTO_UPDATE_MODE
	GALLEN_DBGLOCAL_RUNLOG(3);
	fb_deferred_io_cleanup(&fb_data->info);
#endif

	dma_free_writecombine(&pdev->dev, fb_data->map_size<<1, fb_data->info.screen_base,
			      fb_data->phys_start);

	if (fb_data->pdata->put_pins) {
		GALLEN_DBGLOCAL_RUNLOG(4);
		fb_data->pdata->put_pins();
	}

	/* Release PxP-related resources */
	if (fb_data->pxp_chan != NULL) {
		GALLEN_DBGLOCAL_RUNLOG(5);
		dma_release_channel(&fb_data->pxp_chan->dma_chan);
	}

	dmaengine_put();

	iounmap(epdc_base);

	fb_dealloc_cmap(&fb_data->info.cmap);

	framebuffer_release((struct fb_info *)fb_data);
	platform_set_drvdata(pdev, NULL);

	GALLEN_DBGLOCAL_END();
	return 0;
}

#ifdef CONFIG_PM
extern void lm75_suspend (void);
extern void lm75_resume (void);

static int mxc_epdc_fb_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct mxc_epdc_fb_data *data = platform_get_drvdata(pdev);
	int ret=0;
	
	GALLEN_DBGLOCAL_BEGIN();
	
	if(0!=gptHWCFG->m_val.bFrontLight) {
		extern int FL_suspend(void);

		if(FL_suspend()<0) {
			printk(KERN_WARNING"%s(%d) : F.L suspend fail !\n",__FILE__,__LINE__);
			ret = -1;
			goto out;
		}
	}
	
	//mxc_epdc_fb_wait_update_complete(g_mxc_upd_data.update_marker++,&g_fb_data->info);
	mxc_epdc_fb_flush_updates(data);
	
#if 0
	ret = mxc_epdc_fb_blank(FB_BLANK_POWERDOWN, &data->info);
	if (ret)
		goto out;
#endif


#ifndef DO_NOT_POWEROFF //[
	if(6==gptHWCFG->m_val.bDisplayCtrl) {
		if(tps65185_suspend()>=0) {
			ret = 0;
		}
		else {
			ret = -1;
		}
	}
	else {
		lm75_suspend ();
	}
#endif //]DO_NOT_POWEROFF
out:
	GALLEN_DBGLOCAL_END();
	return ret;
}

static int mxc_epdc_fb_resume(struct platform_device *pdev)
{
	struct mxc_epdc_fb_data *data = platform_get_drvdata(pdev);
	GALLEN_DBGLOCAL_BEGIN();


#ifndef DO_NOT_POWEROFF	//[
	if(6==gptHWCFG->m_val.bDisplayCtrl) {
		tps65185_resume();
	}
	else {
		lm75_resume();
	}
#endif //]DO_NOT_POWEROFF

	gdwLastUpdateJiffies = 0;


	mxc_epdc_fb_blank(FB_BLANK_UNBLANK, &data->info);
	GALLEN_DBGLOCAL_END();
	return 0;
}
#else
#define mxc_epdc_fb_suspend	NULL
#define mxc_epdc_fb_resume	NULL
#endif

static struct platform_driver mxc_epdc_fb_driver = {
	.probe = mxc_epdc_fb_probe,
	.remove = mxc_epdc_fb_remove,
	.suspend = mxc_epdc_fb_suspend,
	.resume = mxc_epdc_fb_resume,
	.driver = {
		   .name = "mxc_epdc_fb",
		   .owner = THIS_MODULE,
		   },
};

/* Callback function triggered after PxP receives an EOF interrupt */
static void pxp_dma_done(void *arg)
{
	struct pxp_tx_desc *tx_desc = to_tx_desc(arg);
	struct dma_chan *chan = tx_desc->txd.chan;
	struct pxp_channel *pxp_chan = to_pxp_channel(chan);
	struct mxc_epdc_fb_data *fb_data = pxp_chan->client;
	
	DBG_MSG("[%s]\n",__FUNCTION__);

	/* This call will signal wait_for_completion_timeout() in send_buffer_to_pxp */
	complete(&fb_data->pxp_tx_cmpl);
}

/* Function to request PXP DMA channel */
static int pxp_chan_init(struct mxc_epdc_fb_data *fb_data)
{
	dma_cap_mask_t mask;
	struct dma_chan *chan;

	/*
	 * Request a free channel
	 */
	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);
	dma_cap_set(DMA_PRIVATE, mask);
	chan = dma_request_channel(mask, NULL, NULL);
	if (!chan) {
		dev_err(fb_data->dev, "Unsuccessfully received channel!!!!\n");
		return -EBUSY;
	}

	dev_dbg(fb_data->dev, "Successfully received channel.\n");

	fb_data->pxp_chan = to_pxp_channel(chan);

	fb_data->pxp_chan->client = fb_data;

	init_completion(&fb_data->pxp_tx_cmpl);

	return 0;
}

/*
 * Function to call PxP DMA driver and send our latest FB update region
 * through the PxP and out to an intermediate buffer.
 * Note: This is a blocking call, so upon return the PxP tx should be complete.
 */
static int pxp_process_update(struct mxc_epdc_fb_data *fb_data,
			      u32 src_width, u32 src_height,
			      struct mxcfb_rect *update_region)
{
	dma_cookie_t cookie;
	struct scatterlist *sg = fb_data->sg;
	struct dma_chan *dma_chan;
	struct pxp_tx_desc *desc;
	struct dma_async_tx_descriptor *txd;
	struct pxp_config_data *pxp_conf = &fb_data->pxp_conf;
	struct pxp_proc_data *proc_data = &fb_data->pxp_conf.proc_data;
	int i, ret;
	int length;
	
	GALLEN_DBGLOCAL_BEGIN();

	dev_dbg(fb_data->dev, "Starting PxP Send Buffer\n");

	/* First, check to see that we have acquired a PxP Channel object */
	if (fb_data->pxp_chan == NULL) {
		GALLEN_DBGLOCAL_RUNLOG(0);
		/*
		 * PxP Channel has not yet been created and initialized,
		 * so let's go ahead and try
		 */
		ret = pxp_chan_init(fb_data);
		if (ret) {
			/*
			 * PxP channel init failed, and we can't use the
			 * PxP until the PxP DMA driver has loaded, so we abort
			 */
			dev_err(fb_data->dev, "PxP chan init failed\n");
			GALLEN_DBGLOCAL_ESC();
			return -ENODEV;
		}
	}

	/*
	 * Init completion, so that we
	 * can be properly informed of the completion
	 * of the PxP task when it is done.
	 */
	init_completion(&fb_data->pxp_tx_cmpl);

	dev_dbg(fb_data->dev, "sg[0] = 0x%x, sg[1] = 0x%x\n",
		sg_dma_address(&sg[0]), sg_dma_address(&sg[1]));

	dma_chan = &fb_data->pxp_chan->dma_chan;

	txd = dma_chan->device->device_prep_slave_sg(dma_chan, sg, 2,
						     DMA_TO_DEVICE,
						     DMA_PREP_INTERRUPT);
	if (!txd) {
		dev_err(fb_data->info.device,
			"Error preparing a DMA transaction descriptor.\n");
		GALLEN_DBGLOCAL_ESC();
		return -EIO;
	}

	txd->callback_param = txd;
	txd->callback = pxp_dma_done;

	/*
	 * Configure PxP for processing of new update region
	 * The rest of our config params were set up in
	 * probe() and should not need to be changed.
	 */
	pxp_conf->s0_param.width = src_width;
	pxp_conf->s0_param.height = src_height;
	proc_data->srect.top = update_region->top;
	proc_data->srect.left = update_region->left;
	proc_data->srect.width = update_region->width;
	proc_data->srect.height = update_region->height;

	/*
	 * Because only YUV/YCbCr image can be scaled, configure
	 * drect equivalent to srect, as such do not perform scaling.
	 */
	proc_data->drect.top = 0;
	proc_data->drect.left = 0;
	proc_data->drect.width = proc_data->srect.width;
	proc_data->drect.height = proc_data->srect.height;

	/* PXP expects rotation in terms of degrees */
	proc_data->rotate = fb_data->epdc_fb_var.rotate * 90;
	if (proc_data->rotate > 270)
	{
		GALLEN_DBGLOCAL_RUNLOG(1);
		proc_data->rotate = 0;
	}

	pxp_conf->out_param.width = update_region->width;
	pxp_conf->out_param.height = update_region->height;

	desc = to_tx_desc(txd);
	length = desc->len;
	for (i = 0; i < length; i++) {
		GALLEN_DBGLOCAL_RUNLOG(2);
		if (i == 0) {/* S0 */
			GALLEN_DBGLOCAL_RUNLOG(3);
			memcpy(&desc->proc_data, proc_data, sizeof(struct pxp_proc_data));
			pxp_conf->s0_param.paddr = sg_dma_address(&sg[0]);
			memcpy(&desc->layer_param.s0_param, &pxp_conf->s0_param,
				sizeof(struct pxp_layer_param));
		} else if (i == 1) {
			GALLEN_DBGLOCAL_RUNLOG(4);
			pxp_conf->out_param.paddr = sg_dma_address(&sg[1]);
			memcpy(&desc->layer_param.out_param, &pxp_conf->out_param,
				sizeof(struct pxp_layer_param));
		}
		/* TODO: OverLay */

		desc = desc->next;
	}

	/* Submitting our TX starts the PxP processing task */
	GALLEN_DBGLOCAL_PRINTMSG("[%s]tx_submit ==>\n",__FUNCTION__);
	cookie = txd->tx_submit(txd);
	GALLEN_DBGLOCAL_PRINTMSG("[%s]tx_submit <==\n",__FUNCTION__);
	dev_dbg(fb_data->info.device, "%d: Submit %p #%d\n", __LINE__, txd,
		cookie);
	if (cookie < 0) {
		dev_err(fb_data->info.device, "Error sending FB through PxP\n");
		GALLEN_DBGLOCAL_ESC();
		return -EIO;
	}

	fb_data->txd = txd;

	/* trigger ePxP */
	dma_async_issue_pending(dma_chan);
	GALLEN_DBGLOCAL_END();
	return 0;
}

static int pxp_complete_update(struct mxc_epdc_fb_data *fb_data, u32 *hist_stat)
{
	int ret;
	/*
	 * Wait for completion event, which will be set
	 * through our TX callback function.
	 */
	ret = wait_for_completion_timeout(&fb_data->pxp_tx_cmpl, HZ / 10);
	if (ret <= 0) {
		dev_info(fb_data->info.device,
			 "PxP operation failed due to %s\n",
			 ret < 0 ? "user interrupt" : "timeout");
		dma_release_channel(&fb_data->pxp_chan->dma_chan);
		fb_data->pxp_chan = NULL;
		
		{
			u32 reg_val;
			
			dump_epdc_reg();
			dump_pending_list(fb_data);
			
			#if 0 // gallen test .
			/* EPDC_CTRL */
			reg_val = __raw_readl(EPDC_CTRL);
			reg_val |= 0x80000000;
			__raw_writel(reg_val, EPDC_CTRL_SET);		
			mdelay(1);
			reg_val &= ~0x80000000;
			__raw_writel(reg_val, EPDC_CTRL_SET);		
			#endif
		}
		
		return ret ? : -ETIMEDOUT;
	}

	if ((fb_data->pxp_conf.proc_data.lut_transform & EPDC_FLAG_USE_CMAP) &&
		fb_data->pxp_conf.proc_data.lut_map_updated)
		fb_data->pxp_conf.proc_data.lut_map_updated = false;

	*hist_stat = to_tx_desc(fb_data->txd)->hist_status;
	dma_release_channel(&fb_data->pxp_chan->dma_chan);
	fb_data->pxp_chan = NULL;

	dev_dbg(fb_data->dev, "TX completed\n");

	return 0;
}

static int __init mxc_epdc_fb_init(void)
{
	return platform_driver_register(&mxc_epdc_fb_driver);
}

#ifndef FW_IN_RAM
late_initcall(mxc_epdc_fb_init);
#else
module_init(mxc_epdc_fb_init);
#endif

static void __exit mxc_epdc_fb_exit(void)
{
	platform_driver_unregister(&mxc_epdc_fb_driver);
}
module_exit(mxc_epdc_fb_exit);

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_DESCRIPTION("MXC EPDC framebuffer driver");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("fb");
