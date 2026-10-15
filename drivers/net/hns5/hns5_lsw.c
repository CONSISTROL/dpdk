/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */

#include <stdint.h>
#include <string.h>

#include <rte_ethdev.h>
#include <rte_errno.h>

#include "hidrv_ppe_pf_api.h"
#include "ndrv_ppe_lsw_pf_api.h"
#include "ndrv_ppe_ppp_pf_api.h"
#include "ppe_lsw_type_api.h"
#include "ppe_type_api.h"

#include "hns5_lsw.h"
#include "hns5_eth.h"
#include "hns5_log.h"

/**
 * @brief Convert disp type enum (hidrv version)
 */
static enum hidrv_lsw_l2_des_type hns5_l2_disp_type_to_hidrv(enum hns5_l2_disp_type disp_type)
{
    switch (disp_type) {
        case HNS5_L2_DISP_TYPE_UCAST:
            return HIDRV_LSW_L2_DES_MPORT_UCAST_NO_TRUNK;
        case HNS5_L2_DISP_TYPE_UCAST_TRUNK:
            return HIDRV_LSW_L2_DES_MPORT_UCAST_TRUNK;
        case HNS5_L2_DISP_TYPE_CPU_PORT:
            return HIDRV_LSW_L2_DES_CPORT_PORT;
        case HNS5_L2_DISP_TYPE_MCAST:
            return HIDRV_LSW_L2_DES_MPORT_MCAST;
        default:
            return HIDRV_LSW_L2_DES_TYPE_INVALID;
    }
}

/**
 * @brief Fill L2 key struct
 */
static void hns5_l2_fill_key(struct hidrv_lsw_l2_key *key, const uint8_t *mac, uint16_t vlan_id)
{
    (void)memcpy(key->mac, mac, RTE_ETHER_ADDR_LEN);
    key->vlan_id = vlan_id;
}

/**
 * @brief Fill L2 actn struct
 */
static void hns5_l2_fill_actn(struct hidrv_lsw_l2_actn *actn,
    const struct hns5_l2_entry *entry)
{
    (void)memset(actn, 0, sizeof(struct hidrv_lsw_l2_actn));

    actn->next_step = HIDRV_LSW_NEXT_GET_DISP_RESULT;  /* Get the disp result directly */
    actn->des_type = hns5_l2_disp_type_to_hidrv(entry->disp_type);

    switch (entry->disp_type) {
        case HNS5_L2_DISP_TYPE_UCAST:
            actn->des_id.des_ucast_no_trunk.port_id = entry->disp_info.ucast.port_id;
            actn->des_id.des_ucast_no_trunk.encode_type = entry->disp_info.ucast.encode_type;
            actn->des_id.des_ucast_no_trunk.sub_chnl_id = entry->disp_info.ucast.sub_chnl_id;
            break;
        case HNS5_L2_DISP_TYPE_UCAST_TRUNK:
            actn->des_id.des_ucast_trunk.trunk_id = entry->disp_info.ucast_trunk.trunk_id;
            actn->des_id.des_ucast_trunk.encode_type = entry->disp_info.ucast_trunk.encode_type;
            break;
        case HNS5_L2_DISP_TYPE_CPU_PORT:
            actn->des_id.des_cpu_port.port_id = entry->disp_info.cpu_port_id;
            break;
        case HNS5_L2_DISP_TYPE_MCAST:
            actn->des_id.des_mcast.mgid = entry->disp_info.mcast.mcast_gid;
            break;
        default:
            break;
    }

    actn->limit_counted = entry->actn_cfg.limit_counted;
    actn->static_item = entry->actn_cfg.static_item;
    actn->aging_en = entry->actn_cfg.aging_en;
    actn->aging_state = entry->actn_cfg.aging_state;
}

/**
 * @brief Fill L2 key struct (internal; lkup/set_disp_* use ndrv APIs)
 */
static void hns5_l2_fill_key_internal(struct ndrv_ppe_lsw_l2_key *key, const uint8_t *mac, uint16_t vlan_id)
{
    (void)memcpy(key->mac, mac, RTE_ETHER_ADDR_LEN);
    key->vlan_id = vlan_id;
    key->sw_domain_id = 0;  /* valid for PPE-120; currently default 0 */
}

int hns5_gen_random_mac(struct rte_ether_addr *mac)
{
    uint64_t random_value;

    if (mac == NULL) {
        return -EINVAL;
    }

    /* Set the OUI prefix, force the locally administered address bit */
    mac->addr_bytes[0] = 0x00;
    mac->addr_bytes[1] = 0x09;
    mac->addr_bytes[2] = 0xC0;
    mac->addr_bytes[0] |= RTE_ETHER_LOCAL_ADMIN_ADDR;

    /* Random value for the last 3 bytes */
    random_value = rte_rand();
    (void)memcpy(&mac->addr_bytes[3], &random_value, 3);

    return 0;
}

