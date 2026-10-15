/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */

#include <ethdev_driver.h>
#include <rte_io.h>
#include <rte_malloc.h>
#include "ndrv_ppe_ppp_pf_api.h"
#include "hns5_log.h"
#include "hns5_eth.h"

static int hns5_ppe_ppp_set_fw_fd_key_tp(struct rte_eth_dev *dev, uint32_t tmplt_id)
{
    int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    union hidrv_ppe_fw_key_field_mask fw_key_field = { 0 };

    fw_key_field.tuple_key.in_dmac = 0x3f;
    fw_key_field.meta_key.des_port = 0x1;

    ret = hidrv_ppe_pf_ftable_set_fw_fd_key_tp(priv->ppe_pf_devid, HIDRV_PPE_FD_STAGE_1, tmplt_id, &fw_key_field);
    if (ret != 0) {
        hns5_err(dev, "set fw fd key tp err %d\n", ret);
        return ret;
    }

    return 0;
}

/*
 * @brief Configure the LSW ingress flow table key template
 * @param[in] dev        Ethernet device
 * @param[in] tmplt_id   Template index
 * @return int           0 on success, error code on failure
 */
static int hns5_ppe_ppp_set_lsw_ing_fd_key_tp(struct rte_eth_dev *dev, uint32_t tmplt_id)
{
    int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    union hidrv_ppe_ing_key_field_mask ing_key_field = { 0 };

    /* Configure meta field mask - supports ingress port */
    ing_key_field.tuple_key.dmac = 0x3f;
    ing_key_field.tuple_key.smac = 0x3f;
    ing_key_field.meta_key.ingress_port = 0x1;

    ret = hidrv_ppe_pf_ftable_set_ingress_fd_key_tp(priv->ppe_pf_devid, HIDRV_PPE_FD_STAGE_1, tmplt_id, &ing_key_field);
    if (ret != 0) {
        hns5_err(dev, "set lsw ing fd key tp err %d\n", ret);
        return ret;
    }

    return 0;
}

/*
 * @brief Configure the LSW egress flow table key template
 * @param[in] dev        Ethernet device
 * @param[in] tmplt_id   Template index
 * @return int           0 on success, error code on failure
 */
static int hns5_ppe_ppp_set_lsw_egr_fd_key_tp(struct rte_eth_dev *dev, uint32_t tmplt_id)
{
    int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    union hidrv_ppe_egr_key_field_mask egr_key_field = { 0 };

    /* Configure egress flow table key field mask - supports DMAC, SMAC, VLAN, ETH_TYPE, etc. */
    egr_key_field.tuple_key.dmac = 0x3f;
    egr_key_field.tuple_key.smac = 0x3f;
    egr_key_field.tuple_key.outer_vlan_tag = 0x3;
    egr_key_field.tuple_key.inner_vlan_tag = 0x3;
    egr_key_field.tuple_key.eth_type = 0x3;
    egr_key_field.tuple_key.ip_tos = 0x1;
    egr_key_field.tuple_key.ip_proto = 0x1;
    egr_key_field.tuple_key.ip_sp = 0xFFFF;
    egr_key_field.tuple_key.ip_dp = 0xFFFF;
    egr_key_field.tuple_key.sp = 0x3;
    egr_key_field.tuple_key.dp = 0x3;


    /* Configure meta field mask - supports egress port */
    egr_key_field.meta_key.egress_port = 0x1;
    egr_key_field.meta_key.vlan_num = 0x1;

    ret = hidrv_ppe_pf_ftable_set_egress_fd_key_tp(priv->ppe_pf_devid, HIDRV_PPE_FD_STAGE_1, tmplt_id, &egr_key_field);
    if (ret != 0) {
        hns5_err(dev, "set lsw egr fd key tp err %d\n", ret);
        return ret;
    }

    return 0;
}

