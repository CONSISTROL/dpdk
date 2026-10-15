#include <stdatomic.h>

#include <syslog.h>

#include <stdlib.h>
#include <string.h>

#include <rte_mbuf.h>
#include <rte_mbuf_ptype.h>
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
#include <rte_bitops.h>
#include <ethdev_vdev.h>

#include "hidrv_subctrl_api.h"
#include "ndrv_ppe_bd_type.h"
#include "ndrv_ppe_pf_api.h"
#include "ndrv_poe_pf_api.h"
#include "ndrv_poe_vf_api.h"
#include "ndrv_ppe_ppp_pf_api.h"
#include "ndrv_ppe_ppp_vf_api.h"
#include "ndrv_ppe_rpu_pf_api.h"
#include "ndrv_ppe_ssu_pf_api.h"
#include "ndrv_ppe_tpu_pf_api.h"
#include "ndrv_ppe_tpu_vf_api.h"
#include "ndrv_ppe_vf_api.h"
#include "ndrv_ppe_mam_pf_api.h"
#include "ndrv_ppe_mam_vf_api.h"
#include "ndrv_ppe_lsw_pf_api.h"
#include "ppe_lsw_type_api.h"
#include "ndrv_ppe_pa_pf_api.h"
#include "node_code_api.h"
#include "hidrv_udev.h"
#include "hidrv_map.h"
#include "hidrv_tm_pf_api.h"
#include "ndrv_ppe_debug_api.h"
#include "hidrv_mag_api.h"
#include "hidrv_poe_pf_api.h"
#include "hidrv_poe_vf_api.h"
#include "hidrv_log_api.h"
#include "hidrv_ppe_pf_api.h"
#include "hidrv_ppe_vf_api.h"
#include "hidrv_mag_xtp.h"
#include "hidrv_ppe_xtp.h"

#include "hns5_eth.h"
#include "hns5_stats.h"
#include "hns5_mbuf.h"
#include "hns5_eth_rxtx.h"
#include "hns5_ppe.h"
#include "hns5_poe.h"
#include "hns5_mag.h"
#include "hns5_log.h"
#include "hns5_mempool.h"
#include "hns5_flow.h"
#include "hns5_tm.h"
#include "hns5_rss.h"
#include "hns5_lsw.h"


#define HNS5_ETH_DEV_INFO_NAME_LEN     64
#define HNS5_ETH_DEFAULT_HW_TXQ_NUM    8
#define HNS5_ETH_POOL_MAX              64      // global max pool count
#define HNS5_ETH_MAX_RX_MEMPOOLS       8       // max mempools per rxq

static const char *g_mag_mode_maps[] = {
    [HIDRV_MAG_PORT_MODE_100BASE_FX] = "100BASE_FX",
    [HIDRV_MAG_PORT_MODE_1000BASE_X] = "1000BASE_X",
    [HIDRV_MAG_PORT_MODE_SGMII] = "SGMII",
    [HIDRV_MAG_PORT_MODE_QSGMII] = "QSGMII",
    [HIDRV_MAG_PORT_MODE_2G5BASE_X] = "2G5BASE_X",
    [HIDRV_MAG_PORT_MODE_10GBASE_R] = "10GBASE_R",
    [HIDRV_MAG_PORT_MODE_FLEXMII_5G2P] = "FLEXMII_5G2P",
    [HIDRV_MAG_PORT_MODE_FLEXMII_10G2P] = "FLEXMII_10G2P",
    [HIDRV_MAG_PORT_MODE_FLEXMII_20G2P] = "FLEXMII_20G2P",
    [HIDRV_MAG_PORT_MODE_FLEXMII_10G4P] = "FLEXMII_10G4P",
    [HIDRV_MAG_PORT_MODE_FLEXMII_20G4P] = "FLEXMII_20G4P",
    [HIDRV_MAG_PORT_MODE_5GBASE_R] = "5GBASE_R",
    [HIDRV_MAG_PORT_MODE_5GBASE_KR] = "5GBASE_KR",
    [HIDRV_MAG_PORT_MODE_XAUI] = "XAUI",
    [HIDRV_MAG_PORT_MODE_25GBASE_R] = "25GBASE_R",
    [HIDRV_MAG_PORT_MODE_40GBASE_R] = "40GBASE_R",
    [HIDRV_MAG_PORT_MODE_50GBASE_R] = "50GBASE_R",
    [HIDRV_MAG_PORT_MODE_100GBASE_R] = "100GBASE_R"
};

static uint32_t g_mag_speed_hidrv_to_rte[] = {
    [HIDRV_MAG_PORT_SPEED_10M] = RTE_ETH_SPEED_NUM_10M,
    [HIDRV_MAG_PORT_SPEED_100M] = RTE_ETH_SPEED_NUM_100M,
    [HIDRV_MAG_PORT_SPEED_1000M] = RTE_ETH_SPEED_NUM_1G,
    [HIDRV_MAG_PORT_SPEED_2G5] = RTE_ETH_SPEED_NUM_2_5G,
    [HIDRV_MAG_PORT_SPEED_5G] = RTE_ETH_SPEED_NUM_5G,
    [HIDRV_MAG_PORT_SPEED_10G] = RTE_ETH_SPEED_NUM_10G,
    [HIDRV_MAG_PORT_SPEED_12G5] = RTE_ETH_SPEED_NUM_NONE,
    [HIDRV_MAG_PORT_SPEED_25G] = RTE_ETH_SPEED_NUM_25G,
    [HIDRV_MAG_PORT_SPEED_40G] = RTE_ETH_SPEED_NUM_40G,
    [HIDRV_MAG_PORT_SPEED_50G] = RTE_ETH_SPEED_NUM_50G,
    [HIDRV_MAG_PORT_SPEED_100G] = RTE_ETH_SPEED_NUM_100G,
    [HIDRV_MAG_PORT_SPEED_NO_USE] = RTE_ETH_SPEED_NUM_UNKNOWN,
};

static uint32_t g_mag_mode_support_speeds[] = {
    [HIDRV_MAG_PORT_MODE_100BASE_FX] = RTE_ETH_LINK_SPEED_100M | RTE_ETH_LINK_SPEED_100M_HD,
    [HIDRV_MAG_PORT_MODE_1000BASE_X] = RTE_ETH_LINK_SPEED_1G,
    [HIDRV_MAG_PORT_MODE_SGMII] = RTE_ETH_LINK_SPEED_10M | RTE_ETH_LINK_SPEED_10M_HD |
                                    RTE_ETH_LINK_SPEED_100M | RTE_ETH_LINK_SPEED_100M_HD | RTE_ETH_LINK_SPEED_1G,
    [HIDRV_MAG_PORT_MODE_QSGMII] = RTE_ETH_LINK_SPEED_10M | RTE_ETH_LINK_SPEED_10M_HD |
                                    RTE_ETH_LINK_SPEED_100M | RTE_ETH_LINK_SPEED_100M_HD | RTE_ETH_LINK_SPEED_1G,
    [HIDRV_MAG_PORT_MODE_2G5BASE_X] = RTE_ETH_LINK_SPEED_2_5G,
    [HIDRV_MAG_PORT_MODE_10GBASE_R] = RTE_ETH_LINK_SPEED_10G,
    [HIDRV_MAG_PORT_MODE_FLEXMII_5G2P] = RTE_ETH_LINK_SPEED_2_5G,
    [HIDRV_MAG_PORT_MODE_FLEXMII_10G2P] = RTE_ETH_LINK_SPEED_5G,
    [HIDRV_MAG_PORT_MODE_FLEXMII_20G2P] = RTE_ETH_LINK_SPEED_10G,
    [HIDRV_MAG_PORT_MODE_FLEXMII_10G4P] = RTE_ETH_LINK_SPEED_2_5G,
    [HIDRV_MAG_PORT_MODE_FLEXMII_20G4P] = RTE_ETH_LINK_SPEED_5G,
    [HIDRV_MAG_PORT_MODE_5GBASE_R] = RTE_ETH_LINK_SPEED_5G,
    [HIDRV_MAG_PORT_MODE_5GBASE_KR] = RTE_ETH_LINK_SPEED_5G,
    [HIDRV_MAG_PORT_MODE_XAUI] = RTE_ETH_LINK_SPEED_2_5G,
    [HIDRV_MAG_PORT_MODE_25GBASE_R] = RTE_ETH_LINK_SPEED_25G,
    [HIDRV_MAG_PORT_MODE_40GBASE_R] = RTE_ETH_LINK_SPEED_40G,
    [HIDRV_MAG_PORT_MODE_50GBASE_R] = RTE_ETH_LINK_SPEED_50G,
    [HIDRV_MAG_PORT_MODE_100GBASE_R] = RTE_ETH_LINK_SPEED_100G
};

