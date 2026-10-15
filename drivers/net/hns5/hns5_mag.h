#ifndef _HNS5_MAG_H_
#define _HNS5_MAG_H_

#include "hns5_eth.h"
#include "hns5_ppe.h"

// mtu = frame - 14(eth hdr) - 4(CRC) - 4(VLAN len)*2
#define HNS5_ETH_OVERHEAD  (RTE_ETHER_HDR_LEN + RTE_ETHER_CRC_LEN + 4 * 2)
#define HNS5_MAG_FRAME_TO_MTU(frame) (frame - HNS5_ETH_OVERHEAD)
#define HNS5_MAG_MTU_TO_FRAME(mtu) (mtu + HNS5_ETH_OVERHEAD)

#define HNS5_FRAME_MAX     PPE_SUPPORT_FRAME_MAX       // use ppe max frame len as whole link max frame len
#define HNS5_MTU_MAX       HNS5_MAG_FRAME_TO_MTU(HNS5_FRAME_MAX)
#define HNS5_FRAME_MIN     64
#define HNS5_MTU_MIN       HNS5_MAG_FRAME_TO_MTU(HNS5_FRAME_MIN)
#define HNS5_MTU_DEFAULT   1500UL
#define HNS5_FRAME_DEFAULT HNS5_MAG_MTU_TO_FRAME(HNS5_MTU_DEFAULT)

int hns5_eth_mag_init(struct rte_eth_dev *dev);

int hns5_eth_set_crc_offload(struct rte_eth_dev *dev, uint32_t enable);

#endif /* _HNS5_MAG_H_ */
