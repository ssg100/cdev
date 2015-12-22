/*
 *
 *  HVDIMM block driver for BSM/MMLS.
 *
 *  (C) 2015 Netlist, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
*/

#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/spinlock.h>

#include "hv_cmd.h"

#define W_END_CMD

static long use_mmls_cdev = 1;
module_param(use_mmls_cdev, long, 0);

static long bsm_start  = 0x0;
module_param(bsm_start, long, 0);

#ifdef RAMDISK
/* Storage Size in bytes. Put a default size here. */
static long bsm_size   = 0x0;
#else
static long bsm_size   = 0x20000;
#endif // RAMDISK
module_param(bsm_size, long, 0);

static long mmls_start = 0x0;
module_param(mmls_start, long, 0);

#ifdef RAMDISK
static long mmls_size  = 0x0;
#else
static long mmls_size  = 0x20000;
#endif // RAMDISK
module_param(mmls_size, long, 0);

char hv_cmd_buffer[CMD_BUFFER_SIZE];
static unsigned char fake_data_buf[65536] = {1, 2, 3, 4, 5, 6, 7, 8};

static void __iomem *bsm_iomem;
static long bsm_mmio_size;
static void __iomem *mmls_iomem;
static long mmls_mmio_size;

static int cmd_status[16];

static struct hv_tm_data_t {
	struct hrtimer timer;
	unsigned short tag;
} mt_data;

static struct hv_data_t {
	unsigned short tag;
	unsigned int sector;
	unsigned long buffer_cb;
	void *cb_func;
	unsigned long timer_cb_func;
} mq_data[QUEUE_DEPTH];

static char cmd_in_q = 0x0;
static spinlock_t cmd_in_q_lock;
static unsigned long cmdq_flags;

static struct HV_BSM_STATUS_t *pBSMstatus;
static struct HV_MMLS_STATUS_t *pMMLSstatus;
static char hv_bsm_status_buf[STATUS_BUFFER_SIZE];
static char hv_mmls_status_buf[STATUS_BUFFER_SIZE];
static char hv_general_status_buf[STATUS_BUFFER_SIZE];

/* local copy of hv command */
static char hv_cmd_local_buffer[CMD_BUFFER_SIZE];

/* use this buffers temporarily until the addresses is defined by HW engr */
static char hv_status_buffer[STATUS_BUFFER_SIZE] = {0x41};

/*for HRtimer callback functions */
static enum hrtimer_restart bsm_r_callback(struct hrtimer *my_timer);
static enum hrtimer_restart bsm_w_callback(struct hrtimer *my_timer);
static enum hrtimer_restart mmls_r_callback(struct hrtimer *my_timer);
static enum hrtimer_restart mmls_w_callback(struct hrtimer *my_timer);

static void clear_cmd_status(void)
{
	struct HV_INQUIRY_STATUS_t	*pStatus;
#ifndef RAMDISK
	unsigned char clear_byte = 0;
#endif

	pStatus = (struct HV_INQUIRY_STATUS_t *)hv_status_buffer;
	/* do not clear, pending HW */
	/* pStatus->cmd_status = 0x0; */
#ifndef RAMDISK
	/* clear cmd status in MMIO */
	memcpy_toio(bsm_iomem+HV_STATUS_OFFSET, &clear_byte, 1);
#endif

}

static int wait_for_cmd_status(int *status, int cnt)
{
	int j;
	struct HV_INQUIRY_STATUS_t	*pStatus;

	pStatus = (struct HV_INQUIRY_STATUS_t *)hv_status_buffer;
	while (1) {
		for (j = 0; j < cnt; j++) {
			if ((pStatus->cmd_status & status[j]) == status[j]) {
				/* printf("completion status is 0x%x\n",
				status[j]); */
				return pStatus->cmd_status;
			}
		}
		/* wait... */
	}

	return 0;
}

static int wait_for_bsm_cmd_status(int *status, int cnt)
{
	int j;

	pBSMstatus = (struct HV_BSM_STATUS_t *)hv_bsm_status_buf;
	while (1) {
		for (j = 0; j < cnt; j++) {
			memcpy_fromio(pBSMstatus, bsm_iomem +
				BSM_WRITE_STATUS_OFFSET, STATUS_BUFFER_SIZE);
			if (((pBSMstatus->cmd_status)&status[j]) == status[j]) {
				pr_debug("waiting status is 0x%x\n",
					pBSMstatus->cmd_status);
				return pBSMstatus->cmd_status;
			}
		}
		/* wait... */
	}

	return 0;
}


static int wait_for_mmls_cmd_status(int *status, int cnt)
{
	int j;

	pMMLSstatus = (struct HV_MMLS_STATUS_t *)hv_mmls_status_buf;
	while (1) {
		for (j = 0; j < cnt; j++) {
			memcpy_fromio(pMMLSstatus, mmls_iomem +
				MMLS_WRITE_STATUS_OFFSET, 64);
			if (((pMMLSstatus->cmd_status) & status[j]) == status[j]
				) {
				pr_debug("waiting status is 0x%x\n",
					pMMLSstatus->cmd_status);
				return pMMLSstatus->cmd_status;
			}
		}
		/* wait... */
	}

	return 0;
}