static enum hidrv_mag_port_speed g_mag_mode_default_speed[] = {
    [HIDRV_MAG_PORT_MODE_100BASE_FX] = HIDRV_MAG_PORT_SPEED_100M,
    [HIDRV_MAG_PORT_MODE_1000BASE_X] = HIDRV_MAG_PORT_SPEED_1000M,
    [HIDRV_MAG_PORT_MODE_SGMII] = HIDRV_MAG_PORT_SPEED_1000M,
    [HIDRV_MAG_PORT_MODE_QSGMII] = HIDRV_MAG_PORT_SPEED_1000M,
    [HIDRV_MAG_PORT_MODE_2G5BASE_X] = HIDRV_MAG_PORT_SPEED_2G5,
    [HIDRV_MAG_PORT_MODE_10GBASE_R] = HIDRV_MAG_PORT_SPEED_10G,
    [HIDRV_MAG_PORT_MODE_FLEXMII_5G2P] = HIDRV_MAG_PORT_SPEED_2G5,
    [HIDRV_MAG_PORT_MODE_FLEXMII_10G2P] = HIDRV_MAG_PORT_SPEED_5G,
    [HIDRV_MAG_PORT_MODE_FLEXMII_20G2P] = HIDRV_MAG_PORT_SPEED_10G,
    [HIDRV_MAG_PORT_MODE_FLEXMII_10G4P] = HIDRV_MAG_PORT_SPEED_2G5,
    [HIDRV_MAG_PORT_MODE_FLEXMII_20G4P] = HIDRV_MAG_PORT_SPEED_5G,
    [HIDRV_MAG_PORT_MODE_5GBASE_R] = HIDRV_MAG_PORT_SPEED_5G,
    [HIDRV_MAG_PORT_MODE_5GBASE_KR] = HIDRV_MAG_PORT_SPEED_5G,
    [HIDRV_MAG_PORT_MODE_XAUI] = HIDRV_MAG_PORT_SPEED_2G5,
    [HIDRV_MAG_PORT_MODE_25GBASE_R] = HIDRV_MAG_PORT_SPEED_25G,
    [HIDRV_MAG_PORT_MODE_40GBASE_R] = HIDRV_MAG_PORT_SPEED_40G,
    [HIDRV_MAG_PORT_MODE_50GBASE_R] = HIDRV_MAG_PORT_SPEED_50G,
    [HIDRV_MAG_PORT_MODE_100GBASE_R] = HIDRV_MAG_PORT_SPEED_100G
};

static const struct rte_eth_fec_capa g_speed_fec_capa_tbl[] = {
    { RTE_ETH_SPEED_NUM_5G, RTE_ETH_FEC_MODE_CAPA_MASK(NOFEC) |
                RTE_ETH_FEC_MODE_CAPA_MASK(BASER) |
                RTE_ETH_FEC_MODE_CAPA_MASK(RS)},

    { RTE_ETH_SPEED_NUM_10G, RTE_ETH_FEC_MODE_CAPA_MASK(NOFEC) |
                RTE_ETH_FEC_MODE_CAPA_MASK(BASER) |
                RTE_ETH_FEC_MODE_CAPA_MASK(RS)},

    { RTE_ETH_SPEED_NUM_25G, RTE_ETH_FEC_MODE_CAPA_MASK(NOFEC) |
                RTE_ETH_FEC_MODE_CAPA_MASK(BASER) |
                RTE_ETH_FEC_MODE_CAPA_MASK(RS)},

    { RTE_ETH_SPEED_NUM_40G, RTE_ETH_FEC_MODE_CAPA_MASK(NOFEC) |
                RTE_ETH_FEC_MODE_CAPA_MASK(BASER) |
                RTE_ETH_FEC_MODE_CAPA_MASK(RS)},

    { RTE_ETH_SPEED_NUM_100G, RTE_ETH_FEC_MODE_CAPA_MASK(NOFEC) |
                RTE_ETH_FEC_MODE_CAPA_MASK(BASER) |
                RTE_ETH_FEC_MODE_CAPA_MASK(RS)},
};

static const uint32_t g_mag_fec_mode_hidrv_to_rte[] = {
    [HIDRV_MAG_NO_FEC] = RTE_ETH_FEC_NOFEC,
    [HIDRV_MAG_FEC_BRFEC] = RTE_ETH_FEC_BASER,
    [HIDRV_MAG_FEC_RSFEC_544] = RTE_ETH_FEC_RS
};

static int hns5_eth_dev_mtu_set(struct rte_eth_dev *dev, uint16_t mtu)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    uint32_t max_frame_len = HNS5_MAG_MTU_TO_FRAME(mtu);

    if (mtu > HNS5_MTU_MAX) {
        hns5_warn(dev, "mtu:%u is out [%u-%u], use max val\n", mtu, HNS5_MTU_MIN, HNS5_MTU_MAX);
        max_frame_len = HNS5_FRAME_MAX;
    }

    if (priv->mag_handle == NULL) {
        hns5_info(dev, "mag not ready, save mtu %u (frame:%u), apply at dev_start\n",
            mtu, max_frame_len);
        priv->param.max_frame_len = max_frame_len;
        return 0;
    }

    ret = hidrv_mag_mac_set_max_frame_len(priv->mag_pf_devid, priv->param.mport_id, max_frame_len);
    if (ret != 0) {
        hns5_err(dev, "failed set frame, mtu:%u frame:%u\n", mtu, max_frame_len);
        return ret;
    }
    priv->param.max_frame_len = max_frame_len;
    hns5_info(dev, "succeed set mtu %u\n", HNS5_MAG_FRAME_TO_MTU(max_frame_len));
    return 0;
}

static int hns5_eth_dev_set_link_down(struct rte_eth_dev *dev)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    ret = hidrv_mag_mac_set_rx_enable(priv->mag_pf_devid, priv->param.mport_id, 0);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_mag_mac_set_rx_enable, mport:%u\n", priv->param.mport_id);
        return -EINVAL;
    }
    ret = hidrv_mag_mac_set_tx_enable(priv->mag_pf_devid, priv->param.mport_id, 0);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_mag_mac_set_tx_enable, mport:%u\n", priv->param.mport_id);
        return -EINVAL;
    }
    dev->data->dev_link.link_status = RTE_ETH_LINK_DOWN;
    dev->data->dev_link.link_speed = RTE_ETH_SPEED_NUM_UNKNOWN;
    hns5_info(dev, "port:%u succeed to linkdown\n", dev->data->port_id);
    return 0;
}

static int hns5_eth_dev_set_link_up(struct rte_eth_dev *dev)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    ret = hidrv_mag_mac_set_rx_enable(priv->mag_pf_devid, priv->param.mport_id, 1);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_mag_mac_set_rx_enable, mport:%u\n", priv->param.mport_id);
        return -EINVAL;
    }
    ret = hidrv_mag_mac_set_tx_enable(priv->mag_pf_devid, priv->param.mport_id, 1);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_mag_mac_set_tx_enable, mport:%u\n", priv->param.mport_id);
        return -EINVAL;
    }
    dev->data->dev_link.link_status = RTE_ETH_LINK_UP;
    if (priv->param.port_speed < RTE_DIM(g_mag_speed_hidrv_to_rte)) {
        dev->data->dev_link.link_speed = g_mag_speed_hidrv_to_rte[priv->param.port_speed];
    } else {
        dev->data->dev_link.link_speed = RTE_ETH_SPEED_NUM_NONE;
    }
    dev->data->dev_link.link_duplex = (priv->param.duplex_mode == HIDRV_MAG_DUPLEX_MODE_FULL) ?
        RTE_ETH_LINK_FULL_DUPLEX : RTE_ETH_LINK_HALF_DUPLEX;
    dev->data->dev_link.link_autoneg = (priv->param.autoneg == 0) ? RTE_ETH_LINK_SPEED_FIXED : RTE_ETH_LINK_SPEED_AUTONEG;
    hns5_info(dev, "port:%u succeed to linkup\n", dev->data->port_id);
    return 0;
}

static void hns5_stop_rx_queues(struct rte_eth_dev *dev, uint32_t vcpu)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    uint32_t cleared_grp_mask = 0;
    int ret;

    for (uint32_t i = 0; i < dev->data->nb_rx_queues; i++) {
        dev->data->rx_queue_state[i] = RTE_ETH_QUEUE_STATE_STOPPED;
        struct hns5_eth_rx_queue *rxq = dev->data->rx_queues[i];
        if (rxq != NULL && (cleared_grp_mask & (1U << rxq->group)) == 0) {
            ret = hidrv_poe_clear_group(priv->poe_vf_devid, vcpu, rxq->group);
            if (ret != 0) {
                hns5_warn(dev, "failed to clear group:%u, ret:%d\n", rxq->group, ret);
            }
            cleared_grp_mask |= (1U << rxq->group);
        }
    }
}

