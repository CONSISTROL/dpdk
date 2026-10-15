/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */

#ifndef _HNS5_LSW_H_
#define _HNS5_LSW_H_

#include <rte_ethdev.h>
#include <ethdev_driver.h>

#include "hns5_eth.h"

/**
 * @brief L2 entry disp type
 */
enum hns5_l2_disp_type {
    HNS5_L2_DISP_TYPE_UCAST = 0,      /**< Unicast non-trunk */
    HNS5_L2_DISP_TYPE_UCAST_TRUNK,    /**< Unicast trunk */
    HNS5_L2_DISP_TYPE_CPU_PORT,       /**< CPU port */
    HNS5_L2_DISP_TYPE_MCAST,          /**< Multicast */
    HNS5_L2_DISP_TYPE_INVALID
};

/**
 * @brief L2 unicast disp info (non-trunk)
 */
struct hns5_l2_disp_ucast {
    uint32_t port_id;       /**< Destination MAC port ID */
    uint32_t encode_type;   /**< Encryption flag */
    uint32_t sub_chnl_id;   /**< Dest sub-port ID, unsupported on PPE-V110 */
};

/**
 * @brief L2 unicast trunk disp info
 */
struct hns5_l2_disp_ucast_trunk {
    uint32_t trunk_id;      /**< Trunk ID */
    uint32_t encode_type;   /**< Encryption flag */
};

/**
 * @brief L2 multicast disp info
 */
struct hns5_l2_disp_mcast {
    uint32_t mcast_gid;     /**< Multicast leaf ID */
};

/**
 * @brief L2 entry action config
 */
struct hns5_l2_actn_cfg {
    uint32_t limit_counted; /**< Count flag for self-learned entries */
    uint32_t static_item;   /**< Static entry flag; not replaceable when set */
    uint32_t aging_en;      /**< Entry aging enable */
    uint32_t aging_state;   /**< Aging state */
};

/**
 * @brief L2 entry info
 */
struct hns5_l2_entry {
    uint8_t mac[RTE_ETHER_ADDR_LEN]; /**< MAC address */
    uint16_t vlan_id;                 /**< VLAN ID */
    uint32_t sw_domain_id;            /**< Switch domain ID, valid for PPE-120 */
    enum hns5_l2_disp_type disp_type;/**< Disp type */
    union {
        struct hns5_l2_disp_ucast ucast;
        struct hns5_l2_disp_ucast_trunk ucast_trunk;
        uint32_t cpu_port_id;
        struct hns5_l2_disp_mcast mcast;
    } disp_info;
    struct hns5_l2_actn_cfg actn_cfg;
};

/**
 * @brief Generate a random MAC address
 * @param[out] mac MAC address pointer
 * @return 0 on success, non-zero on failure
 */
int hns5_gen_random_mac(struct rte_ether_addr *mac);

/**
 * @brief Init L2 MAC entry (called during probe)
 * @param[in] dev Gateway device
 * @return 0 on success, non-zero on failure
 */
int hns5_l2_mac_init(struct rte_eth_dev *dev);

/**
 * @brief Set default MAC address (replace the default MAC)
 * @param[in] dev  Gateway device
 * @param[in] addr New MAC address
 * @return 0 on success, non-zero on failure
 * @note Triggered by rte_eth_dev_default_mac_addr_set; the driver updates the HW entry
 */
int hns5_mac_addr_set(struct rte_eth_dev *dev, struct rte_ether_addr *addr);

/**
 * @brief Add an L2 entry
 * @param[in] dev  Gateway device
 * @param[in] entry L2 entry info
 * @return 0 on success, non-zero on failure
 */
int hns5_l2_add_entry(struct rte_eth_dev *dev, const struct hns5_l2_entry *entry);

/**
 * @brief Delete an L2 entry
 * @param[in] dev  Gateway device
 * @param[in] mac  MAC address
 * @param[in] vlan_id VLAN ID
 * @return 0 on success, non-zero on failure
 */
int hns5_l2_del_entry(struct rte_eth_dev *dev, const uint8_t *mac, uint16_t vlan_id);

/**
 * @brief Modify an L2 entry (auto-add if the key does not exist)
 * @param[in] dev  Gateway device
 * @param[in] entry L2 entry info
 * @return 0 on success, non-zero on failure
 */
int hns5_l2_modify_entry(struct rte_eth_dev *dev, const struct hns5_l2_entry *entry);

/**
 * @brief Look up an L2 entry
 * @param[in] dev  Gateway device
 * @param[in] mac  MAC address
 * @param[in] vlan_id VLAN ID
 * @param[out] valid Whether the entry exists
 * @param[out] entry L2 entry info
 * @return 0 on success, non-zero on failure
 */
int hns5_l2_lkup_entry(struct rte_eth_dev *dev, const uint8_t *mac, uint16_t vlan_id,
    uint32_t *valid, struct hns5_l2_entry *entry);

/**
 * @brief Set the L2 disp action to unicast
 * @param[in] dev  Gateway device
 * @param[in] mac  MAC address
 * @param[in] vlan_id VLAN ID
 * @param[in] ucast Unicast info
 * @param[in] actn_cfg Associated action config
 * @return 0 on success, non-zero on failure
 */
int hns5_l2_set_disp_ucast(struct rte_eth_dev *dev, const uint8_t *mac, uint16_t vlan_id,
    const struct hns5_l2_disp_ucast *ucast, const struct hns5_l2_actn_cfg *actn_cfg);

/**
 * @brief Set the L2 disp action to unicast TRUNK
 * @param[in] dev  Gateway device
 * @param[in] mac  MAC address
 * @param[in] vlan_id VLAN ID
 * @param[in] trunk Trunk info
 * @param[in] actn_cfg Associated action config
 * @return 0 on success, non-zero on failure
 */
int hns5_l2_set_disp_ucast_trunk(struct rte_eth_dev *dev, const uint8_t *mac, uint16_t vlan_id,
    const struct hns5_l2_disp_ucast_trunk *trunk, const struct hns5_l2_actn_cfg *actn_cfg);

/**
 * @brief Set the L2 disp action to CPU PORT
 * @param[in] dev  Gateway device
 * @param[in] mac  MAC address
 * @param[in] vlan_id VLAN ID
 * @param[in] cpu_port_id CPU port ID
 * @param[in] actn_cfg Associated action config
 * @return 0 on success, non-zero on failure
 */
int hns5_l2_set_disp_cpu_port(struct rte_eth_dev *dev, const uint8_t *mac, uint16_t vlan_id,
    uint32_t cpu_port_id, const struct hns5_l2_actn_cfg *actn_cfg);

#endif /* _HNS5_LSW_H_ */
