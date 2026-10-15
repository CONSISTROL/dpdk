#include <stdint.h>

#include <rte_mbuf.h>
#include <rte_net.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_string_fns.h>
#include <bus_vdev_driver.h>
#include <bus_pci_driver.h>
#include <rte_kvargs.h>
#include <rte_errno.h>


#include "ndrv_mag_api.h"
#include "hidrv_subctrl_api.h"
#include "hidrv_udev.h"
#include "hidrv_map.h"
#include "node_code_api.h"
#include "hidrv_ppe_pf_api.h"
#include "hidrv_ppe_xtp.h"
#include "hidrv_ppe_vf_api.h"

#include "hns5_ppe.h"
#include "hns5_eth.h"
#include "hns5_log.h"
#include "hns5_fdir.h"

#define PPE_PF_ID 0
#define PPE_VF_ID 0
#define PPE_DIE_ID  0
#define PPE_DEV_ID 0

enum hidrv_ppe_endian_type g_ppe_endian_mode = HIDRV_PPE_ENDIAN_INVALID_TYPE;

int hns5_ppe_fwl_tnl_config(struct rte_eth_dev *dev, const struct rte_fw_tnl_conf *fw_tnl, uint32_t pool_id)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    if (fw_tnl->enable == 0) {
        return 0;
    }
    /* Dedup check: skip if fw_tnl has been initialized */
    if (priv->fwl_tnl_inited == 1) {
        return 0;
    }
    /* Support batch fw_tnl programming */
    for (uint32_t i = 0; i < fw_tnl->tnl_num; i++) {
        struct hidrv_fw_tnl_conf hidrv_fw_conf = { 0 };

        hidrv_fw_conf.pool_id = pool_id;
        hidrv_fw_conf.base_vqi = fw_tnl->base_vqi;
        hidrv_fw_conf.vqi_add_qos_en = 1;     /* Enabled by default */
        hidrv_fw_conf.poe_bp_grp_id = 0;
        hidrv_fw_conf.aff_ind = 0;
        if (fw_tnl->def_qos_inc == 0) {
            hidrv_fw_conf.qos_conf.default_qos = fw_tnl->def_qos;
        } else {
            hidrv_fw_conf.qos_conf.default_qos = fw_tnl->def_qos + i;
        }
        hidrv_fw_conf.qos_conf.qos_sel = fw_tnl->qos_sel;
        hidrv_fw_conf.order_type = 0;
        hidrv_fw_conf.order_tag_sel = 0;
        hidrv_fw_conf.bmu_qos_drop_en = 1;    /* BMU QoS backpressure enabled by default */
        hidrv_fw_conf.poe_qos_drop_en = 0;
        hidrv_fw_conf.cacheline_allocate_length = 0;
        ret = hidrv_ppe_vf_set_fw_tnl(priv->ppe_vf_devid, fw_tnl->base_tnl + i, &hidrv_fw_conf);
        if (ret != 0) {
            hns5_err(dev, "failed to hidrv_ppe_vf_set_fw_tnl\n");
            return ret;
        }
    }
    /* Save the first fw_tnl config and set the init flag */
    priv->fwl_tnl_conf = *fw_tnl;
    priv->fwl_tnl_inited = 1;
    return 0;
}

/**
 * @brief PPE port-level init steps
    1. Endianness is configured by kernel together; no user-space config needed
    2. Set BD prefetch rate mode of ppe_tnl, included in hidrv_mag_mac_init
    3. Set the poe source port value, included in hidrv_ppe_pf_port_enable
    4. Configure TX port bypass mode
    5. Enable RX LSW/distributor flow tables, included in hidrv_ppe_pf_port_enable
    6. Disable VLAN filtering by default, included in hidrv_ppe_pf_port_enable
    7. Configure multicast scheduling packet compensation length, incl. in hidrv_ppe_pf_setup
    8. Configure port drop watermark, included in hidrv_ppe_pf_set_port_send_pkt_enable
 */