static int hns5_eth_dev_stop(struct rte_eth_dev *dev)
{
    uint32_t lcore_id = hns5_get_lcore_id();
    uint32_t vcpu = lcore_id * 2 + 1;
    int ret;

    ret = hns5_eth_dev_set_link_down(dev);
    if (ret != 0) {
        hns5_err(dev, "failed to linkdown, ret:%d\n", ret);
        return ret;
    }

    hns5_stop_rx_queues(dev, vcpu);

    for (uint32_t i = 0; i < dev->data->nb_tx_queues; i++) {
        dev->data->tx_queue_state[i] = RTE_ETH_QUEUE_STATE_STOPPED;
    }
    dev->data->scattered_rx = false;
    dev->rx_pkt_burst = NULL;
    dev->tx_pkt_burst = NULL;

    hns5_info(dev, "port:%u succeed to stop\n", dev->data->port_id);
    return 0;
}
static int hns5_eth_dev_close(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    handle vfhandle;
    int ret;

    vfhandle = hidrv_ppe_vf_get_ndrv_handle(priv->ppe_vf_devid);
    if (vfhandle != NULL) {
        ret = ndrv_ppe_mam_vf_disable_all_vlp(vfhandle);
        if (ret != 0) {
            hns5_warn(dev, "failed to disable all vlp, ret:%d\n", ret);
        }
    }

    for (int i = 0; i < HNS5_ETH_POOL_MAX; i++) {
        if ((priv->pool_inited & (1 << i)) != 0) {
            ret = hidrv_ppe_vf_close_pool(priv->ppe_vf_devid, i);
            if (ret != 0) {
                hns5_warn(dev, "failed to close pool:%d, ret:%d\n", i, ret);
            }
            priv->pool_inited &= ~(1 << i);
        }
    }

    // hns5_mp_clean_addr_trans_tbl(); // clean bmu blocks, has issues

    /* Clean RSS config (write to hardware) */
    hns5_rss_uninit(dev);

    /* Clean mcast mode ingress flow (matches DMAC I/G bit), avoid residual hw flow table after close */
    if (priv->mcast_mode_flow_added) {
        (void)hns5_ppe_lsw_mcast_mode_config(dev, 0);
    }

    /* Delete the L2 MAC entry added at probe (random MAC) */
    (void)hns5_l2_del_entry(dev, priv->address.addr_bytes, 0);

    dev->data->mac_addrs = NULL;
    hns5_info(dev, "port:%u succeed to close\n", dev->data->port_id);
    return 0;
}

static int hns5_eth_dev_start(struct rte_eth_dev *dev)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct rte_eth_conf *dev_conf = &dev->data->dev_conf;

    for (uint32_t i = 0; i < dev->data->nb_rx_queues; i++) {
        dev->data->rx_queue_state[i] = RTE_ETH_QUEUE_STATE_STARTED;
    }
    for (uint32_t i = 0; i < dev->data->nb_tx_queues; i++) {
        dev->data->tx_queue_state[i] = RTE_ETH_QUEUE_STATE_STARTED;
    }
    if (dev_conf->rxmode.offloads & RTE_ETH_RX_OFFLOAD_SCATTER) {
        dev->data->scattered_rx = true;
    }
    dev->rx_pkt_burst = hns5_eth_rx_burst;
    dev->tx_pkt_burst = hns5_eth_tx_burst;

    ret = hns5_eth_mag_init(dev);
    if (ret != 0) {
        hns5_err(dev, "failed to hns5_eth_mag_init, ret:%d\n", ret);
        return ret;
    }
    ret = hns5_eth_dev_mtu_set(dev, dev->data->mtu);
    if (ret != 0) {
        hns5_err(dev, "failed to hns5_eth_dev_mtu_set, ret:%d\n", ret);
        return ret;
    }

    if ((dev_conf->rxmode.mq_mode & RTE_ETH_MQ_RX_RSS_FLAG) ||
        (dev->data->nb_rx_queues == 1)) {  // temporary workaround
        ret = hns5_config_rss(dev);
        if (ret != 0) {
            hns5_err(dev, "failed to config RSS, ret:%d\n", ret);
            return ret;
        }
    }

    ret = hns5_eth_set_crc_offload(dev, priv->crc_offload);
    if (ret != 0) {
        hns5_err(dev, "failed to set CRC offload, ret:%d\n", ret);
        return ret;
    }

    ret = hns5_eth_dev_set_link_up(dev);
    if (ret != 0) {
        hns5_err(dev, "failed to linkup, ret:%d\n", ret);
        return ret;
    }

    hns5_info(dev, "port:%u succeed to start\n", dev->data->port_id);
    return 0;
}

static int hns5_rte_to_hidrv_port_speed(struct hns5_eth_priv *priv, uint32_t input_speed)
{
    switch (input_speed) {
        case RTE_ETH_LINK_SPEED_10M_HD:
            priv->param.port_speed = HIDRV_MAG_PORT_SPEED_10M;
            priv->param.duplex_mode = HIDRV_MAG_DUPLEX_MODE_HALF;
            break;
        case RTE_ETH_LINK_SPEED_10M:
            priv->param.port_speed = HIDRV_MAG_PORT_SPEED_10M;
            break;
        case RTE_ETH_LINK_SPEED_100M_HD:
            priv->param.port_speed = HIDRV_MAG_PORT_SPEED_100M;
            priv->param.duplex_mode = HIDRV_MAG_DUPLEX_MODE_HALF;
            break;
        case RTE_ETH_LINK_SPEED_100M:
            priv->param.port_speed = HIDRV_MAG_PORT_SPEED_100M;
            break;
        case RTE_ETH_LINK_SPEED_1G:
            priv->param.port_speed = HIDRV_MAG_PORT_SPEED_1000M;
            break;
        case RTE_ETH_LINK_SPEED_2_5G:
            priv->param.port_speed = HIDRV_MAG_PORT_SPEED_2G5;
            break;
        case RTE_ETH_LINK_SPEED_5G:
            priv->param.port_speed = HIDRV_MAG_PORT_SPEED_5G;
            break;
        case RTE_ETH_LINK_SPEED_10G:
            priv->param.port_speed = HIDRV_MAG_PORT_SPEED_10G;
            break;
        case RTE_ETH_LINK_SPEED_25G:
            priv->param.port_speed = HIDRV_MAG_PORT_SPEED_25G;
            break;
        case RTE_ETH_LINK_SPEED_100G:
            priv->param.port_speed = HIDRV_MAG_PORT_SPEED_100G;
            break;
        default:
            return -EINVAL;
    }
    return 0;
}

/* Set VLAN offload: configure VLAN strip per rxmode.offloads */
static int hns5_vlan_offload_set(struct rte_eth_dev *dev, int mask)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct ndrv_ppe_lsw_port_egr_attr egr_attr = {0};
    unsigned int ret;

    if (mask == 0)
        return 0;

    if (mask & RTE_ETH_RX_OFFLOAD_VLAN_STRIP) {
        uint8_t strip = (dev->data->dev_conf.rxmode.offloads & RTE_ETH_RX_OFFLOAD_VLAN_STRIP) ? 1 : 0;

        /* Use MAC port egress attr as template to configure CPU port VLAN strip */
        ret = ndrv_ppe_lsw_pf_get_port_egress_attr(priv->ppe_pfhandle,
            NDRV_PPE_MAC_PORT_TYPE, priv->param.mport_id, &egr_attr);
        if (ret != 0) {
            hns5_err(dev, "get port egress attr failed, ret=%u\n", ret);
            return -EIO;
        }

        egr_attr.vlan_in_strip_en = strip;
        egr_attr.vlan_out_strip_en = strip;
        egr_attr.vlan_in_showtag = !strip;
        egr_attr.vlan_out_showtag = !strip;

        ret = ndrv_ppe_lsw_pf_set_port_egress_attr(priv->ppe_pfhandle,
            NDRV_PPE_CPU_PORT_TYPE, priv->param.cport_id, &egr_attr);
        if (ret != 0) {
            hns5_err(dev, "set port egress attr failed, ret=%u\n", ret);
            return -EIO;
        }

        hns5_info(dev, "VLAN strip %s\n", strip ? "enabled" : "disabled");
    }

    return 0;
}

/**
 * @brief Set port default VLAN (dev_ops.vlan_pvid_set callback, also reused when vlan_insert_config enabled)
 * @param dev eth device
 * @param vlan_id VLAN ID (ignored when on=0)
 * @param on 1-enable VLAN insert and set port default VLAN, 0-disable VLAN insert
 * @return int 0 on success, non-zero on failure
 */
int hns5_vlan_pvid_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct ndrv_ppe_lsw_port_ing_vlan_attr vlan_attr = {0};
    unsigned int ret;

    if (priv->ppe_pfhandle == NULL) {
        hns5_err(dev, "ppe_pfhandle is NULL\n");
        return -EINVAL;
    }

    if (on) {
        priv->port_def_vlan = vlan_id;

        vlan_attr.def_vlan = vlan_id;
        vlan_attr.vlan_ad.vld1_zero0_pvid0 = NDRV_PPE_VLAN_ACTN_RPLC_VLAN;
        vlan_attr.vlan_ad.vld1_zero0_pvid1 = NDRV_PPE_VLAN_ACTN_NA;
        vlan_attr.vlan_ad.vld1_zero1_pvid0 = NDRV_PPE_VLAN_ACTN_RPLC_VLAN;
        vlan_attr.vlan_ad.vld1_zero1_pvid1 = NDRV_PPE_VLAN_ACTN_RPLC_VLAN;
        vlan_attr.vlan_ad.vld0 = NDRV_PPE_VLAN_ACTN_INS;

        ret = ndrv_ppe_lsw_pf_set_port_ingress_vlan_attr(priv->ppe_pfhandle,
            NDRV_PPE_CPU_PORT_TYPE, priv->param.cport_id, 1, &vlan_attr);
        if (ret != 0) {
            hns5_err(dev, "failed to set port ingress vlan attr, ret=%d\n", ret);
            return ret;
        }
        hns5_info(dev, "port VLAN configured: cport=%u, vlan_id=%u\n", priv->param.cport_id, vlan_id);
        priv->vlan_pvid_en = 1;
    } else {
        hns5_info(dev, "VLAN insert disabled\n");
        priv->vlan_pvid_en = 0;
    }

    return 0;
}

