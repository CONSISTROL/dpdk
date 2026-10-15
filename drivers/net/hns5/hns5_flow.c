/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include <stdlib.h>

#include <rte_flow_driver.h>
#include <rte_io.h>
#include <rte_malloc.h>
#include "hns5_log.h"
#include "hns5_eth.h"

/* Special Filter id for non-specific packet flagging. Don't change value */
#define ETHER_TYPE_MASK 0xFFFF
#define IPPROTO_MASK 0xFF

static enum rte_flow_item_type hns5_items[] = {
    RTE_FLOW_ITEM_TYPE_PORT_ID,
    RTE_FLOW_ITEM_TYPE_ETH,
    RTE_FLOW_ITEM_TYPE_VLAN,
    RTE_FLOW_ITEM_TYPE_IPV4,
    RTE_FLOW_ITEM_TYPE_IPV6,
    RTE_FLOW_ITEM_TYPE_UDP,
    RTE_FLOW_ITEM_TYPE_TCP,
    RTE_FLOW_ITEM_TYPE_SCTP
};

static enum rte_flow_item_type first_items[] = {
    RTE_FLOW_ITEM_TYPE_ETH,
    RTE_FLOW_ITEM_TYPE_VLAN,
    RTE_FLOW_ITEM_TYPE_IPV4,
    RTE_FLOW_ITEM_TYPE_IPV6,
    RTE_FLOW_ITEM_TYPE_UDP,
    RTE_FLOW_ITEM_TYPE_TCP,
    RTE_FLOW_ITEM_TYPE_SCTP
};

static enum rte_flow_item_type L2_next_items[] = {
    RTE_FLOW_ITEM_TYPE_VLAN,
    RTE_FLOW_ITEM_TYPE_IPV4,
    RTE_FLOW_ITEM_TYPE_IPV6
};

static enum rte_flow_item_type L3_next_items[] = {
    RTE_FLOW_ITEM_TYPE_UDP,
    RTE_FLOW_ITEM_TYPE_TCP,
    RTE_FLOW_ITEM_TYPE_SCTP
};

struct items_step_mngr {
    enum rte_flow_item_type *items;
    size_t count;
};

static inline struct hns5_flow_counter *hns5_counter_lookup(struct rte_eth_dev *dev, uint32_t id)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hns5_flow_counter *cnt;

    LIST_FOREACH(cnt, &priv->flow_counters, next) {
        if (cnt->id == id) {
            return cnt;
        }
    }
    return NULL;
}

static int hns5_counter_new(struct rte_eth_dev *dev, uint32_t indirect, uint32_t id, struct rte_flow_error *error)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hns5_flow_counter *cnt;
    unsigned long long value;
    int ret;

    cnt = hns5_counter_lookup(dev, id);
    if (cnt != NULL) {
        if (!cnt->indirect || cnt->indirect != indirect) {
            return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ACTION_CONF, cnt,
                                      "Counter id is used, indirect flag not match");
        }
        /* Clear the indirect counter on first use. */
        if (cnt->indirect && cnt->ref_cnt == 1) {
            ret = hidrv_ppe_pf_get_fd_flow_hit_cnt(priv->ppe_pf_devid, HIDRV_PPE_FD_FW_FD_STAGE1, id, &value); // check read-clear
            if (ret != 0) {
                return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_HANDLE, cnt, "get counter value failed");
            }
        }
        cnt->ref_cnt++;
        return 0;
    }

    /* Clear the counter by read ops because the counter is read-clear */
    ret = hidrv_ppe_pf_get_fd_flow_hit_cnt(priv->ppe_pf_devid, HIDRV_PPE_FD_FW_FD_STAGE1, id, &value);
    if (ret != 0) {
        return rte_flow_error_set(error, EIO, RTE_FLOW_ERROR_TYPE_HANDLE, NULL, "Clear counter failed!");
    }

    cnt = rte_zmalloc("hns5 fd counter", sizeof(*cnt), 0);
    if (cnt == NULL) {
        return rte_flow_error_set(error, ENOMEM, RTE_FLOW_ERROR_TYPE_HANDLE, cnt, "Alloc mem for counter failed");
    }
    cnt->id = id;
    cnt->indirect = indirect;
    cnt->ref_cnt = 1;
    cnt->hits = 0;
    LIST_INSERT_HEAD(&priv->flow_counters, cnt, next);

    return 0;
}

static int hns5_counter_query(struct rte_eth_dev *dev, struct rte_flow *flow, struct rte_flow_query_count *qc,
    struct rte_flow_error *error)
{
    struct hns5_flow_counter *cnt;
    unsigned long long hit_cnt;
    int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    cnt = hns5_counter_lookup(dev, flow->counter_id);
    if (cnt == NULL) {
        return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_HANDLE, NULL, "Can't find counter id");
    }

    ret = hidrv_ppe_pf_get_fd_flow_hit_cnt(priv->ppe_pf_devid, HIDRV_PPE_FD_FW_FD_STAGE1, flow->counter_id, &hit_cnt);
    if (ret != 0) {
        rte_flow_error_set(error, -ret, RTE_FLOW_ERROR_TYPE_HANDLE, NULL, "Read counter fail.");
        return ret;
    }
    qc->hits_set = 1;
    qc->hits = hit_cnt;
    qc->bytes_set = 0;
    qc->bytes = 0;

    return 0;
}

static int hns5_counter_release(struct rte_eth_dev *dev, uint32_t id)
{
    struct hns5_flow_counter *cnt;

    cnt = hns5_counter_lookup(dev, id);
    if (cnt == NULL) {
        hns5_err(dev, "Can't find available counter to release");
        return -EINVAL;
    }
    cnt->ref_cnt--;
    if (cnt->ref_cnt == 0) {
        LIST_REMOVE(cnt, next);
        rte_free(cnt);
    }
    return 0;
}
static int hns5_handle_action_queue(struct rte_eth_dev *dev __rte_unused, const struct rte_flow_action *action,
    struct hns5_fdir_rule *rule, struct rte_flow_error *error __rte_unused)
{
    const struct rte_flow_action_queue *queue;

