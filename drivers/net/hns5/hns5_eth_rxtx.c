#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_debug.h>
#include <rte_dev.h>
#include <rte_eal.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_malloc.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_kvargs.h>
#include <rte_atomic.h>
#include <rte_memcpy.h>
#include <rte_io.h>
#include <rte_mbuf_ptype.h>

#include "hidrv_udev.h"
#include "hidrv_map.h"
#include "hidrv_poe_pf_api.h"
#include "hidrv_poe_vf_api.h"
#include "hidrv_ppe_pf_api.h"
#include "hidrv_ppe_vf_api.h"
#include "hidrv_subctrl_api.h"
#include "ndrv_poe_pf_api.h"
#include "ndrv_poe_vf_api.h"
#include "ndrv_ppe_bd_type.h"
#include "native_reg_opt.h"
#include "ndrv_ppe_vf_api.h"
#include "ndrv_ppe_pf_api.h"
#include "hidrv_tm_pf_api.h"
#include "hidrv_tm_vf_api.h"
#include "node_code_api.h"

#include "hns5_mbuf.h"
#include "hns5_eth.h"
#include "hns5_poe.h"
#include "hns5_eth_rxtx.h"
#include "hns5_ppe.h"
#include "hns5_log.h"
#include "hns5_mempool.h"
#include "hns5_version.h"

#if defined(RTE_ARCH_ARM)
#if defined(RTE_ARCH_64)
#define dcbz(p) { asm volatile("dc zva, %0" : : "r" (p) : "memory"); }
#define dcbz_64(p) dcbz(p)
#endif
#endif

#define HNS5_ETH_CLEAR_PPE_BD(bd)      dcbz_64((void *)(bd))
#define HNS5_POE_INVALID_SOURCE_PORT   (0x7f)

#define HNS5_TX_CKSUM_OFFLOAD_MASK (RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IPV6 | RTE_MBUF_F_TX_L4_MASK)
#define HNS5_L2_LEN_UNIT 2
#define HNS5_L3_LEN_UNIT 4
#define HNS5_L4_LEN_UNIT 4
#define HNS5_TM_PKT_LEN_PAD 24

static __rte_always_inline void hns5_ppe_txd_post_process(ppe_tx_desc_t *ppe_txd)
{
    ppe_tx_desc_t *txd = (ppe_tx_desc_t *)ppe_txd;
    uint64_t nextaddr;
    uint32_t i;

    switch (g_ppe_endian_mode) {
        case HIDRV_PPE_32BE:
            do {
                uint32_t *p = (uint32_t *)txd;

                nextaddr = txd->head.next_addr_h;
                nextaddr = (nextaddr << 32) | txd->head.next_addr_l;

                for (i = 0; i < sizeof(ppe_tx_desc_t); i += 4, p++) {
                    *p = rte_bswap32(*p);
                }

                if (nextaddr == 0) {
                    return;
                }
                txd = hns5_bmu_phys2virt(nextaddr);
            } while (nextaddr);
            break;
        case HIDRV_PPE_64BE:
            do {
                uint64_t *p = (uint64_t *)txd;

                nextaddr = txd->head.next_addr_h;
                nextaddr = (nextaddr << 32) | txd->head.next_addr_l;

                for (i = 0; i < sizeof(ppe_tx_desc_t); i += 8, p++) {
                    *p = rte_bswap64(*p);
                }

                if (nextaddr == 0) {
                    return;
                }
                txd = hns5_bmu_phys2virt(nextaddr);
            } while (nextaddr);
            break;
        default:
            return;
    }
}

