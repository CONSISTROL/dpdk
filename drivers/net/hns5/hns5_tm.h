#ifndef _HNS5_TM_H_
#define _HNS5_TM_H_

#include "native.h"

#define TM_PORT_NUM (48)    /* 48 physical ports */
#define TM_TRUNK_NUM (64)   /* 64 trunk ports */
#define TM_VI_NUM (80)      /* 80 VIs */
#define TM_GLOC_NUM (128)   /* 128 logical port aggregations */
#define TM_LOC_NUM (512)    /* 512 logical ports */
#define TM_QUEUE_NUM (1024) /* 1024 queues */

#define QUEUE_NUM_PER_PORT (8)
#define TM_CFG_FINISH 1
#define TM_PF_ID 0
#define TM_VF_ID 0
#define TM_DEV_ID 0
#define TM_INIT_PORT_NUM (32)

int hns5_tm_platform_dev_probe(void);

#endif /* _HNS5_TM_H_ */