    queue = (const struct rte_flow_action_queue *)action->conf;

    rule->key_conf.ad.next_step = HNS5_FD_ACTION_ACCEPT_PACKET;
    rule->key_conf.ad.queue_id = queue->index;

    return 0;
}

static __rte_unused int hns5_handle_action_indirect(struct rte_eth_dev *dev, const struct rte_flow_action *action,
    struct hns5_fdir_rule *rule, struct rte_flow_error *error)
{
    const struct rte_flow_action_handle *indir = action->conf;

    if (indir->indirect_type != HNS5_INDIRECT_ACTION_TYPE_COUNT) {
        return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ACTION_CONF, action, "Invalid indirect type");
    }

    if (hns5_counter_lookup(dev, indir->counter_id) == NULL) {
        return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ACTION_CONF, action, "Counter id not exist");
    }

    rule->key_conf.ad.static_cnt_idx = indir->counter_id;
    rule->act_cnt.id = indir->counter_id;
    rule->flags |= (HNS5_RULE_FLAG_COUNTER | HNS5_RULE_FLAG_COUNTER_INDIR);

    return 0;
}

/*
 * Parse actions structure from the provided pattern.
 * The pattern is validated as the items are copied.
 *
 * @param actions[in]
 * @param rule[out]
 *   NIC specific actions derived from the actions.
 * @param error[out]
 */
static int hns5_handle_actions(struct rte_eth_dev *dev, const struct rte_flow_action actions[],
    struct hns5_fdir_rule *rule, struct rte_flow_error *error)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    const struct rte_flow_action_count *act_count;
    uint32_t counter_num;
    int ret;

    for (; actions->type != RTE_FLOW_ACTION_TYPE_END; actions++) {
        switch (actions->type) {
            case RTE_FLOW_ACTION_TYPE_QUEUE:
                ret = hns5_handle_action_queue(dev, actions, rule, error);
                if (ret != 0) {
                    return ret;
                }
                break;
            case RTE_FLOW_ACTION_TYPE_DROP:
                rule->key_conf.ad.next_step = HNS5_FD_ACTION_DROP_PACKET;
                break;
            case RTE_FLOW_ACTION_TYPE_COUNT:
                act_count = (const struct rte_flow_action_count *)actions->conf;
                counter_num = priv->fdir.fd_cfg.cnt_num[HNS5_FD_STAGE_1];
                if (act_count->id >= counter_num) {
                    return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ACTION_CONF, actions,
                                              "Invalid counter id");
                }
                rule->key_conf.ad.static_cnt_idx = act_count->id;
                rule->act_cnt = *act_count;
                rule->flags |= HNS5_RULE_FLAG_COUNTER;
                rule->flags &= ~HNS5_RULE_FLAG_COUNTER_INDIR;
                break;
            case RTE_FLOW_ACTION_TYPE_VOID:
                break;
            default:
                return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ACTION, NULL, "Unsupported action");
        }
    }

    return 0;
}

static int hns5_check_attr(const struct rte_flow_attr *attr, struct rte_flow_error *error)
{
    if (!attr->ingress) {
        return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ATTR_INGRESS, attr, "Ingress can't be zero");
    }
    if (attr->egress) {
        return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ATTR_EGRESS, attr, "Not support egress");
    }
    if (attr->transfer) {
        return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ATTR_TRANSFER, attr, "No support for transfer");
    }
    if (attr->group) {
        return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ATTR_GROUP, attr, "Not support group");
    }
    return 0;
}

static int hns5_check_tuple(struct rte_eth_dev *dev, const struct hns5_fdir_rule *rule, struct rte_flow_error *error)
{
    /* Refer to enum HNS5_FD_TUPLE */
    const char *const err_msg[] = {
        "Not support outer dst mac",
        "Not support outer src mac",
        "Not support outer vlan tag outer",
        "Not support outer vlan tag inner",
        "Not support outer eth type",
        "Not support outer l2 user def",
        "Not support outer ip tos",
        "Not support outer ip proto",
        "Not support outer src ip",
        "Not support outer dst ip",
        "Not support outer l3 user def",
        "Not support outer src port",
        "Not support outer dst port",
        "Not support outer l4 user def",
        "Not support outer tun vni",
        "Not support outer tun flow id",
        "Not support inner dst mac",
        "Not support inner src mac",
        "Not support inner vlan tag outer",
        "Not support inner vlan tag inner",
        "Not support inner eth type",
        "Not support inner l2 user def",
        "Not support inner ip tos",
        "Not support inner ip proto",
        "Not support inner src ip",
        "Not support inner dst ip",
        "Not support inner l3 user def",
        "Not support inner src port",
        "Not support inner dst port",
        "Not support inner l4 user def",
        "Not support dsa tag",
    };
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    uint64_t tuple_active = priv->fdir.fd_cfg.key_cfg[HNS5_FD_STAGE_1].tuple_active;
    uint32_t i;

    for (i = 0; i < MAX_TUPLE; i++) {
        if ((rule->tuple_set & BIT(i)) == 0) {
            continue;
        }
        if (tuple_active & BIT(i)) {
            continue;
        }
        hns5_err(dev, "hns5_check_tuple: unsupported tuple bit %u, err=%s\n", i, err_msg[i]);
        return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ITEM, NULL, err_msg[i]);
    }

    return 0;
}

static int hns5_check_meta(struct rte_eth_dev *dev, const struct hns5_fdir_rule *rule, struct rte_flow_error *error)
{
    /* Refer to enum HNS5_FD_META */
    const char *const err_msg[] = {
        "Not support packet type id",
        "Not support ip fragement",
        "Not support rocee type",
        "Not support next key",
        "Not support outer vlan num",
        "Not support inner vlan num",
        "Not support source port",
        "Not support dest port",
        "Not support tunnel packet",
        "Not support crypt dec",
        "Not support mpls packet",
        "Not support packet type l3id",
        "Not support packet type l4id",
        "Not support packet type l5id",
        "Not support src subport id",
        "Not support des subport id",
        "Not support template id",
    };
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    uint32_t meta_active = priv->fdir.fd_cfg.key_cfg[HNS5_FD_STAGE_1].meta_data_active;
    uint32_t i;

    for (i = 0; i < MAX_MATA; i++) {
        if ((rule->mata_set & BIT(i)) == 0) {
            continue;
        }
        if (meta_active & BIT(i)) {
            continue;
        }
        return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ITEM, NULL, err_msg[i]);
    }

    return 0;
}