static __rte_always_inline int hns5_txd_L3_enable_checksum(void *txd_head, struct rte_mbuf *pkt) {
    ppe_tx_desc_t* ppe_txd_head = (ppe_tx_desc_t*)txd_head;
    if (pkt->ol_flags & RTE_MBUF_F_TX_IPV4) {
        ppe_txd_head->head.l3_len = pkt->l3_len / HNS5_L3_LEN_UNIT;
        if (unlikely(pkt->ol_flags & RTE_MBUF_F_TX_IP_CKSUM)) {
            ppe_txd_head->head.l3t = HNS5_L3T_IPV4;
            ppe_txd_head->head.l3_cs = 1;
        } else {
            ppe_txd_head->head.l3t = 0x3;
            ppe_txd_head->head.l3_cs = 0;
        }
    } else if (pkt->ol_flags & RTE_MBUF_F_TX_IPV6) {
        ppe_txd_head->head.l3_len = pkt->l3_len / HNS5_L3_LEN_UNIT;
        ppe_txd_head->head.l3t = HNS5_L3T_IPV6;
        ppe_txd_head->head.l3_cs = 0;
    }
    return 0;
}

static __rte_always_inline int hns5_txd_L4_enable_checksum(void *txd_head, struct rte_mbuf *pkt) {
    ppe_tx_desc_t* ppe_txd_head = (ppe_tx_desc_t*)txd_head;
    uint8_t l4t;

    switch (pkt->ol_flags & RTE_MBUF_F_TX_L4_MASK) {
        case RTE_MBUF_F_TX_TCP_CKSUM:
            l4t = HNS5_L4T_TCP;
            break;
        case RTE_MBUF_F_TX_UDP_CKSUM:
            l4t = HNS5_L4T_UDP;
            break;
        case RTE_MBUF_F_TX_SCTP_CKSUM:
            l4t = HNS5_L4T_SCTP;
            break;
        default:
            return 0;
    }

    ppe_txd_head->head.l4t = l4t;
    ppe_txd_head->head.l4_cs = 1;
    ppe_txd_head->head.l4_len = pkt->l4_len / HNS5_L4_LEN_UNIT;
    return 0;
}

static int __rte_hot hns5_mbuf_to_ppe_txd(struct rte_mbuf *pkt, void **ppe_txd, uint64_t *phys_addr)
{
    ppe_tx_desc_t *ppe_txd_head = NULL;
    ppe_tx_desc_t *txd_prev = NULL;
    ppe_tx_desc_t *txd = NULL;
    uint64_t ppe_txd_phys_addr;
    uint32_t offset;
    uint8_t segs_num;
    int i;

    ppe_txd_head = (ppe_tx_desc_t *)((char *)pkt - MBUFF_FROM_PPE_OFFSET); // 320

    ppe_txd_phys_addr = rte_pktmbuf_m2ppe_iova(pkt);
    offset = rte_pktmbuf_m2ppe_data_offset(pkt); // 576
    segs_num = pkt->nb_segs;

    if (unlikely((ppe_txd_phys_addr & 0x3f) != 0)) {
        RTE_LOG(ERR, PMD, "[hns5] mbuf_to_ppe_txd: ppe_txd_phys_addr unaligned, addr=0x%lx\n", ppe_txd_phys_addr);
        return -EFAULT;
    }
    if (unlikely(offset > 0xffff)) {
        RTE_LOG(ERR, PMD, "[hns5] mbuf_to_ppe_txd: offset too large, offset=%u\n", offset);
        return -EBADF;
    }

    *phys_addr = ppe_txd_phys_addr;

    HNS5_ETH_CLEAR_PPE_BD(ppe_txd_head);

    ppe_txd_head->head.data_offset = offset;
    ppe_txd_head->head.data_len = pkt->data_len;
    ppe_txd_head->head.pkt_len = pkt->pkt_len;
    ppe_txd_head->head.refitself_addr_l = (uint32_t)ppe_txd_phys_addr;
    ppe_txd_head->head.refitself_addr_h = (uint32_t)(ppe_txd_phys_addr >> 32);
    ppe_tnl_fill_tx_head_pd_info(&(ppe_txd_head->head));
    ppe_txd_head->head.wb = 0;

    if (pkt->ol_flags & HNS5_TX_CKSUM_OFFLOAD_MASK) {
        ppe_txd_head->head.ckst = 0;
        ppe_txd_head->head.l2_len = pkt->l2_len / HNS5_L2_LEN_UNIT;
        hns5_txd_L3_enable_checksum(ppe_txd_head, pkt);
        hns5_txd_L4_enable_checksum(ppe_txd_head, pkt);
        pkt->ol_flags = 0;
        pkt->tx_offload = 0;
    } else {
        ppe_txd_head->head.ckst = 1;
    }

    if (unlikely(segs_num > 1)) {
        txd_prev = ppe_txd_head;
        for (i = 1; i < segs_num; i++) {
            pkt = pkt->next;
            if (unlikely(pkt == NULL)) {
                RTE_LOG(ERR, PMD, "[hns5] mbuf_to_ppe_txd: pkt->next is NULL at seg %d\n", i);
                return -EFAULT;
            }
            txd = (ppe_tx_desc_t *)(void *)((char *)pkt - MBUFF_FROM_PPE_OFFSET);
            ppe_txd_phys_addr = rte_pktmbuf_m2ppe_iova(pkt);
            offset = rte_pktmbuf_m2ppe_data_offset(pkt);

            if (unlikely((ppe_txd_phys_addr & 0x3f) != 0)) {
                RTE_LOG(ERR, PMD, "[hns5] mbuf_to_ppe_txd: seg %d ppe_txd_phys_addr unaligned, addr=0x%lx\n", i, ppe_txd_phys_addr);
                return -EFAULT;
            }
            if (unlikely(offset > 0xffff)) {
                RTE_LOG(ERR, PMD, "[hns5] mbuf_to_ppe_txd: seg %d offset too large, offset=%u\n", i, offset);
                return -EBADF;
            }
            txd->frag.t_tag_val = 0;
            txd->frag.next_addr_l = 0;
            txd->frag.next_addr_h = 0;
            txd->frag.data_offset = offset;
            txd->frag.data_len = pkt->data_len;
            txd->frag.refitself_addr_l = (uint32_t)ppe_txd_phys_addr;
            txd->frag.refitself_addr_h = (ppe_txd_phys_addr >> 32) & 0xFFFF; // 32: upper 32 address bits
            txd_prev->frag.next_addr_l = txd->frag.refitself_addr_l;
            txd_prev->frag.next_addr_h = txd->frag.refitself_addr_h;
            txd_prev = txd;
        }
    } else if (pkt->pkt_len != pkt->data_len) {
        RTE_LOG(ERR, PMD, "[hns5] mbuf_to_ppe_txd: seg_num=1 but pkt_len != data_len, pkt_len=%u, data_len=%u\n",
            pkt->pkt_len, pkt->data_len);
        return -EBADF;
    }

    *ppe_txd = ppe_txd_head;

    return 0;
}