int bsm_read_command(unsigned int tag,
					unsigned int sector,
					unsigned int lba,
					unsigned char *buf,
					unsigned char async,
					void *callback_func)
{
	struct HV_CMD_BSM_t	*pBsm;
	ktime_t period;
	unsigned char query_status;
	unsigned char gen_cmd_status;
	unsigned char current_cnt;
	unsigned char next_cnt;
	unsigned char prog_status;


	pBsm = (struct HV_CMD_BSM_t *) hv_cmd_local_buffer;
	pBsm->cmd = BSM_READ;
	*(unsigned short *)&pBsm->tag = tag;
	*(unsigned int *)&pBsm->sector = sector;
	*(unsigned int *)&pBsm->lba = lba+bsm_start/512;
	*(unsigned char *)&pBsm->more_data = 0x0;

	/* spin_lock_irqsave(&cmd_in_q_lock,cmdq_flags); */
	pr_debug("bsm_read_command starting... tag=%d\n", tag);
	pBSMstatus = (struct HV_BSM_STATUS_t *)hv_general_status_buf;

	/* check if Error bit is set */
	memcpy_fromio(&gen_cmd_status, bsm_iomem +
				GENERAL_STATUS_OFFSET, sizeof(unsigned char));

	if ((gen_cmd_status & DEVICE_ERROR) == DEVICE_ERROR) {
		pr_err("Device Error during BSM-READ request!!!\n");
		return(-1);
	}

	/* send bsm-read command */
	memcpy_toio(bsm_iomem+BSM_READ_CMD_OFFSET, hv_cmd_local_buffer,
			CMD_BUFFER_SIZE);

	if (async == 0) {		/* synchronous mode */

		/* Assume 4k block data from block driver */
		/* wait for bsm read ready */
		udelay(5);	/* delay 5 u sec */

		/* fetch the current sync counter */
		memcpy_fromio(&query_status, bsm_iomem +
				QUERY_STATUS_OFFSET, sizeof(unsigned char));
		current_cnt = query_status & CMD_SYNC_COUNTER;
		pr_debug("current_cnt=%i\n", current_cnt);

		bsm_query_command(tag);
		/*??????????????????***************************************/
		current_cnt--;	/****** REMOVE this line in real testing!!!!! */
		/*??????????????????***************************************/

		memcpy_fromio(&query_status, bsm_iomem +
				QUERY_STATUS_OFFSET, sizeof(unsigned char));
		next_cnt = query_status & CMD_SYNC_COUNTER;
		prog_status = query_status & PROGRESS_STATUS;
		pr_debug(" next_cnt=%i, prog_status=0x%x\n", next_cnt,
				prog_status);
		if ((next_cnt == (current_cnt+1)) && (prog_status == CMD_DONE)) {
			pr_debug("query status meets!!!\n");
		} else {
			pr_err("something wrong! sync counter doesn't work!\n");
			return (-1);
		}

#if 0		/* don't need this check? */
		/* make sure ready for read */
		cmd_status[0] = BSM_READ_READY;	/*0x20*/
		wait_for_bsm_cmd_status(cmd_status, 1);
#endif
		/* copy data from HW */
		memcpy_fromio(buf, bsm_iomem + BSM_READ_DATA_OFFSET,
				sector*HV_BLOCK_SIZE);
#ifdef W_END_CMD
		/* send termination command */
		*(unsigned char *)&pBsm->cmd = BSM_READ;
		*(unsigned char *)&pBsm->more_data = 0xEE;
		memcpy_toio(bsm_iomem+BSM_READ_CMD_OFFSET, hv_cmd_local_buffer,
			CMD_BUFFER_SIZE);
#endif
	} else if (async == 1) {

		spin_lock_irqsave(&cmd_in_q_lock, cmdq_flags);
		pr_debug("***async B-R mode operation:\n");
		pr_debug("tag=%d, callback_func=0x%lx, buf=0x%lx\n",
			tag, (unsigned long)callback_func, (unsigned long)buf);

		/* save command data into queue array */
		mq_data[tag].tag = tag;
		mq_data[tag].sector = sector;
		mq_data[tag].buffer_cb = (unsigned long)buf;
		mq_data[tag].timer_cb_func = (unsigned long)bsm_r_callback;
		mq_data[tag].cb_func = callback_func;


		if (cmd_in_q == 0) {

			/* arrange timer for call back */
			period = ktime_set(0, 5000);		/* 5 usec */
			hrtimer_init(&mt_data.timer, CLOCK_MONOTONIC,
				HRTIMER_MODE_REL);
			mt_data.tag = tag;
			mt_data.timer.function = bsm_r_callback;

			hrtimer_start(&mt_data.timer, period, HRTIMER_MODE_REL);
		}

		cmd_in_q++;
		pr_debug("before B-R callback: cmd_in_q=%d, tag=%d\n",
			cmd_in_q, tag);

		spin_unlock_irqrestore(&cmd_in_q_lock, cmdq_flags);
	} else {
		pr_err("asynchrounous mode was assigned with wrong value\n");
		return (-1);
	}

	return 1;
}

static enum hrtimer_restart bsm_r_callback(struct hrtimer *my_timer)
{
	struct HV_CMD_BSM_t	*pBsm;
	int i;
	ktime_t period, curr_time;

	struct hv_tm_data_t *my_tm_data;
	unsigned char *buf;
	unsigned char query_status;
	unsigned char current_cnt;
	unsigned char next_cnt;
	unsigned char prog_status;
	unsigned short next_tag, curr_tag;
	char br_cmd_local_buf[CMD_BUFFER_SIZE];

	void (*my_cb_func)(int);

	pr_debug("## %s, in timer call back\n", __func__);

	my_tm_data = container_of(my_timer, struct hv_tm_data_t, timer);

	curr_tag = my_tm_data->tag;
	buf = (char *)mq_data[curr_tag].buffer_cb;

	pr_debug("B-R callback: curr_tag=%d, buf=0x%lx\n", curr_tag, (long)buf);


	/* fetch the current sync counter */
	memcpy_fromio(&query_status, bsm_iomem +
				QUERY_STATUS_OFFSET, sizeof(unsigned char));
	current_cnt = query_status & CMD_SYNC_COUNTER;

	/* query for command progress */
	bsm_query_command(curr_tag);

		/*??????????????????***************************************/
		current_cnt--;	/****** REMOVE this line in real testing!!! */
		/*??????????????????***************************************/

	memcpy_fromio(&query_status, bsm_iomem + QUERY_STATUS_OFFSET,
		sizeof(unsigned char));
	next_cnt = query_status & CMD_SYNC_COUNTER;
	prog_status = query_status & PROGRESS_STATUS;
	pr_debug(" next_cnt=%i, prog_status=%i\n", next_cnt, prog_status);
	if ((next_cnt == (current_cnt+1)) && (prog_status == CMD_DONE)) {
		pr_debug("hrtimer query status meets!!!\n");
	} else {
		pr_err("something wrong! sync counter doesn't work!\n");
		return (-1);
	}
#if 0
	/* make sure ready for read */
	cmd_status[0] = BSM_READ_READY;	/*0x20*/
	wait_for_bsm_cmd_status(cmd_status, 1);
#endif
	/* copy data from HW */
	memcpy_fromio(buf, bsm_iomem + BSM_READ_DATA_OFFSET,
			(mq_data[curr_tag].sector)*HV_BLOCK_SIZE);

	/* prepare and send termination command */
	memset(br_cmd_local_buf, 0, CMD_BUFFER_SIZE);

	pBsm = (struct HV_CMD_BSM_t *) br_cmd_local_buf;
	pBsm->cmd = BSM_READ;
	*(unsigned short *)&pBsm->tag = mq_data[curr_tag].tag;
	*(unsigned int *)&pBsm->sector = mq_data[curr_tag].sector;
	*(unsigned int *)&pBsm->lba = 0xFF;
	*(unsigned char *)&pBsm->more_data = 0xEE;
#ifdef W_END_CMD
	memcpy_toio(bsm_iomem+BSM_READ_CMD_OFFSET, br_cmd_local_buf,
			CMD_BUFFER_SIZE);
#endif

	/* move to the driver callback function*/
	for (i = 0; i < 2; i++)
		pr_debug("read data: buf:%d=0x%x\n", i, buf[i]);

	my_cb_func = mq_data[curr_tag].cb_func;
	pr_debug("my_cb_func=0x%lx\n", (unsigned long)my_cb_func);
	(*my_cb_func)(curr_tag);

	/* check if any remaining cmd in queue */
	cmd_in_q--;

	pr_debug("remaining Q for schedule: cmd_in_q=%d\n", cmd_in_q);

	curr_time = ktime_get();
	if (cmd_in_q == 0) {
		pr_debug("=> no more cmd left in Q!\n");

		return HRTIMER_NORESTART;
	} else {
		/* get the tag for next cmd in queue */
		next_tag = (curr_tag + 1) % QUEUE_DEPTH;
		pr_debug("in timer setup: cmd_in_q=%d, next_tag=%d\n",
			cmd_in_q, next_tag);

		/* schedule the next timer for callback*/
		period = ktime_set(0, 5000);		/* 5 usec */
		my_tm_data->tag = next_tag;
		my_timer->function = (void *)mq_data[next_tag].timer_cb_func;
		hrtimer_forward(my_timer, curr_time, period);

		return HRTIMER_RESTART;
	}
}


