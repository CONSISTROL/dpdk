/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */

#ifndef _HNS5_FDIR_H_
#define _HNS5_FDIR_H_

#include <stdint.h>
#include <stdbool.h>

#include <rte_flow.h>

#include "hidrv_ppe_pf_api.h"

#define HNS5_FW_FD_TP_ID 2 // Template ID of FW flow table key
#define HNS5_LSW_ING_FD_TP_ID 3 // Template ID of ingress LSW flow table key
#define HNS5_LSW_EGR_FD_TP_ID 4 // Template ID of egress LSW flow table key
#define HNS5_FD_TABLE_ID 0
#define HNS5_FD_START_INDEX 100 // Stage-1 FW flow table, start entry ID 100
#define HNS5_LSW_ING_START_INDEX 200 // Ingress LSW flow table, start entry ID 200
#define HNS5_LSW_EGR_START_INDEX 300 // Egress LSW flow table, start entry ID 300
#define HNS5_FD_MAX_NUM 100 // Stage-1 FW flow table, max 100 entries
#define HNS5_LSW_ING_MAX_NUM 100 // Ingress LSW flow table, max 100 entries
#define HNS5_LSW_EGR_MAX_NUM 100 // Egress LSW flow table, max 100 entries
#define HNS5_FD_MAX_COUNTER 160 // Number of flow table hit counters

/* Ingress flow table stage-1 init macros */
#define HNS5_INGRESS_STAGE1_START_INDEX 0
#define HNS5_INGRESS_STAGE1_MAX_NUM 128
/* Egress flow table stage-1 init macros */
#define HNS5_EGRESS_STAGE1_START_INDEX 128
#define HNS5_EGRESS_STAGE1_MAX_NUM 128
/* FW flow table stage-1 init macros */
#define HNS5_FD_FW_FD_STAGE1_START_INDEX 256
#define HNS5_FD_FW_FD_STAGE1_MAX_NUM 128

#define HNS5_VLAN_TAG_NUM_MAX 2
#define HNS5_RULE_FLAG_COUNTER 0x4
#define HNS5_RULE_FLAG_COUNTER_INDIR 0x8
#define HNS5_FD_DATA_WORD_MAX_NUM 13

struct hns5_fd_key_cfg {
    uint64_t tuple_active;
    uint32_t meta_data_active;
};

enum HNS5_FD_STAGE {
    HNS5_FD_STAGE_1,
    HNS5_FD_STAGE_2,
    HNS5_FD_STAGE_NUM,
};

enum HNS5_FD_ACTION {
    HNS5_FD_ACTION_ACCEPT_PACKET,
    HNS5_FD_ACTION_DROP_PACKET,
};

struct hns5_fd_cfg {
    struct hidrv_ppp_fw_tcam_bit_mode_cfg fd_tcam_mode;
    uint32_t rule_num[HNS5_FD_STAGE_NUM]; /* rule entry number */
    uint16_t cnt_num[HNS5_FD_STAGE_NUM];  /* rule hit counter number */
    struct hns5_fd_key_cfg key_cfg[HNS5_FD_STAGE_NUM];
};

/*
 *  A structure used to define fields of a FDIR related info.
 */
struct hns5_fdir_info {
    struct hns5_fd_cfg fd_cfg;
};

/* OT_XXX indicates tuples in tunnel header of tunnel packet
 * IN_XXX indicate tuples in tunneled header of tunnel packet or
 *           tuples of non-tunnel packet
 */
enum HNS5_FD_TUPLE {
    OT_DMAC,
    OT_SMAC,
    OT_VLAN_TAG_OUTER,
    OT_VLAN_TAG_INNER,
    OT_ETH_TYPE,
    OT_L2_USER_DEF,
    OT_IP_TOS,
    OT_IP_PROTO,
    OT_SRC_IP,
    OT_DST_IP,
    OT_L3_USER_DEF,
    OT_SPORT,
    OT_DPORT,
    OT_L4_USER_DEF,
    OT_TUN_VNI,
    OT_TUN_FLOW_ID,
    IN_DMAC,
    IN_SMAC,
    IN_VLAN_TAG_OUTER,
    IN_VLAN_TAG_INNER,
    IN_ETH_TYPE,
    IN_L2_USER_DEF,
    IN_IP_TOS,
    IN_IP_PROTO,
    IN_SRC_IP,
    IN_DST_IP,
    IN_L3_USER_DEF,
    IN_SPORT,
    IN_DPORT,
    IN_L4_USER_DEF,
    DSA_TAG,
    MAX_TUPLE,
};

enum HNS5_FD_META {
    PACKET_TYPE_ID,
    IP_FRAGMENT,
    ROCCE_TYPE,
    NEXT_KEY,
    OT_VLAN_NUM,
    IN_VLAN_NUM,
    SRC_PORT,
    DST_PORT,
    TUNNEL_PACKET,
    CRYPT_DEC,
    MPLS_PACKET,
    PACKET_TYPE_L3ID,
    PACKET_TYPE_L4ID,
    PACKET_TYPE_L5ID,
    SRC_SUBPORT_ID,
    DES_SUBPORT_ID,
    TEMPLATE_ID,
    MAX_MATA,
};

union fw_key_word_info {
    struct  __attribute__((packed)) {
        uint32_t dmac_byte5 : 8;
        uint32_t dmac_byte4 : 8;
        uint32_t dmac_byte3 : 8;
        uint32_t dmac_byte2 : 8;

        /* word1 */
        uint32_t dmac_byte1 : 8;
        uint32_t dmac_byte0 : 8;
        uint32_t rerv1 : 16;