void hns5_eth_set_default_tx_msg(struct hns5_eth_tx_queue *txq)
{
    union ndrv_poe_cpu_add_msg *msg = &txq->default_tx_msg;

    msg->forward_tm.tp = txq->tm_port;
    msg->forward_tm.queue_id = txq->tm_queue_id;

    msg->msg_len = NDRV_POE_MSG_SIZE_256BIT;
    msg->t_len = ((uint32_t)NDRV_POE_FORWARD_TM >> 4) & 0xf; /* 4: shift right by 4 bits */
    msg->t_tagvalue = ((uint32_t)NDRV_POE_FORWARD_TM) & 0xf;
    msg->forward_tm.location = 0;

    msg->forward_tm.t_tagvlauetime = 0x1;
    msg->forward_tm.t_lentime = 0x2;
    msg->forward_tm.rls_ind = 1; /* release flag */
}

#define TM_PF_ID 0
#define TM_VF_ID 0
#define TM_DEV_ID 0
#define TM_CFG_FINISH 1

bool hns5_get_tm_init_status(void)
{
    int ret;
    uint32_t tm_status, vf_enable;

    ret = hidrv_tm_get_pf_devid(TM_DEV_ID, TM_PF_ID);
    if (ret != 0) {
        hns5_rxtx(ERR, "failed to hidrv_tm_get_pf_devid, ret:%d\n", ret);
        return false;
    }
    ret = hidrv_tm_pf_get_cfg_ok_status(TM_PF_ID, &tm_status);
    if (ret != 0) {
        hns5_rxtx(ERR, "failed to hidrv_tm_pf_get_cfg_ok_status, ret:%d\n", ret);
        return false;
    }
    if (tm_status != TM_CFG_FINISH) {
        return false;
    }

    ret = hidrv_tm_get_vf_devid(TM_DEV_ID, TM_PF_ID, TM_VF_ID);
    if (ret < 0) {
        hns5_rxtx(ERR, "failed to hidrv_tm_get_vf_devid, ret:%d\n", ret);
        return false;
    }
    ret = hidrv_tm_vf_get_vf_fun_enable(TM_DEV_ID , &vf_enable);
    if (ret != 0) {
        hns5_rxtx(ERR, "failed to hidrv_tm_vf_get_vf_fun_enable, ret:%d\n", ret);
        return false;
    }
    if (vf_enable != TM_CFG_FINISH) {
        return false;
    }

    return true;
}