static int hns5_parse_port_id(const struct rte_flow_item *item, struct hns5_fdir_rule *rule,
                              struct rte_flow_error *error __rte_unused)
{
    const struct rte_flow_item_port_id *port_spec;
    const struct rte_flow_item_port_id *port_mask;

    /* Only used to describe the protocol stack. */
    if (item->spec == NULL && item->mask == NULL) {
        return 0;
    }

    port_mask = item->mask;
    if (port_mask != NULL) {
        hns5_set_bit(rule->mata_set, DST_PORT, 1);
        rule->key_conf.mask.dst_port = port_mask->id;
    }

    port_spec = item->spec;
    if (port_spec != NULL) {
        rule->key_conf.key.dst_port = port_spec->id;
    }

    return 0;
}

static int hns5_parse_eth(const struct rte_flow_item *item, struct hns5_fdir_rule *rule,
                          struct rte_flow_error *error __rte_unused)
{
    const struct rte_flow_item_eth *eth_spec = NULL;
    const struct rte_flow_item_eth *eth_mask = NULL;

    /* Only used to describe the protocol stack. */
    if (item->spec == NULL && item->mask == NULL) {
        return 0;
    }

    eth_mask = item->mask;
    if (eth_mask != NULL) {
        if (eth_mask->type != 0) {
            hns5_set_bit(rule->tuple_set, IN_ETH_TYPE, 1);
            rule->key_conf.mask.in_eth_type = rte_be_to_cpu_16(eth_mask->type);
        }
        if (rte_is_zero_ether_addr(&eth_mask->src) == 0) {
            hns5_set_bit(rule->tuple_set, IN_SMAC, 1);
            memcpy(rule->key_conf.mask.smac, eth_mask->src.addr_bytes, RTE_ETHER_ADDR_LEN);
        }
        if (rte_is_zero_ether_addr(&eth_mask->dst) == 0) {
            hns5_set_bit(rule->tuple_set, IN_DMAC, 1);
            memcpy(rule->key_conf.mask.dmac, eth_mask->dst.addr_bytes, RTE_ETHER_ADDR_LEN);
        }
        if (eth_mask->has_vlan != 0) {
            rule->has_vlan_mask = true;
        }
    }

    eth_spec = item->spec;
    if ((eth_mask != NULL) && (eth_mask->has_vlan != 0) && (eth_spec->has_vlan != 0)) {
        rule->vlan_num++;
        rule->has_vlan_key = true;
    }

    rule->key_conf.key.in_eth_type = rte_be_to_cpu_16(eth_spec->type);
    memcpy(rule->key_conf.key.smac, eth_spec->src.addr_bytes, RTE_ETHER_ADDR_LEN);
    memcpy(rule->key_conf.key.dmac, eth_spec->dst.addr_bytes, RTE_ETHER_ADDR_LEN);

    return 0;
}

static int hns5_parse_vlan_key_mask(const struct rte_flow_item *item,
    struct hns5_fdir_rule *rule, struct rte_flow_error *error)
{
    const struct rte_flow_item_vlan *vlan_spec = NULL;
    const struct rte_flow_item_vlan *vlan_mask = NULL;

    /* Only used to describe the protocol stack. */
    if (item->spec == NULL && item->mask == NULL) {
        return 0;
    }

    vlan_mask = item->mask;
    if (vlan_mask != NULL) {
        if (vlan_mask->tci) {
            if (rule->vlan_num == 1) {
                hns5_set_bit(rule->tuple_set, IN_VLAN_TAG_OUTER, 1);
                rule->key_conf.mask.in_vlan_tag_outer = rte_be_to_cpu_16(vlan_mask->tci);
            } else {
                hns5_set_bit(rule->tuple_set, IN_VLAN_TAG_INNER, 1);
                rule->key_conf.mask.in_vlan_tag_inner = rte_be_to_cpu_16(vlan_mask->tci);
            }
        }
        if (vlan_mask->has_more_vlan) {
            rule->has_more_vlan_mask = true;
        }
    }

    vlan_spec = item->spec;
    if (rule->vlan_num == 1) {
        rule->key_conf.key.in_vlan_tag_outer = rte_be_to_cpu_16(vlan_spec->tci);
    } else {
        rule->key_conf.key.in_vlan_tag_inner = rte_be_to_cpu_16(vlan_spec->tci);
    }

    if ((vlan_mask != NULL) && vlan_mask->has_more_vlan && vlan_spec->has_more_vlan) {
        rule->vlan_num++;
        if (rule->vlan_num > HNS5_VLAN_TAG_NUM_MAX) {
            return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM, item, "Vlan_num is more than 2");
        }
        rule->has_more_vlan_key = true;
    }

    return 0;
}

static int hns5_parse_vlan(const struct rte_flow_item *item,
    struct hns5_fdir_rule *rule, struct rte_flow_error *error)
{
    if (rule->has_vlan_mask && !rule->has_vlan_key) {
        return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM, item,
                                  "VLAN item is conflict with 'has_vlan is 0' in ETH item");
    }

    if (rule->has_more_vlan_mask && !rule->has_more_vlan_key) {
        return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM, item,
                                  "VLAN item is conflict with 'has_more_vlan is 0' in the previous VLAN item");
    }

    if (rule->has_vlan_mask && rule->has_vlan_key) {
        rule->has_vlan_mask = false;
        rule->vlan_num--;
    }

    if (rule->has_more_vlan_mask && rule->has_more_vlan_key) {
        rule->has_more_vlan_mask = false;
        rule->vlan_num--;
    }

    rule->vlan_num++;
    if (rule->vlan_num > HNS5_VLAN_TAG_NUM_MAX) {
        return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM, item, "Vlan_num is more than 2");
    }

    return hns5_parse_vlan_key_mask(item, rule, error);
}

