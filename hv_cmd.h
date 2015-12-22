/*
 *
 *  HVDIMM header file for BSM/MMLS.
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

#define RAMDISK

#define DATA_BUFFER_SIZE		(64*1024)
#define	CMD_BUFFER_SIZE			64
#define	STATUS_BUFFER_SIZE		64

#define	HV_DATA_OFFSET			0x0
#define HV_CMD_OFFSET			(HV_DATA_OFFSET+DATA_BUFFER_SIZE)
#define HV_STATUS_OFFSET		(HV_CMD_OFFSET+CMD_BUFFER_SIZE)
#define HV_BLOCK_SIZE			512
#define FS_BLOCK_SIZE			4096
#define QUEUE_DEPTH			16

/* data commands */

#define MMLS_READ	0x10
#define MMLS_WRITE	0x20
#define PAGE_SWAP	0x50
#define BSM_BACKUP	0x60
#define BSM_RESTORE	0x61
#define BSM_WRITE	0x40
#define BSM_READ	0x30
#define BSM_QWRITE	0x41
#define	BSM_QREAD	0x31
#define QUERY		0x70

/* control commands */

#define NOP		0x00
#define RESET		0xE0
#define CONFIG		0x90
#define INQUIRY		0x91
#define TRIM		0x80
#define FW_UPDATE	0xF0
#define ABORT		0xE1

/* general status definitions */
#define BSM_WRITE_READY		0x10
#define MMLS_WRITE_READY	0x10
#define BSM_READ_READY		0x20
#define MMLS_READ_READY		0x20
#define	DEVICE_ERROR		0x40

/* query status definitions */

#define	CMD_SYNC_COUNTER	0x03
#define	PROGRESS_STATUS		0x0C	/* 00: no cmd*/
					/* 01: cmd in FIFO */
					/* 10: cmd being processed */
					/* 11: cmd done */
#define CMD_BEING_PROCESSED	0x08
#define CMD_DONE		0x0C
#define DEVICE_SUCCESS		0x40

/* data commands address offset */
#define BSM_WRITE_CMD_OFFSET		0x00000000		// 0x0000
#define BSM_READ_CMD_OFFSET		BSM_WRITE_CMD_OFFSET	// 0x0000
#define QUERY_CMD_OFFSET		BSM_WRITE_CMD_OFFSET	// 0x0000
#define MMLS_WRITE_CMD_OFFSET		0x00000080		// 0x0080
#define MMLS_READ_CMD_OFFSET		MMLS_WRITE_CMD_OFFSET	// 0x0080

#define BSM_WRITE_DATA_OFFSET		0x00008000		// 0x8000
#define BSM_READ_DATA_OFFSET		0x0000C000		// 0xc000

#define BSM_WRITE_STATUS_OFFSET		0x00004000
#define GENERAL_STATUS_OFFSET		BSM_WRITE_STATUS_OFFSET	// 0x4000
#define BSM_READ_STATUS_OFFSET		GENERAL_STATUS_OFFSET	// 0x4000
#define MMLS_WRITE_STATUS_OFFSET	GENERAL_STATUS_OFFSET	// 0x4000
#define MMLS_READ_STATUS_OFFSET		GENERAL_STATUS_OFFSET	// 0x4040

#define QUERY_STATUS_OFFSET		0x00004040	
					// was original:BSM_READ_STATUS_OFFSET
#define ECC_CMD_OFFSET			0x00010000

/* ECC training constants */
#define ECC_ADR_SHFT			0
#define ECC_CMDS_NUM			128
#define ECC_REPEAT_NUM			8

struct HV_CMD_MMLS_t {
	unsigned char cmd;
	unsigned char tag[2];
	unsigned char reserve1[5];
	unsigned char sector[4];
	unsigned char reserve2[4];
	unsigned char lba[4];
	unsigned char reserve3[4];
	unsigned char mm_addr[8];
	unsigned char more_data;	/* byte 32 */
	unsigned char reserve4[31];
};

struct HV_CMD_SWAP_t {
	unsigned char cmd;
	unsigned char tag[2];
	unsigned char reserve1[5];
	unsigned char page_out_sector[4];
	unsigned char page_in_sector[4];
	unsigned char page_out_lba[4];
	unsigned char page_in_lba[4];
	unsigned char mm_addr_out[4];
	unsigned char mm_addr_in[4];
	unsigned char reserve2[32];
};

struct HV_CMD_BSM_t {
	unsigned char cmd;
	unsigned char tag[2];
	unsigned char reserve1[5];
	unsigned char sector[4];
	unsigned char reserve2[4];
	unsigned char lba[4];		/* byte 16 */
	unsigned char reserve3[12];
	unsigned char more_data;	/* byte 32 */
	unsigned char reserve4[31];
};

struct HV_BSM_STATUS_t {
	unsigned char cmd_status;
	unsigned char error_code;
	unsigned char tag[2];
	unsigned char reserve1[12];
	unsigned char remaining_sector_size[4];
	unsigned char remaining_size_swap[4];
	unsigned char next_lba[4];
	unsigned char next_lba_swap[4];
	unsigned char reserve2[32];
};