static uint16_t __rte_hot hns5_eth_forward_to_tm(struct hns5_eth_tx_queue *txq, struct rte_mbuf **pkt,
    uint16_t burst_cnt)
{
    int ret;
    uint64_t phys_addr;
    struct rte_mbuf *mbuf = NULL;
    union ndrv_poe_cpu_add_msg *msg = NULL;
    uint32_t num;
    uint32_t lcore_id = hns5_get_lcore_id();
    static rte_atomic32_t tm_inited = RTE_ATOMIC32_INIT(0);
    // TM init may not be done here; sending before TM init is hard to debug, so check before TX

    if (unlikely(rte_atomic32_read(&tm_inited) == 0)) {
        if (hns5_get_tm_init_status() != true) {
            return 0;
        }
        rte_atomic32_set(&tm_inited, 1);
        hns5_rxtx(INFO, "tm is inited, allow to send pkt\n");
    }

    txq->vcpu = lcore_id * 2 + 1;
    if (unlikely(burst_cnt > HNS5_ETH_TX_BURST_CNT)) {
        burst_cnt = HNS5_ETH_TX_BURST_CNT;
    }
    msg = txq->msgs;
    for (num = 0; num < burst_cnt; num++) {
        ppe_tx_desc_t *ppe_txd = NULL;
        mbuf = pkt[num];

        // poe msg is 512-bit but forward_tm only uses the first 32 bytes; memcpy_s would hurt performance
        rte_memcpy(msg + num, &txq->default_tx_msg, 32);
        msg[num].sourceport = CPU_SOURCE_PORT(lcore_id);
        msg[num].forward_tm.len = mbuf->pkt_len + HNS5_TM_PKT_LEN_PAD;
        msg[num].forward_tm.rel = (mbuf->nb_segs > 1) ? 1 : 0;
        ret = hns5_mbuf_to_ppe_txd(mbuf, (void **)&ppe_txd, &phys_addr);
        if (unlikely(ret != 0)) {
            txq->stats.oerrors_txd++;
            hns5_rxtx(ERR, "failed to hns5_mbuf_to_ppe_txd, paddr:0x%lx ret=%d\n", phys_addr, ret);
            break;
        }

        /*
         * Default sc=1 skips LSW and goes straight to MPort: no LSW
         * processing is needed except VLAN insertion. Fall back to sc=0
         * through LSW only when port VLAN insertion (pvid) is enabled
         * (hns5_vlan_pvid_set sets priv->vlan_pvid_en=1); LSW embeds the
         * tag, default port VLAN in priv->port_def_vlan. Flags are stored
         * in priv, not ol_flags, so read them after hns5_mbuf_to_ppe_txd.
         */
        ppe_txd->head.sc = 0x1;
        ppe_txd->head.target_fun = txq->tm_port;
        if (txq->priv != NULL && txq->priv->vlan_pvid_en) {
            ppe_txd->head.sc = 0x0;
            ppe_txd->head.target_fun = 0;
        }

        hns5_ppe_txd_post_process(ppe_txd);

        msg[num].forward_tm.addr_31_0 = (uint32_t)phys_addr;
        msg[num].forward_tm.addr_47_32 = (uint32_t)(phys_addr >> 32); /* 32: upper 32 address bits */

        ret = (int)ndrv_poe_vf_add_msg(txq->poe_vfhandle, txq->vcpu, &msg[num]);
        if (unlikely(ret != 0)) {
            txq->stats.oerrors_msg++;
            hns5_rxtx(ERR, "failed to ndrv_poe_vf_add_msg failed, ret:%d\n", ret);
            break;
        }
    }

    txq->stats.opackets += num;
    for (uint32_t i = 0; i < num; i++) {
        txq->stats.obytes += pkt[i]->pkt_len;
    }
    return (uint16_t)num;
}