int hns5_l2_mac_init(struct rte_eth_dev *dev)
{
    int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct rte_ether_addr *mac = &priv->address;
    struct hns5_l2_entry entry = {0};

    if (priv == NULL) {
        return -EINVAL;
    }

    /* Generate a random MAC address */
    ret = hns5_gen_random_mac(mac);
    if (ret != 0) {
        hns5_err(dev, "failed to hns5_gen_random_mac, ret:%d\n", ret);
        return ret;
    }

    /* Build the L2 entry */
    (void)memcpy(entry.mac, mac->addr_bytes, RTE_ETHER_ADDR_LEN);
    entry.vlan_id = 0;
    entry.disp_type = HNS5_L2_DISP_TYPE_CPU_PORT;
    entry.disp_info.cpu_port_id = priv->param.cport_id;
    entry.actn_cfg.static_item = 1;  /* Static entry; not replaceable */
    entry.actn_cfg.aging_en = 0;     /* Disable aging */

    /* Add the L2 entry */
    ret = hns5_l2_add_entry(dev, &entry);
    if (ret != 0) {
        hns5_err(dev, "failed to hns5_l2_add_entry, ret:%d\n", ret);
        return ret;
    }

    hns5_info(dev, "L2 MAC initialized: %02x:%02x:%02x:%02x:%02x:%02x, cport:%u\n",
        mac->addr_bytes[0], mac->addr_bytes[1], mac->addr_bytes[2],
        mac->addr_bytes[3], mac->addr_bytes[4], mac->addr_bytes[5],
        priv->param.cport_id);
    return 0;
}

int hns5_mac_addr_set(struct rte_eth_dev *dev, struct rte_ether_addr *addr)
{
    int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct rte_ether_addr old_mac;
    struct hns5_l2_entry entry = {0};

    if (priv == NULL || addr == NULL) {
        return -EINVAL;
    }

    if (priv->ppe_pfhandle == NULL) {
        hns5_err(dev, "ppe_pfhandle is NULL\n");
        return -EINVAL;
    }

    (void)memcpy(old_mac.addr_bytes, priv->address.addr_bytes, RTE_ETHER_ADDR_LEN);

    (void)memcpy(entry.mac, addr->addr_bytes, RTE_ETHER_ADDR_LEN);
    entry.vlan_id = 0;
    entry.disp_type = HNS5_L2_DISP_TYPE_CPU_PORT;
    entry.disp_info.cpu_port_id = priv->param.cport_id;
    entry.actn_cfg.static_item = 1;  /* Static entry; not replaceable */
    entry.actn_cfg.aging_en = 0;     /* Disable aging */

    ret = hns5_l2_add_entry(dev, &entry);
    if (ret != 0) {
        hns5_err(dev, "failed to add new MAC, ret=%d\n", ret);
        return ret;
    }

    if (memcmp(old_mac.addr_bytes, addr->addr_bytes, RTE_ETHER_ADDR_LEN) != 0) {
        ret = hns5_l2_del_entry(dev, old_mac.addr_bytes, 0);
        if (ret != 0) {
            hns5_warn(dev, "failed to del old MAC, ret=%d\n", ret);
        }
    }

    (void)memcpy(priv->address.addr_bytes, addr->addr_bytes, RTE_ETHER_ADDR_LEN);

    hns5_info(dev, "MAC addr set: %02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x\n",
        old_mac.addr_bytes[0], old_mac.addr_bytes[1], old_mac.addr_bytes[2],
        old_mac.addr_bytes[3], old_mac.addr_bytes[4], old_mac.addr_bytes[5],
        addr->addr_bytes[0], addr->addr_bytes[1], addr->addr_bytes[2],
        addr->addr_bytes[3], addr->addr_bytes[4], addr->addr_bytes[5]);

    return 0;
}

/**
 * @brief Fill L2 actn struct (simplified, for set_l2_fd_actn_disp_* funcs)
 */
static void hns5_l2_fill_actn_internal(struct ndrv_ppe_lsw_l2_actn *ndrv_actn, const struct hns5_l2_actn_cfg *actn_cfg)
{
    (void)memset(ndrv_actn, 0, sizeof(struct ndrv_ppe_lsw_l2_actn));
    ndrv_actn->limit_counted = actn_cfg->limit_counted;
    ndrv_actn->static_item = actn_cfg->static_item;
    ndrv_actn->aging_en = actn_cfg->aging_en;
    ndrv_actn->aging_state = actn_cfg->aging_state;
}