/**
 * @brief User configures NIC parameters
 * @note Default input is 0 if not specified, init per mode
 * @note Only one speed can be passed for special config
 * @attention Autoneg is supported only by some modes
 */
static int hns5_eth_dev_configure(struct rte_eth_dev *dev)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct rte_eth_conf *dev_conf = &dev->data->dev_conf;
    uint32_t input_speed = 0;

    /* default single TC config */
    priv->num_tc = 1;
    priv->hw_tc_map = 0x01;  /* TC0 only */

    if (priv->param.port_mode >= HIDRV_MAG_PORT_MODE_INVALID) {
        hns5_err(dev, "invalid port mode:%u\n", priv->param.port_mode);
        return -EINVAL;
    }
    priv->param.duplex_mode = HIDRV_MAG_DUPLEX_MODE_FULL;
    if (dev_conf->link_speeds == RTE_ETH_LINK_SPEED_AUTONEG) {
        if (priv->param.port_mode == HIDRV_MAG_PORT_MODE_100BASE_FX || priv->param.port_mode == HIDRV_MAG_PORT_MODE_1000BASE_X ||
            priv->param.port_mode == HIDRV_MAG_PORT_MODE_SGMII || priv->param.port_mode == HIDRV_MAG_PORT_MODE_QSGMII)  {
            priv->param.autoneg = 1;
        }
        priv->param.port_speed = g_mag_mode_default_speed[priv->param.port_mode];
    } else {
        // invalid input: speed and autoneg passed together
        if (!(dev_conf->link_speeds & RTE_ETH_LINK_SPEED_FIXED)) {
            hns5_err(dev, "can not input speed in autoneg mode\n");
            return -EINVAL;
        }
        // invalid input: multiple speeds passed
        input_speed = dev_conf->link_speeds & ~RTE_ETH_LINK_SPEED_FIXED;
        if (input_speed & (input_speed - 1)) {
            hns5_err(dev, "can only input one speed\n");
            return -EINVAL;
        }
        // invalid input: speed not supported by mode
        if ((input_speed & g_mag_mode_support_speeds[priv->param.port_mode]) == 0) {
            hns5_err(dev, "unsupport port speed:0x%x for port mode:%u\n", input_speed, priv->param.port_mode);
            return -EINVAL;
        }
        ret = hns5_rte_to_hidrv_port_speed(priv, input_speed);
        if (ret != 0) {
            hns5_err(dev, "failed to get hidrv speed form 0x%x\n", input_speed);
            return ret;
        }
    }
    hns5_info(dev, "mode:%u speed:%u autoneg:%u duplex_mode:%u\n", priv->param.port_mode, priv->param.port_speed,
        priv->param.autoneg, priv->param.duplex_mode);

    if (dev_conf->rxmode.mq_mode & RTE_ETH_MQ_RX_RSS_FLAG) {
        ret = hns5_rss_apply_conf(dev);
        if (ret != 0) {
            hns5_err(dev, "failed to apply RSS conf, ret:%d\n", ret);
            return ret;
        }
    }

    priv->crc_offload = (dev_conf->rxmode.offloads & RTE_ETH_RX_OFFLOAD_KEEP_CRC) ? 0 : 1;

    /* Apply VLAN offload: keep hw consistent with dev_conf when app skips vlan_offload_set */
    ret = hns5_vlan_offload_set(dev, RTE_ETH_RX_OFFLOAD_VLAN_STRIP);
    if (ret != 0) {
        hns5_err(dev, "failed to apply VLAN offload config, ret:%d\n", ret);
        return ret;
    }

    /* Configure VLAN insert per txmode offloads, diff against flow table add state */
    uint8_t vlan_insert_en = (dev_conf->txmode.offloads & RTE_ETH_TX_OFFLOAD_VLAN_INSERT) ? 1 : 0;
    if (vlan_insert_en != priv->vlan_insert_flow_added) {
        ret = hns5_ppe_vlan_insert_config(dev, vlan_insert_en);
        if (ret != 0) {
            hns5_err(dev, "failed to apply VLAN insert config, ret:%d\n", ret);
            return ret;
        }
    }

    return 0;
}

static int hns5_eth_dev_infos_get(struct rte_eth_dev *dev, struct rte_eth_dev_info *dev_info)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    dev_info->min_mtu = HNS5_MTU_MIN;
    dev_info->max_mtu = HNS5_MTU_MAX;
    dev_info->max_mac_addrs = 1;
    dev_info->max_rx_pktlen = PPE_SUPPORT_FRAME_MAX;
    /* RX queue limit set by POE CPU group queue count, see hns5_poe.h:HNS5_POE_CPU_GRP_QUE_NUM */
    dev_info->max_rx_queues = HNS5_POE_CPU_GRP_QUE_NUM;
    dev_info->max_tx_queues = (uint16_t)HNS5_ETH_MAX_TXQ_NUM;
    dev_info->min_rx_bufsize = 256;     // bmu buf granule 256 bytes
    dev_info->rx_offload_capa = RTE_ETH_RX_OFFLOAD_SCATTER | RTE_ETH_RX_OFFLOAD_RSS_HASH |
                                RTE_ETH_RX_OFFLOAD_KEEP_CRC | RTE_ETH_RX_OFFLOAD_VLAN_STRIP;
    dev_info->tx_offload_capa = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                                RTE_ETH_TX_OFFLOAD_TCP_CKSUM | RTE_ETH_TX_OFFLOAD_VLAN_INSERT;
    dev_info->dev_capa = 0;
    dev_info->speed_capa = g_mag_mode_support_speeds[priv->param.port_mode];
    /* RSS capability */
    dev_info->flow_type_rss_offloads = HNS5_ETH_RSS_SUPPORT;
    dev_info->hash_key_size = HNS5_RSS_KEY_SIZE;
    dev_info->reta_size = HNS5_RSS_RETA_SIZE;
    return 0;
}

/**
 * @brief Init ppe pool
 * @attention Reentrant, guarded against duplicate init
 * @return ret: 0:succeed, < 0:failed
*/
static int hns5_eth_ppe_pool_init(struct rte_eth_dev *dev, struct rte_mempool *mp)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    int ret = 0;
    struct hidrv_ppe_pool_conf pool_cnf = { 0 };

    if (dev == NULL || mp == NULL) {
        hns5_err(dev, "%s\n", strerror(EINVAL));
        return -EINVAL;
    }
    if ((priv->pool_inited & (1 << mp->pool_id)) != 0) {
        hns5_info(dev, "pool_id:%lu already init\n", mp->pool_id);
        return 0;
    }
    pool_cnf.align_size = 0;
    pool_cnf.buffer_size = rte_pktmbuf_data_room_size(mp) - RTE_PKTMBUF_HEADROOM + rte_pktmbuf_ppe_skip(mp) + BMU_BD_LEN;
    pool_cnf.sop_skip = rte_pktmbuf_ppe_skip(mp);
    pool_cnf.not_sop_skip = rte_pktmbuf_ppe_skip(mp);
    ret = hidrv_ppe_vf_open_pool(priv->ppe_vf_devid, mp->pool_id, &pool_cnf);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_vf_open_pool, pool_id:%lu ret:%d\n", mp->pool_id, ret);
        return ret;
    }
    priv->pool_inited |= (1 << mp->pool_id);
    hns5_info(dev, "pool_id:%lu succeed init\n", mp->pool_id);
    return 0;
}

/**
 * @brief Init pool bound to rxq
 * @return pool_id: >= 0:succeed, < 0:failed
 */
static int hns5_eth_rxq_pool_init(struct rte_eth_dev *dev, uint16_t rx_queue_id,
                                    struct rte_mempool *mp)
{
    int ret = 0;
    uint16_t pool_id = 0;

    if (mp == NULL) {
        return 0;
    }
    if (mp->pool_id >= HNS5_ETH_POOL_MAX) {
        hns5_err(dev, "%s, pool_id is more than max:%u\n", strerror(EINVAL), HNS5_ETH_POOL_MAX - 1);
        return -EINVAL;
    }
    pool_id = (uint16_t)mp->pool_id;
    ret = hns5_eth_ppe_pool_init(dev, mp);
    if (ret != 0) {
        hns5_err(dev, "failed to hns5_eth_ppe_pool_init, port:%d rxq:%u pool_id:%u\n",
            dev->data->port_id, rx_queue_id, pool_id);
        return ret;
    }
    return (int)pool_id;
}