/**
 * @brief PPE mport init, port level
*/
static int hns5_ppe_mport_init(struct rte_eth_dev *dev)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    ret = hidrv_ppe_pf_set_bypass(priv->ppe_pf_devid, priv->param.mport_id, HIDRV_PPE_DIR_RX,
        priv->param.cport_id, HIDRV_PPE_BYPASS_MODE);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_pf_set_bypass, tx sport:%u dport:%u, ret:%d\n",
            priv->param.mport_id, priv->param.cport_id, ret);
        return ret;
    }
    ret = hidrv_ppe_pf_set_port_send_pkt_enable(priv->ppe_pf_devid, HIDRV_PPE_MAC_PORT_TYPE, priv->param.mport_id, 1);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_pf_set_port_send_pkt_enable, ret:%d\n", ret);
        return ret;
    }
    /* Per HiSilicon advice: use these params for 100G, chip defaults for other rates */
    if (priv->param.port_mode == HIDRV_MAG_PORT_MODE_100GBASE_R) {
        struct hidrv_ppe_port_pause_bp_cfg rx_pause_bp_cfg = {
            .buf_num = 0x500,
            .high_thr = 0x280,
            .low_thr = 0x140,
        };
        ret = hidrv_ppe_pf_set_port_pause_bp_cfg(priv->ppe_pf_devid, HIDRV_PPE_MAC_PORT_TYPE,
            priv->param.mport_id, &rx_pause_bp_cfg);
        if (ret != 0) {
            hns5_err(dev, "failed to hidrv_ppe_pf_set_port_pause_bp_cfg\n");
            return ret;
        }
    }
    return 0;
}

/**
 * @brief PPE cport init, port level
*/
static int hns5_ppe_cport_init(struct rte_eth_dev *dev)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    ret = hidrv_ppe_pf_port_enable(priv->ppe_pf_devid, priv->param.cport_id, 1);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_pf_port_enable, ret:%d\n", ret);
        return ret;
    }
    ret = hidrv_ppe_pf_set_bypass(priv->ppe_pf_devid, priv->param.cport_id, HIDRV_PPE_DIR_TX,
        priv->param.mport_id, HIDRV_PPE_BYPASS_MODE);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_pf_set_bypass, tx sport:%u dport:%u, ret:%d\n",
            priv->param.cport_id, priv->param.mport_id, ret);
        return ret;
    }
    ret = hidrv_ppe_pf_set_port_send_pkt_enable(priv->ppe_pf_devid, HIDRV_PPE_CPU_PORT_TYPE, priv->param.cport_id, 1);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_pf_set_port_send_pkt_enable, ret:%d\n", ret);
        return ret;
    }
    /* Per HiSilicon advice: use these params for 100G, chip defaults for other rates */
    if (priv->param.port_mode == HIDRV_MAG_PORT_MODE_100GBASE_R) {
        struct hidrv_ppe_port_pause_bp_cfg tx_pause_bp_cfg = {
            .buf_num = 0x3C0,
            .high_thr = 0x320,
            .low_thr = 0x320,
        };

        ret = hidrv_ppe_pf_set_port_pause_bp_cfg(priv->ppe_pf_devid, HIDRV_PPE_CPU_PORT_TYPE,
            priv->param.cport_id, &tx_pause_bp_cfg);
        if (ret != 0) {
            hns5_err(dev, "failed to hidrv_ppe_pf_set_port_pause_bp_cfg\n");
            return ret;
        }
    }
    return 0;
}