struct HV_MMLS_STATUS_t {
	unsigned char cmd_status;
	unsigned char error_code;
	unsigned char tag[2];
	unsigned char reserve1[4];
	unsigned char current_counter;
	unsigned char reserve2[7];
	unsigned char remaining_sector_size[4];
	unsigned char remaining_size_swap[4];
	unsigned char next_lba[4];
	unsigned char next_lba_swap[4];
	unsigned char reserve3[32];
};

struct HV_CMD_RESET_t {
	unsigned char cmd;
	unsigned char tag[2];
	unsigned char reserve1[61];
};

struct HV_CMD_QUERY_t {
	unsigned char cmd;
	unsigned char tag[2];
	unsigned char reserve1[61];
};

struct HV_CMD_ABORT_t {
	unsigned char cmd;
	unsigned char tag[2];
	unsigned char tag_id[2];
	unsigned char reserve1[59];
};

struct HV_CMD_FW_UPDATE_t {
	unsigned char cmd;
	unsigned char tag[2];
	unsigned char tag_id[2];
	unsigned char reserve1[59];
};

struct HV_CMD_TRIM_t {
	unsigned char cmd;
	unsigned char tag[2];
	unsigned char reserve1[5];
	unsigned char sector[4];
	unsigned char reserve2[4];
	unsigned char lba[4];
	unsigned char reserve3[44];
};

struct HV_CMD_CONFIG_t {
	unsigned char cmd;
	unsigned char tag[2];
	unsigned char reserve1;
	unsigned char size_of_emmc[4];
	unsigned char size_of_rdimm[4];
	unsigned char size_of_mmls[4];
	unsigned char size_of_bsm[4];
	unsigned char size_of_nvdimm[4];
	unsigned char timeout_emmc[4];
	unsigned char timeout_rdimm[4];
	unsigned char timeout_mmls[4];
	unsigned char timeout_bsm[4];
	unsigned char timeout_nvdimm[4];
	unsigned char reserve2[20];
};

struct HV_CMD_INQUIRY_t {
	unsigned char cmd;
	unsigned char tag[2];
	unsigned char reserve1[61];
};


struct HV_INQUIRY_STATUS_t {
	unsigned char cmd_status;
	unsigned char tag[2];
	unsigned char reserve1;
	unsigned char size_of_emmc[4];
	unsigned char size_of_rdimm[4];
	unsigned char size_of_mmls[4];
	unsigned char size_of_bsm[4];
	unsigned char size_of_nvdimm[4];
	unsigned char timeout_emmc[4];
	unsigned char timeout_rdimm[4];
	unsigned char timeout_mmls[4];
	unsigned char timeout_bsm[4];
	unsigned char timeout_nvdimm[4];
	unsigned char reserve2[20];
};

int reset_command(unsigned int tag);
int bsm_query_command(unsigned int tag);
int mmls_query_command(unsigned int tag);
int ecc_train_command(void);
int inquiry_command(unsigned int tag);
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
					unsigned int to_nvdimm);
int mmls_read_command(unsigned int tag,
				unsigned int sector,
				unsigned int lba,
				unsigned long mm_addr,
				unsigned char async,
				void *callback_func);
int mmls_write_command(unsigned int tag,
				unsigned int sector,
				unsigned int lba,
				unsigned long mm_addr,
				unsigned char async,
				void *callback_func);
int page_swap_command(unsigned int tag,
					   unsigned int o_sector,
					   unsigned int i_sector,
					   unsigned int o_lba,
					   unsigned int i_lba,
					   unsigned int o_mm_addr,
					   unsigned int i_mm_addr);
int bsm_read_command(unsigned int tag,
					unsigned int sector,
					unsigned int lba,
					unsigned char *buf,
					unsigned char async,
					void *call_back);
int bsm_write_command(unsigned int tag,
					unsigned int sector,
					unsigned int lba,
					unsigned char *buf,
					unsigned char async,
					void *call_back);
int bsm_qread_command(unsigned int tag,
					   unsigned int sector,
					   unsigned int lba,
					   unsigned char *buf);
int bsm_qwrite_command(unsigned int tag,
					    unsigned int sector,
					    unsigned int lba,
						unsigned char *buf);
int bsm_backup_command(unsigned int tag,
					    unsigned int sector,
					    unsigned int lba);
int bsm_restore_command(unsigned int tag,
					     unsigned int sector,
					     unsigned int lba);

struct HV_BSM_IO_t {
	long b_size;
#ifdef RAMDISK
	void *b_iomem;
#else
	void __iomem *b_iomem;
#endif
};

struct HV_MMLS_IO_t {
	long m_size;
	void __iomem *m_iomem;
	phys_addr_t phys_start;	/* phys addr of mmls start */
};

void get_bsm_iodata(struct HV_BSM_IO_t *p_bio_data);
void get_mmls_iodata(struct HV_MMLS_IO_t *p_mio_data);

int single_cmd_init(void);
int single_cmd_exit(void);
int bsm_io_init(void);
int mmls_io_init(void);
int single_cmd_io_init(void);
void bsm_iomem_release(void);
void mmls_iomem_release(void);
void spin_for_cmd_init(void);