int bsm_write_command(unsigned int tag,
					unsigned int sector,
					unsigned int lba,
					unsigned char *buf,
					unsigned char async,
					void *callback_func)
{
	ktime_t period;
	unsigned char gen_cmd_status;
	unsigned char query_status;
	unsigned char current_cnt;
	unsigned char next_cnt;
	unsigned char prog_status;

	struct HV_CMD_BSM_t	*pBsm;

	pr_debug("bsm_write_command starting... tag=%d\n", tag);

	/* check if Error bit is set */
	memcpy_fromio(&gen_cmd_status, bsm_iomem +
				GENERAL_STATUS_OFFSET, sizeof(unsigned char));
	pr_debug("gen_cmd_status=0x%x\n", gen_cmd_status);

	if ((gen_cmd_status & DEVICE_ERROR) == DEVICE_ERROR) {
		pr_err("Device Error during BSM-WRITE request!!!\n");
		return(-1);
	}

	/* arrange command structure */
	pBsm = (struct HV_CMD_BSM_t *) hv_cmd_local_buffer;
	pBsm->cmd = BSM_WRITE;
	*(unsigned short *)&pBsm->tag = tag;
	*(unsigned int *)&pBsm->sector = sector;
	*(unsigned int *)&pBsm->lba = lba+bsm_start/512;
	*(unsigned char *)&pBsm->more_data = 0x0;

	/* check if BSM-write ready */
	cmd_status[0] = BSM_WRITE_READY;	/* 0x10 */
	wait_for_bsm_cmd_status(cmd_status, 1);

	/* send command to MMIO when BSM-WRITE ready*/
	memcpy_toio(bsm_iomem+BSM_WRITE_CMD_OFFSET, hv_cmd_local_buffer,
		CMD_BUFFER_SIZE);

	/* send data to MMIO buffer */
	memcpy_toio(bsm_iomem+BSM_WRITE_DATA_OFFSET, buf, sector*HV_BLOCK_SIZE);

#ifdef W_END_CMD
	/* send a termination command */
	*(unsigned char *)&pBsm->more_data = 0xEE;
	memcpy_toio(bsm_iomem+BSM_WRITE_CMD_OFFSET, hv_cmd_local_buffer,
		CMD_BUFFER_SIZE);
#endif

	if (async == 0) {		/* synchronous mode */
		/* wait for completion */
		ndelay(5000);	/* delay 5 usec */

		/* fetch the current sync counter */
		memcpy_fromio(&query_status, bsm_iomem +
				QUERY_STATUS_OFFSET, sizeof(unsigned char));
		current_cnt = query_status & CMD_SYNC_COUNTER;

		/* query for command progress */
		bsm_query_command(tag);

		/*??????????????????***************************************/
		current_cnt--;	/****** REMOVE this line in real testing!!! */
		/*??????????????????***************************************/

		memcpy_fromio(&query_status, bsm_iomem + QUERY_STATUS_OFFSET,
				sizeof(unsigned char));
		next_cnt = query_status & CMD_SYNC_COUNTER;
		prog_status = query_status & PROGRESS_STATUS;
		pr_debug(" next_cnt=%i, prog_sts=%i\n", next_cnt, prog_status);
		if ((next_cnt == (current_cnt+1)) &&
				(prog_status == CMD_DONE)) {
			pr_debug("query status meets!!!\n");
			pr_debug("sync bsm-write cmd is done!\n");
		} else {
			pr_err("something wrong! sync counter doesn't match!\n");
			return (-1);
		}

	} else if (async == 1) {
		spin_lock_irqsave(&cmd_in_q_lock, cmdq_flags);
		pr_debug("***async B-W mode operation:\n");
		pr_debug("tag=%d, callback_func=0x%lx, buf=0x%lx\n",
			tag, (unsigned long)callback_func, (unsigned long)buf);

		/* save command data into queue array */
		mq_data[tag].tag = tag;
		mq_data[tag].sector = sector;
		mq_data[tag].buffer_cb = (unsigned long)buf;
		mq_data[tag].timer_cb_func = (unsigned long)bsm_w_callback;
		mq_data[tag].cb_func = callback_func;

		if (cmd_in_q == 0) {
			/* arrange timer for call back */
			period = ktime_set(0, 5000);		/* 5 usec */
			hrtimer_init(&mt_data.timer, CLOCK_MONOTONIC,
				HRTIMER_MODE_REL);
			mt_data.tag = tag;
			mt_data.timer.function = bsm_w_callback;

			hrtimer_start(&mt_data.timer, period, HRTIMER_MODE_REL);
		}

		cmd_in_q++;
		pr_debug("before B-W callback: cmd_in_q=%d\n", cmd_in_q);

		spin_unlock_irqrestore(&cmd_in_q_lock, cmdq_flags);
	} else {
		pr_err("asynchrounous mode was assigned with wrong value\n");
		return (-1);		/* prepare HRtimer for callback later */
	}

	return 1;
}

static enum hrtimer_restart bsm_w_callback(struct hrtimer *my_timer)
{
	struct hv_tm_data_t *my_tm_data;
	unsigned char *buf;
	unsigned char query_status;
	unsigned char current_cnt;
	unsigned char next_cnt;
	unsigned char prog_status;
	unsigned short next_tag, curr_tag;
	ktime_t period, curr_time;

	void (*my_cb_func)(int);

	pr_debug("## %s, in timer call back\n", __func__);

	my_tm_data = container_of(my_timer, struct hv_tm_data_t, timer);

	curr_tag = my_tm_data->tag;
	buf = (void *)mq_data[curr_tag].buffer_cb;

	pr_debug("B_W callback: curr_tag=%d, buf=0x%lx\n", curr_tag, (long)buf);

	/* fetch the current sync counter */
	memcpy_fromio(&query_status, bsm_iomem +
				QUERY_STATUS_OFFSET, sizeof(unsigned char));
	current_cnt = query_status & CMD_SYNC_COUNTER;
	pr_debug("B-W: current_cnt before query: %i\n", current_cnt);

	/* query for command progress */
	bsm_query_command(curr_tag);

		/*??????????????????***************************************/
		current_cnt--;	/****** REMOVE this line in real testing!!!! */
		/*??????????????????***************************************/

	memcpy_fromio(&query_status, bsm_iomem + QUERY_STATUS_OFFSET,
		sizeof(unsigned char));
	next_cnt = query_status & CMD_SYNC_COUNTER;
	prog_status = query_status & PROGRESS_STATUS;
	pr_debug(" next_cnt=%i, prog_status=%i\n", next_cnt, prog_status);
	if ((next_cnt == (current_cnt+1)) && (prog_status == CMD_DONE)) {
		pr_debug("B_W: hrtimer query status meets!!!\n");
	} else {
		pr_err("something wrong! sync counter doesn't work!\n");
		return (-1);
	}

	/* move to the driver callback function*/
	my_cb_func = mq_data[curr_tag].cb_func;
	/* pr_debug("my_cb_func=0x%lx\n", my_cb_func); */
	(*my_cb_func)(curr_tag);