static int hns5_ppe_port_init(struct rte_eth_dev *dev)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hidrv_ppe_pf_conf ppe_pf_conf = { 0 };
    struct hidrv_ppe_endian_cfg ppe_endian_cfg = { 0 };

    ret = hidrv_ppe_get_pf_devid(PPE_DIE_ID, PPE_PF_ID);
    if (ret < 0) {
        hns5_err(dev, "failed to hidrv_ppe_get_pf_devid, ret:%d\n", ret);
        return ret;
    }
    priv->ppe_pf_devid = ret;

    priv->ppe_pfhandle = hidrv_ppe_pf_get_ndrv_handle(priv->ppe_pf_devid);
    if (priv->ppe_pfhandle == NULL) {
        hns5_err(dev, "failed to hidrv_ppe_pf_get_ndrv_handle\n");
        return -EINVAL;
    }

    ret = hidrv_ppe_get_vf_devid(PPE_DIE_ID, PPE_PF_ID, PPE_VF_ID);
    if (ret < 0) {
        hns5_err(dev, "failed to hidrv_ppe_get_vf_devid, ret:%d\n", ret);
        return ret;
    }
    priv->ppe_vf_devid = ret;

    ret = hidrv_ppe_pf_setup(priv->ppe_pf_devid, &ppe_pf_conf, NULL);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_pf_setup, ret:%d\n", ret);
        return ret;
    }

    // RPU/TPU endianness is configured by kernel; DPDK only reads it
    if (g_ppe_endian_mode == HIDRV_PPE_ENDIAN_INVALID_TYPE) {
        ret = hidrv_ppe_pf_get_tnl_endian_cfg(priv->ppe_pf_devid, 0, &ppe_endian_cfg); // Get endianness
        if (ret != 0) {
            hns5_err(dev, "failed to hidrv_ppe_pf_get_tnl_endian_cfg, ret:%d\n", ret);
            return ret;
        }
        g_ppe_endian_mode = ppe_endian_cfg.rx_bd;
        hns5_info(dev, "succeed to hidrv_ppe_pf_get_tnl_endian_cfg, ppe_endian:%u\n", g_ppe_endian_mode);
    }
    return 0;
}

int hns5_eth_ppe_init(struct rte_eth_dev *dev)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    hns5_info(dev, "mport:%u cport:%u\n", priv->param.mport_id, priv->param.cport_id);
    ret = hns5_ppe_port_init(dev);
    if (ret != 0) {
        return ret;
    }
    ret = hns5_ppe_cport_init(dev);
    if (ret != 0) {
        return ret;
    }
    ret = hns5_ppe_mport_init(dev);
    if (ret != 0) {
        return ret;
    }
    return 0;
}

/**
 * @brief PPE mport init, LSW mode
 */
static int hns5_ppe_mport_init_lsw(struct rte_eth_dev *dev)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    ret = hidrv_ppe_pf_set_bypass(priv->ppe_pf_devid, priv->param.mport_id, HIDRV_PPE_DIR_RX,
        priv->param.cport_id, HIDRV_PPE_LSW_MODE);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_pf_set_bypass(lsw), tx sport:%u dport:%u, ret:%d\n",
            priv->param.mport_id, priv->param.cport_id, ret);
        return ret;
    }
    ret = hidrv_ppe_pf_set_port_send_pkt_enable(priv->ppe_pf_devid, HIDRV_PPE_MAC_PORT_TYPE, priv->param.mport_id, 1);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_pf_set_port_send_pkt_enable, ret:%d\n", ret);
        return ret;
    }
    /* Per HiSilicon advice: use these params for 100G, chip defaults for other rates */
    if (priv->param.port_mode == HIDRV_MAG_PORT_MODE_100GBASE_R) {
        struct hidrv_ppe_port_pause_bp_cfg rx_pause_bp_cfg = {
            .buf_num = 0x500,
            .high_thr = 0x280,
            .low_thr = 0x140,
        };
        ret = hidrv_ppe_pf_set_port_pause_bp_cfg(priv->ppe_pf_devid, HIDRV_PPE_MAC_PORT_TYPE,
            priv->param.mport_id, &rx_pause_bp_cfg);
        if (ret != 0) {
            hns5_err(dev, "failed to hidrv_ppe_pf_set_port_pause_bp_cfg\n");
            return ret;
        }
    }
    return 0;
}

/**
 * @brief PPE cport init, LSW mode
 */