int hns5_fdir_common_config(struct rte_eth_dev *dev)
{
    int ret = 0;
    struct hns5_eth_priv *priv = NULL;
    /*
     * Flow table TCAM resource allocation (hardware-verified, do not modify:
     * start index and segment size must not overlap and must stay in TCAM range)
     * start_index array index maps to hidrv_ppe_fd_tcam_type enum:
     * [0] = HIDRV_PPE_FD_LSW_INGRESS_STAGE1 (ingress flow table stage 1)
     * [1] = HIDRV_PPE_FD_LSW_INGRESS_STAGE2 (ingress flow table stage 2)
     * [2] = HIDRV_PPE_FD_LSW_EGRESS_STAGE1 (egress flow table stage 1)
     * [3] = HIDRV_PPE_FD_FW_FD_STAGE1 (fw fd flow table stage 1)
     * [4] = HIDRV_PPE_FD_FW_FD_STAGE2 (fw fd flow table stage 2)
     */
    uint32_t start_index[HIDRV_PPE_FD_TCAM_TYPE_MAX] = {
        100,  /* Ingress stage1 start index, range [100, 150) */
        0,    /* Ingress stage2 unused */
        0,    /* Egress stage1 unused */
        150,  /* Fw fd stage1 start index, range [150, 200) */
        0     /* Fw fd stage2 unused */
    };
    uint32_t entry_num[HIDRV_PPE_FD_TCAM_TYPE_MAX] = {
        50,   /* Ingress stage1 allocated 50 entries */
        0,    /* Ingress stage2 unused */
        0,    /* Egress stage1 unused */
        50,   /* Fw fd stage1 allocated 50 entries */
        0     /* Fw fd stage2 unused */
    };
    static bool is_inited = false;

    if (is_inited == true) {
        return 0;
    }

    priv = hns5_eth_priv_get(dev);

    /* Configure all flow table TCAM resources */
    ret = hidrv_ppe_pf_ftable_mixed_setup(priv->ppe_pf_devid, HNS5_FD_TABLE_ID, start_index, entry_num);
    if (ret != 0) {
        hns5_err(dev, "set ftable mixed setup failed, %d\n", ret);
        return -1;
    }

    /* Configure fw fd flow table key template */
    ret = hns5_ppe_ppp_set_fw_fd_key_tp(dev, HNS5_FW_FD_TP_ID);
    if (ret != 0) {
        hns5_err(dev, "hns5 set fw fd key tp failed, %d\n", ret);
        return ret;
    }

    /* Configure ingress flow table key template */
    ret = hns5_ppe_ppp_set_lsw_ing_fd_key_tp(dev, HNS5_LSW_ING_FD_TP_ID);
    if (ret != 0) {
        hns5_err(dev, "hns5 set lsw ing fd key tp failed, %d\n", ret);
        return ret;
    }

    /* Configure egress flow table key template */
    ret = hns5_ppe_ppp_set_lsw_egr_fd_key_tp(dev, HNS5_LSW_EGR_FD_TP_ID);
    if (ret != 0) {
        hns5_err(dev, "hns5 set lsw egr fd key tp failed, %d\n", ret);
        return ret;
    }

    is_inited = true;

    return 0;
}

static int hns5_ppe_ppp_cfg_fd_key_match_tp(struct rte_eth_dev *eth_dev)
{
    int ret;
    uint32_t tmplt_id = HNS5_FW_FD_TP_ID;
    struct hidrv_ppp_fd_key_tp_info tp_info = { 0 };
    static uint64_t fd_key_match_tp_mask = 0x0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(eth_dev);

    if (((fd_key_match_tp_mask >> tmplt_id) & 0x1) == 0) {
        tp_info.port_bmp.cpu_port_bitmap = 1ULL << priv->param.cport_id;
        tp_info.port_bmp.mac_port_bitmap = 1ULL << priv->param.mport_id;
        tp_info.l3_id = 0;
        tp_info.l3_id_msk = 0;
        tp_info.tp_id.fw_fd.tp0 = tmplt_id;
        tp_info.tp_id.fw_fd.tp1 = tmplt_id;
    } else {
        ret = hidrv_ppe_pf_ftable_get_fd_key_match_tp(priv->ppe_pf_devid, HIDRV_PPP_FW_TBL, tmplt_id, &tp_info);
        if (ret != 0) {
            hns5_err(eth_dev, "hns5 get fd key match tp failed, %d\n", ret);
            return ret;
        }
        tp_info.port_bmp.cpu_port_bitmap |= 1ULL << priv->param.cport_id;
        tp_info.port_bmp.mac_port_bitmap |= 1ULL << priv->param.mport_id;
    }

    ret = hidrv_ppe_pf_ftable_set_fd_key_match_tp(priv->ppe_pf_devid, HIDRV_PPP_FW_TBL, tmplt_id, &tp_info);
    if (ret != 0) {
        hns5_err(eth_dev, "hns5 set fd key match tp failed, %d\n", ret);
        return ret;
    }
    fd_key_match_tp_mask |= 1ULL << tmplt_id;

    return 0;
}