        uint32_t rerv2[7];

        /* word9 */
        uint32_t rerv9 : 26;
        uint32_t des_port : 6;
    } key320;
    struct  __attribute__((packed)) {
        /* word0-word1 */
        uint64_t in_ip_tos : 8;
        uint64_t in_eth_type : 16;
        uint64_t in_vlan_tag_inner : 16;
        uint64_t in_vlan_tag_outer : 16;
        uint64_t dmac_byte5 : 8; //A5
        /* word2 */
        uint32_t dmac_byte4 : 8;
        uint32_t dmac_byte3 : 8;
        uint32_t dmac_byte2 : 8;
        uint32_t dmac_byte1 : 8;
        /* word3 */
        uint32_t dmac_byte0 : 8; //A0
        uint32_t resv0 : 24;
        /* word4 */
        uint32_t resv1 : 6;
        uint32_t des_port : 12;
        uint32_t src_port : 12;
        uint32_t in_vlan_num : 2;
    } key160;
    uint32_t word[13];
};

/* LSW ingress flow table KEY WORD INFO */
union ing_lsw_key_word_info {
    struct {
        /* word0 */
        uint32_t dmac_byte5 : 8;
        uint32_t dmac_byte4 : 8;
        uint32_t dmac_byte3 : 8;
        uint32_t dmac_byte2 : 8;
        /* word1 */
        uint32_t dmac_byte1 : 8;
        uint32_t dmac_byte0 : 8;
        uint32_t rsrv1 : 16;

        /* word2-word8 */
        uint32_t rsrv2[7];

        /* word9: rerv padding before des_port to make 320 bits */
        uint32_t rerv9 : 25;
        uint32_t ingress_port : 7;
    } key320;
    uint32_t word[13];
};


struct hns5_ppp_fd_key {
    uint32_t key[HNS5_FD_DATA_WORD_MAX_NUM];
    uint32_t key_msk[HNS5_FD_DATA_WORD_MAX_NUM];
};

struct fw_key_soft_info {
    uint8_t dmac[RTE_ETHER_ADDR_LEN] __rte_aligned(2);
    uint8_t smac[RTE_ETHER_ADDR_LEN] __rte_aligned(2);
    uint16_t in_vlan_tag_outer;
    uint16_t in_vlan_tag_inner;
    uint16_t in_eth_type;

    uint8_t in_ip_tos;
    uint8_t in_ip_proto;

    uint32_t in_sip;
    uint32_t in_dip;

    uint16_t in_sp;
    uint16_t in_dp;

    uint16_t src_port;
    uint16_t dst_port;

    uint8_t in_vlan_num;
};

struct fw_action {
    uint32_t next_step;
    uint32_t queue_id;
    uint32_t static_cnt_idx;
};

struct ppe_fw_fd_key_ad {
    struct fw_key_soft_info key;
    struct fw_key_soft_info mask;
    struct fw_action ad;
};

/* LSW flow table KEY/MASK structure (shared with FW table) */
struct ppe_lsw_fd_key_ad {
    struct fw_key_soft_info key;
    struct fw_key_soft_info mask;
    struct fw_action ad;
};

struct hns5_fdir_rule {
    struct ppe_fw_fd_key_ad key_conf;
    uint32_t entry; // fdir id
    uint32_t tuple_set;
    uint32_t mata_set;

    uint32_t flags;
    struct rte_flow_action_count act_cnt;

    uint8_t vlan_num;
    bool has_vlan_mask;
    bool has_vlan_key;
    bool has_more_vlan_mask;
    bool has_more_vlan_key;
};

/* Ingress LSW flow table rule structure */
struct hns5_lsw_ing_fdir_rule {
    struct ppe_lsw_fd_key_ad key_conf;
    uint32_t entry;      // flow table entry ID
    uint32_t tuple_set;  // tuple enable bitmap
    uint32_t mata_set;   // meta enable bitmap
    uint32_t flags;
    struct rte_flow_action_count act_cnt;
    uint8_t vlan_num;
    bool has_vlan_mask;
    bool has_vlan_key;
    bool has_more_vlan_mask;
    bool has_more_vlan_key;
};

/* Egress LSW flow table rule structure */
struct hns5_lsw_egr_fdir_rule {
    struct ppe_lsw_fd_key_ad key_conf;
    uint32_t entry;      // flow table entry ID
    uint32_t tuple_set;  // tuple enable bitmap
    uint32_t mata_set;   // meta enable bitmap
    uint32_t flags;
    struct rte_flow_action_count act_cnt;
    uint8_t vlan_num;
    bool has_vlan_mask;
    bool has_vlan_key;
    bool has_more_vlan_mask;
    bool has_more_vlan_key;
};

/* FDIR filter list structure */
struct hns5_fdir_rule_ele {
    struct hns5_fdir_rule fdir_conf;
};

int hns5_fdir_common_config(struct rte_eth_dev *eth_dev);
int hns5_fdir_port_config(struct rte_eth_dev *dev);
// Resolve mutual dependency with hns5_eth.h
struct hns5_eth_priv;
int hns5_fdir_filter_program(struct rte_eth_dev *dev, struct hns5_fdir_rule *rule, bool del);

/* Ingress LSW flow table configuration */
int hns5_lsw_ing_fdir_filter_program(struct rte_eth_dev *dev, struct hns5_lsw_ing_fdir_rule *rule, bool del);

/* Egress LSW flow table configuration */
int hns5_lsw_egr_fdir_filter_program(struct rte_eth_dev *dev, struct hns5_lsw_egr_fdir_rule *rule, bool del);

#endif /* _HNS5_FDIR_H_ */