uint16_t __rte_hot hns5_eth_tx_burst(void *txq, struct rte_mbuf **tx_pkts, uint16_t nb_pkts)
{
    uint16_t tx_num = 0;
    struct hns5_eth_tx_queue *hns5_txq = (struct hns5_eth_tx_queue *)txq;

    tx_num = hns5_eth_forward_to_tm(hns5_txq, tx_pkts, nb_pkts);

    return tx_num;
}

static __rte_always_inline void hns5_ppe_rxd_prev_process(ppe_rx_desc_t *ppe_rxd)
{
    ppe_rx_desc_t *rxd = ppe_rxd;

    switch (g_ppe_endian_mode) {
        case HIDRV_PPE_32BE:
            {
                uint32_t *p = (uint32_t *)rxd;
                for (uint32_t i = 0; i < sizeof(ppe_rx_desc_t); i += 4, p++) {
                    *p = rte_bswap32(*p);
                }
            }
            break;
        case HIDRV_PPE_64BE:
            {
                uint64_t *p = (uint64_t *)rxd;
                for (uint32_t i = 0; i < sizeof(ppe_rx_desc_t); i += 8, p++) {
                    *p = rte_bswap64(*p);
                }
            }
            break;
        default:
            return;
    }
}

static __rte_always_inline int hns5_order_dequeue(handle poe_vfhandle, uint32_t vcpu, union ndrv_poe_msg *pkt_msg)
{
    int ret;
    union ndrv_poe_cpu_add_msg msg;

    msg.msg_len = NDRV_POE_MSG_SIZE_128BIT;
    msg.t_len = (NDRV_POE_SWITCH_NULL >> 4) & 0xf; /* 4: shift right by 4 bits */
    msg.t_tagvalue = NDRV_POE_SWITCH_NULL & 0xf;
    msg.switch_null.cmd_type = NDRV_POE_ORDER_CMD_SWITCH_NULL;
    msg.switch_null.order_bindid_low = pkt_msg->packet_msg.order_bindid_low;
    msg.switch_null.order_bindid_hi = pkt_msg->packet_msg.info.bind_info.order_bindid_hi;
    msg.switch_null.order_bindid_hi_ex = pkt_msg->packet_msg.order_bindid_hi_ex;
    msg.switch_null.order_bindval = pkt_msg->packet_msg.order_bindval;

    ret = ndrv_poe_vf_add_msg(poe_vfhandle, vcpu, &msg);
    if (unlikely(ret != 0)) {
        hns5_rxtx(ERR, "failed to ndrv_poe_vf_add_msg, ret:%d\n", ret);
        return ret;
    }
    return 0;
}