/**
 * @brief Init RX queue, init poe queue and program fw_tnl
 * @attention fw_tnl/group etc. are derived inside driver from vdev devargs: group feeds
 *            poe group config via internal var, fw_tnl goes to a local var then to hns5_ppe_fwl_tnl_config
*/
static int hns5_eth_rx_queue_setup(struct rte_eth_dev *dev, uint16_t rx_queue_id,
                                    uint16_t nb_rx_desc,
                                    unsigned int socket_id __rte_unused,
                                    const struct rte_eth_rxconf *rx_conf __rte_unused,
                                    struct rte_mempool *mp)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hns5_eth_rx_queue *rxq = NULL;
    int pool_id = 0;
    uint32_t tnl_num = 0;
    /* share_group/share_qid derived from devargs, rx_conf from app not used */
    uint16_t group = priv->param.share_group;
    uint16_t qid = rx_queue_id;
    /* fw_tnl derived from devargs into local var, then sent via hns5_ppe_fwl_tnl_config */
    struct rte_fw_tnl_conf fw_tnl_local = { 0 };

    if (rx_queue_id >= HNS5_POE_CPU_GRP_QUE_NUM) {
        hns5_err(dev, "%s, port:%u rx_queue_id:%u exceeds max:%u\n",
            strerror(EINVAL), dev->data->port_id, rx_queue_id,
            HNS5_POE_CPU_GRP_QUE_NUM - 1);
        return -EINVAL;
    }

    /* fw_tnl params derived from devargs into local vars:
     * tnl_num takes nb_rx_queues (valid after configure, safe at first rx_queue_setup),
     * bitfield caps at 4 bits (max 15), clamped; base_vqi=share_group*8; qos_sel/def_qos/def_qos_inc fixed */
    fw_tnl_local.enable = priv->param.fw_tnl_enable;
    fw_tnl_local.base_tnl = priv->param.base_tnl;
    tnl_num = dev->data->nb_rx_queues;
    if (tnl_num > 15) {
        hns5_warn(dev, "nb_rx_queues:%u exceeds fw_tnl tnl_num bitfield max:15, clamp to 15\n", tnl_num);
        tnl_num = 15;
    }
    fw_tnl_local.tnl_num = tnl_num;
    fw_tnl_local.base_vqi = priv->param.share_group * HNS5_POE_CPU_GRP_QUE_NUM;
    fw_tnl_local.qos_sel = 0;
    fw_tnl_local.def_qos = 0;
    fw_tnl_local.def_qos_inc = 1;

    if (group > HNS5_ETH_GROUP_MAX ||
        (qid > HNS5_POE_CPU_GRP_QUE_NUM && qid != HNS5_ETH_RXQ_BY_GROUP)) {
        hns5_err(dev, "%s, port:%u rxq:%u share_group:0x%u share_qid:0x%u\n",
            strerror(EINVAL), dev->data->port_id, rx_queue_id, group, qid);
        return -EINVAL;
    }
    pool_id = hns5_eth_rxq_pool_init(dev, rx_queue_id, mp);
    if (pool_id < 0) {
        return pool_id;
    }

    rxq = priv->rxq + rx_queue_id;
    dev->data->rx_queues[rx_queue_id] = rxq;
    rxq->pool_id = (uint16_t)pool_id;
    rxq->port_id = dev->data->port_id;
    rxq->group = group;
    rxq->queue = qid;
    rxq->poe_vfhandle = priv->poe_vfhandle;
    if (qid == HNS5_ETH_RXQ_BY_GROUP) {
        rxq->type = HNS5_ETH_RXQ_TYPE_GROUP;
    } else {
        rxq->type = HNS5_ETH_RXQ_TYPE_QUEUE;
    }


    ret = hns5_eth_rx_queue_setup_poe(dev, rxq->group, nb_rx_desc);
    if (ret != 0) {
        hns5_err(dev, "failed to hns5_eth_rx_queue_setup_poe, port:%d rxq:%u group:%u ret:%d\n",
            dev->data->port_id, rx_queue_id, rxq->group, ret);
        return ret;
    }
    ret = hns5_ppe_fwl_tnl_config(dev, &fw_tnl_local, rxq->pool_id);
    if (ret != 0) {
        hns5_err(dev, "failed to hns5_ppe_fwl_tnl_config, port:%d rxq:%u pool_id:%u ret:%d\n",
            dev->data->port_id, rx_queue_id, rxq->pool_id, ret);
        return ret;
    }

    return 0;
}

static int hns5_eth_tx_queue_setup(struct rte_eth_dev *dev, uint16_t tx_queue_id, uint16_t nb_tx_desc __rte_unused,
    unsigned int socket_id __rte_unused, const struct rte_eth_txconf *tx_conf)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hns5_eth_tx_queue *txq = NULL;
    // queue spec check
    if (tx_conf == NULL || tx_queue_id >= HNS5_ETH_MAX_TXQ_NUM) {
        hns5_err(dev, "%s\n", strerror(EINVAL));
        return -EINVAL;
    }
    txq = priv->txq + tx_queue_id;
    dev->data->tx_queues[tx_queue_id] = (void *)txq;

    txq->port_id = dev->data->port_id;
    txq->tm_port = priv->param.cport_id;
    txq->tm_queue_id = priv->param.hw_txq_start + (tx_queue_id % priv->param.hw_txq_num);
    txq->poe_vfhandle = priv->poe_vfhandle;
    txq->priv = priv;
    hns5_info(dev, "[BIND] txq[%d] bound to cport %u, tm_port = %u\n", tx_queue_id, priv->param.cport_id, txq->tm_port);

    hns5_eth_set_default_tx_msg(txq);
    hns5_dbg(dev, "portid:%u txq:%u tm_port:%u tm_queue:%u\n",
        txq->port_id, tx_queue_id, txq->tm_port, txq->tm_queue_id);
    return 0;
}

static int hns5_eth_link_update(struct rte_eth_dev *dev, int wait_to_complete __rte_unused)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct rte_eth_link *link = &(dev->data->dev_link);
    enum hidrv_mag_link_status link_status = HIDRV_MAG_LINK_INVALID;

    ret = hidrv_mag_mac_get_link_status(priv->mag_pf_devid, priv->param.mport_id, &link_status);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_mag_mac_get_link_status, ret:%d\n", ret);
        return ret;
    }
    link->link_status = (link_status == HIDRV_MAG_LINK_UP) ? RTE_ETH_LINK_UP : RTE_ETH_LINK_DOWN;
    return 0;
}

static int hns5_eth_fec_get_capability(struct rte_eth_dev *dev, struct rte_eth_fec_capa *speed_fec_capa,
    unsigned int num)
{
    if (dev == NULL) {
        return -EINVAL;
    }
    if (speed_fec_capa == NULL) {
        return RTE_DIM(g_speed_fec_capa_tbl);
    }
    if (num < RTE_DIM(g_speed_fec_capa_tbl)) {
        return -EINVAL;
    }
    memcpy(speed_fec_capa, g_speed_fec_capa_tbl, sizeof(g_speed_fec_capa_tbl));

    return 0;
}

static int hns5_eth_fec_get(struct rte_eth_dev *dev, uint32_t *fec_capa)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    enum hidrv_mag_fec_mode mag_fec_mode_hidrv = HIDRV_MAG_FEC_INVALID;

    ret = hidrv_mag_mac_get_fec_mode(priv->mag_pf_devid, priv->param.mport_id, &mag_fec_mode_hidrv);
    if (ret != 0 || mag_fec_mode_hidrv >= HIDRV_MAG_FEC_INVALID) {
        hns5_err(dev, "failed to hidrv_mag_mac_get_fec_mode, ret:%d fec_mode:%d\n", ret, mag_fec_mode_hidrv);
        return ret;
    }
    *fec_capa = g_mag_fec_mode_hidrv_to_rte[mag_fec_mode_hidrv];

    return 0;
}

static int hns5_eth_fec_set(struct rte_eth_dev *dev, uint32_t fec_capa)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    enum hidrv_mag_fec_mode mag_fec_mode_hidrv = HIDRV_MAG_FEC_INVALID;

    for (uint32_t i = 0; i < RTE_DIM(g_mag_fec_mode_hidrv_to_rte); i++) {
        if (g_mag_fec_mode_hidrv_to_rte[i] == fec_capa) {
            mag_fec_mode_hidrv = i;
            break;
        }
    }
    if (mag_fec_mode_hidrv == HIDRV_MAG_FEC_INVALID) {
        hns5_err(dev, "failed to find hidrv_mag_fec_mode, fec_capa:%u\n", fec_capa);
        return -EINVAL;
    }
    ret = hidrv_mag_mac_set_fec_mode(priv->mag_pf_devid, priv->param.mport_id, mag_fec_mode_hidrv);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_mag_mac_set_fec_mode, ret:%d\n", ret);
        return ret;
    }

    return 0;
}

/**
 * @brief Get flow control status, only on/off supported, no parameter config
*/
static int hns5_eth_flow_ctrl_get(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    unsigned int pause_rx_enable = 0;
    unsigned int pause_tx_enable = 0;

    ret = hidrv_mag_get_pause_enable(priv->mag_pf_devid, priv->param.mport_id, HIDRV_MAG_DIR_RX, &pause_rx_enable);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_mag_get_pause_enable, mport:%u dir:rx ret:%d\n", priv->param.mport_id, ret);
        return ret;
    }
    ret = hidrv_mag_get_pause_enable(priv->mag_pf_devid, priv->param.mport_id, HIDRV_MAG_DIR_TX, &pause_tx_enable);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_mag_get_pause_enable, mport:%u dir:tx ret:%d\n", priv->param.mport_id, ret);
        return ret;
    }
    if (pause_rx_enable == 0 && pause_tx_enable == 0) {
        fc_conf->mode = RTE_ETH_FC_NONE;
    } else if (pause_rx_enable == 1 && pause_tx_enable == 0) {
        fc_conf->mode = RTE_ETH_FC_RX_PAUSE;
    } else if (pause_rx_enable == 0 && pause_tx_enable == 1) {
        fc_conf->mode = RTE_ETH_FC_TX_PAUSE;
    } else {
        fc_conf->mode = RTE_ETH_FC_FULL;
    }

    return 0;
}

