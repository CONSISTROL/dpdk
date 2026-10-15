#include <string.h>
#include <errno.h>

#include "ndrv_mag_api.h"
#include "hidrv_mag_api.h"
#include "hidrv_mag_xtp.h"

#include "hns5_mag.h"
#include "hns5_log.h"

#define MAG_PF_ID 0
#define MAG_VF_ID 0
#define MAG_DEV_ID 0
#define MAG_DIE_ID  0

static int hns5_mag_module_init(struct rte_eth_dev *dev)
{
    static bool inited = false;
    int ret = 0;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    struct hidrv_mag_setup_params setup_param = { 0 };
    struct hidrv_mag_ext_para ext_para = { 0 };

    if (inited == true) {
        return 0;
    }
    ret = hidrv_mag_setup(priv->mag_pf_devid, &setup_param, &ext_para);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_mag_setup, ret:%d\n", ret);
        return ret;
    }
    inited = true;
    return 0;
}

int hns5_eth_mag_init(struct rte_eth_dev *dev)
{
    int ret = 0;
    struct hidrv_mag_mac_init_params init_param = { 0 };
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);
    static uint64_t inited = 0;

    if (inited & (1ULL << priv->param.mport_id)) {
        return 0;
    }
    priv->mag_pf_devid = hidrv_mag_get_pf_devid(MAG_DIE_ID, MAG_PF_ID);
    if (priv->mag_pf_devid < 0) {
        hns5_err(dev, "failed to hidrv_mag_get_pf_devid\n");
        return -EINVAL;
    }
    ret = hns5_mag_module_init(dev);
    if (ret != 0) {
        return ret;
    }

    priv->mag_handle = hidrv_mag_get_ndrv_handle(priv->mag_pf_devid);
    if (priv->mag_handle == NULL) {
        hns5_err(dev, "failed to get mag handle\n");
        return -EINVAL;
    }

    init_param.port_mode = priv->param.port_mode;
    init_param.port_speed = priv->param.port_speed;
    init_param.fec_mode = HIDRV_MAG_NO_FEC;
    init_param.duplex_mode = priv->param.duplex_mode;
    init_param.neg_enable = priv->param.autoneg;
    init_param.max_frame_len = HNS5_FRAME_DEFAULT;
    init_param.min_frame_len = HNS5_FRAME_MIN;
    ret = hidrv_mag_mac_init(priv->mag_pf_devid, priv->param.mport_id, &init_param);
    if (ret != 0) {
        hns5_err(dev, "failed to hidrv_mag_mac_init, mport:%u\n", priv->param.mport_id);
        return ret;
    }
    inited |= (1ULL << priv->param.mport_id);

    return 0;
}

int hns5_eth_set_crc_offload(struct rte_eth_dev *dev, uint32_t enable)
{
    unsigned int ret;
    struct hns5_eth_priv *priv = hns5_eth_priv_get(dev);

    if (priv->mag_handle == NULL) {
        hns5_err(dev, "mag handle not initialized\n");
        return -EINVAL;
    }

    ret = ndrv_mag_mac_set_fcs_enable(priv->mag_handle, priv->param.mport_id, NDRV_MAG_DIR_TX, enable);
    if (ret != 0) {
        hns5_err(dev, "failed to set TX FCS enable, ret:%u\n", ret);
        return (int)ret;
    }

    ret = ndrv_mag_mac_set_fcs_enable(priv->mag_handle, priv->param.mport_id, NDRV_MAG_DIR_RX, enable);
    if (ret != 0) {
        hns5_err(dev, "failed to set RX FCS enable, ret:%u\n", ret);
        return (int)ret;
    }

    ret = ndrv_mag_mac_set_fcs_strip(priv->mag_handle, priv->param.mport_id, enable);
    if (ret != 0) {
        hns5_err(dev, "failed to set FCS strip, ret:%u\n", ret);
        return (int)ret;
    }

    hns5_info(dev, "CRC offload %s\n", enable ? "enabled" : "disabled");
    return 0;
}