int hns5_l2_add_entry(struct rte_eth_dev *dev, const struct hns5_l2_entry *entry)
{
    int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hidrv_lsw_l2_key hidrv_key = {0};
    struct hidrv_lsw_l2_actn hidrv_actn = {0};

    if (priv->ppe_pf_devid < 0) {
        hns5_err(dev, "ppe_pf_devid is invalid(%d)\n", priv->ppe_pf_devid);
        return -EINVAL;
    }

    hns5_l2_fill_key(&hidrv_key, entry->mac, entry->vlan_id);
    hns5_l2_fill_actn(&hidrv_actn, entry);

    ret = hidrv_ppe_lsw_add_l2_entry(priv->ppe_pf_devid, &hidrv_key, &hidrv_actn);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_lsw_add_l2_entry, ret=%d\n", ret);
        return ret;
    }

    return 0;
}

int hns5_l2_del_entry(struct rte_eth_dev *dev, const uint8_t *mac, uint16_t vlan_id)
{
    int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hidrv_lsw_l2_key hidrv_key = {0};

    if (priv->ppe_pf_devid < 0) {
        hns5_err(dev, "ppe_pf_devid is invalid(%d)\n", priv->ppe_pf_devid);
        return -EINVAL;
    }

    hns5_l2_fill_key(&hidrv_key, mac, vlan_id);

    ret = hidrv_ppe_lsw_del_l2_entry(priv->ppe_pf_devid, &hidrv_key);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_lsw_del_l2_entry, ret=%d\n", ret);
        return ret;
    }

    return 0;
}

int hns5_l2_modify_entry(struct rte_eth_dev *dev, const struct hns5_l2_entry *entry)
{
    int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hidrv_lsw_l2_key hidrv_key = {0};
    struct hidrv_lsw_l2_actn hidrv_actn = {0};

    if (priv->ppe_pf_devid < 0) {
        hns5_err(dev, "ppe_pf_devid is invalid(%d)\n", priv->ppe_pf_devid);
        return -EINVAL;
    }

    hns5_l2_fill_key(&hidrv_key, entry->mac, entry->vlan_id);
    hns5_l2_fill_actn(&hidrv_actn, entry);

    ret = hidrv_ppe_lsw_modify_l2_entry(priv->ppe_pf_devid, &hidrv_key, &hidrv_actn);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_lsw_modify_l2_entry, ret=%d\n", ret);
        return ret;
    }

    return 0;
}

int hns5_l2_lkup_entry(struct rte_eth_dev *dev, const uint8_t *mac, uint16_t vlan_id,
    uint32_t *valid, struct hns5_l2_entry *entry)
{
    int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct ndrv_ppe_lsw_l2_key ndrv_key = {0};
    struct ndrv_ppe_lsw_l2_actn_info ndrv_actn = {0};

    if (priv->ppe_pfhandle == NULL) {
        hns5_err(dev, "ppe_pfhandle is NULL\n");
        return -EINVAL;
    }

    if (valid == NULL || entry == NULL) {
        hns5_err(dev, "invalid parameter, valid=%p, entry=%p\n", valid, entry);
        return -EINVAL;
    }

    hns5_l2_fill_key_internal(&ndrv_key, mac, vlan_id);

    ret = ndrv_ppe_lsw_pf_lkup_l2_entry(priv->ppe_pfhandle, &ndrv_key, valid, &ndrv_actn);
    if (ret != 0) {
        hns5_err(dev, "failed to ndrv_ppe_lsw_pf_lkup_l2_entry, ret=%d\n", ret);
        return ret;
    }

    if (*valid == 0) {
        return 0;
    }

    /* Fill the returned entry info */
    (void)memcpy(entry->mac, ndrv_key.mac, RTE_ETHER_ADDR_LEN);
    entry->vlan_id = ndrv_key.vlan_id;
    entry->sw_domain_id = ndrv_key.sw_domain_id;

    entry->disp_type = (enum hns5_l2_disp_type)ndrv_actn.des_type;
    switch (ndrv_actn.des_type) {
        case NDRV_PPE_LSW_L2_DES_MPORT_UCAST_NO_TRUNK:
            entry->disp_info.ucast.port_id = ndrv_actn.des_id.des_ucast_no_trunk.port_id;
            entry->disp_info.ucast.encode_type = ndrv_actn.des_id.des_ucast_no_trunk.encode_type;
            entry->disp_info.ucast.sub_chnl_id = ndrv_actn.des_id.des_ucast_no_trunk.sub_chnl_id;
            break;
        case NDRV_PPE_LSW_L2_DES_MPORT_UCAST_TRUNK:
            entry->disp_info.ucast_trunk.trunk_id = ndrv_actn.des_id.des_ucast_trunk.trunk_id;
            entry->disp_info.ucast_trunk.encode_type = ndrv_actn.des_id.des_ucast_trunk.encode_type;
            break;
        case NDRV_PPE_LSW_L2_DES_CPORT_PORT:
            entry->disp_info.cpu_port_id = ndrv_actn.des_id.des_cpu_port.port_id;
            break;
        case NDRV_PPE_LSW_L2_DES_MPORT_MCAST:
            entry->disp_info.mcast.mcast_gid = ndrv_actn.des_id.des_mcast.mcast_gid;
            break;
        default:
            break;
    }

    entry->actn_cfg.limit_counted = ndrv_actn.limit_counted;
    entry->actn_cfg.static_item = ndrv_actn.static_item;
    entry->actn_cfg.aging_en = ndrv_actn.aging_en;
    entry->actn_cfg.aging_state = ndrv_actn.aging_state;

    return 0;
}