	/* check if any remaining cmd in queue */
	cmd_in_q--;

	pr_debug("remaining Q for schedule: cmd_in_q=%d\n", cmd_in_q);

	curr_time = ktime_get();

	if (cmd_in_q == 0) {
		pr_debug("=> no more cmd left in Q!\n");

		return HRTIMER_NORESTART;
	} else {
		/* get the tag for next cmd in queue */
		next_tag = (curr_tag + 1) % QUEUE_DEPTH;
		pr_debug("in timer setup: cmd_in_q=%d, next_tag=%d\n",
			cmd_in_q, next_tag);

		/* schedule the next timer for callback*/
		my_tm_data->tag = next_tag;
		my_timer->function = (void *)mq_data[next_tag].timer_cb_func;
		period = ktime_set(0, 5000);		/* 5 usec */
		hrtimer_forward(my_timer, curr_time, period);

		return HRTIMER_RESTART;
	}
}

int mmls_read_command(unsigned int tag,
				unsigned int sector,
				unsigned int lba,
				unsigned long mm_addr,
				unsigned char async,
				void *callback_func)
{
	ktime_t period;
	unsigned char query_status;
	unsigned char gen_cmd_status;
	unsigned char current_cnt;
	unsigned char next_cnt;
	unsigned char prog_status;
	void *p_temp;
	struct HV_CMD_MMLS_t *pMMLS;

	pr_debug("Received mmls_read command...  tag=%d", tag);
	pr_debug("tag=%d sector=0x%x lba= 0x%x mm_addr=0x%lx\n",
	tag, sector, lba, mm_addr);

	/* prepare command structure */
	pMMLS = (struct HV_CMD_MMLS_t *) hv_cmd_local_buffer;

	pMMLS->cmd = MMLS_READ;
	*(unsigned short *)&pMMLS->tag = tag;
	*(unsigned int *)&pMMLS->sector = sector;
	*(unsigned int *)&pMMLS->lba = lba+mmls_start/512;
	*(unsigned char *)&pMMLS->more_data = 0x0;

	/* prepare physical address for mmls read command */
	p_temp = (void *)virt_to_phys((volatile void *)mm_addr);
	*(unsigned long *)&pMMLS->mm_addr = (unsigned long)p_temp;
	pr_debug("mmls read: virt_to_phys: p_temp=0x%lx\n",
		(unsigned long)p_temp);

	/* check if Error bit is set */
	memcpy_fromio(&gen_cmd_status, mmls_iomem +
				GENERAL_STATUS_OFFSET, sizeof(unsigned char));
	pr_debug("gen_cmd_status=0x%x\n", gen_cmd_status);
	if ((gen_cmd_status & DEVICE_ERROR) == DEVICE_ERROR) {
		pr_err("Device Error during MMLS-READ request!!!\n");
		return(-1);
	}

	/* send mmls read command */
	/* pr_debug("***mmls_iomem: 0x%lx", mmls_iomem); */
	pr_debug("mmls command = 0x%lx\n",
		(unsigned long)(mmls_iomem+MMLS_READ_CMD_OFFSET));
	memcpy_toio(mmls_iomem+MMLS_READ_CMD_OFFSET, hv_cmd_local_buffer,
	CMD_BUFFER_SIZE);

	if (async == 0) {		/* synchronous mode */

		/* Assume 4k block data from block driver */
		/* wait for mmls read ready */
		udelay(5);	/* delay 5 u sec */

		/* fetch the current sync counter */
		memcpy_fromio(&query_status, mmls_iomem +
				QUERY_STATUS_OFFSET, sizeof(unsigned char));
		current_cnt = query_status & CMD_SYNC_COUNTER;
		pr_debug("mmls-read: current_cnt=%i\n", current_cnt);

		mmls_query_command(tag);
			/*??????????????????**********************************/
			current_cnt--;	/* REMOVE this line in real testing! */
			/*??????????????????**********************************/

		memcpy_fromio(&query_status, mmls_iomem + QUERY_STATUS_OFFSET,
				sizeof(unsigned char));
		next_cnt = query_status & CMD_SYNC_COUNTER;
		prog_status = query_status & PROGRESS_STATUS;
		pr_debug(" next_cnt=%i, prog_status=0x%x\n", next_cnt,
			prog_status);
		if ((next_cnt == (current_cnt+1)) &&
				(prog_status == CMD_DONE)) {
			pr_debug("mmls-read: query status meets!!!\n");
		} else {
			pr_err("something wrong! sync counter doesn't work!\n");
		return (-1);
		}

		/* make sure mmls-read ready for data move */
		cmd_status[0] = MMLS_READ_READY;	/*0x20*/
		wait_for_mmls_cmd_status(cmd_status, 1);

		/* apply fake write */
		memcpy((unsigned char *)mm_addr, fake_data_buf,
			sector*HV_BLOCK_SIZE);

#ifdef W_END_CMD
		/* send a termination command */
		pMMLS->cmd = MMLS_READ;
		*(unsigned char *)&pMMLS->more_data = 0xEE;
		memcpy_toio(mmls_iomem+MMLS_READ_CMD_OFFSET,
			hv_cmd_local_buffer, CMD_BUFFER_SIZE);
#endif
	}

	else if (async == 1) {		/* async mode */
		spin_lock_irqsave(&cmd_in_q_lock, cmdq_flags);
		pr_debug("***async M-R mode operation:\n");
		pr_debug("tag=%d, callback_func=0x%lx, mm_addr=0x%lx\n",
		tag, (unsigned long)callback_func, (unsigned long)mm_addr);

		/* save command data into queue array */
		mq_data[tag].tag = tag;
		mq_data[tag].sector = sector;
		mq_data[tag].buffer_cb = (unsigned long)mm_addr;
		mq_data[tag].timer_cb_func = (unsigned long)mmls_r_callback;
		mq_data[tag].cb_func = callback_func;

		if (cmd_in_q == 0) {

			/* arrange timer for call back */
			period = ktime_set(0, 5000);		/* 5 usec */
			hrtimer_init(&mt_data.timer, CLOCK_MONOTONIC,
				HRTIMER_MODE_REL);
			mt_data.tag = tag;
			mt_data.timer.function = mmls_r_callback;

			hrtimer_start(&mt_data.timer, period, HRTIMER_MODE_REL);
		}

		cmd_in_q++;

		pr_debug("before M-R callback: cmd_in_q=%d, tag=%d\n",
			cmd_in_q, tag);
		spin_unlock_irqrestore(&cmd_in_q_lock, cmdq_flags);
	} else {
		pr_err("MMLS-READ async mode was assigned with wrong value\n");
		return (-1);
	}

	return 1;
}

static enum hrtimer_restart mmls_r_callback(struct hrtimer *my_timer)
{
	struct HV_CMD_MMLS_t	*pMMLS;
	int i;
	ktime_t period, curr_time;
	struct hv_tm_data_t *my_tm_data;
	unsigned char *buf;
	unsigned char query_status;
	unsigned char current_cnt;
	unsigned char next_cnt;
	unsigned char prog_status;
	unsigned short next_tag, curr_tag;
	char mr_cmd_local_buf[CMD_BUFFER_SIZE];
	void *p_temp;