static int hns5_ppe_ppp_cfg_ingress_key_match_tp(struct rte_eth_dev *eth_dev)
{
    int ret;
    uint32_t tmplt_id = HNS5_LSW_ING_FD_TP_ID;
    struct hidrv_ppp_fd_key_tp_info tp_info = { 0 };
    static uint64_t ingress_key_match_tp_mask = 0x0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(eth_dev);

    if (((ingress_key_match_tp_mask >> tmplt_id) & 0x1) == 0) {
        tp_info.port_bmp.cpu_port_bitmap = 1ULL << priv->param.cport_id;
        tp_info.port_bmp.mac_port_bitmap = 1ULL << priv->param.mport_id;
        tp_info.l3_id = 0;
        tp_info.l3_id_msk = 0;
        tp_info.tp_id.ing_fd.tp0 = tmplt_id;
        tp_info.tp_id.ing_fd.tp1 = tmplt_id;
    } else {
        ret = hidrv_ppe_pf_ftable_get_fd_key_match_tp(priv->ppe_pf_devid, HIDRV_PPP_LSW_ING_TBL, tmplt_id, &tp_info);
        if (ret != 0) {
            hns5_err(eth_dev, "hns5 get ingress key match tp failed, %d\n", ret);
            return ret;
        }
        tp_info.port_bmp.cpu_port_bitmap |= 1ULL << priv->param.cport_id;
        tp_info.port_bmp.mac_port_bitmap |= 1ULL << priv->param.mport_id;
    }

    ret = hidrv_ppe_pf_ftable_set_fd_key_match_tp(priv->ppe_pf_devid, HIDRV_PPP_LSW_ING_TBL, tmplt_id, &tp_info);
    if (ret != 0) {
        hns5_err(eth_dev, "hns5 set ingress key match tp failed, %d\n", ret);
        return ret;
    }
    ingress_key_match_tp_mask |= 1ULL << tmplt_id;

    return 0;
}

static int hns5_ppe_ppp_cfg_egress_key_match_tp(struct rte_eth_dev *eth_dev)
{
    int ret;
    uint32_t tmplt_id = HNS5_LSW_EGR_FD_TP_ID;
    struct hidrv_ppp_fd_key_tp_info tp_info = { 0 };
    static uint64_t egress_key_match_tp_mask = 0x0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(eth_dev);

    if (((egress_key_match_tp_mask >> tmplt_id) & 0x1) == 0) {
        tp_info.port_bmp.cpu_port_bitmap = 1ULL << priv->param.cport_id;
        tp_info.port_bmp.mac_port_bitmap = 1ULL << priv->param.mport_id;
        tp_info.l3_id = 0;
        tp_info.l3_id_msk = 0;
        tp_info.tp_id.egr_fd.tp0 = tmplt_id;
    } else {
        ret = hidrv_ppe_pf_ftable_get_fd_key_match_tp(priv->ppe_pf_devid, HIDRV_PPP_LSW_EGR_TBL, tmplt_id, &tp_info);
        if (ret != 0) {
            hns5_err(eth_dev, "hns5 get egress key match tp failed, %d\n", ret);
            return ret;
        }
        tp_info.port_bmp.cpu_port_bitmap |= 1ULL << priv->param.cport_id;
        tp_info.port_bmp.mac_port_bitmap |= 1ULL << priv->param.mport_id;
    }

    ret = hidrv_ppe_pf_ftable_set_fd_key_match_tp(priv->ppe_pf_devid, HIDRV_PPP_LSW_EGR_TBL, tmplt_id, &tp_info);
    if (ret != 0) {
        hns5_err(eth_dev, "hns5 set egress key match tp failed, %d\n", ret);
        return ret;
    }
    egress_key_match_tp_mask |= 1ULL << tmplt_id;

    return 0;
}