int hns5_l2_set_disp_ucast(struct rte_eth_dev *dev, const uint8_t *mac, uint16_t vlan_id,
    const struct hns5_l2_disp_ucast *ucast, const struct hns5_l2_actn_cfg *actn_cfg)
{
    int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct ndrv_ppe_lsw_l2_key ndrv_key = {0};
    struct ndrv_ppe_fd_des_ucast ndrv_ucast = {0};
    struct ndrv_ppe_lsw_l2_actn ndrv_actn = {0};

    if (priv->ppe_pfhandle == NULL) {
        hns5_err(dev, "ppe_pfhandle is NULL\n");
        return -EINVAL;
    }

    hns5_l2_fill_key_internal(&ndrv_key, mac, vlan_id);

    ndrv_ucast.port_id = ucast->port_id;
    ndrv_ucast.encode_type = ucast->encode_type;
    ndrv_ucast.sub_chnl_id = ucast->sub_chnl_id;

    hns5_l2_fill_actn_internal(&ndrv_actn, actn_cfg);

    ret = ndrv_ppe_lsw_pf_set_l2_fd_actn_disp_des_ucast(priv->ppe_pfhandle, &ndrv_key, &ndrv_ucast, &ndrv_actn);
    if (ret != 0) {
        hns5_err(dev, "failed to ndrv_ppe_lsw_pf_set_l2_fd_actn_disp_des_ucast, ret=%d\n", ret);
        return ret;
    }

    return 0;
}

int hns5_l2_set_disp_ucast_trunk(struct rte_eth_dev *dev, const uint8_t *mac, uint16_t vlan_id,
    const struct hns5_l2_disp_ucast_trunk *trunk, const struct hns5_l2_actn_cfg *actn_cfg)
{
    int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct ndrv_ppe_lsw_l2_key ndrv_key = {0};
    struct ndrv_ppe_fd_des_ucast_trunk ndrv_trunk = {0};
    struct ndrv_ppe_lsw_l2_actn ndrv_actn = {0};

    if (priv->ppe_pfhandle == NULL) {
        hns5_err(dev, "ppe_pfhandle is NULL\n");
        return -EINVAL;
    }

    hns5_l2_fill_key_internal(&ndrv_key, mac, vlan_id);

    ndrv_trunk.trunk_id = trunk->trunk_id;
    ndrv_trunk.encode_type = trunk->encode_type;

    hns5_l2_fill_actn_internal(&ndrv_actn, actn_cfg);

    ret = ndrv_ppe_lsw_pf_set_l2_fd_actn_disp_des_ucast_trunk(priv->ppe_pfhandle, &ndrv_key, &ndrv_trunk, &ndrv_actn);
    if (ret != 0) {
        hns5_err(dev, "failed to ndrv_ppe_lsw_pf_set_l2_fd_actn_disp_des_ucast_trunk, ret=%d\n", ret);
        return ret;
    }

    return 0;
}

int hns5_l2_set_disp_cpu_port(struct rte_eth_dev *dev, const uint8_t *mac, uint16_t vlan_id,
    uint32_t cpu_port_id, const struct hns5_l2_actn_cfg *actn_cfg)
{
    int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct ndrv_ppe_lsw_l2_key ndrv_key = {0};
    struct ndrv_ppe_lsw_l2_actn ndrv_actn = {0};

    if (priv->ppe_pfhandle == NULL) {
        hns5_err(dev, "ppe_pfhandle is NULL\n");
        return -EINVAL;
    }

    hns5_l2_fill_key_internal(&ndrv_key, mac, vlan_id);
    hns5_l2_fill_actn_internal(&ndrv_actn, actn_cfg);

    ret = ndrv_ppe_lsw_pf_set_l2_fd_actn_disp_des_cpu_port(priv->ppe_pfhandle, &ndrv_key, cpu_port_id, &ndrv_actn);
    if (ret != 0) {
        hns5_err(dev, "failed to ndrv_ppe_lsw_pf_set_l2_fd_actn_disp_des_cpu_port, ret=%d\n", ret);
        return ret;
    }

    return 0;
}