static int __rte_hot
construct_mbuf_with_poe_msg_chain(struct rte_mbuf *ppmbuf, uint64_t next_phys_addr, struct rte_mbuf *fistmbuf)
{
    struct rte_mbuf *tail = ppmbuf;

    while (next_phys_addr != 0) {
        ppe_rx_desc_t *rxd = (ppe_rx_desc_t *)hns5_bmu_phys2virt(next_phys_addr);

        if (unlikely(rxd == NULL)) {
            hns5_rxtx(ERR, "failed to hns5_bmu_phys2virt, next_phys_addr:0x%lx\n", next_phys_addr);
            /* Free chained segments (head freed by caller) to avoid dangling pointers/leaks */
            struct rte_mbuf *seg = ppmbuf->next;

            ppmbuf->next = NULL;
            while (seg != NULL) {
                struct rte_mbuf *nx = seg->next;

                seg->next = NULL;
                rte_mempool_put(seg->pool, seg);
                seg = nx;
            }
            return -EINVAL;
        }
        rte_prefetch0(rxd);
        rte_prefetch0((char *)hns5_pktmbuf_ppe2mbuf(rxd));
        rte_prefetch0((char *)hns5_pktmbuf_ppe2mbuf(rxd) + RTE_CACHE_LINE_SIZE);
        hns5_ppe_rxd_prev_process(rxd);

        struct rte_mbuf *mbuf = hns5_pktmbuf_ppe2mbuf(rxd);
        mbuf->data_off = rte_pktmbuf_ppe2m_data_offset(mbuf, rxd->frag.data_offset);
        tail->next = mbuf;
        mbuf->next = 0;
        mbuf->ol_flags = 0;
        mbuf->data_len = rxd->frag.data_len;
        fistmbuf->nb_segs++;

        if (likely((rxd->head.next_addr_l | rxd->head.next_addr_h) == 0)) {
            return 0;
        }

        next_phys_addr = ((((uint64_t)rxd->head.next_addr_h) << 32) | rxd->head.next_addr_l);
        tail = mbuf;
    }
    return 0;
}

static __rte_hot void release_chain_rxd_buf(ppe_rx_desc_t *rxd)
{
    ppe_rx_desc_t *next_rxd;
    uint64_t phys_addr;
    struct rte_mbuf *mbuf;

    if (unlikely((rxd->head.next_addr_l | rxd->head.next_addr_h) != 0)) {
        next_rxd = rxd;
        do {
            rxd = next_rxd;
            phys_addr = ((uint64_t)(next_rxd->head.next_addr_h) << 32) + next_rxd->head.next_addr_l;
            next_rxd = (ppe_rx_desc_t *)hns5_bmu_phys2virt(phys_addr);
            mbuf = hns5_pktmbuf_ppe2mbuf(rxd);
            rte_mempool_put(mbuf->pool, mbuf);
            if (next_rxd != NULL) {
                hns5_ppe_rxd_prev_process(next_rxd);
            }
        } while (next_rxd != NULL);
    } else {
        mbuf = hns5_pktmbuf_ppe2mbuf(rxd);
        rte_mempool_put(mbuf->pool, mbuf);
    }
}

static void hns5_eth_ppe_rxd_dump(ppe_rx_desc_t *rxd, struct rte_mbuf *mbuf)
{
    hns5_rxtx(ERR, "rxd: paddr:0x%llx data_offset:%u data_len:%u pkt_len:%u\n",
        (unsigned long long)hns5_bmu_virt2phys(rxd), rxd->head.data_offset, rxd->head.data_len, rxd->head.pkt_len);
    hns5_rxtx(ERR, "mbuf: buf_iova:0x%llx, buf_len:%u\n", (unsigned long long)mbuf->buf_iova, mbuf->buf_len);
    hns5_rxtx(ERR, "\n");
}

static __rte_always_inline uint32_t hns5_l3_l4_to_ptype(uint8_t l3_id, uint8_t l4_id)
{
    static const uint32_t l3_ptype_table[16] = {
        [0x0] = RTE_PTYPE_L3_IPV4,
        [0x1] = RTE_PTYPE_L3_IPV6,
        [0x2] = RTE_PTYPE_L2_ETHER_ARP,
        [0x4] = RTE_PTYPE_L3_IPV4_EXT,
        [0x5] = RTE_PTYPE_L3_IPV6_EXT,
        [0x6] = RTE_PTYPE_L2_ETHER_LLDP,
        [0xA] = RTE_PTYPE_L2_ETHER_TIMESYNC,
    };

    static const uint32_t l4_ptype_table[16] = {
        [0x0] = RTE_PTYPE_L4_UDP,
        [0x1] = RTE_PTYPE_L4_TCP,
        [0x2] = RTE_PTYPE_TUNNEL_GRE,
        [0x3] = RTE_PTYPE_L4_SCTP,
        [0x4] = RTE_PTYPE_L4_IGMP,
        [0x5] = RTE_PTYPE_L4_ICMP,
        [0x6] = RTE_PTYPE_TUNNEL_ESP,
    };
    RTE_BUILD_BUG_ON(RTE_DIM(l3_ptype_table) != 16);
    RTE_BUILD_BUG_ON(RTE_DIM(l4_ptype_table) != 16);

    return l3_ptype_table[l3_id & 0xF] | l4_ptype_table[l4_id & 0xF]; // HW guarantees IDs < 16, only guard here
}

