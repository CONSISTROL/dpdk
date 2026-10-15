#ifndef _HNS5_MBUF_H_
#define _HNS5_MBUF_H_

#include <rte_mbuf.h>
#include "rte_mbuf.h"

#include "hns5_mempool.h"

#ifdef __cplusplus
extern "C" {
#endif

#define rte_pktmbuf_ppe2m_data_offset(m, ppe_data_off) \
    ((ppe_data_off) - MBUFF_FROM_PPE_OFFSET - sizeof(struct rte_mbuf) - (m)->priv_size)

#define rte_pktmbuf_ppe_skip(mp) \
    (BUM_HARD_HEAD_SKIP_LEN + (mp)->header_size + sizeof(struct rte_mbuf) + rte_pktmbuf_priv_size(mp) + \
    RTE_PKTMBUF_HEADROOM)

#define rte_pktmbuf_m2ppe_iova(m) \
    ((rte_iova_t)((uintptr_t)(m)->buf_iova - MBUFF_FROM_PPE_OFFSET - sizeof(struct rte_mbuf) - (m)->priv_size))

#define rte_pktmbuf_m2ppe_data_offset(m) \
    (MBUFF_FROM_PPE_OFFSET + sizeof(struct rte_mbuf) + (m)->priv_size + (m)->data_off)

// Get mbuf from ppe rxd descriptor index
#define hns5_pktmbuf_ppe2mbuf(rxd)    (struct rte_mbuf *)((char *)rxd + MBUFF_FROM_PPE_OFFSET)
#ifdef __cplusplus
}
#endif

#endif /* _HNS5_MBUF_H_ */