	void (*my_cb_func)(int);

	pr_debug("## %s, in mmls-read timer call back\n", __func__);

	my_tm_data = container_of(my_timer, struct hv_tm_data_t, timer);

	curr_tag = my_tm_data->tag;
	buf = (void *)mq_data[curr_tag].buffer_cb;
	pr_debug("M-R callback: curr_tag=%d, buf=0x%lx\n", curr_tag,
		(unsigned long)buf);

	/* fetch the current sync counter */
	memcpy_fromio(&query_status, mmls_iomem +
				QUERY_STATUS_OFFSET, sizeof(unsigned char));
	current_cnt = query_status & CMD_SYNC_COUNTER;

	/* query for command progress */
	mmls_query_command(curr_tag);

		/*??????????????????***************************************/
		current_cnt--;	/***** REMOVE this line in real testing!!!!! */
		/*??????????????????***************************************/

	memcpy_fromio(&query_status, mmls_iomem + QUERY_STATUS_OFFSET,
			sizeof(unsigned char));
	next_cnt = query_status & CMD_SYNC_COUNTER;
	prog_status = query_status & PROGRESS_STATUS;
	pr_debug(" next_cnt=%i, prog_status=%i\n",
			next_cnt, prog_status);
	if ((next_cnt == (current_cnt+1)) && (prog_status == CMD_DONE)) {
		pr_debug("hrtimer mmls_read query status meets!!!\n");
	} else {
		pr_err("something wrong! sync counter doesn't work!\n");
		return (-1);
	}

	/* make sure mmls-read ready for data move */
	cmd_status[0] = MMLS_READ_READY;	/*0x20*/
	wait_for_mmls_cmd_status(cmd_status, 1);

	/* apply fake write */
	memcpy(buf, fake_data_buf, (mq_data[curr_tag].sector)*HV_BLOCK_SIZE);

	/* prepare virtual address for mmls read command */
	p_temp = (void *)virt_to_phys((volatile void *)buf);


	/* prepare and send termination command */
	memset(mr_cmd_local_buf, 0, CMD_BUFFER_SIZE);
#ifdef W_END_CMD
	pMMLS = (struct HV_CMD_MMLS_t *) mr_cmd_local_buf;
	pMMLS->cmd = MMLS_READ;
	*(unsigned short *)&pMMLS->tag = curr_tag;
	*(unsigned int *)&pMMLS->sector = mq_data[curr_tag].sector;
	*(unsigned int *)&pMMLS->lba = 0xFF;
	*(unsigned char *)&pMMLS->more_data = 0xEE;
	*(unsigned long *)&pMMLS->mm_addr = (unsigned long)p_temp;

	memcpy_toio(mmls_iomem+MMLS_READ_CMD_OFFSET, mr_cmd_local_buf,
			CMD_BUFFER_SIZE);
#endif

	/* call the driver callback function*/
	for (i = 0; i < 2; i++)
		pr_debug("mmls read data:buf:%d=0x%x\n", i, buf[i]);

	my_cb_func = mq_data[curr_tag].cb_func;
	pr_debug("my_cb_func=0x%lx\n", (unsigned long)my_cb_func);
	(*my_cb_func)(curr_tag);

	/* check if any remaining cmd in queue */
	cmd_in_q--;
	pr_debug("M-R remaining Q for schedule: cmd_in_q=%d\n", cmd_in_q);

	curr_time = ktime_get();
	if (cmd_in_q == 0) {
		pr_debug("=> no more cmd left in Q!\n");
		return HRTIMER_NORESTART;
	} else {
		/* get the tag for next command in queue */
		next_tag = ((curr_tag) + 1) % QUEUE_DEPTH;
		pr_debug("in M-R timer setup: cmd_in_q=%d, next_tag=%d\n",
			cmd_in_q, next_tag);
		/* schedule the next timer for callback*/
		period = ktime_set(0, 5000);		/* 5 usec */
		my_tm_data->tag = next_tag;
		my_timer->function = (void *)mq_data[next_tag].timer_cb_func;
		hrtimer_forward(my_timer, curr_time, period);
		return HRTIMER_RESTART;
	}
}

int mmls_write_command(unsigned int tag,
				unsigned int sector,
				unsigned int lba,
				unsigned long mm_addr,
				unsigned char async,
				void *callback_func)
{
	ktime_t period;
	unsigned char gen_cmd_status;
	void *p_temp;
	unsigned char query_status;
	unsigned char current_cnt;
	unsigned char next_cnt;
	unsigned char prog_status;
	struct HV_CMD_MMLS_t *pMMLS;

	pr_debug("Received mmls_write command:");
	pr_debug("tag=%d sector=0x%x lba=0x%x mm_addr=0x%lx\n",
		tag, sector, lba, mm_addr);

	/* check if Error bit is set */
	memcpy_fromio(&gen_cmd_status, mmls_iomem +
				GENERAL_STATUS_OFFSET, sizeof(unsigned char));
	pr_debug("gen_cmd_status=0x%x\n", gen_cmd_status);
	if ((gen_cmd_status & DEVICE_ERROR) == DEVICE_ERROR) {
		pr_err("Device Error during MMLS-WRITE request!!!\n");
		return(-1);
	}

	/* arrange command structure */
	pMMLS = (struct HV_CMD_MMLS_t *) hv_cmd_local_buffer;
	pMMLS->cmd = MMLS_WRITE;
	*(unsigned short *)&pMMLS->tag = tag;
	*(unsigned int *)&pMMLS->sector = sector;
	*(unsigned int *)&pMMLS->lba = lba+mmls_start/512;
	*(unsigned char *)&pMMLS->more_data = 0x0;

	/* locate physical address for mmls-write command */
	p_temp = (void *)virt_to_phys((volatile void *)mm_addr);
	*(unsigned long *)&pMMLS->mm_addr = (unsigned long)p_temp;
	pr_debug("mmls write: virt_to_phys: p_temp=0x%lx\n",
		(unsigned long)p_temp);

	/* check if MMLS-write ready */
	cmd_status[0] = MMLS_WRITE_READY;	/* 0x10 */
	wait_for_mmls_cmd_status(cmd_status, 1);


	/* send mmls-write command */
	memcpy_toio(mmls_iomem+MMLS_WRITE_CMD_OFFSET, hv_cmd_local_buffer,
		CMD_BUFFER_SIZE);

	/* apply fake read */
	pr_debug("fake_data_buf=0x%lx mm_addr=0x%lx\n",
			(long)fake_data_buf, mm_addr);
	memcpy(fake_data_buf, (unsigned char *)mm_addr, sector*HV_BLOCK_SIZE);
#ifdef W_END_CMD
	/* send a termination command */
	*(unsigned char *)&pMMLS->more_data = 0xEE;
	memcpy_toio(mmls_iomem+MMLS_WRITE_CMD_OFFSET, hv_cmd_local_buffer,
		CMD_BUFFER_SIZE);
#endif
	if (async == 0) {		/* synchronous mode */
		/* wait for completion */
		ndelay(5000);	/* delay 5 usec */

		/* fetch the current sync counter */
		memcpy_fromio(&query_status, mmls_iomem +
				QUERY_STATUS_OFFSET, sizeof(unsigned char));
		current_cnt = query_status & CMD_SYNC_COUNTER;

		/* query for command progress */
		mmls_query_command(tag);

		/*??????????????????***************************************/
		current_cnt--;	/***** REMOVE this line in real testing!!!!! */
		/*??????????????????***************************************/

		memcpy_fromio(&query_status, mmls_iomem + QUERY_STATUS_OFFSET,
			sizeof(unsigned char));
		next_cnt = query_status & CMD_SYNC_COUNTER;
		prog_status = query_status & PROGRESS_STATUS;
		pr_debug(" next_cnt=%i, prog_status=%i\n",
			next_cnt, prog_status);
		if ((next_cnt == (current_cnt+1)) &&
				(prog_status == CMD_DONE)) {
			pr_debug("query status meets!!!\n");
			pr_debug("sync mmls-write cmd is done!\n");
		} else {
			pr_err("something wrong! sync counter doesn't work!\n");
			return (-1);
		}

	} else if (async == 1) {
		spin_lock_irqsave(&cmd_in_q_lock, cmdq_flags);
		pr_debug("***async M-W mode operation:\n");
		pr_debug("tag=%d, callback_func=0x%lx, mm_addr=0x%lx\n",
		tag, (unsigned long)callback_func, (unsigned long)mm_addr);

		/* save command data into queue array */
		mq_data[tag].tag = tag;
		mq_data[tag].sector = sector;
		mq_data[tag].buffer_cb = (unsigned long)mm_addr;
		mq_data[tag].timer_cb_func = (unsigned long)mmls_w_callback;
		mq_data[tag].cb_func = callback_func;

		if (cmd_in_q == 0) {

			/* arrange timer for call back */
			period = ktime_set(0, 5000);		/* 5 usec */
			/* period = ktime_set(0, 12000); */
			hrtimer_init(&mt_data.timer, CLOCK_MONOTONIC,
				HRTIMER_MODE_REL);
			mt_data.tag = tag;
			mt_data.timer.function = mmls_w_callback;

			hrtimer_start(&mt_data.timer, period, HRTIMER_MODE_REL);
		}

		cmd_in_q++;
		pr_debug("before M-W callback: cmd_in_q=%d\n", cmd_in_q);
		spin_unlock_irqrestore(&cmd_in_q_lock, cmdq_flags);

	} else {
		pr_err("asynchrounous mode was assigned with wrong value\n");
		return (-1);		/* prepare HRtimer for callback later */
	}

	return 1;
}