static int __rte_hot
construct_mbuf_with_poe_msg(const union ndrv_poe_msg *get_msg, ppe_rx_desc_t *rxd, struct rte_mbuf **ppmbuf,
    uint32_t seqn)
{
    (void)seqn;
    int ret = 0;
    int error_sign = 0;
    struct rte_mbuf *mbuf = hns5_pktmbuf_ppe2mbuf(rxd);
    uint64_t next_paddr;
    uint16_t data_offset;
    uint16_t data_len;
    uint16_t pkt_len;

    /* HW DMA writes back the PPE RX descriptor non-atomically: fields like
     * data_len/data_offset first, whole-packet stats (pkt_len/timestamp)
     * last. The descriptor may not be fully written back when poe_msg
     * arrives. rte_io_rmb() prevents CPU/compiler read reordering */
    rte_io_rmb();
    next_paddr = ((uint64_t)rxd->head.next_addr_h << 32) | rxd->head.next_addr_l;
    data_offset = rxd->head.data_offset;
    data_len = rxd->head.data_len;
    pkt_len = rxd->head.pkt_len;

    error_sign = (rxd->head.status & 0x18) | (rxd->head.l2e) | (rxd->head.l3e) | (rxd->head.l4e);
    if (unlikely(error_sign)) {
        hns5_rxtx(ERR, "failed to check rxd, status:%d l2e:%d l3e:%d l4e:%d\n",
            rxd->head.status, rxd->head.l2e, rxd->head.l3e, rxd->head.l4e);
        (void)release_chain_rxd_buf(rxd);
        return -EINVAL;
    }


    if (unlikely(data_offset > 2000 || data_offset < (MBUFF_FROM_PPE_OFFSET + sizeof(struct rte_mbuf)))) {  /* data_offset from PPE/sPPE is typically fixed at 704 */
        hns5_eth_ppe_rxd_dump(rxd, mbuf);
        (void)release_chain_rxd_buf(rxd);
        return -EINVAL;
    }

    rte_pktmbuf_reset(mbuf);
    mbuf->data_off = rte_pktmbuf_ppe2m_data_offset(mbuf, data_offset);
    mbuf->port = get_msg->packet_msg.sourceport;
    mbuf->data_len = data_len;
    mbuf->pkt_len = pkt_len;
    mbuf->hash.rss = get_msg->packet_msg.vqi;
    mbuf->ol_flags = RTE_MBUF_F_RX_L4_CKSUM_GOOD | RTE_MBUF_F_RX_IP_CKSUM_GOOD;
    mbuf->packet_type = hns5_l3_l4_to_ptype(rxd->head.l3_id, rxd->head.l4_id);
    *ppmbuf = mbuf;

    if (likely(next_paddr == 0)) {
        return 0;
    }

    ret = construct_mbuf_with_poe_msg_chain(*ppmbuf, next_paddr, *ppmbuf);
    if (unlikely(ret != 0)) {
        struct rte_mbuf *mbuf = hns5_pktmbuf_ppe2mbuf(rxd);
        rte_mempool_put(mbuf->pool, mbuf);
        return -EINVAL;
    }
    return 0;
}

static __rte_always_inline ppe_rx_desc_t *hns5_poe_msg_to_ppe_rx_desc(struct hns5_eth_rx_queue *rxq,
    union ndrv_poe_msg *msg, uint32_t vcpu)
{
    uint64_t paddr = 0;
    ppe_rx_desc_t *ppe_rxd = NULL;