static bool hns5_check_ipv4_mask_supported(const struct rte_flow_item_ipv4 *ipv4_mask)
{
    if (ipv4_mask->hdr.total_length || ipv4_mask->hdr.packet_id || ipv4_mask->hdr.fragment_offset ||
        ipv4_mask->hdr.time_to_live || ipv4_mask->hdr.hdr_checksum) {
        return false;
    }

    return true;
}

static int hns5_parse_ipv4(const struct rte_flow_item *item,
    struct hns5_fdir_rule *rule, struct rte_flow_error *error)
{
    const struct rte_flow_item_ipv4 *ipv4_spec = NULL;
    const struct rte_flow_item_ipv4 *ipv4_mask = NULL;

    hns5_set_bit(rule->tuple_set, IN_ETH_TYPE, 1);
    rule->key_conf.key.in_eth_type = RTE_ETHER_TYPE_IPV4;
    rule->key_conf.mask.in_eth_type = ETHER_TYPE_MASK;

    /* Only used to describe the protocol stack. */
    if (item->spec == NULL && item->mask == NULL) {
        return 0;
    }

    if (item->mask != NULL) {
        ipv4_mask = item->mask;
        if (!hns5_check_ipv4_mask_supported(ipv4_mask)) {
            return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM_MASK, item,
                                      "Only support src & dst ip,tos,proto in IPV4");
        }

        if (ipv4_mask->hdr.src_addr != 0) {
            hns5_set_bit(rule->tuple_set, IN_SRC_IP, 1);
            rule->key_conf.mask.in_sip = rte_be_to_cpu_32(ipv4_mask->hdr.src_addr);
        }

        if (ipv4_mask->hdr.dst_addr != 0) {
            hns5_set_bit(rule->tuple_set, IN_DST_IP, 1);
            rule->key_conf.mask.in_dip = rte_be_to_cpu_32(ipv4_mask->hdr.dst_addr);
        }

        if (ipv4_mask->hdr.type_of_service != 0) {
            hns5_set_bit(rule->tuple_set, IN_IP_TOS, 1);
            rule->key_conf.mask.in_ip_tos = ipv4_mask->hdr.type_of_service;
        }

        if (ipv4_mask->hdr.next_proto_id != 0) {
            hns5_set_bit(rule->tuple_set, IN_IP_PROTO, 1);
            rule->key_conf.mask.in_ip_proto = ipv4_mask->hdr.next_proto_id;
        }
    } else {
        hns5_set_bit(rule->tuple_set, IN_SRC_IP, 1);
        rule->key_conf.mask.in_sip = UINT32_MAX;
    }

    ipv4_spec = item->spec;
    rule->key_conf.key.in_sip = rte_be_to_cpu_32(ipv4_spec->hdr.src_addr);
    rule->key_conf.key.in_dip = rte_be_to_cpu_32(ipv4_spec->hdr.dst_addr);
    rule->key_conf.key.in_ip_tos = ipv4_spec->hdr.type_of_service;
    rule->key_conf.key.in_ip_proto = ipv4_spec->hdr.next_proto_id;

    return 0;
}

static int hns5_parse_ipv6(const struct rte_flow_item *item,
    struct hns5_fdir_rule *rule, struct rte_flow_error *error)
{
    const struct rte_flow_item_ipv6 *ipv6_spec = NULL;
    const struct rte_flow_item_ipv6 *ipv6_mask = NULL;

    hns5_set_bit(rule->tuple_set, IN_ETH_TYPE, 1);
    rule->key_conf.key.in_eth_type = RTE_ETHER_TYPE_IPV6;
    rule->key_conf.mask.in_eth_type = ETHER_TYPE_MASK;

    /* Only used to describe the protocol stack. */
    if (item->spec == NULL && item->mask == NULL) {
        return 0;
    }

    if (item->mask != NULL) {
        ipv6_mask = item->mask;
        if (ipv6_mask->hdr.vtc_flow || ipv6_mask->hdr.payload_len || ipv6_mask->hdr.hop_limits) {
            return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM_MASK, item,
                                      "Only support src & dst ip,proto in IPV6");
        }
        rule->key_conf.mask.in_sip = rte_be_to_cpu_32(*(const rte_be32_t *)ipv6_mask->hdr.src_addr);
        rule->key_conf.mask.in_dip = rte_be_to_cpu_32(*(const rte_be32_t *)ipv6_mask->hdr.dst_addr);
        rule->key_conf.mask.in_ip_proto = ipv6_mask->hdr.proto;
        if (rule->key_conf.mask.in_sip) {
            hns5_set_bit(rule->tuple_set, IN_SRC_IP, 1);
        }
        if (rule->key_conf.mask.in_dip) {
            hns5_set_bit(rule->tuple_set, IN_DST_IP, 1);
        }
        if (ipv6_mask->hdr.proto) {
            hns5_set_bit(rule->tuple_set, IN_IP_PROTO, 1);
        }
    }

    ipv6_spec = item->spec;
    rule->key_conf.key.in_sip = rte_be_to_cpu_32(*(const rte_be32_t *)ipv6_spec->hdr.src_addr);
    rule->key_conf.key.in_dip = rte_be_to_cpu_32(*(const rte_be32_t *)ipv6_spec->hdr.dst_addr);
    rule->key_conf.key.in_ip_proto = ipv6_spec->hdr.proto;

    return 0;
}

static bool hns5_check_tcp_mask_supported(const struct rte_flow_item_tcp *tcp_mask)
{
    if (tcp_mask->hdr.sent_seq || tcp_mask->hdr.recv_ack || tcp_mask->hdr.data_off || tcp_mask->hdr.tcp_flags ||
        tcp_mask->hdr.rx_win || tcp_mask->hdr.cksum || tcp_mask->hdr.tcp_urp) {
        return false;
    }

    return true;
}