static enum hrtimer_restart mmls_w_callback(struct hrtimer *my_timer)
{
	int i;
	ktime_t period, curr_time;
	struct hv_tm_data_t *my_tm_data;
	unsigned char *buf;
	unsigned char query_status;
	unsigned char current_cnt;
	unsigned char next_cnt;
	unsigned char prog_status;
	unsigned short next_tag, curr_tag;

	void (*my_cb_func)(int);

	pr_debug("## %s, in timer call back\n", __func__);

	my_tm_data = container_of(my_timer, struct hv_tm_data_t, timer);

	curr_tag = my_tm_data->tag;
	buf = (unsigned char *)mq_data[curr_tag].buffer_cb;

	pr_debug("M_W callback: curr_tag=%d, buf=0x%lx\n", curr_tag, (long)buf);
	for (i = 0; i < 4; i++)
		pr_debug("BW: buf_%d = 0x%x\n", i, buf[i]);

	/* fetch the current sync counter */
	memcpy_fromio(&query_status, mmls_iomem +
				QUERY_STATUS_OFFSET, sizeof(unsigned char));
	current_cnt = query_status & CMD_SYNC_COUNTER;
	pr_debug("M-W: current_cnt before query: %i\n", current_cnt);

	/* query for command progress */
	mmls_query_command(curr_tag);

		/*??????????????????***************************************/
		current_cnt--;	/***** REMOVE this line in real testing!!!!! */
		/*??????????????????***************************************/

	memcpy_fromio(&query_status, mmls_iomem +
			QUERY_STATUS_OFFSET, sizeof(unsigned char));
	pr_debug("async M-W: query_status=0x%x\n", query_status);
	next_cnt = query_status & CMD_SYNC_COUNTER;
	prog_status = query_status & PROGRESS_STATUS;
	pr_debug("next_cnt=%i, prog_status=%i\n",
			next_cnt, prog_status);
	if ((next_cnt == (current_cnt+1)) && (prog_status == CMD_DONE)) {
		pr_debug("M_W: hrtimer query status meets!!!\n");
	} else {
		pr_err("something wrong! sync counter doesn't work!\n");
		return (-1);
	}

	/* move to the driver callback function */
	my_cb_func = mq_data[curr_tag].cb_func;
	/* pr_debug("my_cb_func=0x%lx\n", my_cb_func); */
	(*my_cb_func)(curr_tag);

	/* check if any remaining cmd in queue */
	cmd_in_q--;
	pr_debug("remaining Q for schedule: cmd_in_q=%d\n", cmd_in_q);
	curr_time = ktime_get();

	if (cmd_in_q == 0) {
		pr_debug("=> no more cmd left in Q!\n");

		return HRTIMER_NORESTART;
	} else {
		/* get the tag for next command in queue */
		next_tag = (curr_tag + 1) % QUEUE_DEPTH;
		pr_debug("in timer setup: cmd_in_q=%d, next_tag=%d\n",
			cmd_in_q, next_tag);

		/* schedule the next timer for callback*/
		my_tm_data->tag = next_tag;
		my_timer->function = (void *)mq_data[next_tag].timer_cb_func;
		period = ktime_set(0, 5000);		/* 5 usec */
		/* period = ktime_set(0, 12000); */
		hrtimer_forward(my_timer, curr_time, period);
		return HRTIMER_RESTART;
	}
}

int reset_command(unsigned int tag)
{
	struct HV_CMD_RESET_t *pReset;

	pr_debug("Received RESET command tag=%d\n", tag);

	pReset = (struct HV_CMD_RESET_t *)hv_cmd_local_buffer;
	pReset->cmd = RESET;
	*(short *)&pReset->tag = tag;

	/* clear command status */
	clear_cmd_status();

	/* trigger the command */
	pr_debug("trigger the command\n");
	memcpy_toio(bsm_iomem+BSM_WRITE_CMD_OFFSET, hv_cmd_local_buffer,
		CMD_BUFFER_SIZE);
	/* memcpy(hv_cmd_buffer, hv_cmd_local_buffer, CMD_BUFFER_SIZE); */

	/* wait for completion */
	pr_debug("wait for completion\n");
	cmd_status[0] = 0x41;
	wait_for_cmd_status(cmd_status, 1);

	return 1;
}