static int hns5_ppe_cport_init_lsw(struct rte_eth_dev *dev)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    ret = hidrv_ppe_pf_port_enable(priv->ppe_pf_devid, priv->param.cport_id, 1);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_pf_port_enable, ret:%d\n", ret);
        return ret;
    }
    ret = hidrv_ppe_pf_set_bypass(priv->ppe_pf_devid, priv->param.cport_id, HIDRV_PPE_DIR_TX,
        priv->param.mport_id, HIDRV_PPE_LSW_MODE);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_pf_set_bypass(lsw), tx sport:%u dport:%u, ret:%d\n",
            priv->param.cport_id, priv->param.mport_id, ret);
        return ret;
    }
    ret = hidrv_ppe_pf_set_port_send_pkt_enable(priv->ppe_pf_devid, HIDRV_PPE_CPU_PORT_TYPE, priv->param.cport_id, 1);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_pf_set_port_send_pkt_enable, ret:%d\n", ret);
        return ret;
    }
    /* Per HiSilicon advice: use these params for 100G, chip defaults for other rates */
    if (priv->param.port_mode == HIDRV_MAG_PORT_MODE_100GBASE_R) {
        struct hidrv_ppe_port_pause_bp_cfg tx_pause_bp_cfg = {
            .buf_num = 0x3C0,
            .high_thr = 0x320,
            .low_thr = 0x320,
        };

        ret = hidrv_ppe_pf_set_port_pause_bp_cfg(priv->ppe_pf_devid, HIDRV_PPE_CPU_PORT_TYPE,
            priv->param.cport_id, &tx_pause_bp_cfg);
        if (ret != 0) {
            hns5_err(dev, "failed to hidrv_ppe_pf_set_port_pause_bp_cfg\n");
            return ret;
        }
    }
    return 0;
}

/**
 * @brief PPE LSW mode init entry
 *        Differs from bypass mode: uses HIDRV_PPE_LSW_MODE instead of HIDRV_PPE_BYPASS_MODE
 */
int hns5_eth_ppe_init_lsw(struct rte_eth_dev *dev)
{
    int ret = 0;

    ret = hns5_ppe_port_init(dev);
    if (ret != 0) {
        return ret;
    }
    ret = hns5_ppe_cport_init_lsw(dev);
    if (ret != 0) {
        return ret;
    }
    ret = hns5_ppe_mport_init_lsw(dev);
    if (ret != 0) {
        return ret;
    }
    ret = hns5_ppe_lsw_fltr_iso_set(dev);
    if (ret != 0) {
        return ret;
    }
    ret = hns5_ppe_lsw_set_ucast_loop(dev, 1);
    if (ret != 0) {
        return ret;
    }
    return 0;
}

/**
 * @brief Configure port isolation properties
*/
int hns5_ppe_lsw_fltr_iso_set(struct rte_eth_dev *dev)
{
    int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hidrv_lsw_fltr_iso_cfg fltr_iso_cfg = {0};

    if (priv == NULL) {
        return -EINVAL;
    }

    if (priv->ppe_pf_devid < 0) {
        hns5_err(dev, "ppe_pf_devid is invalid(%d)\n", priv->ppe_pf_devid);
        return -EINVAL;
    }

    uint32_t mac_port_mask = (1U << priv->param.mport_id);
    uint32_t cpu_port_mask = (1U << priv->param.cport_id);

    fltr_iso_cfg.ucast_iso.mac_port_bitmap = mac_port_mask;
    fltr_iso_cfg.ucast_iso.cpu_port_bitmap = cpu_port_mask;
    fltr_iso_cfg.mcast_iso.mac_port_bitmap = 0xffffffff;
    fltr_iso_cfg.mcast_iso.cpu_port_bitmap = 0xffffffff;

    /* Configure MAC port isolation */
    ret = hidrv_ppe_lsw_set_fltr_iso(priv->ppe_pf_devid, HIDRV_LSW_MAC_PORT_TYPE,
        priv->param.mport_id, &fltr_iso_cfg);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_lsw_set_fltr_iso(mac), ret=%d\n", ret);
        return ret;
    }

    /* Configure CPU port isolation */
    ret = hidrv_ppe_lsw_set_fltr_iso(priv->ppe_pf_devid, HIDRV_LSW_CPU_PORT_TYPE,
        priv->param.cport_id, &fltr_iso_cfg);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_lsw_set_fltr_iso(cpu), ret=%d\n", ret);
        return ret;
    }

    hns5_info(dev, "port isolation configured: mport=%u cport=%u mac_mask=0x%08x cpu_mask=0x%08x\n",
        priv->param.mport_id, priv->param.cport_id, mac_port_mask, cpu_port_mask);

    return 0;
}

