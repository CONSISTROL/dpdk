#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

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


#include "hidrv_subctrl_api.h"
#include "hidrv_udev.h"
#include "hidrv_map.h"
#include "hidrv_poe_pf_api.h"
#include "hidrv_poe_vf_api.h"
#include "hidrv_poe_xtp.h"
#include "ndrv_poe_pf_api.h"
#include "ndrv_poe_vf_api.h"

#include "hns5_poe.h"
#include "hns5_eth.h"
#include "hns5_log.h"
#include "hns5_mempool.h"

#define POE_PF_ID 0
#define POE_VF_ID 0
#define POE_DEV_ID 0
#define POE_DIE_ID  0

#define ISOLATE_DEFAULT_VALUE 0xffffffff

static int hns5_poe_module_init(struct rte_eth_dev *dev)
{
    static bool inited = false;
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    if (inited == true) {
        return 0;
    }
    ret = hidrv_poe_order_set_timeout(priv->poe_pf_devid, HNS5_POE_ORDER_TMOUT_TIME);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_poe_order_set_timeout, ret:%d\n", ret);
        return ret;
    }
    inited = true;
    return 0;
}

static int hns5_poe_port_init(struct rte_eth_dev *dev)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    ret = hidrv_poe_get_pf_devid(POE_DIE_ID, POE_PF_ID);
    if (ret < 0) {
        hns5_err(dev, "failed to hidrv_poe_get_pf_devid, ret:%d\n", ret);
        return ret;
    }
    priv->poe_pf_devid = ret;
    priv->poe_pfhandle = hidrv_poe_pf_get_ndrv_handle(priv->poe_pf_devid);
    if (priv->poe_pfhandle == NULL) {
        hns5_err(dev, "failed to hidrv_poe_pf_get_ndrv_handle\n");
        return ret;
    }

    ret = hidrv_poe_get_vf_devid(POE_DIE_ID, POE_PF_ID, POE_VF_ID);
    if (ret < 0) {
        hns5_err(dev, "failed to hidrv_poe_get_vf_devid, ret:%d\n", ret);
        return ret;
    }
    priv->poe_vf_devid = ret;
    priv->poe_vfhandle = hidrv_poe_vf_get_ndrv_handle(priv->poe_vf_devid);
    if (priv->poe_vfhandle == NULL) {
        hns5_err(dev, "failed to hidrv_poe_vf_get_ndrv_handle\n");
        return ret;
    }
    return 0;
}

int hns5_eth_poe_init(struct rte_eth_dev *dev)
{
    int ret = 0;

    ret = hns5_poe_port_init(dev); // init poe pfhandle and vfhandle
    if (ret != 0) {
        return ret;
    }
    ret = hns5_poe_module_init(dev); // only order timeout is set
    if (ret != 0) {
        return ret;
    }
    return 0;
}

int hns5_eth_rx_queue_setup_poe(struct rte_eth_dev *dev, uint32_t group, uint16_t depth)
{
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hidrv_poe_grp_cfg config = { 0 };
    struct hidrv_poe_q_th_tp_sel_cfg templete_cfg = { 0 };
    static uint64_t inited = 0;

    /* Prevent duplicate initialization */
    if (inited & (uint64_t)(1 << group)) {
        hns5_dbg(dev, "group:%u already init\n", group);
        return 0;
    }
    config.order_en = 0;
    config.bp_ppe_th = depth;
    config.int_th = 0;
    config.flow_en = 1;
    ret = hidrv_poe_open_group(priv->poe_vf_devid, group, &config);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_poe_open_group, ret:%d\n", ret);
        return ret;
    }
    /* Init all queues inside the group at once */
    for (uint32_t i = 0; i < HNS5_POE_CPU_GRP_QUE_NUM; i++) {
        ret = hidrv_poe_set_group_queue_weight(priv->poe_vf_devid, group, i, 255);
        if (ret != 0) {
            hns5_err(dev, "failed to hidrv_poe_set_group_queue_weight, ret:%d group:%u queue:%u\n", ret, group, i);
            return ret;
        }
        ret = hidrv_poe_set_group_queue_priority(priv->poe_vf_devid, group, i, i);
        if (ret != 0) {
            hns5_err(dev, "failed to hidrv_poe_set_group_queue_priority, ret:%d group:%u queue:%u\n", ret, group, i);
            return ret;
        }
        /* Template values configured by kernel */
        templete_cfg.q_low_th_tpid = HNS5_POE_TH_TPMLTE_ID;
        templete_cfg.q_high_th_tpid = HNS5_POE_TH_TPMLTE_ID;
        templete_cfg.q_indraw_pkt_th_tpid = HNS5_POE_TH_TPMLTE_ID;
        ret = hidrv_poe_set_q_th_tp_sel(priv->poe_vf_devid, group, i, &templete_cfg);
        if (ret != 0) {
            hns5_err(dev, "failed to hidrv_poe_set_q_th_tp_sel, ret:%d group:%u queue:%u\n", ret, group, i);
            return ret;
        }
    }

    inited |= (uint64_t)(1 << group);
    return 0;
}