int bsm_query_command(unsigned int tag)
{
	struct HV_CMD_QUERY_t *pQuery;

	pr_notice("Received BSM QUERY command tag=%d\n", tag);

	/* clear command buffer */
	/* memset(hv_cmd_local_buffer, 0, CMD_BUFFER_SIZE); */

	/* prepare query command */
	pQuery = (struct HV_CMD_QUERY_t *)hv_cmd_local_buffer;
	pQuery->cmd = QUERY;
	*(short *)&pQuery->tag = tag;

	/* transmit the command */
	memcpy_toio(bsm_iomem+QUERY_CMD_OFFSET, hv_cmd_local_buffer,
		CMD_BUFFER_SIZE);
	pr_debug("query cmd: 0x%x, 0x%x\n", hv_cmd_local_buffer[0],
		hv_cmd_local_buffer[1]);

	pr_notice("bsm query command is done...\n");
	return 1;
}

int mmls_query_command(unsigned int tag)
{
	struct HV_CMD_QUERY_t *pQuery;

	pr_notice("Received MMLS QUERY command tag=%d\n", tag);

	pQuery = (struct HV_CMD_QUERY_t *)hv_cmd_local_buffer;
	pQuery->cmd = QUERY;
	*(short *)&pQuery->tag = tag;

	/* transmit the command */
	memcpy_toio(mmls_iomem+MMLS_WRITE_CMD_OFFSET, hv_cmd_local_buffer,
		CMD_BUFFER_SIZE);

	pr_notice("mmls query command is done...\n");
	return 1;
}

int ecc_train_command(void)
{
	unsigned short i, j;
	unsigned long ecc_data[ECC_REPEAT_NUM];

	pr_notice("Received ecc train command!\n");

	/* prepare and sending training sequence */
	for (i = 0; i < ECC_CMDS_NUM; i++) {

		for (j = 0; j < ECC_REPEAT_NUM; j++)
			ecc_data[j] = (unsigned long)(i << ECC_ADR_SHFT);

		/* transmit the command */
		memcpy_toio(bsm_iomem + ECC_CMD_OFFSET, ecc_data,
			CMD_BUFFER_SIZE);
	}

	pr_notice("eee train command is done...\n");
	return 1;
}


int inquiry_command(unsigned int tag)
{
	struct HV_CMD_INQUIRY_t	*pInquiry;

	pr_debug("Received Inquiry Command tag=%d\n", tag);

	pInquiry = (struct HV_CMD_INQUIRY_t *)hv_cmd_local_buffer;
	pInquiry->cmd = INQUIRY;
	*(short *)&pInquiry->tag = tag;

	/* clear command status */
	clear_cmd_status();

	/* trigger the command */
	pr_debug("trigger the command\n");
	memcpy_toio(bsm_iomem+BSM_WRITE_CMD_OFFSET, hv_cmd_local_buffer,
		CMD_BUFFER_SIZE);
	/* memcpy(hv_cmd_buffer, hv_cmd_local_buffer, CMD_BUFFER_SIZE); */

	/* wait for completion */
	pr_debug("wait for completion\n");
	cmd_status[0] = 0x41;
	wait_for_cmd_status(cmd_status, 1);

	return 1;
}

int config_command(unsigned int tag,
					unsigned int sz_emmc,
					unsigned int sz_rdimm,
					unsigned int sz_mmls,
					unsigned int sz_bsm,
					unsigned int sz_nvdimm,
					unsigned int to_emmc,
					unsigned int to_rdimm,
					unsigned int to_mmls,
					unsigned int to_bsm,
					unsigned int to_nvdimm)
{
	struct HV_CMD_CONFIG_t *pConfig;

	pr_debug("Received Config command\n");

	pConfig = (struct HV_CMD_CONFIG_t *) hv_cmd_local_buffer;
	pConfig->cmd = CONFIG;
	*(short *)&pConfig->tag = tag;
	*(int *)&pConfig->size_of_emmc = sz_emmc;
	*(int *)&pConfig->size_of_rdimm = sz_rdimm;
	*(int *)&pConfig->size_of_mmls = sz_mmls;
	*(int *)&pConfig->size_of_bsm = sz_bsm;
	*(int *)&pConfig->size_of_nvdimm = sz_nvdimm;
	*(int *)&pConfig->timeout_emmc = to_emmc;
	*(int *)&pConfig->timeout_rdimm = to_rdimm;
	*(int *)&pConfig->timeout_mmls = to_mmls;
	*(int *)&pConfig->timeout_bsm = to_bsm;
	*(int *)&pConfig->timeout_nvdimm = to_nvdimm;

	/* clear command status */
	clear_cmd_status();

	/* trigger the command */
	pr_debug("trigger the command\n");
	memcpy_toio(bsm_iomem+BSM_WRITE_CMD_OFFSET, hv_cmd_local_buffer,
		CMD_BUFFER_SIZE);
	/* memcpy(hv_cmd_buffer, hv_cmd_local_buffer, CMD_BUFFER_SIZE); */

	/* wait for completion */
	pr_debug("wait for completion\n");
	cmd_status[0] = 0x41;
	wait_for_cmd_status(cmd_status, 1);

	return 1;
}



int page_swap_command(unsigned int tag,
					   unsigned int o_sector,
					   unsigned int i_sector,
					   unsigned int o_lba,
					   unsigned int i_lba,
					   unsigned int o_mm_addr,
					   unsigned int i_mm_addr)
{
	struct HV_CMD_SWAP_t *pSwap;

	pr_debug("Received PAGE SWAP command\n");

	pSwap = (struct HV_CMD_SWAP_t *) hv_cmd_local_buffer;
	pSwap->cmd = PAGE_SWAP;
	*(unsigned short *)&pSwap->tag = tag;
	*(unsigned int *)&pSwap->page_out_sector = o_sector;
	*(unsigned int *)&pSwap->page_in_sector = i_sector;
	*(unsigned int *)&pSwap->page_out_lba = o_lba;
	*(unsigned int *)&pSwap->page_in_lba = i_lba;
	*(unsigned int *)&pSwap->mm_addr_out = o_mm_addr;
	*(unsigned int *)&pSwap->mm_addr_in = i_mm_addr;

	/* clear command status */
	clear_cmd_status();

	/* trigger the command */
	pr_debug("trigger the command\n");
	memcpy(hv_cmd_buffer, hv_cmd_local_buffer, CMD_BUFFER_SIZE);

	/* wait for completion */
	pr_debug("wait for completion\n");
	cmd_status[0] = 0x41;
	wait_for_cmd_status(cmd_status, 1);

	return 1;
}


int bsm_qread_command(unsigned int tag,
					  unsigned int sector,
					  unsigned int lba,
					  unsigned char *buf)
{
	struct HV_CMD_BSM_t	*pBsm;

	pr_debug("Received BSM QREAD command");
	pr_debug(" tag=%d sector=0x%x lba=0x%x buf=0x%lx\n",
		tag, sector, lba, (unsigned long)buf);

	pBsm = (struct HV_CMD_BSM_t *) hv_cmd_local_buffer;
	pBsm->cmd = BSM_QREAD;
	*(unsigned short *)&pBsm->tag = tag;
	*(unsigned int *)&pBsm->sector = sector;
	*(unsigned int *)&pBsm->lba = lba;

	/* clear command status */
	clear_cmd_status();

	/* trigger the command */
	pr_debug("trigger the command\n");
	memcpy(hv_cmd_buffer, hv_cmd_local_buffer, CMD_BUFFER_SIZE);

	/* wait for completion */
	pr_debug("wait for completion\n");
	cmd_status[0] = 0x41;
	wait_for_cmd_status(cmd_status, 1);

	return 1;
}