/**
 * @brief Configure unicast loopback enable
 * @param dev NIC device
 * @param enable 1-Enable loopback 0-Disable loopback
 */
int hns5_ppe_lsw_set_ucast_loop(struct rte_eth_dev *dev, unsigned int enable)
{
    int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    unsigned int loop_en = 0;

    if (priv == NULL) {
        return -EINVAL;
    }

    if (priv->ppe_pf_devid < 0) {
        hns5_err(dev, "ppe_pf_devid is invalid(%d)\n", priv->ppe_pf_devid);
        return -EINVAL;
    }

    /* Get current config first */
    ret = hidrv_ppe_lsw_get_ucast_loop_enable(priv->ppe_pf_devid, HIDRV_PPE_MAC_PORT_TYPE,
        priv->param.mport_id, &loop_en);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_lsw_get_ucast_loop_enable, ret=%d\n", ret);
        return ret;
    }

    hns5_info(dev, "ucast loopback origin: mport=%u loop_en=%u\n", priv->param.mport_id, loop_en);

    /* Set new loopback config */
    ret = hidrv_ppe_lsw_set_ucast_loop_enable(priv->ppe_pf_devid, HIDRV_PPE_MAC_PORT_TYPE,
        priv->param.mport_id, enable);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_lsw_set_ucast_loop_enable, ret=%d\n", ret);
        return ret;
    }

    hns5_info(dev, "ucast loopback configured: mport=%u enable=%u\n", priv->param.mport_id, enable);

    return 0;
}

/**
 * @brief Convert LSW ingress flow key from software to hardware format
 * @param[in] dev              Ethernet device
 * @param[in] port_id          Port ID (cport)
 * @param[in] dmac             Destination MAC, effective when match_dmac is 1
 * @param[in] match_dmac       Match dmac or not, 1-match 0-no match
 * @param[in] match_ing_port   Match ingress port (des_port) or not, 1-match 0-no match
 * @param[out] fd_key          Converted flow key
 * @param[out] ad              Converted flow action
 * @return int                 0 on success, error code on failure
 */
static int ing_lsw_key_soft_to_hardware(struct rte_eth_dev *dev, uint16_t port_id,
    struct hns5_ppp_fd_key *fd_key, union hidrv_ppe_fd_actn *ad,
    const uint8_t *dmac, unsigned int match_dmac, unsigned int match_ing_port)
{
    (void)dev;

    uint32_t i;
    union ing_lsw_key_word_info key_info = {0};
    union ing_lsw_key_word_info mask_info = {0};

    if (match_dmac && dmac != NULL) {
        key_info.key320.dmac_byte0 = dmac[0];
        key_info.key320.dmac_byte1 = dmac[1];
        key_info.key320.dmac_byte2 = dmac[2];
        key_info.key320.dmac_byte3 = dmac[3];
        key_info.key320.dmac_byte4 = dmac[4];
        key_info.key320.dmac_byte5 = dmac[5];

        mask_info.key320.dmac_byte0 = 0xff;
        mask_info.key320.dmac_byte1 = 0xff;
        mask_info.key320.dmac_byte2 = 0xff;
        mask_info.key320.dmac_byte3 = 0xff;
        mask_info.key320.dmac_byte4 = 0xff;
        mask_info.key320.dmac_byte5 = 0xff;
    }

    if (match_ing_port) {
        key_info.key320.ingress_port = port_id;
        mask_info.key320.ingress_port = 0x7f;  /* Match all 6 bits */
    }

    for (i = 0; i < HNS5_FD_DATA_WORD_MAX_NUM; i++) {
        fd_key->key[i] = key_info.word[i];
        fd_key->key_msk[i] = mask_info.word[i];
    }

    ad->lsw_ing_actn.next_step = 0;
    ad->lsw_ing_actn.des_type = HIDRV_LSW_ING_FD_DES_MPORT_UCAST_NO_TRUNK;
    ad->lsw_ing_actn.des_data.des_ucast_no_trunk.des_port_id = port_id;
    ad->lsw_ing_actn.des_data.des_ucast_no_trunk.des_encode_type = 0;
    ad->lsw_ing_actn.des_data.des_ucast_no_trunk.des_sub_chnl_id = 0;
    ad->lsw_ing_actn.inner_vlan_modify = 0;
    ad->lsw_ing_actn.outer_vlan_modify = 0;
    ad->lsw_ing_actn.vlan_filter_en = 0;

    return 0;
}