static int hns5_parse_tcp(const struct rte_flow_item *item, struct hns5_fdir_rule *rule, struct rte_flow_error *error)
{
    const struct rte_flow_item_tcp *tcp_spec = NULL;
    const struct rte_flow_item_tcp *tcp_mask = NULL;

    hns5_set_bit(rule->tuple_set, IN_IP_PROTO, 1);
    rule->key_conf.key.in_ip_proto = IPPROTO_TCP;
    rule->key_conf.mask.in_ip_proto = IPPROTO_MASK;

    /* Only used to describe the protocol stack. */
    if (item->spec == NULL && item->mask == NULL) {
        return 0;
    }

    if (item->mask != NULL) {
        tcp_mask = item->mask;
        if (!hns5_check_tcp_mask_supported(tcp_mask)) {
            return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM_MASK, item,
                                      "Only support src & dst port in TCP");
        }

        if (tcp_mask->hdr.src_port) {
            hns5_set_bit(rule->tuple_set, IN_SPORT, 1);
            rule->key_conf.mask.in_sp = rte_be_to_cpu_16(tcp_mask->hdr.src_port);
        }
        if (tcp_mask->hdr.dst_port) {
            hns5_set_bit(rule->tuple_set, IN_DPORT, 1);
            rule->key_conf.mask.in_dp = rte_be_to_cpu_16(tcp_mask->hdr.dst_port);
        }
    }

    tcp_spec = item->spec;
    rule->key_conf.key.in_sp = rte_be_to_cpu_16(tcp_spec->hdr.src_port);
    rule->key_conf.key.in_dp = rte_be_to_cpu_16(tcp_spec->hdr.dst_port);

    return 0;
}

static int hns5_parse_udp(const struct rte_flow_item *item, struct hns5_fdir_rule *rule, struct rte_flow_error *error)
{
    const struct rte_flow_item_udp *udp_spec = NULL;
    const struct rte_flow_item_udp *udp_mask = NULL;

    hns5_set_bit(rule->tuple_set, IN_IP_PROTO, 1);
    rule->key_conf.key.in_ip_proto = IPPROTO_UDP;
    rule->key_conf.mask.in_ip_proto = IPPROTO_MASK;

    /* Only used to describe the protocol stack. */
    if (item->spec == NULL && item->mask == NULL) {
        return 0;
    }

    if (item->mask != NULL) {
        udp_mask = item->mask;
        if (udp_mask->hdr.dgram_len || udp_mask->hdr.dgram_cksum) {
            return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM_MASK, item,
                                      "Only support src & dst port in UDP");
        }
        if (udp_mask->hdr.src_port) {
            hns5_set_bit(rule->tuple_set, IN_SPORT, 1);
            rule->key_conf.mask.in_sp = rte_be_to_cpu_16(udp_mask->hdr.src_port);
        }
        if (udp_mask->hdr.dst_port) {
            hns5_set_bit(rule->tuple_set, IN_DPORT, 1);
            rule->key_conf.mask.in_dp = rte_be_to_cpu_16(udp_mask->hdr.dst_port);
        }
    }

    udp_spec = item->spec;
    rule->key_conf.key.in_sp = rte_be_to_cpu_16(udp_spec->hdr.src_port);
    rule->key_conf.key.in_dp = rte_be_to_cpu_16(udp_spec->hdr.dst_port);

    return 0;
}

static int hns5_parse_sctp(const struct rte_flow_item *item,
    struct hns5_fdir_rule *rule, struct rte_flow_error *error)
{
    const struct rte_flow_item_sctp *sctp_spec = NULL;
    const struct rte_flow_item_sctp *sctp_mask = NULL;

    hns5_set_bit(rule->tuple_set, IN_IP_PROTO, 1);
    rule->key_conf.key.in_ip_proto = IPPROTO_SCTP;
    rule->key_conf.mask.in_ip_proto = IPPROTO_MASK;

    /* Only used to describe the protocol stack. */
    if (item->spec == NULL && item->mask == NULL) {
        return 0;
    }

    if (item->mask != NULL) {
        sctp_mask = item->mask;
        if (sctp_mask->hdr.cksum)
            return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM_MASK, item,
                                      "Only support src & dst port in SCTP");
        if (sctp_mask->hdr.src_port) {
            hns5_set_bit(rule->tuple_set, IN_SPORT, 1);
            rule->key_conf.mask.in_sp = rte_be_to_cpu_16(sctp_mask->hdr.src_port);
        }
        if (sctp_mask->hdr.dst_port) {
            hns5_set_bit(rule->tuple_set, IN_DPORT, 1);
            rule->key_conf.mask.in_dp = rte_be_to_cpu_16(sctp_mask->hdr.dst_port);
        }
    }

    sctp_spec = item->spec;
    rule->key_conf.key.in_sp = rte_be_to_cpu_16(sctp_spec->hdr.src_port);
    rule->key_conf.key.in_dp = rte_be_to_cpu_16(sctp_spec->hdr.dst_port);

    return 0;
}

struct parse_item {
    enum rte_flow_item_type type;
    int (*parse_func)(const struct rte_flow_item *item, struct hns5_fdir_rule *rule, struct rte_flow_error *error);
    struct items_step_mngr mngr;
};

struct parse_item g_parse_items[] = {
    [RTE_FLOW_ITEM_TYPE_PORT_ID] = {RTE_FLOW_ITEM_TYPE_PORT_ID, hns5_parse_port_id,
                                    {first_items, RTE_DIM(first_items)}},
    [RTE_FLOW_ITEM_TYPE_ETH] = {RTE_FLOW_ITEM_TYPE_ETH, hns5_parse_eth, {L2_next_items, RTE_DIM(L2_next_items)}},
    [RTE_FLOW_ITEM_TYPE_VLAN] = {RTE_FLOW_ITEM_TYPE_VLAN, hns5_parse_vlan, {L2_next_items, RTE_DIM(L2_next_items)}},
    [RTE_FLOW_ITEM_TYPE_IPV4] = {RTE_FLOW_ITEM_TYPE_IPV4, hns5_parse_ipv4, {L3_next_items, RTE_DIM(L3_next_items)}},
    [RTE_FLOW_ITEM_TYPE_IPV6] = {RTE_FLOW_ITEM_TYPE_IPV6, hns5_parse_ipv6, {L3_next_items, RTE_DIM(L3_next_items)}},
    [RTE_FLOW_ITEM_TYPE_TCP] = {RTE_FLOW_ITEM_TYPE_TCP, hns5_parse_tcp, {L3_next_items, RTE_DIM(L3_next_items)}},
    [RTE_FLOW_ITEM_TYPE_UDP] = {RTE_FLOW_ITEM_TYPE_UDP, hns5_parse_udp, {L3_next_items, RTE_DIM(L3_next_items)}},
    [RTE_FLOW_ITEM_TYPE_SCTP] = {RTE_FLOW_ITEM_TYPE_SCTP, hns5_parse_sctp, {L3_next_items, RTE_DIM(L3_next_items)}},
};