int hns5_fdir_port_config(struct rte_eth_dev *dev)
{
    int ret = 0;
    struct hidrv_ppp_fw_tcam_bit_mode_cfg cfg = {0};
    struct hns5_fd_key_cfg *key_cfg = NULL;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    ret = hidrv_ppe_pf_get_fw_tcam_bit_mode(priv->ppe_pf_devid, &cfg);
    if (ret != 0) {
        hns5_err(dev, "hns5 get tcam bit mode failed, %d\n", ret);
        return -1;
    }
    priv->fdir.fd_cfg.fd_tcam_mode = cfg;
    priv->fdir.fd_cfg.rule_num[HNS5_FD_STAGE_1] = HNS5_FD_FW_FD_STAGE1_MAX_NUM; // Max flow table entries
    priv->fdir.fd_cfg.cnt_num[HNS5_FD_STAGE_1] = HNS5_FD_MAX_COUNTER; // Max flow table counters

    key_cfg = &priv->fdir.fd_cfg.key_cfg[HNS5_FD_STAGE_1];
    key_cfg->tuple_active = BIT(IN_DMAC) | BIT(IN_SMAC) | BIT(IN_VLAN_TAG_OUTER) | BIT(IN_VLAN_TAG_INNER) |
                            BIT(IN_ETH_TYPE) | BIT(IN_IP_TOS) | BIT(IN_IP_PROTO) | BIT(IN_SRC_IP) |
                            BIT(IN_DST_IP) | BIT(IN_SPORT) | BIT(IN_DPORT);

    key_cfg->meta_data_active = BIT(DST_PORT);

    /* Configure ingress flow table key template match table */
    ret = hns5_ppe_ppp_cfg_ingress_key_match_tp(dev);
    if (ret != 0) {
        hns5_err(dev, "hns5 config ingress key match tp failed, %d\n", ret);
        return ret;
    }

    /* Configure egress flow table key template match table */
    ret = hns5_ppe_ppp_cfg_egress_key_match_tp(dev);
    if (ret != 0) {
        hns5_err(dev, "hns5 config egress key match tp failed, %d\n", ret);
        return ret;
    }

    return hns5_ppe_ppp_cfg_fd_key_match_tp(dev);
}

static int fw_key_soft_to_hardware(struct rte_eth_dev *dev, struct ppe_fw_fd_key_ad *fw_info,
    struct hns5_ppp_fd_key *fd_key, union hidrv_ppe_fd_actn *ad)
{
    uint32_t i;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    union fw_key_word_info key_info = { 0 };
    union fw_key_word_info mask_info = { 0 };
    struct hidrv_ppp_fw_tcam_bit_mode_cfg bit_mode = { 0 };

    bit_mode = priv->fdir.fd_cfg.fd_tcam_mode;
    if (bit_mode.bit_mode_0 == HIDRV_PPP_FW_FD_320_BIT_MODE) {
        key_info.key320.des_port = fw_info->key.dst_port;
        key_info.key320.dmac_byte0 = fw_info->key.dmac[0];
        key_info.key320.dmac_byte1 = fw_info->key.dmac[1];
        key_info.key320.dmac_byte2 = fw_info->key.dmac[2];
        key_info.key320.dmac_byte3 = fw_info->key.dmac[3];
        key_info.key320.dmac_byte4 = fw_info->key.dmac[4];
        key_info.key320.dmac_byte5 = fw_info->key.dmac[5];

        mask_info.key320.des_port = fw_info->mask.dst_port;
        mask_info.key320.dmac_byte0 = fw_info->mask.dmac[0];
        mask_info.key320.dmac_byte1 = fw_info->mask.dmac[1];
        mask_info.key320.dmac_byte2 = fw_info->mask.dmac[2];
        mask_info.key320.dmac_byte3 = fw_info->mask.dmac[3];
        mask_info.key320.dmac_byte4 = fw_info->mask.dmac[4];
        mask_info.key320.dmac_byte5 = fw_info->mask.dmac[5];
    } else {
        RTE_LOG(ERR, PMD, "[dpdk_eth]ppp tcam bit mode is err is (%x)\n", bit_mode.bit_mode_0);
        return -1;
    }

    for (i = 0; i < HNS5_FD_DATA_WORD_MAX_NUM; i++) {
        fd_key->key[i] = key_info.word[i];
        fd_key->key_msk[i] = mask_info.word[i];
    }

    ad->fw_actn.next_step = fw_info->ad.next_step;
    ad->fw_actn.queue_id = fw_info->ad.queue_id;
    ad->fw_actn.static_cnt_idx = fw_info->ad.static_cnt_idx;
    ad->fw_actn.static_cnt_vld = 0x1;
    ad->fw_actn.next_key = 0x0;
    ad->fw_actn.queue_id_mode = 0x1;

    return 0;
}


