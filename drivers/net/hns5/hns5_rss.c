/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2026 HiSilicon.
 */

#include "hns5_rss.h"

/* Default RSS Toeplitz hash key */
const uint8_t hns5_default_rss_key[HNS5_RSS_KEY_SIZE] = {
    0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2,
    0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0,
    0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4,
    0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c,
    0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa};

/* RSS internal function declarations */
static int hns5_rss_enable_sequence(struct rte_eth_dev *dev);
static int hns5_rss_idt_alloc_to_ppe(struct rte_eth_dev *dev);
static int hns5_rss_idt_to_ppe(struct rte_eth_dev *dev);
static int hns5_rss_hash_to_ppe(struct rte_eth_dev *dev);
static int hns5_rss_ipv4_ipv6_enable_to_ppe(struct rte_eth_dev *dev);
static int hns5_rss_non_ip_enable_to_ppe(struct rte_eth_dev *dev);
static int hns5_rss_fw_not_hit_actn_to_ppe(struct rte_eth_dev *dev);
static int hns5_rss_reset_indir_table(struct rte_eth_dev *dev);

/**
 * @brief Send RSS hash config to PPE hardware
 */
static int hns5_rss_hash_to_ppe(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct ndrv_ppp_rss_mst_hash_cfg mst_hash_cfg = {0};
    struct ndrv_ppp_rss_simple_hash_cfg simple_hash_cfg = {0};
    unsigned int ppe_tnl = (unsigned int)priv->param.cport_id;
    unsigned int ret;

    if (priv->rss.algo == RTE_ETH_HASH_FUNCTION_SIMPLE_XOR) {
        simple_hash_cfg.gen_mode = NDRV_PPP_RSS_USE_HASH_LSB_9BIT;
        simple_hash_cfg.gro_en = 0;
        memcpy(simple_hash_cfg.hash_key, priv->rss.key, HNS5_RSS_KEY_SIZE);
        ret = ndrv_ppe_ppp_pf_set_rss_simple_hash(priv->ppe_pfhandle, ppe_tnl, &simple_hash_cfg);
        if (ret != 0) {
            hns5_err(dev, "failed to set RSS simple hash, ret:%u\n", ret);
            return -EINVAL;
        }
    } else {
        mst_hash_cfg.sym_alg_en = (priv->rss.algo == RTE_ETH_HASH_FUNCTION_SYMMETRIC_TOEPLITZ) ? 1 : 0;
        mst_hash_cfg.sym_alg_mode_sel = 1;
        mst_hash_cfg.gro_en = 0;
        memcpy(mst_hash_cfg.hash_key, priv->rss.key, HNS5_RSS_KEY_SIZE);
        ret = ndrv_ppe_ppp_pf_set_rss_mst_hash(priv->ppe_pfhandle, ppe_tnl, &mst_hash_cfg);
        if (ret != 0) {
            hns5_err(dev, "failed to set RSS master hash, ret:%u\n", ret);
            return -EINVAL;
        }
    }

    return 0;
}

/**
 * @brief Send RSS indirect table allocation to PPE hardware
 */