static int hns5_parse_normal(const struct rte_flow_item *item, struct hns5_fdir_rule *rule,
    struct items_step_mngr *step_mngr, struct rte_flow_error *error)
{
    int ret;
    struct parse_item *parse_item = NULL;

    if (item->spec == NULL && item->mask != NULL) {
        return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM, item,
                                  "Can't configure FDIR with mask but without spec");
    }

    if (item->type > RTE_FLOW_ITEM_TYPE_SCTP || item->type < 0) {
        return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ITEM, NULL, "Unsupported normal type!");
    }

    parse_item = &g_parse_items[item->type];
    if (parse_item == NULL) {
        return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ITEM, NULL, "Failed find parse item!");
    }

    ret = parse_item->parse_func(item, rule, error);
    step_mngr->items = parse_item->mngr.items;
    step_mngr->count = parse_item->mngr.count;

    return ret;
}

static int hns5_validate_item(const struct rte_flow_item *item, struct items_step_mngr step_mngr,
    struct rte_flow_error *error)
{
    uint32_t i;

    if (item->last != NULL) {
        return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ITEM_LAST, item,
                                  "Not supported last point for range");
    }

    for (i = 0; i < step_mngr.count; i++) {
        if (item->type == step_mngr.items[i]) {
            break;
        }
    }

    if (i == step_mngr.count) {
        return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM, item, "Inval or missing item");
    }
    return 0;
}

/*
 * Parse the flow director rule.
 * The supported PATTERN:
 *   case: non-tunnel packet:
 *     ETH : src-mac, dst-mac, ethertype
 *     VLAN: tag1, tag2
 *     IPv4: src-ip, dst-ip, tos, proto
 *     IPv6: src-ip(last 32 bit addr), dst-ip(last 32 bit addr), proto
 *     UDP : src-port, dst-port
 *     TCP : src-port, dst-port
 *     SCTP: src-port, dst-port
 * The supported ACTION:
 *    QUEUE
 *    DROP
 *    COUNT
 */
static int hns5_parse_fdir_filter(struct rte_eth_dev *dev, const struct rte_flow_attr *attr __rte_unused,
                                  const struct rte_flow_item pattern[], const struct rte_flow_action actions[],
                                  struct hns5_fdir_rule *rule, struct rte_flow_error *error)
{
    const struct rte_flow_item *item = NULL;
    struct items_step_mngr step_mngr = { 0 };
    int ret = 0;

    step_mngr.items = hns5_items;
    step_mngr.count = RTE_DIM(hns5_items);
    for (item = pattern; item->type != RTE_FLOW_ITEM_TYPE_END; item++) {
        if (item->type == RTE_FLOW_ITEM_TYPE_VOID) {
            continue;
        }

        ret = hns5_validate_item(item, step_mngr, error);
        if (ret != 0) {
            return ret;
        }

        ret = hns5_parse_normal(item, rule, &step_mngr, error);
        if (ret != 0) {
            return ret;
        }
    }

    ret = hns5_check_tuple(dev, rule, error);
    if (ret != 0) {
        return ret;
    }

    ret = hns5_check_meta(dev, rule, error);
    if (ret != 0) {
        return ret;
    }

    return hns5_handle_actions(dev, actions, rule, error);
}

static int hns5_flow_args_check(const struct rte_flow_attr *attr, const struct rte_flow_item pattern[],
    const struct rte_flow_action actions[], struct rte_flow_error *error)
{
    if (pattern == NULL) {
        return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ITEM_NUM, NULL, "NULL pattern.");
    }

    if (actions == NULL) {
        return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ACTION_NUM, NULL, "NULL action.");
    }

    if (attr == NULL) {
        return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_ATTR, NULL, "NULL attribute.");
    }

    return hns5_check_attr(attr, error);
}

/*
 * Check if the flow rule is supported by hns5.
 * It only checks the format. Don't guarantee the rule can be programmed into
 * the HW. Because there can be no enough room for the rule.
 */
static int hns5_flow_validate(struct rte_eth_dev *dev, const struct rte_flow_attr *attr,
    const struct rte_flow_item pattern[], const struct rte_flow_action actions[],
    struct rte_flow_error *error, struct hns5_filter_info *filter_info)
{
    union hns5_filter_conf *conf = NULL;
    int ret = 0;

    ret = hns5_flow_args_check(attr, pattern, actions, error);
    if (ret != 0) {
        return ret;
    }

    filter_info->type = RTE_ETH_FILTER_FDIR;
    conf = &filter_info->conf;

    return hns5_parse_fdir_filter(dev, attr, pattern, actions, &conf->fdir_conf, error);
}

