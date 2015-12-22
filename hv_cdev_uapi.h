
#ifndef _UAPI_LINUX_ADR_H_
#define _UAPI_LINUX_ADR_H_

struct hv_mmls_range {
	uint64_t offset; 	/* offset includes mmap offset */
	uint64_t size; 		/* size of memory to be flushed */
};

/* ADR device size */
#define HV_MMLS_SIZE		_IOR('p', 0x01, unsigned long)
/* ADR flush range */
#define HV_MMLS_FLUSH_RANGE	_IOW('p', 0x02, struct hv_mmls_range)
/* Dump n-bytes of mem */
#define HV_MMLS_DUMP_MEM	_IOW('p', 0x03, struct hv_mmls_range)

#endif