int bsm_qwrite_command(unsigned int tag,
					   unsigned int sector,
					   unsigned int lba,
					   unsigned char *buf)
{
	struct HV_CMD_BSM_t	*pBsm;

	pr_debug("Received BSM QWRITE command");
	pr_debug("tag=%d sector=0x%x lba=0x%x buf=0x%lx\n",
		tag, sector, lba, (unsigned long)buf);

	pBsm = (struct HV_CMD_BSM_t *) hv_cmd_local_buffer;
	pBsm->cmd = BSM_QWRITE;
	*(unsigned short *)&pBsm->tag = tag;
	*(unsigned int *)&pBsm->sector = sector;
	*(unsigned int *)&pBsm->lba = lba;

	/* clear command status */
	clear_cmd_status();

	/* trigger the command */
	pr_debug("trigger the command\n");
	memcpy(hv_cmd_buffer, hv_cmd_local_buffer, CMD_BUFFER_SIZE);

	/* wait for completion */
	pr_debug("wait for completion\n");
	cmd_status[0] = 0x41;
	wait_for_cmd_status(cmd_status, 1);

	return 1;
}

int bsm_backup_command(unsigned int tag,
					   unsigned int sector,
					   unsigned int lba)
{
	struct HV_CMD_BSM_t	*pBsm;

	pr_debug("Received BSM BACKUP Command");
	pr_debug(" tag=%d sector=0x%x lba=0x%x\n", tag, sector, lba);

	pBsm = (struct HV_CMD_BSM_t *) hv_cmd_local_buffer;
	pBsm->cmd = BSM_BACKUP;
	*(unsigned short *)&pBsm->tag = tag;
	*(unsigned int *)&pBsm->sector = sector;
	*(unsigned int *)&pBsm->lba = lba;

	/* clear command status */
	clear_cmd_status();

	/* trigger the command */
	pr_debug("trigger the command\n");
	memcpy(hv_cmd_buffer, hv_cmd_local_buffer, CMD_BUFFER_SIZE);

	/* wait for completion */
	pr_debug("wait for completion\n");
	cmd_status[0] = 0x41;
	wait_for_cmd_status(cmd_status, 1);

	return 1;
}

int bsm_restore_command(unsigned int tag,
					    unsigned int sector,
					    unsigned int lba)
{
	struct HV_CMD_BSM_t	*pBsm;

	pr_debug("Received BSM RESTORE command");
	pr_debug(" tag=%d sector=0x%x lba=0x%x\n", tag, sector, lba);

	pBsm = (struct HV_CMD_BSM_t *) hv_cmd_local_buffer;
	pBsm->cmd = BSM_RESTORE;
	*(unsigned short *)&pBsm->tag = tag;
	*(unsigned int *)&pBsm->sector = sector;
	*(unsigned int *)&pBsm->lba = lba;

	/* clear command status */
	clear_cmd_status();

	/* trigger the command */
	pr_debug("trigger the command\n");
	memcpy(hv_cmd_buffer, hv_cmd_local_buffer, CMD_BUFFER_SIZE);

	/* wait for completion */
	pr_debug("wait for completion\n");
	cmd_status[0] = 0x41;
	wait_for_cmd_status(cmd_status, 1);

	return 1;
}

int bsm_io_init(void) /* memory region */
{
	pr_warn("hv: entered bsm_init\n");

	/*
	 * Sanity check on BSM start/size
	 */
	if (bsm_start == 0 || bsm_size == 0) {
		pr_warn("hv: bsm_start/bsm_size is zero\n");
		return -EINVAL;
	}

	/*
	 * Request BSM IO space
	 */
/* #ifdef RAMDISK */
	bsm_mmio_size = bsm_size;
/* #else */
/*	bsm_mmio_size = 0x20000; */	/* temp 128k */
/* #endif */
	release_mem_region(bsm_start, bsm_mmio_size);
	if (!request_mem_region(bsm_start, bsm_mmio_size, "bsm")) {
		pr_warn("hv: unable to request BSM IO space starting 0x%lx\n",
			(unsigned long)bsm_start);
		return -ENOSPC;
	}
	bsm_iomem = ioremap_wc(bsm_start, bsm_mmio_size);
	pr_notice("hv: successfully requested BSM IO space 0x%lx 0x%lx\n",
		(unsigned long)bsm_iomem, (unsigned long)bsm_mmio_size);

	return 0;
}

int mmls_io_init(void)
{
	pr_warn("hv: entered mmls_init\n");

	/*
	 * Sanity check on MMLS start/size
	 */

	if (mmls_start == 0 || mmls_size == 0) {
		pr_warn("hv: mmls_start/mmls_size is zero\n");
		return -EINVAL;
	}

	/*
	 * Request MMLS IO space
	 */
	mmls_mmio_size = mmls_size;
	release_mem_region(mmls_start, mmls_mmio_size);
	if (!request_mem_region(mmls_start, mmls_mmio_size, "mmls")) {
		pr_warn("hv: unable to request MMLS IO space starting 0x%lx\n",
			(unsigned long)mmls_start);
		return -ENOSPC;
	}
	mmls_iomem = ioremap_wc(mmls_start, mmls_mmio_size);
	pr_notice
		("hv: successfully requested MMLS IO space VA: 0x%lx size: 0x%lx\n",
		(unsigned long)mmls_iomem, (unsigned long)mmls_mmio_size);
	pr_notice("hv: physical address: 0x%lX\n", mmls_start);

	return 0;
}

int single_cmd_io_init(void) /* memory region */
{

	pr_warn("before: bsm_start=0x%lx, bsm_size=%d\n",
		(unsigned long)bsm_start, (int)bsm_size);
	pr_warn("before: mmls_start=0x%lx, mmls_size=%d\n",
		(unsigned long)mmls_start, (int)mmls_size);

	bsm_io_init();
	mmls_io_init();

	pr_warn("bsm_iomem=0x%lx, bsm_size=%d\n", (unsigned long)bsm_iomem,
			(int) bsm_size);
	pr_warn("mmls_iomem=0x%lx, mmls_size=%d\n", (unsigned long)mmls_iomem,
			(int)mmls_size);

	return 0;
}

void bsm_iomem_release(void)
{
	release_mem_region(bsm_start, bsm_mmio_size);
}

void mmls_iomem_release(void)
{
	release_mem_region(mmls_start, mmls_mmio_size);
}

void get_bsm_iodata(struct HV_BSM_IO_t *p_bio_data)
{
	p_bio_data->b_size = bsm_size;
	p_bio_data->b_iomem = bsm_iomem;
}

void get_mmls_iodata(struct HV_MMLS_IO_t *p_mio_data)
{
	p_mio_data->m_size = mmls_size;
	p_mio_data->m_iomem = mmls_iomem;
	p_mio_data->phys_start = (phys_addr_t) mmls_start;
}

int get_use_mmls_cdev(void)
{
	pr_err("%s: ", __func__);
	return use_mmls_cdev;
}
EXPORT_SYMBOL(get_use_mmls_cdev);

void spin_for_cmd_init(void)
{
	spin_lock_init(&cmd_in_q_lock);
}