    paddr = (((uint64_t)msg->packet_msg.addr_47_32) << 32) + msg->packet_msg.addr_31_0;
    ppe_rxd = (ppe_rx_desc_t *)hns5_bmu_phys2virt(paddr);
    if(unlikely(ppe_rxd == NULL)) {
        hns5_rxtx(ERR, "get addr : 0x%llx ppe rxd fail\n", (long long)paddr);
        if (unlikely(msg->packet_msg.order_bindval)) {
            (void)hns5_order_dequeue(rxq->poe_vfhandle, vcpu, msg);
        }
        return NULL;
    }
    hns5_ppe_rxd_prev_process(ppe_rxd);
    return ppe_rxd;
}

static uint32_t __rte_hot hns5_eth_rx_burst_group_queue(struct hns5_eth_rx_queue *rxq,
    struct rte_mbuf **rx_pkts, uint16_t nb_pkts, bool by_queue)
{
    union ndrv_poe_msg msg = { 0 };
    uint32_t i = 0;
    int ret = 0;
    struct rte_mbuf *mbuf = NULL;
    uint16_t ret_num = 0;
    ppe_rx_desc_t *ppe_rxd = NULL;
    uint32_t lcore_id = hns5_get_lcore_id();
    uint32_t vcpu = lcore_id * 2 + 1;

    for (i = 0; i < nb_pkts; i++) {
        if (by_queue) {
            ret = ndrv_poe_vf_get_que_msg(rxq->poe_vfhandle, vcpu, rxq->group, rxq->queue, &msg);
        } else {
            ret = ndrv_poe_vf_get_grp_msg(rxq->poe_vfhandle, vcpu, rxq->group, &msg);
        }
        if (unlikely(ret != 0)) {
            break;
        }
        ppe_rxd = hns5_poe_msg_to_ppe_rx_desc(rxq, &msg, vcpu);
        if (unlikely(ppe_rxd == NULL)) {
            rxq->stats.ierrors_msg++;
            continue;
        }

        ret = construct_mbuf_with_poe_msg(&msg, ppe_rxd, &mbuf, 0);
        if (unlikely(ret != 0)) {
            rxq->stats.ierrors_rxd++;
            if (unlikely(msg.packet_msg.order_bindval)) {
                (void)hns5_order_dequeue(rxq->poe_vfhandle, vcpu, &msg);
            }
        }
        if (likely(ret == 0)) {
            rx_pkts[ret_num] = mbuf;
            ret_num++;
        }
    }

    return ret_num;
}

static uint32_t __rte_hot hns5_eth_rx_burst_queue(struct hns5_eth_rx_queue *rxq, struct rte_mbuf **rx_pkts,
    uint16_t nb_pkts)
{
    return hns5_eth_rx_burst_group_queue(rxq, rx_pkts, nb_pkts, true);
}

static uint32_t __rte_hot hns5_eth_rx_burst_group(struct hns5_eth_rx_queue *rxq, struct rte_mbuf **rx_pkts,
    uint16_t nb_pkts)
{
    return hns5_eth_rx_burst_group_queue(rxq, rx_pkts, nb_pkts, false);
}

typedef uint32_t (*hns5_eth_rx_pkt_burst_t)(struct hns5_eth_rx_queue *rxq,
    struct rte_mbuf **rx_pkts, uint16_t nb_pkts);

hns5_eth_rx_pkt_burst_t hns5_eth_rx_burst_ops[HNS5_ETH_RXQ_TYPE_INVALID] = {
    hns5_eth_rx_burst_group,
    hns5_eth_rx_burst_queue,
};

uint16_t __rte_hot hns5_eth_rx_burst(void *q, struct rte_mbuf **rx_pkts, uint16_t nb_pkts)
{
    struct hns5_eth_rx_queue *rxq = (struct hns5_eth_rx_queue *)q;
    uint16_t rx_num = 0;

    if (unlikely(rxq == NULL || rxq->type >= HNS5_ETH_RXQ_TYPE_INVALID ||
        hns5_eth_rx_burst_ops[rxq->type] == NULL)) {
        return 0;
    }

    rx_num = hns5_eth_rx_burst_ops[rxq->type](rxq, rx_pkts, nb_pkts);
    rxq->stats.ipackets += rx_num;
    for (uint16_t i = 0; i < rx_num; i++) {
        rxq->stats.ibytes += rx_pkts[i]->pkt_len;
    }
    return rx_num;
}