static void show_rule_info(struct rte_eth_dev *dev, struct hns5_fdir_rule *rule)
{
    hns5_dbg(dev, "entry: %u\n"
              "dmac: %2hhx-%2hhx-%2hhx-%2hhx-%2hhx-%2hhx smac: %2hhx-%2hhx-%2hhx-%2hhx-%2hhx-%2hhx\n"
              "in_vlan_tag_outer: 0x%hx in_vlan_tag_inner: 0x%hx in_eth_type: 0x%hx in_ip_tos: 0x%hhx\n"
              "in_ip_proto: 0x%hhx in_sip: %u in_dip: %u\n"
              "in_sp: %hu in_dp:%hu src_port:%hu dst_port:%hu\n",
              rule->entry,
              rule->key_conf.key.dmac[0], rule->key_conf.key.dmac[1], rule->key_conf.key.dmac[2], // dmac addr 2
              rule->key_conf.key.dmac[3], rule->key_conf.key.dmac[4], rule->key_conf.key.dmac[5], // dmac addr 3,4,5
              rule->key_conf.key.smac[0], rule->key_conf.key.smac[1], rule->key_conf.key.smac[2], // smac addr 2
              rule->key_conf.key.smac[3], rule->key_conf.key.smac[4], rule->key_conf.key.smac[5], // smac addr 3,4,5
              rule->key_conf.key.in_vlan_tag_outer, rule->key_conf.key.in_vlan_tag_inner,
              rule->key_conf.key.in_eth_type, rule->key_conf.key.in_ip_tos, rule->key_conf.key.in_ip_proto,
              rule->key_conf.key.in_sip, rule->key_conf.key.in_dip,
              rule->key_conf.key.in_sp, rule->key_conf.key.in_dp,
              rule->key_conf.key.src_port, rule->key_conf.key.dst_port);
}

int hns5_fdir_filter_program(struct rte_eth_dev *dev, struct hns5_fdir_rule *rule, bool del)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hns5_ppp_fd_key fd_key = { 0 };
    union hidrv_ppe_fd_actn actn = { 0 };
    uint32_t entry = 0;

    if (del) {
        ret = hidrv_ppe_pf_ftable_mixed_del_entry(priv->ppe_pf_devid, HNS5_FD_TABLE_ID,
            HIDRV_PPE_FD_FW_FD_STAGE1, rule->entry);
        if (ret != 0) {
            hns5_err(dev, "hns5 failed to delete soft entry: %u ret = %d\n", rule->entry, ret);
            show_rule_info(dev, rule);
            return ret;
        }

        ret = hidrv_ppe_pf_ftable_mixed_del_hw_entry(priv->ppe_pf_devid, HNS5_FD_TABLE_ID,
            HIDRV_PPE_FD_FW_FD_STAGE1, rule->entry);
        if (ret != 0) {
            hns5_err(dev, "hns5 failed to delete hardware entry: %u ret = %d\n", rule->entry, ret);
            show_rule_info(dev, rule);
            return ret;
        }

        return 0;
    }

    ret = hidrv_ppe_pf_ftable_mixed_alloc_entry(priv->ppe_pf_devid, HNS5_FD_TABLE_ID, HIDRV_PPE_FD_FW_FD_STAGE1, &entry);
    if (ret != 0) {
        hns5_err(dev, "hns5 conflict with existing fdir entry(%u), %d", entry, ret);
        return -EINVAL;
    }

    ret = fw_key_soft_to_hardware(dev, &rule->key_conf, &fd_key, &actn);
    if (ret != 0) {
        hns5_err(dev, "hns5 fw key soft to hardware failed, %d", ret);
        return -EINVAL;
    }

    ret = hidrv_ppe_pf_ftable_mixed_set_entry(priv->ppe_pf_devid, HNS5_FD_TABLE_ID, HIDRV_PPE_FD_FW_FD_STAGE1, entry,
        fd_key.key, fd_key.key_msk, HNS5_FD_DATA_WORD_MAX_NUM, &actn);
    if (ret != 0) {
        hns5_err(dev, "hns5 failed to set fdir: %u, ret = %d\n", rule->entry, ret);
        show_rule_info(dev, rule);
        (void)hidrv_ppe_pf_ftable_mixed_del_entry(priv->ppe_pf_devid, HNS5_FD_TABLE_ID, HIDRV_PPE_FD_FW_FD_STAGE1, entry);
        return -EINVAL;
    }

    rule->entry = entry;

    return 0;
}