/*
 * Add ingress flow rule for VLAN Insert: match packets with ingress_port=cport, redirect to macport
 * ingress_port[6] = 0 (from CPU), ingress_port[4:0] = cport
 */
static int hns5_ppe_vlan_insert_flow_add(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    union hidrv_ppe_fd_actn actn = {0};
    struct hns5_ppp_fd_key fd_key = {0};
    uint32_t entry;
    int ret;

    if (priv == NULL || priv->ppe_pf_devid < 0)
        return -EINVAL;

    ret = hidrv_ppe_pf_ftable_mixed_alloc_entry(priv->ppe_pf_devid, HNS5_FD_TABLE_ID,
        HIDRV_PPE_FD_LSW_INGRESS_STAGE1, &entry);
    if (ret != 0) {
        hns5_err(dev, "failed to alloc lsw ing flow entry, ret=%d\n", ret);
        return ret;
    }

    /* Convert LSW ingress key/action: match ingress port (cport), no dmac match */
    ret = ing_lsw_key_soft_to_hardware(dev, priv->param.cport_id, &fd_key, &actn, NULL, 0, 1);
    if (ret != 0) {
        hns5_err(dev, "lsw ing key soft to hardware failed, ret=%d\n", ret);
        goto del_entry;
    }

    ret = hidrv_ppe_pf_ftable_mixed_set_entry(priv->ppe_pf_devid, HNS5_FD_TABLE_ID,
        HIDRV_PPE_FD_LSW_INGRESS_STAGE1, entry, fd_key.key, fd_key.key_msk,
        HNS5_FD_DATA_WORD_MAX_NUM, &actn);
    if (ret != 0) {
        hns5_err(dev, "failed to set lsw ing vlan insert flow, ret=%d\n", ret);
        goto del_entry;
    }

    priv->vlan_insert_flow_entry = entry;
    hns5_info(dev, "vlan insert flow added: cport=%u, entry=%u\n", priv->param.cport_id, entry);

    return 0;

del_entry:
    (void)hidrv_ppe_pf_ftable_mixed_del_entry(priv->ppe_pf_devid, HNS5_FD_TABLE_ID,
        HIDRV_PPE_FD_LSW_INGRESS_STAGE1, entry);
    return ret;
}

/* Delete ingress flow rule for VLAN Insert */
static int hns5_ppe_vlan_insert_flow_del(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    uint32_t entry;
    int ret;

    if (priv == NULL || priv->ppe_pf_devid < 0)
        return -EINVAL;

    entry = priv->vlan_insert_flow_entry;

    ret = hidrv_ppe_pf_ftable_mixed_del_entry(priv->ppe_pf_devid, HNS5_FD_TABLE_ID,
        HIDRV_PPE_FD_LSW_INGRESS_STAGE1, entry);
    if (ret != 0) {
        hns5_err(dev, "failed to delete lsw ing vlan insert flow, ret=%d\n", ret);
        return ret;
    }

    ret = hidrv_ppe_pf_ftable_mixed_del_hw_entry(priv->ppe_pf_devid, HNS5_FD_TABLE_ID,
        HIDRV_PPE_FD_LSW_INGRESS_STAGE1, entry);
    if (ret != 0) {
        hns5_err(dev, "failed to delete lsw ing vlan insert hw entry, ret=%d\n", ret);
        return ret;
    }

    hns5_info(dev, "vlan insert flow deleted: entry=%u\n", entry);

    return 0;
}