static int hns5_flow_create_fdir_rule(struct rte_eth_dev *dev, struct hns5_fdir_rule *fdir_rule,
    struct rte_flow_error *error, struct rte_flow *flow)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hns5_fdir_rule_ele *fdir_rule_ptr = NULL;
    struct hns5_flow *hns5_flow_ptr = NULL;
    bool indir = 0;
    int ret = 0;

    indir = !!(fdir_rule->flags & HNS5_RULE_FLAG_COUNTER_INDIR);
    if (fdir_rule->flags & HNS5_RULE_FLAG_COUNTER) {
        ret = hns5_counter_new(dev, indir, fdir_rule->act_cnt.id, error);
        if (ret != 0) {
            return ret;
        }

        flow->counter_id = fdir_rule->act_cnt.id;
    }

    fdir_rule_ptr = rte_zmalloc("hns5 fdir rule", sizeof(struct hns5_fdir_rule_ele), 0);
    if (fdir_rule_ptr == NULL) {
        hns5_err(dev, "failed to allocate fdir_rule memory.\n");
        ret = -ENOMEM;
        goto err_malloc;
    }

    hns5_flow_ptr = rte_zmalloc("hns5 flow", sizeof(struct hns5_flow), 0);
    if (hns5_flow_ptr == NULL) {
        hns5_err(dev, "failed to allocate hns5_flow memory.\n");
        ret = -ENOMEM;
        goto err_flow_alloc;
    }

    /*
	 * After all the preceding tasks are successfully configured, configure
	 * rules to the hardware to simplify the rollback of rules in the
	 * hardware.
	 */

    ret = hns5_fdir_filter_program(dev, fdir_rule, false);
    if (ret != 0) {
        hns5_err(dev, "hns5_flow_create_fdir_rule: fdir_filter_program failed, ret=%d\n", ret);
        goto err_fdir_filter;
    }

    memcpy(&fdir_rule_ptr->fdir_conf, fdir_rule, sizeof(struct hns5_fdir_rule));
    hns5_flow_ptr->fdir_rule = fdir_rule_ptr;

    LIST_INSERT_HEAD(&priv->flow_list, hns5_flow_ptr, next);

    flow->rule = fdir_rule_ptr;
    flow->filter_type = RTE_ETH_FILTER_FDIR;

    return 0;

err_fdir_filter:
    rte_free(hns5_flow_ptr);
err_flow_alloc:
    rte_free(fdir_rule_ptr);
err_malloc:
    if (fdir_rule->flags & HNS5_RULE_FLAG_COUNTER) {
        hns5_counter_release(dev, fdir_rule->act_cnt.id);
    }

    return ret;
}

/*
 * Create or destroy a flow rule.
 * Theorically one rule can match more than one filters.
 * We will let it use the filter which it hit first.
 * So, the sequence matters.
 */
static struct rte_flow *hns5_flow_create(struct rte_eth_dev *dev, const struct rte_flow_attr *attr,
    const struct rte_flow_item pattern[], const struct rte_flow_action actions[],
    struct rte_flow_error *error)
{
    struct hns5_filter_info filter_info = {0};
    union hns5_filter_conf *conf = NULL;
    struct rte_flow *flow = NULL;
    int ret = 0;

    ret = hns5_flow_validate(dev, attr, pattern, actions, error, &filter_info);
    if (ret != 0) {
        return NULL;
    }

    flow = rte_zmalloc("hns5 flow", sizeof(struct rte_flow), 0);
    if (flow == NULL) {
        rte_flow_error_set(error, ENOMEM, RTE_FLOW_ERROR_TYPE_HANDLE, NULL, "Failed to allocate flow memory");
        return NULL;
    }

    conf = &filter_info.conf;
    ret = hns5_flow_create_fdir_rule(dev, &conf->fdir_conf, error, flow);
    if (ret == 0) {
        return flow;
    }

    rte_flow_error_set(error, -ret, RTE_FLOW_ERROR_TYPE_HANDLE, NULL, "Failed to create flow");
    rte_free(flow);

    return NULL;
}

/* Destroy a flow rule on hns5. */
static int hns5_flow_destroy(struct rte_eth_dev *dev, struct rte_flow *flow, struct rte_flow_error *error)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hns5_fdir_rule_ele *fdir_rule_ptr = NULL;
    struct hns5_flow *hns5_flow_ptr = NULL;
    struct hns5_flow *hns5_flow_next = NULL;
    enum rte_filter_type filter_type = 0;
    struct hns5_fdir_rule fdir_rule = { 0 };
    int ret = 0;

    if (flow == NULL) {
        return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_HANDLE, flow, "Flow is NULL");
    }

    filter_type = flow->filter_type;
    switch (filter_type) {
        case RTE_ETH_FILTER_FDIR:
            fdir_rule_ptr = (struct hns5_fdir_rule_ele *)flow->rule;
            memcpy(&fdir_rule, &fdir_rule_ptr->fdir_conf, sizeof(struct hns5_fdir_rule));

            ret = hns5_fdir_filter_program(dev, &fdir_rule, true);
            if (ret != 0) {
                return rte_flow_error_set(error, EIO, RTE_FLOW_ERROR_TYPE_HANDLE, flow, "Destroy FDIR fail.Try again");
            }
            if (fdir_rule.flags & HNS5_RULE_FLAG_COUNTER) {
                hns5_counter_release(dev, fdir_rule.act_cnt.id);
            }

            for (hns5_flow_ptr = LIST_FIRST(&priv->flow_list); hns5_flow_ptr != NULL;
                 hns5_flow_ptr = hns5_flow_next) {
                hns5_flow_next = LIST_NEXT(hns5_flow_ptr, next);
                if (hns5_flow_ptr->fdir_rule == fdir_rule_ptr) {
                    LIST_REMOVE(hns5_flow_ptr, next);
                    rte_free(hns5_flow_ptr);
                    break;
                }
            }

            rte_free(fdir_rule_ptr);
            fdir_rule_ptr = NULL;
            break;
        default:
            return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_HANDLE, flow, "Unsupported filter type");
    }

    rte_free(flow);

    return 0;
}

/* Query an existing flow rule. */
static int hns5_flow_query(struct rte_eth_dev *dev, struct rte_flow *flow, const struct rte_flow_action *actions,
    void *data, struct rte_flow_error *error)
{
    struct rte_flow_query_count *qc = NULL;
    int ret;

    if (flow->rule == NULL) {
        return rte_flow_error_set(error, EINVAL, RTE_FLOW_ERROR_TYPE_HANDLE, NULL, "invalid rule");
    }
    for (; actions->type != RTE_FLOW_ACTION_TYPE_END; actions++) {
        switch (actions->type) {
            case RTE_FLOW_ACTION_TYPE_VOID:
                break;
            case RTE_FLOW_ACTION_TYPE_COUNT:
                qc = (struct rte_flow_query_count *)data;
                ret = hns5_counter_query(dev, flow, qc, error);
                if (ret != 0) {
                    return ret;
                }

                break;
            default:
                return rte_flow_error_set(error, ENOTSUP, RTE_FLOW_ERROR_TYPE_ACTION, actions,
                                          "action is not supported");
        }
    }

    return 0;
}

