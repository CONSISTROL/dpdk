#ifndef _HNS5_POE_H_
#define _HNS5_POE_H_

#include <stdint.h>

#include <rte_ethdev.h>

#include "ndrv_poe_pf_api.h"
#include "ndrv_poe_vf_api.h"

#include "hns5_eth.h"

#define HNS5_ETH_GROUP_MAX         50
#define HNS5_POE_ORDER_TMOUT_TIME  3000
#define HNS5_POE_CPU_GROUP_MAX_NUM 24
#define HNS5_POE_ACC_GROUP_MAX_NUM 18
#define HNS5_POE_BP_GROUP_MAX_NUM  16

#define HNS5_POE_CPU_GRP_QUE_NUM   8
#define HNS5_POE_TH_TPMLTE_ID      0

int hns5_eth_poe_init(struct rte_eth_dev *dev);
/**
 * @brief Init poe HW groups and their queues, and bind vcpu during RX queue setup
 * @attention Not concurrent-safe but reentrant; no duplicate init internally
*/
int hns5_eth_rx_queue_setup_poe(struct rte_eth_dev *dev, uint32_t group, uint16_t depth);

#endif /* _HNS5_POE_H_ */