static int hns5_eth_flow_ctrl_set(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    unsigned int rx_enable_get = 0;
    unsigned int tx_enable_get = 0;
    unsigned int rx_enable_set = 0;
    unsigned int tx_enable_set = 0;

    ret = hidrv_mag_get_pause_enable(priv->mag_pf_devid, priv->param.mport_id, HIDRV_MAG_DIR_RX, &rx_enable_get);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_mag_get_pause_enable, mport:%u dir:rx ret:%d\n", priv->param.mport_id, ret);
        return ret;
    }
    ret = hidrv_mag_get_pause_enable(priv->mag_pf_devid, priv->param.mport_id, HIDRV_MAG_DIR_TX, &tx_enable_get);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_mag_get_pause_enable, mport:%u dir:tx ret:%d\n", priv->param.mport_id, ret);
        return ret;
    }
    if (fc_conf->mode == RTE_ETH_FC_FULL) {
        rx_enable_set = 1;
        tx_enable_set = 1;
    } else if (fc_conf->mode == RTE_ETH_FC_RX_PAUSE) {
        rx_enable_set = 1;
    } else if (fc_conf->mode == RTE_ETH_FC_TX_PAUSE) {
        tx_enable_set = 1;
    }

    if (rx_enable_get != rx_enable_set) {
        ret = hidrv_mag_set_pause_enable(priv->mag_pf_devid, priv->param.mport_id, HIDRV_MAG_DIR_RX, rx_enable_set);
        if (ret != 0) {
            hns5_err(dev, "failed to hidrv_mag_set_pause_enable, mport:%u dir:rx ret:%d\n", priv->param.mport_id, ret);
            return ret;
        }
    }
    if (tx_enable_get != tx_enable_set) {
        ret = hidrv_mag_set_pause_enable(priv->mag_pf_devid, priv->param.mport_id, HIDRV_MAG_DIR_TX, tx_enable_set);
        if (ret != 0) {
            hns5_err(dev, "failed to hidrv_mag_set_pause_enable, mport:%u dir:tx ret:%d\n", priv->param.mport_id, ret);
            return ret;
        }
    }

    return 0;
}


static const char *MZ_RTE_ETH_DEV_RX_QUEUE = "rte_eth_dev_rx_queue";
static const char *MZ_RTE_ETH_DEV_TX_QUEUE = "rte_eth_dev_tx_queue";
/**
 * @brief Release RX queue
 * @note  Queue objects live in the static priv->rxq[] array, managed with priv lifetime,
 *        no memory freed here; ppe pool/poe group are shared across queues and released
 *        by dev_close, not by single-queue release; the framework
 *        (rte_eth_dev_rx_queue_release) clears dev->data->rx_queues[] references.
 * @param rxq  queue ptr from framework, NULL means queue not setup
 */
static void hns5_eth_rx_queue_release(void *rxq)
{
    struct hns5_eth_rx_queue *queue = (struct hns5_eth_rx_queue *)rxq;
    struct rte_eth_dev *dev = NULL;
    uint16_t port_id = 0;

    if (queue == NULL) {
        /* queue not setup or already released, return */
        return;
    }
    port_id = queue->port_id;
    dev = &rte_eth_devices[port_id];

    /* reset queue object state to prevent stale data misuse */
    memset(queue, 0, sizeof(*queue));
    queue->type = HNS5_ETH_RXQ_TYPE_INVALID;

    hns5_dbg(dev, "port:%u rxq released\n", port_id);
}

/**
 * @brief Release TX queue
 * @note  txq has no dynamic resources (queue object in static priv->txq[] array),
 *        only reset its state; the framework (rte_eth_dev_tx_queue_release)
 *        clears dev->data->tx_queues[] references.
 * @param txq  queue ptr from framework, NULL means queue not setup
 */
static void hns5_eth_tx_queue_release(void *txq)
{
    struct hns5_eth_tx_queue *queue = (struct hns5_eth_tx_queue *)txq;
    struct rte_eth_dev *dev = NULL;
    uint16_t port_id = 0;

    if (queue == NULL) {
        /* queue not setup or already released, return */
        return;
    }
    port_id = queue->port_id;
    dev = &rte_eth_devices[port_id];

    /* txq has no dynamic resources, just reset queue object state */
    memset(queue, 0, sizeof(*queue));

    hns5_dbg(dev, "port:%u txq released\n", port_id);
}

/* Packet types recognized on the driver RX path (aligned with hns5_l3_l4_to_ptype L3/L4 maps) */
static const uint32_t hns5_supported_ptypes[] = {
    RTE_PTYPE_L2_ETHER_ARP,
    RTE_PTYPE_L2_ETHER_LLDP,
    RTE_PTYPE_L2_ETHER_TIMESYNC,
    RTE_PTYPE_L3_IPV4,
    RTE_PTYPE_L3_IPV4_EXT,
    RTE_PTYPE_L3_IPV6,
    RTE_PTYPE_L3_IPV6_EXT,
    RTE_PTYPE_L4_ICMP,
    RTE_PTYPE_L4_IGMP,
    RTE_PTYPE_L4_SCTP,
    RTE_PTYPE_L4_TCP,
    RTE_PTYPE_L4_UDP,
    RTE_PTYPE_TUNNEL_ESP,
    RTE_PTYPE_TUNNEL_GRE,
    RTE_PTYPE_UNKNOWN,
};

static const uint32_t *
hns5_dev_supported_ptypes_get(__rte_unused struct rte_eth_dev *dev)
{
    return hns5_supported_ptypes;
}

static const struct eth_dev_ops hns5_eth_dev_ops = {
    .dev_configure = hns5_eth_dev_configure,
    .dev_infos_get = hns5_eth_dev_infos_get,
    .dev_supported_ptypes_get = hns5_dev_supported_ptypes_get,
    .dev_start = hns5_eth_dev_start,
    .dev_stop = hns5_eth_dev_stop,
    .dev_close = hns5_eth_dev_close,
    .dev_set_link_up = hns5_eth_dev_set_link_up,
    .dev_set_link_down = hns5_eth_dev_set_link_down,
    .rx_queue_setup = hns5_eth_rx_queue_setup,
    .rx_queue_release = hns5_eth_rx_queue_release,
    .tx_queue_setup = hns5_eth_tx_queue_setup,
    .tx_queue_release = hns5_eth_tx_queue_release,
    .link_update = hns5_eth_link_update,
    .stats_get = hns5_eth_stats_get,
    .stats_reset = hns5_eth_stats_reset,
    .xstats_get = hns5_eth_xstats_get,
    .xstats_reset = hns5_eth_xstats_reset,
    .xstats_get_names = hns5_eth_xstats_get_names,
    .flow_ctrl_get = hns5_eth_flow_ctrl_get,  // flow control
    .flow_ctrl_set = hns5_eth_flow_ctrl_set,
    .fec_get_capability = hns5_eth_fec_get_capability, // FEC
    .fec_get = hns5_eth_fec_get,
    .fec_set = hns5_eth_fec_set,
    .mtu_set = hns5_eth_dev_mtu_set,
    .flow_ops_get = hns5_dev_flow_ops_get,
    /* VLAN offload */
    .vlan_offload_set = hns5_vlan_offload_set,
    .vlan_pvid_set = hns5_vlan_pvid_set,
    /* RSS */
    .rss_hash_update = hns5_rss_hash_update,
    .rss_hash_conf_get = hns5_rss_hash_conf_get,
    .reta_update = hns5_rss_reta_update,
    .reta_query = hns5_rss_reta_query,
    .promiscuous_enable = hns5_promiscuous_enable,
    .promiscuous_disable = hns5_promiscuous_disable,
    .allmulticast_enable = hns5_allmulticast_enable,
    .allmulticast_disable = hns5_allmulticast_disable,
    .mac_addr_set = hns5_mac_addr_set,
};

static struct rte_eth_dev *hns5_eth_dev_create(struct rte_vdev_device *vdev)
{
    struct rte_eth_dev_data *data = NULL;
    struct rte_eth_dev *dev = NULL;
    struct hns5_eth_priv *priv = NULL;
    const unsigned flags = 0;
    const struct rte_memzone *mz_rx = NULL;
    const struct rte_memzone *mz_tx = NULL;
    char dev_name[HNS5_ETH_DEV_INFO_NAME_LEN] = {0};
    const char *name = NULL;

    name = rte_vdev_device_name(vdev);
    if (name == NULL) {
        return NULL;
    }
    // private data mounted on eth_dev->data->dev_private
    dev = rte_eth_vdev_allocate(vdev, sizeof(struct hns5_eth_priv));
    if (dev == NULL) {
        return NULL;
    }
    hns5_info(dev, "[BIND] eth_dev allocated: port_id = %u, device_name = %s\n", dev->data->port_id, rte_vdev_device_name(vdev));
    data = dev->data;
    priv = data->dev_private;

