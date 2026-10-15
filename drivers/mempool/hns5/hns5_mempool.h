
#ifndef _HNS5_MEMPOOL_H_
#define _HNS5_MEMPOOL_H_
#include "native.h"
#include <sched.h>
#include "hns5_version.h"
/*
 * BMU data structure:
 * scene1 : sec_from_ppe_offset == ppe_bd_len
 * |bmu_hdr|ppe_bd|sec_bd               |sa_bd|hard_rsvd|mp_hdr|rte_mbuf|...
 * scene2 : sec_from_ppe_offset == 0, first 16 words of sec_bd reuse ppe_bd
 * |bmu_hdr|ppe_bd/sec_bd_16B|sec_bd_16B|sa_bd|hard_rsvd|mp_hdr|rte_mbuf|...
 */
#define BMU_BD_LEN   64
#define PPE_BD_LEN   64
#define SEC_BD_LEN   128
#define SA_BD_LEN    64 /* SA shares region with SEC_PRIV */
#define MEMPOOL_HEAD 64 /* the 64bytes is the rte_mempool_objhdr */

#define HARD_RSVD_LEN 0
#define SEC_FROM_PPE_OFFSET (PPE_BD_LEN)
#define BMU_QOS0_ADDR (0xA8000000 + 0x04060000 + 0x1100)
#define BMU_QOS8_ADDR (0xA8000000 + 0x04050000 + 0x0440)

#define BUM_HARD_HEAD_LEN        (BMU_BD_LEN + SEC_FROM_PPE_OFFSET + SEC_BD_LEN + SA_BD_LEN + HARD_RSVD_LEN)
#define BUM_HARD_HEAD_SKIP_LEN   (BUM_HARD_HEAD_LEN - BMU_BD_LEN - PPE_BD_LEN)
#define SA_FROM_PPE_OFFSET       (SEC_FROM_PPE_OFFSET + SEC_BD_LEN)
#define SEC_PRIV_FROM_PPE_OFFSET (SA_FROM_PPE_OFFSET) /* SA shares region with SEC_PRIV */
/* mbuf offsets */
#define MBUFF_FROM_SA_OFFSET          (SA_BD_LEN + HARD_RSVD_LEN + MEMPOOL_HEAD)
#define MBUFF_FROM_SEC_PRIV_OFFSET    (MBUFF_FROM_SA_OFFSET)
#define MBUFF_FROM_SEC_OFFSET         (SEC_BD_LEN + SA_BD_LEN + HARD_RSVD_LEN + MEMPOOL_HEAD)
#define MBUFF_FROM_PPE_OFFSET         (SEC_FROM_PPE_OFFSET + MBUFF_FROM_SEC_OFFSET)
#define MBUFF_FROM_BMU_OFFSET         (BMU_BD_LEN + MBUFF_FROM_PPE_OFFSET)

#define HNS5_BMU_ADDR_TRANS_MAX 512
#define HNS5_BMU_ADDR_TRANS_NAME "HNS5_BMU_ADD_TRANS_TBL"

struct hns5_bmu_addr_trans_info {
    rte_rwlock_t lock;
    uint32_t num;
    uint32_t current_blk_num;
    struct {
        uint64_t phys;
        uint64_t virt;
        uint32_t size;
        uint32_t pool_id;
    } tbl[HNS5_BMU_ADDR_TRANS_MAX];
};
extern struct hns5_bmu_addr_trans_info *g_hns5_bmu_addr_trans_tbl;

static __rte_always_inline void *hns5_bmu_phys2virt(uint64_t phys_addr)
{
    uint32_t i;
    struct hns5_bmu_addr_trans_info *p;

    if (unlikely(g_hns5_bmu_addr_trans_tbl == NULL)) {
        const struct rte_memzone *mz;
        mz = rte_memzone_lookup(HNS5_BMU_ADDR_TRANS_NAME);
        if (NULL == mz) {
            mz = rte_memzone_reserve(HNS5_BMU_ADDR_TRANS_NAME, sizeof(struct hns5_bmu_addr_trans_info),
                                     rte_socket_id(), 0);
            if (NULL == mz) {
                RTE_LOG(ERR, PMD, "reserve mem failed for HNS5_BMU_ADD_TRANS_TBL\n");
                return NULL;
            }
            memset(mz->addr, 0, sizeof(struct hns5_bmu_addr_trans_info));
        }
        g_hns5_bmu_addr_trans_tbl = (struct hns5_bmu_addr_trans_info *)(mz->addr);
    }

    p = g_hns5_bmu_addr_trans_tbl;

    for (i = 0; i < p->num; i++) {
        if (phys_addr >= p->tbl[i].phys && phys_addr < (p->tbl[i].phys + p->tbl[i].size)) {
            return (void *)(p->tbl[i].virt + (phys_addr - p->tbl[i].phys));
        }
    }

    return NULL;
}

static __rte_always_inline uint64_t hns5_bmu_virt2phys(void *vaddr)
{
    uint32_t i;
    struct hns5_bmu_addr_trans_info *p;

    if (unlikely(g_hns5_bmu_addr_trans_tbl == NULL)) {
        const struct rte_memzone *mz;
        mz = rte_memzone_lookup(HNS5_BMU_ADDR_TRANS_NAME);
        if (NULL == mz) {
            mz = rte_memzone_reserve(HNS5_BMU_ADDR_TRANS_NAME, sizeof(struct hns5_bmu_addr_trans_info),
                                     rte_socket_id(), 0);
            if (NULL == mz) {
                RTE_LOG(ERR, PMD, "reserve mem failed for HNS5_BMU_ADD_TRANS_TBL\n");
                return 0;
            }
            memset(mz->addr, 0, sizeof(struct hns5_bmu_addr_trans_info));
        }
        g_hns5_bmu_addr_trans_tbl = (struct hns5_bmu_addr_trans_info *)(mz->addr);
    }

    p = g_hns5_bmu_addr_trans_tbl;

    uint64_t tmp = (uint64_t)vaddr;
    for (i = 0; i < p->num; i++) {
        if (tmp >= p->tbl[i].virt && tmp < (p->tbl[i].virt + p->tbl[i].size))
            return (uint64_t)(p->tbl[i].phys + (tmp - p->tbl[i].virt));
    }

    return 0;
}

static __rte_always_inline uint32_t hns5_get_lcore_id(void)
{
    uint32_t core = rte_lcore_id();

    if (unlikely(core >= HNS5_CORE_MAX_NUM)) {
        core = sched_getcpu();
        if (core >= HNS5_CORE_MAX_NUM) {
            core = 0;
        }
    }

    return core;
}

void __rte_internal hns5_mp_clean_addr_trans_tbl(void);

#endif /* _HNS5_MEMPOOL_H_ */