static int hns5_rss_idt_alloc_to_ppe(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct ndrv_ppp_rss_idt_alloc_cfg idt_alloc = {0};
    unsigned int ppe_tnl = (unsigned int)priv->param.cport_id;
    unsigned int ret;
    // block must not change: hash takes low 8 bits to cover 256 entries
    idt_alloc.block_offset = 0;
    idt_alloc.block_num = 64;  /* 64 blocks × 4 entries/block = 256 entries */

    ret = ndrv_ppe_ppp_pf_set_rss_idt_table_alloc(priv->ppe_pfhandle, ppe_tnl, &idt_alloc);
    if (ret != 0) {
        hns5_err(dev, "failed to set RSS IDT alloc, ret:%u\n", ret);
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief Send RSS indirect table entries to PPE hardware
 * @note Table has 1024 entries; low 8-bit index maps: block=idx/4, entry=idx%4
 */
static int hns5_rss_idt_to_ppe(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    unsigned int ret;
    uint16_t i;
    unsigned int tbl_idx, item_idx;
    uint16_t nb_rx_queues = RTE_MIN(dev->data->nb_rx_queues, (uint16_t)8);

    for (i = 0; i < HNS5_RSS_INDIRECT_TABLE_SIZE; i++) {
        tbl_idx = i / 16;    /* 1024 / 16 = 64 blocks */
        item_idx = i % 16;   /* 16 entries per block */
        ret = ndrv_ppe_ppp_pf_set_rss_idt_table(priv->ppe_pfhandle, tbl_idx, item_idx,
                                                  i % nb_rx_queues);
        if (ret != 0) {
            hns5_err(dev, "failed to set RSS IDT table[%u], ret:%u\n", i, ret);
            return -EINVAL;
        }
    }

    return 0;
}

/**
 * @brief Calculate the tc_size value
 * @param rx_queue_num Number of RX queues
 * @return tc_size (2^tc_size = actual queue count, max 8 means 256 queues)
 */
static unsigned int hns5_calc_tc_size(unsigned int rx_queue_num)
{
    if (rx_queue_num == 0) {
        return 0;
    }
    return rte_log2_u64(rte_align64pow2(rx_queue_num));
}

/**
 * @brief Parse and apply the RSS config to priv
 * @param dev Ethernet device
 * @param priv Ethernet device private data
 * @param rss_conf RSS config
 * @return 0 on success, non-zero on failure
 */
static int hns5_rss_parse_conf(struct rte_eth_dev *dev, struct hns5_eth_priv *priv,
                                 const struct rte_eth_rss_conf *rss_conf)
{
    uint64_t hf = rss_conf->rss_hf;
    if ((hf & ~HNS5_ETH_RSS_SUPPORT) != 0) {
        hns5_warn(dev, "RSS hf(0x%llx) contains unsupported flags, "
                    "unsupported:0x%llx, will be cleared\n",
                    (unsigned long long)hf, (unsigned long long)(hf & ~HNS5_ETH_RSS_SUPPORT));
        hf &= HNS5_ETH_RSS_SUPPORT;
    }
    priv->rss.hf = hf;

    if (rss_conf->rss_key != NULL && rss_conf->rss_key_len > 0) {
        uint8_t key_len = (rss_conf->rss_key_len < HNS5_RSS_KEY_SIZE) ?
                          rss_conf->rss_key_len : HNS5_RSS_KEY_SIZE;
        (void)memcpy(priv->rss.key, rss_conf->rss_key, key_len);
        priv->rss.key_len = key_len;
    }

    return 0;
}

/**
 * @brief Send RSS TC mode config to PPE hardware
 */
static int hns5_rss_tc_mode_to_ppe(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct ndrv_ppp_rss_tc_mode_data tc_data = {0};
    unsigned int ret;
    unsigned int tc_id;
    unsigned int cport_id = (unsigned int)priv->param.cport_id;
    uint16_t rss_size_per_tc;
    unsigned int tc_offset;
    unsigned int base_tnl_offset;
    unsigned int enabled_tc_count = 0;

    for (tc_id = 0; tc_id < 8; tc_id++) {
        if (priv->hw_tc_map & (1 << tc_id)) {
            enabled_tc_count++;
        }
    }

    if (enabled_tc_count == 0) {
        return 0;
    }

    rss_size_per_tc = dev->data->nb_rx_queues / enabled_tc_count;

    base_tnl_offset = (priv->fwl_tnl_inited == 1) ? priv->fwl_tnl_conf.base_tnl : 0;
    tc_offset = base_tnl_offset;

    for (tc_id = 0; tc_id < 8; tc_id++) {
        if (priv->hw_tc_map & (1 << tc_id)) {
            tc_data.tc_offset = tc_offset;
            tc_data.tc_size = hns5_calc_tc_size(rss_size_per_tc);

            unsigned int next_offset = tc_offset + rss_size_per_tc;
            if (next_offset > 256) {
                hns5_err(dev, "RSS TC%u config exceeds limit, "
                    "tc_offset=%u rss_size_per_tc=%u\n",
                    tc_id, tc_offset, rss_size_per_tc);
                return -EINVAL;
            }
            tc_offset = next_offset;
            ret = ndrv_ppe_ppp_pf_set_rss_tc_mode(priv->ppe_pfhandle, cport_id, tc_id, 1, &tc_data);
            if (ret != 0) {
                hns5_err(dev, "failed to set RSS TC mode %u, ret:%u\n", tc_id, ret);
                return -EINVAL;
            }
        }
    }

    return 0;
}

/**
 * @brief Send IPv4/IPv6 packet RSS enable config to PPE hardware
 */
static int hns5_rss_ipv4_ipv6_enable_to_ppe(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct ndrv_ppe_lsw_rss_ipv4_en ipv4_en = {0};
    struct ndrv_ppe_lsw_rss_ipv6_en ipv6_en = {0};
    uint64_t hf = priv->rss.hf;
    unsigned int ret;

    if (hf & (RTE_ETH_RSS_NONFRAG_IPV4_TCP | RTE_ETH_RSS_IPV4)) {
        ipv4_en.tcp.src_ip_en = 1;
        ipv4_en.tcp.dst_ip_en = 1;
        ipv4_en.tcp.src_port_en = 1;
        ipv4_en.tcp.dst_port_en = 1;
    }
    if (hf & (RTE_ETH_RSS_NONFRAG_IPV4_UDP | RTE_ETH_RSS_IPV4)) {
        ipv4_en.udp.src_ip_en = 1;
        ipv4_en.udp.dst_ip_en = 1;
        ipv4_en.udp.src_port_en = 1;
        ipv4_en.udp.dst_port_en = 1;
    }
    if (hf & (RTE_ETH_RSS_NONFRAG_IPV4_SCTP | RTE_ETH_RSS_IPV4)) {
        ipv4_en.sctp.src_ip_en = 1;
        ipv4_en.sctp.dst_ip_en = 1;
        ipv4_en.sctp.veri_tag_en = 1;
    }
    if (hf & (RTE_ETH_RSS_NONFRAG_IPV4_OTHER | RTE_ETH_RSS_IPV4)) {
        ipv4_en.non_ipv4.src_ip_en = 1;
        ipv4_en.non_ipv4.dst_ip_en = 1;
        ipv4_en.non_ipv4.prot_en = 1;
    }
    if (hf & (RTE_ETH_RSS_FRAG_IPV4 | RTE_ETH_RSS_IPV4)) {
        ipv4_en.frag_prot.src_ip_en = 1;
        ipv4_en.frag_prot.dst_ip_en = 1;
    }

    if (hf & (RTE_ETH_RSS_NONFRAG_IPV6_TCP | RTE_ETH_RSS_IPV6)) {
        ipv6_en.tcp.src_ip_en = 1;
        ipv6_en.tcp.dst_ip_en = 1;
        ipv6_en.tcp.src_port_en = 1;
        ipv6_en.tcp.dst_port_en = 1;
    }
    if (hf & (RTE_ETH_RSS_NONFRAG_IPV6_UDP | RTE_ETH_RSS_IPV6)) {
        ipv6_en.udp.src_ip_en = 1;
        ipv6_en.udp.dst_ip_en = 1;
        ipv6_en.udp.src_port_en = 1;
        ipv6_en.udp.dst_port_en = 1;
    }
    if (hf & (RTE_ETH_RSS_NONFRAG_IPV6_SCTP | RTE_ETH_RSS_IPV6)) {
        ipv6_en.sctp.src_ip_en = 1;
        ipv6_en.sctp.dst_ip_en = 1;
        ipv6_en.sctp.src_veri_tag_en = 1;
    }
    if (hf & (RTE_ETH_RSS_NONFRAG_IPV6_OTHER | RTE_ETH_RSS_IPV6)) {
        ipv6_en.non_ipv6.src_ip_en = 1;
        ipv6_en.non_ipv6.dst_ip_en = 1;
        ipv6_en.non_ipv6.next_header_en = 1;
    }
    if (hf & (RTE_ETH_RSS_FRAG_IPV6 | RTE_ETH_RSS_IPV6)) {
        ipv6_en.frag_prot.src_ip_en = 1;
        ipv6_en.frag_prot.dst_ip_en = 1;
    }

    ret = ndrv_ppe_lsw_pf_rss_set_ipv4_enable(priv->ppe_pfhandle, &ipv4_en);
    if (ret != 0) {
        hns5_err(dev, "failed to set RSS IPv4 enable, ret:%u\n", ret);
        return -EINVAL;
    }

    ret = ndrv_ppe_lsw_pf_rss_set_ipv6_enable(priv->ppe_pfhandle, &ipv6_en);
    if (ret != 0) {
        hns5_err(dev, "failed to set RSS IPv6 enable, ret:%u\n", ret);
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief Send non-IP packet RSS enable config to PPE hardware
 */
static int hns5_rss_non_ip_enable_to_ppe(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    union ndrv_ppe_rss_non_ip_en non_ip_en = {0};
    unsigned int ret;

    if (priv->rss.hf & RTE_ETH_RSS_L2_PAYLOAD) {
        non_ip_en.dmac_en = 1;
        non_ip_en.smac_en = 1;
        non_ip_en.vlan1_en = 1;
        non_ip_en.vlan2_en = 1;
        non_ip_en.eth_type_en = 1;
    }

    ret = ndrv_ppe_lsw_pf_rss_set_non_ip_enable(priv->ppe_pfhandle, &non_ip_en);
    if (ret != 0) {
        hns5_err(dev, "failed to set RSS non-IP enable, ret:%u\n", ret);
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief Send RSS fallback config for flow table miss to PPE hardware
 * @note Do not drop on flow table miss; route to RSS distribution
 */
static int hns5_rss_fw_not_hit_actn_to_ppe(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    unsigned int ret;

    ret = ndrv_ppe_ppp_pf_set_fw_not_hit_actn(priv->ppe_pfhandle, 1, NULL);
    if (ret != 0) {
        hns5_err(dev, "failed to set RSS fw not hit action, ret:%u\n", ret);
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief Reset the RSS indirect table (all zeros)
 * @param dev Ethernet device
 * @return 0 on success, non-zero on failure
 */
static int hns5_rss_reset_indir_table(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    unsigned int ret;
    uint16_t i;
    unsigned int tbl_idx, item_idx;

    for (i = 0; i < HNS5_RSS_INDIRECT_TABLE_SIZE; i++) {
        tbl_idx = i / 16;
        item_idx = i % 16;
        ret = ndrv_ppe_ppp_pf_set_rss_idt_table(priv->ppe_pfhandle,
            tbl_idx, item_idx, 0);
        if (ret != 0) {
            hns5_err(dev, "failed to reset RSS IDT table[%u], ret:%u\n", i, ret);
            return -EINVAL;
        }
    }

    return 0;
}

/**
 * @brief Full RSS enable sequence
 * @note Call order:
 *   1. Hash algorithm config
 *   2. Indirect table allocation
 *   3. Indirect table entries
 *   4. TC mode config
 *   5. Packet type enable
 */
static int hns5_rss_enable_sequence(struct rte_eth_dev *dev)
{
    int ret;

    ret = hns5_rss_idt_alloc_to_ppe(dev);
    if (ret != 0) {
        return ret;
    }

    ret = hns5_rss_idt_to_ppe(dev);
    if (ret != 0) {
        return ret;
    }

    ret = hns5_rss_tc_mode_to_ppe(dev);
    if (ret != 0) {
        return ret;
    }

    ret = hns5_rss_ipv4_ipv6_enable_to_ppe(dev);
    if (ret != 0) {
        return ret;
    }

    ret = hns5_rss_non_ip_enable_to_ppe(dev);
    if (ret != 0) {
        return ret;
    }

    ret = hns5_rss_fw_not_hit_actn_to_ppe(dev);
    if (ret != 0) {
        return ret;
    }

    ret = hns5_rss_hash_to_ppe(dev);
    if (ret != 0) {
        return ret;
    }
    return 0;
}

/**
 * @brief Set RSS default parameters
 * @param priv Ethernet device private data
 * @note Only set software config, no hardware write
 */
void hns5_rss_set_default_args(struct hns5_eth_priv *priv)
{
    priv->rss.hf = 0;
    priv->rss.algo = RTE_ETH_HASH_FUNCTION_TOEPLITZ;
    (void)memcpy(priv->rss.key, hns5_default_rss_key, HNS5_RSS_KEY_SIZE);
    priv->rss.key_len = HNS5_RSS_KEY_SIZE;
    priv->rss.reta_size = 0;
}

/**
 * @brief Apply RSS config from dev_conf to priv->rss
 * @param dev Ethernet device
 * @return 0 on success, non-zero on failure
 * @note Only update software config, no hardware write
 */
int hns5_rss_apply_conf(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct rte_eth_rss_conf *rss_conf = &dev->data->dev_conf.rx_adv_conf.rss_conf;
    int ret;

    ret = hns5_rss_parse_conf(dev, priv, rss_conf);
    if (ret != 0) {
        return ret;
    }
    return 0;
}

/**
 * @brief Apply RSS software config to hardware
 * @param dev Ethernet device
 * @return 0 on success, non-zero on failure
 * @note Read config from priv->rss and write to hardware; use default if not initialized
 */
int hns5_config_rss(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    int ret;
    uint16_t i;
    uint16_t nb_rx_queues = dev->data->nb_rx_queues;

    if (nb_rx_queues == 0) {
        hns5_err(dev, "nb_rx_queues is 0, invalid to config rss\n");
        return -EINVAL;
    }

    if (priv->rss.reta_size == 0) {
        for (i = 0; i < HNS5_RSS_RETA_SIZE; i++) {
            priv->rss.reta[i] = i % nb_rx_queues;
        }
        priv->rss.reta_size = HNS5_RSS_RETA_SIZE;
    }

    ret = hns5_rss_enable_sequence(dev);
    if (ret != 0) {
        hns5_err(dev, "failed to init RSS, ret:%d\n", ret);
        return ret;
    }

    return 0;
}

/**
 * @brief RSS de-initialization
 * @param dev Ethernet device
 */
void hns5_rss_uninit(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    /* Reset the indirect table in hardware */
    (void)hns5_rss_reset_indir_table(dev);

    /* Clear RSS config */
    priv->rss.hf = 0;
    priv->rss.reta_size = 0;
}

/**
 * @brief Update RSS RETA table
 * @note Write to hardware immediately
 */
int hns5_rss_reta_update(struct rte_eth_dev *dev, struct rte_eth_rss_reta_entry64 *reta_conf,
                          uint16_t reta_size)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    uint16_t i, idx, shift;
    uint16_t nb_rx_queues = dev->data->nb_rx_queues;

    if (dev == NULL || reta_conf == NULL) {
        hns5_err(dev, "invalid parameter\n");
        return -EINVAL;
    }

    if (reta_size == 0 || reta_size > HNS5_RSS_RETA_SIZE) {
        hns5_err(dev, "invalid reta_size:%u\n", reta_size);
        return -EINVAL;
    }

    for (i = 0; i < reta_size; i++) {
        idx = i / HNS5_RSS_RETA_GROUP_SIZE;
        shift = i % HNS5_RSS_RETA_GROUP_SIZE;
        if (reta_conf[idx].mask & (1ULL << shift)) {
            uint16_t queue = (uint16_t)reta_conf[idx].reta[shift];
            if (queue >= nb_rx_queues) {
                hns5_err(dev, "reta[%u] queue:%u >= nb_rx_queues:%u\n",
                    i, queue, nb_rx_queues);
                return -EINVAL;
            }
            priv->rss.reta[i] = queue;
        }
    }

    priv->rss.reta_size = reta_size;

    if (hns5_rss_idt_alloc_to_ppe(dev) != 0) {
        return -EINVAL;
    }
    return hns5_rss_idt_to_ppe(dev);
}

/**
 * @brief Query RSS RETA table
 */
int hns5_rss_reta_query(struct rte_eth_dev *dev, struct rte_eth_rss_reta_entry64 *reta_conf,
                         uint16_t reta_size)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    uint16_t i, idx, shift;

    if (dev == NULL || reta_conf == NULL) {
        hns5_err(dev, "invalid parameter\n");
        return -EINVAL;
    }

    if (reta_size == 0) {
        hns5_err(dev, "reta_size is 0\n");
        return -EINVAL;
    }

    for (i = 0; i < reta_size && i < priv->rss.reta_size; i++) {
        idx = i / HNS5_RSS_RETA_GROUP_SIZE;
        shift = i % HNS5_RSS_RETA_GROUP_SIZE;
        if (reta_conf[idx].mask & (1ULL << shift)) {
            reta_conf[idx].reta[shift] = priv->rss.reta[i];
        }
    }

    return 0;
}

/**
 * @brief Update RSS hash algorithm and key
 * @note Write to hardware immediately if configured; otherwise update software state only
 */
int hns5_rss_hash_update(struct rte_eth_dev *dev, struct rte_eth_rss_conf *rss_conf)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    int ret;

    if (dev == NULL || rss_conf == NULL) {
        hns5_err(dev, "invalid parameter\n");
        return -EINVAL;
    }

    ret = hns5_rss_parse_conf(dev, priv, rss_conf);
    if (ret != 0) {
        return ret;
    }

    ret = hns5_rss_ipv4_ipv6_enable_to_ppe(dev);
    if (ret != 0) {
        return ret;
    }
    ret = hns5_rss_non_ip_enable_to_ppe(dev);
    if (ret != 0) {
        return ret;
    }
    return hns5_rss_hash_to_ppe(dev);
}

/**
 * @brief Query RSS hash config
 */
int hns5_rss_hash_conf_get(struct rte_eth_dev *dev, struct rte_eth_rss_conf *rss_conf)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    if (dev == NULL || rss_conf == NULL) {
        hns5_err(dev, "invalid parameter\n");
        return -EINVAL;
    }

    rss_conf->rss_hf = priv->rss.hf;
    if (rss_conf->rss_key != NULL && rss_conf->rss_key_len > 0) {
        uint8_t key_len = (rss_conf->rss_key_len < priv->rss.key_len) ?
                          rss_conf->rss_key_len : priv->rss.key_len;
        (void)memcpy(rss_conf->rss_key, priv->rss.key, key_len);
        rss_conf->rss_key_len = key_len;
    } else {
        rss_conf->rss_key_len = priv->rss.key_len;
    }

    return 0;
}