    data->mac_addrs = &priv->address;
    snprintf(dev_name, sizeof(dev_name), "%s_%d", MZ_RTE_ETH_DEV_RX_QUEUE, dev->data->port_id);
    mz_rx = rte_memzone_reserve(dev_name, sizeof(void *) * HNS5_ETH_MAX_RXQ_NUM, rte_socket_id(), flags);
    if (mz_rx == NULL) {
        rte_errno = ENOMEM;
        return NULL;
    }
    memset(mz_rx->addr, 0, sizeof(void *) * HNS5_ETH_MAX_RXQ_NUM);
    data->rx_queues = mz_rx->addr;

    snprintf(dev_name, sizeof(dev_name), "%s_%d", MZ_RTE_ETH_DEV_TX_QUEUE, dev->data->port_id);
    mz_tx = rte_memzone_reserve(dev_name, sizeof(void *) * HNS5_ETH_MAX_TXQ_NUM, rte_socket_id(), flags);
    if (mz_tx == NULL) {
        rte_errno = ENOMEM;
        (void)rte_memzone_free(mz_rx);
        return NULL;
    }
    memset(mz_tx->addr, 0, sizeof(void *) * HNS5_ETH_MAX_TXQ_NUM);
    data->tx_queues = mz_tx->addr;

    data->nb_rx_queues = (uint16_t)HNS5_ETH_MAX_RXQ_NUM;
    data->nb_tx_queues = (uint16_t)HNS5_ETH_MAX_TXQ_NUM;
    data->numa_node = rte_socket_id();

    hns5_rss_set_default_args(priv);

    dev->dev_ops = &hns5_eth_dev_ops;

    return dev;
}

static int vdev_args_parse_uint32(__rte_unused const char *key, const char *value, void *opaque)
{
    uint32_t *val = opaque;
    char *endptr = NULL;
    unsigned long tmp = 0;

    if (value == NULL || opaque == NULL) {
        return -EINVAL;
    }
    errno = 0;
    // empty third arg auto-detects number base
    tmp = strtoul(value, &endptr, 0);

    if (errno != 0 || endptr == value || tmp > UINT32_MAX) {
        return -EINVAL;
    }

    *val = (uint32_t)tmp;
    return 0;
}

static const char *
hns5_kvargs_get(const struct rte_kvargs *kvlist, const char *key)
{
	unsigned int i;

	if (kvlist == NULL)
		return NULL;
	for (i = 0; i < kvlist->count; ++i) {
		if (key != NULL && strcmp(kvlist->pairs[i].key, key) != 0)
			continue;
		return kvlist->pairs[i].value;
	}
	return NULL;
}

/**
 * @attention Mismatched mode/speed errors at mac init, not checked here
*/
static int hns5_eth_mac_params_parse(struct rte_eth_dev *dev, struct rte_kvargs *kvlist,
    struct hns5_eth_priv *priv)
{
    const char *args_mode = NULL;

    args_mode = hns5_kvargs_get(kvlist, "mode");
    if (args_mode == NULL) {
        hns5_err(dev, "no args:mode\n");
        return -EINVAL;
    }
    priv->param.port_mode = HIDRV_MAG_PORT_MODE_INVALID;
    for (uint32_t i = 0; i < RTE_DIM(g_mag_mode_maps); i++) {
        if (strcmp(g_mag_mode_maps[i], args_mode) == 0) {
            priv->param.port_mode = i;
            break;
        }
    }
    if (priv->param.port_mode == HIDRV_MAG_PORT_MODE_INVALID) {
        hns5_err(dev, "invalid mode:%s\n", args_mode);
        return -EINVAL;
    }

    priv->param.port_speed = HIDRV_MAG_PORT_SPEED_NO_USE;

    return 0;
}

/**
* @brief Parse vdev args
*/
static int hns5_eth_params_parse(struct rte_vdev_device *vdev, struct rte_eth_dev *dev)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    const char *args = NULL;
    struct rte_kvargs *kvlist = NULL;

    args = rte_vdev_device_args(vdev);
    if (args == NULL) {
        hns5_err(dev, "failed to rte_vdev_device_args\n");
        return -EINVAL;
    }
    kvlist = rte_kvargs_parse(args, NULL);
    if (kvlist == NULL) {
        hns5_err(dev, "failed to rte_kvargs_parse\n");
        return -EINVAL;
    }
    if (rte_kvargs_count(kvlist, "port") != 1) {
        goto free_kvargs;
    }
    ret = rte_kvargs_process(kvlist, "port", &vdev_args_parse_uint32, &priv->param.cport_id);
    if (ret < 0) {
        goto free_kvargs;
    }

    /* share_group required, assigned externally to avoid group conflicts across ports */
    if (rte_kvargs_count(kvlist, "share_group") != 1) {
        hns5_err(dev, "share_group must be specified in devargs, assigned externally to avoid group conflicts\n");
        goto free_kvargs;
    }
    ret = rte_kvargs_process(kvlist, "share_group", &vdev_args_parse_uint32, &priv->param.share_group);
    if (ret < 0) {
        goto free_kvargs;
    }
    if (priv->param.share_group > HNS5_ETH_GROUP_MAX) {
        hns5_err(dev, "invalid share_group:%u, max:%u\n", priv->param.share_group, HNS5_ETH_GROUP_MAX);
        goto free_kvargs;
    }

    ret = hns5_eth_mac_params_parse(dev, kvlist, priv);
    if (ret != 0) {
        goto free_kvargs;
    }

    /* fw_tnl optional, enabled by default, devargs fw_tnl=0 disables */
    uint32_t fw_tnl_enable = 1;
    if (rte_kvargs_count(kvlist, "fw_tnl") == 1) {
        ret = rte_kvargs_process(kvlist, "fw_tnl", &vdev_args_parse_uint32, &fw_tnl_enable);
        if (ret < 0) {
            goto free_kvargs;
        }
        if (fw_tnl_enable != 0 && fw_tnl_enable != 1) {
            hns5_err(dev, "invalid fw_tnl:%u, expect 0 or 1\n", fw_tnl_enable);
            goto free_kvargs;
        }
    }
    priv->param.fw_tnl_enable = (uint8_t)fw_tnl_enable;
    /* base_tnl optional, default=cport_id*8 (step HNS5_POE_CPU_GRP_QUE_NUM, 8 per cport), devargs override */
    priv->param.base_tnl = priv->param.cport_id * HNS5_POE_CPU_GRP_QUE_NUM;
    if (rte_kvargs_count(kvlist, "base_tnl") == 1) {
        ret = rte_kvargs_process(kvlist, "base_tnl", &vdev_args_parse_uint32, &priv->param.base_tnl);
        if (ret < 0) {
            goto free_kvargs;
        }
    }

    /* optional args, defaults used if absent */
    if (rte_kvargs_count(kvlist, "macport") == 1) {
        ret = rte_kvargs_process(kvlist, "macport", &vdev_args_parse_uint32, &priv->param.mport_id);
        if (ret < 0) {
            goto free_kvargs;
        }
    } else {
        priv->param.mport_id = priv->param.cport_id;
    }
    /* cport/mport used as port bitmap index in (1U << id) masks, must be < 32 to avoid shift UB */
    if (priv->param.cport_id >= 32 || priv->param.mport_id >= 32) {
        hns5_err(dev, "invalid cport_id:%u/mport_id:%u, must be < 32\n",
            priv->param.cport_id, priv->param.mport_id);
        goto free_kvargs;
    }
    if (rte_kvargs_count(kvlist, "hw_txq_num") == 1) {
        ret = rte_kvargs_process(kvlist, "hw_txq_num", &vdev_args_parse_uint32, &priv->param.hw_txq_num);
        if (ret < 0) {
            goto free_kvargs;
        }
    } else {
        priv->param.hw_txq_num = HNS5_ETH_DEFAULT_HW_TXQ_NUM;
    }
    if (rte_kvargs_count(kvlist, "hw_txq_id") == 1) {
        ret = rte_kvargs_process(kvlist, "hw_txq_id", &vdev_args_parse_uint32, &priv->param.hw_txq_start);
        if (ret < 0) {
            goto free_kvargs;
        }
    } else {
        priv->param.hw_txq_start = priv->param.cport_id * 16; /* TM queue maps 16x port, matches default hw_txq_num=16 */
    }

    rte_kvargs_free(kvlist);

    hns5_info(dev, "cport:%u macport:%u mode:%u speed:%u share_group:%u base_tnl:%u fw_tnl_enable:%u hw_txq_start:%u hw_txq_num:%u\n",
        priv->param.cport_id, priv->param.mport_id, priv->param.port_mode, priv->param.port_speed,
        priv->param.share_group, priv->param.base_tnl, priv->param.fw_tnl_enable,
        priv->param.hw_txq_start, priv->param.hw_txq_num);
    return 0;

free_kvargs:
    rte_kvargs_free(kvlist);
    return -EINVAL;
}