static int hns5_flow_validate_wrap(struct rte_eth_dev *dev, const struct rte_flow_attr *attr,
    const struct rte_flow_item pattern[], const struct rte_flow_action actions[],
    struct rte_flow_error *error)
{
    struct rte_eth_dev_data *dev_data = hns5_eth_get_dev_data(dev);
    struct hns5_filter_info filter_info = {0};
    int ret;

    pthread_mutex_lock(&dev_data->flow_ops_mutex);
    ret = hns5_flow_validate(dev, attr, pattern, actions, error, &filter_info);
    pthread_mutex_unlock(&dev_data->flow_ops_mutex);

    return ret;
}

static struct rte_flow *hns5_flow_create_wrap(struct rte_eth_dev *dev, const struct rte_flow_attr *attr,
    const struct rte_flow_item pattern[],
    const struct rte_flow_action actions[], struct rte_flow_error *error)
{
    struct rte_eth_dev_data *dev_data = hns5_eth_get_dev_data(dev);
    struct rte_flow *flow = NULL;

    pthread_mutex_lock(&dev_data->flow_ops_mutex);
    flow = hns5_flow_create(dev, attr, pattern, actions, error);
    pthread_mutex_unlock(&dev_data->flow_ops_mutex);

    return flow;
}

static int hns5_flow_destroy_wrap(struct rte_eth_dev *dev, struct rte_flow *flow, struct rte_flow_error *error)
{
    struct rte_eth_dev_data *dev_data = hns5_eth_get_dev_data(dev);
    int ret;

    pthread_mutex_lock(&dev_data->flow_ops_mutex);
    ret = hns5_flow_destroy(dev, flow, error);
    pthread_mutex_unlock(&dev_data->flow_ops_mutex);

    return ret;
}

static int hns5_flow_query_wrap(struct rte_eth_dev *dev, struct rte_flow *flow, const struct rte_flow_action *actions,
    void *data, struct rte_flow_error *error)
{
    struct rte_eth_dev_data *dev_data = hns5_eth_get_dev_data(dev);
    int ret;

    pthread_mutex_lock(&dev_data->flow_ops_mutex);
    ret = hns5_flow_query(dev, flow, actions, data, error);
    pthread_mutex_unlock(&dev_data->flow_ops_mutex);

    return ret;
}

static int hns5_flow_flush(struct rte_eth_dev *dev, struct rte_flow_error *error __rte_unused)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hns5_flow *hns5_flow_ptr = NULL;
    struct hns5_flow *hns5_flow_next = NULL;
    struct hns5_flow_counter *cnt = NULL;
    struct hns5_flow_counter *cnt_next = NULL;
    int ret = 0;

    for (hns5_flow_ptr = LIST_FIRST(&priv->flow_list); hns5_flow_ptr != NULL;
         hns5_flow_ptr = hns5_flow_next) {
        hns5_flow_next = LIST_NEXT(hns5_flow_ptr, next);
        ret = hns5_fdir_filter_program(dev, &hns5_flow_ptr->fdir_rule->fdir_conf, true);
        if (ret != 0) {
            hns5_err(dev, "hns5 flow flush delete hardware rule failed, ret = %d\n", ret);
        }
        LIST_REMOVE(hns5_flow_ptr, next);
        rte_free(hns5_flow_ptr->fdir_rule);
        rte_free(hns5_flow_ptr);
    }

    for (cnt = LIST_FIRST(&priv->flow_counters); cnt != NULL; cnt = cnt_next) {
        cnt_next = LIST_NEXT(cnt, next);
        LIST_REMOVE(cnt, next);
        rte_free(cnt);
    }

    return ret;
}

static const struct rte_flow_ops hns5_flow_ops = {
    .validate = hns5_flow_validate_wrap,
    .create = hns5_flow_create_wrap,
    .destroy = hns5_flow_destroy_wrap,
    .query = hns5_flow_query_wrap,
    .flush = hns5_flow_flush,
    .isolate = NULL,
};

int hns5_dev_flow_ops_get(struct rte_eth_dev *dev __rte_unused,
                          const struct rte_flow_ops **ops)
{
    if (ops == NULL)
        return -EINVAL;
    *ops = &hns5_flow_ops;
    return 0;
}

int hns5_flow_init(struct rte_eth_dev *dev)
{
    int ret;
    struct rte_eth_dev_data *dev_data = hns5_eth_get_dev_data(dev);
    pthread_mutexattr_t attr;

    if (rte_eal_process_type() != RTE_PROC_PRIMARY) {
        return 0;
    }
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&dev_data->flow_ops_mutex, &attr);
    dev_data->dev_flags |= RTE_ETH_DEV_FLOW_OPS_THREAD_SAFE;

    ret = hns5_fdir_common_config(dev);
    if (ret != 0) {
        hns5_err(dev, "hns5 fdir common config fail, ret:%d\n", ret);
        return ret;
    }

    ret = hns5_fdir_port_config(dev);
    if (ret != 0) {
        hns5_err(dev, "hns5 fdir port config fail, ret:%d\n", ret);
        return ret;
    }

    return 0;
}

void hns5_flow_uninit(struct rte_eth_dev *dev)
{
    struct rte_eth_dev_data *dev_data = NULL;
    struct rte_flow_error error;

    if (rte_eal_process_type() != RTE_PROC_PRIMARY) {
        return;
    }

    /* Release all created flow and counter resources, symmetric to hns5_flow_init */
    (void)hns5_flow_flush(dev, &error);

    dev_data = hns5_eth_get_dev_data(dev);
    pthread_mutex_destroy(&dev_data->flow_ops_mutex);
    dev_data->dev_flags &= ~RTE_ETH_DEV_FLOW_OPS_THREAD_SAFE;
}