/**
 * @brief Configure CPU port VLAN Insert and ingress flow table
 * @param dev NIC device
 * @param vlan_insert_en VLAN Insert enable flag: 1-enable (set port VLAN attr, mask L2 VLAN match, add flow), 0-restore
 * @return int 0 on success, other on failure
 */
int hns5_ppe_vlan_insert_config(struct rte_eth_dev *dev, uint8_t vlan_insert_en)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    int ret;

    if (priv->ppe_pfhandle == NULL) {
        hns5_err(dev, "ppe_pfhandle is NULL\n");
        return -EINVAL;
    }

    if (vlan_insert_en) {
        /* Idempotency guard: skip if ingress flow already added */
        if (priv->vlan_insert_flow_added) {
            return 0;
        }

        /* Enable port ingress VLAN attr for VLAN Insert (reuse pvid semantics, def_vlan from port_def_vlan) */
        ret = hns5_vlan_pvid_set(dev, priv->port_def_vlan, 1);
        if (ret != 0) {
            return ret;
        }

        /* Add ingress flow: match ingress_port=cport, redirect to macport */
        ret = hns5_ppe_vlan_insert_flow_add(dev);
        if (ret != 0) {
            return ret;
        }

        priv->vlan_insert_flow_added = 1;
        hns5_info(dev, "vlan insert enabled: cport=%u, def_vlan=%u\n",
            priv->param.cport_id, priv->port_def_vlan);
    } else {
        /* Disable port VLAN attr, restore L2 VLAN match, delete ingress flow */
        ret = hns5_vlan_pvid_set(dev, 0, 0);
        if (ret != 0) {
            return ret;
        }

        ret = hns5_ppe_vlan_insert_flow_del(dev);
        if (ret != 0) {
            return ret;
        }

        priv->vlan_insert_flow_added = 0;
        hns5_info(dev, "vlan insert disabled: cport=%u\n", priv->param.cport_id);
    }

    return 0;
}

/*
 * Add ingress flow rule for multicast mode: only match the I/G bit (bit0) of the first DMAC byte,
 * I/G=1 identifies a multicast frame; hit frames are sent up to the CPU port (cport)
 */