static int net_hns5_vdev_probe(struct rte_vdev_device *vdev)
{
    int ret = 0;
    struct rte_eth_dev *dev = NULL;

    dev = hns5_eth_dev_create(vdev);
    if (dev == NULL) {
        return -EINVAL;
    }
    ret = hns5_eth_params_parse(vdev, dev);
    if (ret != 0) {
        hns5_err(dev, "failed to hns5_eth_params_parse\n");
        return ret;
    }
    /* accelerator hardware init */
    ret = hns5_eth_poe_init(dev);
    if (ret != 0) {
        hns5_err(dev, "failed to hns5_eth_poe_init, ret:%d\n", ret);
        return ret;
    }
    ret = hns5_eth_ppe_init_lsw(dev);
    if (ret != 0) {
        hns5_err(dev, "failed to hns5_eth_ppe_init, ret:%d\n", ret);
        return ret;
    }
    ret = hns5_flow_init(dev);
    if (ret != 0) {
        hns5_err(dev, "failed to hns5_flow_init, ret:%d\n", ret);
        return ret;
    }
    if (hns5_get_tm_init_status() != true) {
        ret = hns5_tm_platform_dev_probe();
        if (ret != 0) {
            hns5_err(dev, "failed to hns5_tm_platform_dev_probe, ret:%d\n", ret);
            return ret;
        }
    }

    /* Generate random MAC and init L2 entry */
    ret = hns5_l2_mac_init(dev);
    if (ret != 0) {
        hns5_err(dev, "failed to hns5_l2_mac_init, ret:%d\n", ret);
        return ret;
    }

    rte_eth_dev_probing_finish(dev);
    return 0;
}

static int net_hns5_vdev_remove(struct rte_vdev_device *vdev)
{
    const char *name = rte_vdev_device_name(vdev);
    struct rte_eth_dev *dev = (struct rte_eth_dev *)rte_eth_dev_allocated(name);
    char dev_name[HNS5_ETH_DEV_INFO_NAME_LEN] = {0};
    const struct rte_memzone *mz_rx = NULL;
    const struct rte_memzone *mz_tx = NULL;

    if (name == NULL)
        return -EINVAL;

    if (dev == NULL)
        return -EINVAL;

    (void)hns5_eth_dev_stop(dev);

    rte_eth_dev_release_port(dev);
    snprintf(dev_name, sizeof(dev_name), "%s_%d", MZ_RTE_ETH_DEV_RX_QUEUE, dev->data->port_id);
    mz_rx = rte_memzone_lookup(dev_name);
    if (mz_rx != NULL) {
        rte_memzone_free(mz_rx);
    }
    snprintf(dev_name, sizeof(dev_name), "%s_%d", MZ_RTE_ETH_DEV_TX_QUEUE, dev->data->port_id);
    mz_tx = rte_memzone_lookup(dev_name);
    if (mz_tx != NULL) {
        rte_memzone_free(mz_tx);
    }
    return 0;
}

int hns5_set_ppe_bypass_mode(uint8_t port_id, enum hidrv_ppe_bypass_mode mode)
{
    struct rte_eth_dev *dev = &rte_eth_devices[port_id];
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    int ret;

    if (priv == NULL) {
        return -EINVAL;
    }

    if (mode != HIDRV_PPE_LSW_MODE && mode != HIDRV_PPE_BYPASS_MODE) {
        return -EINVAL;
    }

    ret = hidrv_ppe_pf_set_bypass(priv->ppe_pf_devid, priv->param.mport_id, HIDRV_PPE_DIR_RX,
        priv->param.cport_id, mode);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_pf_set_bypass rx, ret:%d\n", ret);
        return ret;
    }

    ret = hidrv_ppe_pf_set_bypass(priv->ppe_pf_devid, priv->param.cport_id, HIDRV_PPE_DIR_TX,
        priv->param.mport_id, mode);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_pf_set_bypass tx, ret:%d\n", ret);
        return ret;
    }

    hns5_info(dev,
        "[PPE_BYPASS] === PPE BYPASS MODE SET OK ===\n"
        "  port_id:       %u\n"
        "  ppe_pf_devid:  %d\n"
        "  mode:          %u\n",
        port_id, priv->ppe_pf_devid, mode);

    return 0;
}

int hns5_lsw_mac_table_write(uint8_t port_id, struct rte_ether_addr *mac)
{
    struct rte_eth_dev *dev = &rte_eth_devices[port_id];
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hidrv_lsw_l2_key hidrv_key;
    struct hidrv_lsw_l2_actn hidrv_actn;
    int ret;

    if (priv == NULL || mac == NULL) {
        return -EINVAL;
    }

    memset(&hidrv_key, 0, sizeof(hidrv_key));
    memset(&hidrv_actn, 0, sizeof(hidrv_actn));

    (void)memcpy(hidrv_key.mac, mac->addr_bytes, HNS5_ETH_ALEN);
    hidrv_key.vlan_id = 0;

    hidrv_actn.des_type = HIDRV_LSW_L2_DES_CPORT_PORT;
    hidrv_actn.des_id.des_cpu_port.port_id = priv->param.cport_id;
    hidrv_actn.static_item = 1;
    hidrv_actn.aging_en = 0;

    ret = hidrv_ppe_lsw_add_l2_entry(priv->ppe_pf_devid, &hidrv_key, &hidrv_actn);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_ppe_lsw_add_l2_entry, ret:%d\n", ret);
        return ret;
    }

    return 0;
}

int hns5_promiscuous_enable(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    int ret;

    if (priv == NULL) {
        return -EINVAL;
    }

    if (priv->promisc_mode == 1) {
        return 0;
    }

    priv->promisc_mode = 1;

    ret = hns5_set_ppe_bypass_mode(dev->data->port_id, HIDRV_PPE_BYPASS_MODE);
    if (ret != 0) {
        hns5_err(dev, "failed to set bypass mode, ret:%d\n", ret);
        priv->promisc_mode = 0;
        return ret;
    }

    hns5_info(dev, "[PROMISC] port:%u promiscuous mode enabled, set PPE to HIDRV_PPE_BYPASS_MODE\n",
        dev->data->port_id);

    return 0;
}

int hns5_promiscuous_disable(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    int ret;

    if (priv == NULL) {
        return -EINVAL;
    }

    if (priv->promisc_mode == 0) {
        return 0;
    }

    hns5_info(dev,
        "[PROMISC] === DISABLE PROMISCUOUS ===\n"
        "  port_id:       %u\n"
        "  mac_addr:      %02x:%02x:%02x:%02x:%02x:%02x\n"
        "  mport_id:      %u\n"
        "  cport_id:      %u\n"
        "  current mode:  %u(PROMISC)\n"
        "  target mode:   %u(NORMAL)\n",
        dev->data->port_id,
        priv->address.addr_bytes[0], priv->address.addr_bytes[1], priv->address.addr_bytes[2],
        priv->address.addr_bytes[3], priv->address.addr_bytes[4], priv->address.addr_bytes[5],
        priv->param.mport_id,
        priv->param.cport_id,
        priv->promisc_mode,
        HIDRV_PPE_LSW_MODE);

    priv->promisc_mode = 0;

    ret = hns5_set_ppe_bypass_mode(dev->data->port_id, HIDRV_PPE_LSW_MODE);
    if (ret != 0) {
        hns5_err(dev, "failed to set lsx mode, ret:%d\n", ret);
        priv->promisc_mode = 1;
        return ret;
    }

    return 0;
}

/* Enable mcast mode: add ingress flow matching DMAC I/G bit, send mcast to CPU/RX */
int hns5_allmulticast_enable(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    int ret;

    if (priv == NULL) {
        return -EINVAL;
    }

    ret = hns5_ppe_lsw_mcast_mode_config(dev, 1);
    if (ret != 0) {
        hns5_err(dev, "failed to enable mcast mode, ret:%d\n", ret);
        return ret;
    }

    hns5_info(dev, "[MCAST] port:%u allmulticast mode enabled, dmac I/G bit flow added\n",
        dev->data->port_id);

    return 0;
}

/* Disable mcast mode: delete ingress flow matching DMAC I/G bit, drop mcast */
int hns5_allmulticast_disable(struct rte_eth_dev *dev)
{
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    int ret;

    if (priv == NULL) {
        return -EINVAL;
    }

    ret = hns5_ppe_lsw_mcast_mode_config(dev, 0);
    if (ret != 0) {
        hns5_err(dev, "failed to disable mcast mode, ret:%d\n", ret);
        return ret;
    }

    hns5_info(dev, "[MCAST] port:%u allmulticast mode disabled, dmac I/G bit flow deleted\n",
        dev->data->port_id);

    return 0;
}

static struct rte_vdev_driver net_hns5_pmd = {
    .probe =  net_hns5_vdev_probe,
    .remove = net_hns5_vdev_remove,
};

RTE_PMD_REGISTER_VDEV(net_hns5, net_hns5_pmd);
RTE_PMD_REGISTER_PARAM_STRING(net_hns5,
    "port=<0-31>"
    "macport=<0-31>"
    "share_group=<0-31>"
    "fw_tnl=<0|1>"
    "base_tnl=<0-4095>"
    "hw_txq_id=<N>"
    "hw_txq_num=<N>"
    "mode=<100BASE_FX|1000BASE_X|SGMII|QSGMII|2G5BASE_X|10GBASE_R|FLEXMII_5G2P|FLEXMII_10G2P|FLEXMII_20G2P|\
    FLEXMII_10G4P|FLEXMII_20G4P|5GBASE_R|5GBASE_KR|XAUI|25GBASE_R|40GBASE_R|50GBASE_R|100GBASE_R>"
);