static int hns5_ppe_mcast_mode_flow_add(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    union ing_lsw_key_word_info key_info = {0};
    union ing_lsw_key_word_info mask_info = {0};
    union hidrv_ppe_fd_actn actn = {0};
    struct hns5_ppp_fd_key fd_key = {0};
    uint32_t entry;
    uint32_t i;
    int ret;

    if (priv == NULL || priv->ppe_pf_devid < 0)
        return -EINVAL;

    ret = hidrv_ppe_pf_ftable_mixed_alloc_entry(priv->ppe_pf_devid, HNS5_FD_TABLE_ID,
        HIDRV_PPE_FD_LSW_INGRESS_STAGE1, &entry);
    if (ret != 0) {
        hns5_err(dev, "failed to alloc lsw ing mcast flow entry, ret=%d\n", ret);
        return ret;
    }

    /* Match only packets with I/G bit (bit0) of first DMAC byte = 1 (multicast); other key bits ignored */
    key_info.key320.dmac_byte0 = 0x01;   /* I/G bit = 1 */
    key_info.key320.ingress_port = priv->param.mport_id | 0x40;
    mask_info.key320.dmac_byte0 = 0x01;  /* Mask bit0 only */
    mask_info.key320.ingress_port = 0x7f;

    for (i = 0; i < HNS5_FD_DATA_WORD_MAX_NUM; i++) {
        fd_key.key[i] = key_info.word[i];
        fd_key.key_msk[i] = mask_info.word[i];
    }

    /* Hit frames are multicast: send to CPU port */
    actn.lsw_ing_actn.next_step = 0;
    actn.lsw_ing_actn.des_type = HIDRV_LSW_ING_FD_DES_CPORT_PORT;
    actn.lsw_ing_actn.des_data.des_cpu_port.des_port_id = priv->param.cport_id;
    actn.lsw_ing_actn.inner_vlan_modify = 0;
    actn.lsw_ing_actn.outer_vlan_modify = 0;
    actn.lsw_ing_actn.vlan_filter_en = 0;

    ret = hidrv_ppe_pf_ftable_mixed_set_entry(priv->ppe_pf_devid, HNS5_FD_TABLE_ID,
        HIDRV_PPE_FD_LSW_INGRESS_STAGE1, entry, fd_key.key, fd_key.key_msk,
        HNS5_FD_DATA_WORD_MAX_NUM, &actn);
    if (ret != 0) {
        hns5_err(dev, "failed to set lsw ing mcast flow, ret=%d\n", ret);
        goto del_entry;
    }

    priv->mcast_mode_flow_entry = entry;

    return 0;

del_entry:
    (void)hidrv_ppe_pf_ftable_mixed_del_entry(priv->ppe_pf_devid, HNS5_FD_TABLE_ID,
        HIDRV_PPE_FD_LSW_INGRESS_STAGE1, entry);
    return ret;
}

/* Delete ingress flow rule for multicast mode */
static int hns5_ppe_mcast_mode_flow_del(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    uint32_t entry;
    int ret;

    if (priv == NULL || priv->ppe_pf_devid < 0)
        return -EINVAL;

    entry = priv->mcast_mode_flow_entry;

    ret = hidrv_ppe_pf_ftable_mixed_del_entry(priv->ppe_pf_devid, HNS5_FD_TABLE_ID,
        HIDRV_PPE_FD_LSW_INGRESS_STAGE1, entry);
    if (ret != 0) {
        hns5_err(dev, "failed to delete lsw ing mcast flow, ret=%d\n", ret);
        return ret;
    }

    ret = hidrv_ppe_pf_ftable_mixed_del_hw_entry(priv->ppe_pf_devid, HNS5_FD_TABLE_ID,
        HIDRV_PPE_FD_LSW_INGRESS_STAGE1, entry);
    if (ret != 0) {
        hns5_err(dev, "failed to delete lsw ing mcast hw entry, ret=%d\n", ret);
        return ret;
    }

    return 0;
}

/**
 * @brief Configure multicast mode (enable: receive multicast, disable: drop multicast)
 * @param dev NIC device
 * @param enable 1-enable multicast mode (add ingress flow matching DMAC I/G bit, mcast frames sent to CPU),
 *               0-disable multicast mode (delete the flow, mcast frames dropped by default path)
 * @return int 0 on success, other on failure
 */
int hns5_ppe_lsw_mcast_mode_config(struct rte_eth_dev *dev, uint8_t enable)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    int ret;

    if (priv == NULL || priv->ppe_pfhandle == NULL) {
        hns5_err(dev, "ppe_pfhandle is NULL\n");
        return -EINVAL;
    }

    if (enable) {
        /* Idempotency guard: skip if ingress flow already added */
        if (priv->mcast_mode_flow_added) {
            return 0;
        }

        ret = hns5_ppe_mcast_mode_flow_add(dev);
        if (ret != 0) {
            return ret;
        }

        priv->mcast_mode_flow_added = 1;
    } else {
        if (!priv->mcast_mode_flow_added) {
            return 0;
        }

        ret = hns5_ppe_mcast_mode_flow_del(dev);
        if (ret != 0) {
            return ret;
        }

        priv->mcast_mode_flow_added = 0;
    }

    return 0;
}
