#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_debug.h>
#include <rte_dev.h>
#include <rte_eal.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_malloc.h>
#include <rte_pci.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_kvargs.h>

#include "hidrv_udev.h"
#include "hidrv_map.h"
#include "hidrv_tm_pf_api.h"
#include "hidrv_tm_vf_api.h"
#include "ndrv_tm_pf_api.h"

#include "hns5_mempool.h"
#include "hns5_tm.h"
#include "hns5_log.h"

#define TM_SHAPING_IR_1G 1000000000
#define TM_SHAPING_IR_MAX_25G 25000000000ULL

#define TM_DEFAULT_Q_DEPTH 0x80
#define TM_PACKET_SIZE 32

static int g_default_port_num = 0;

#define TM_MAX_USER_QUEUE_NUM 2048
#define HNS5_TM_LD_LIFO_TOTAL_SIZE ((26 * 1024 - 8) * 2)
#define HNS5_TM_LD_LIFO_QUEUE_MIN_SIZE 8
#define HNS5_TM_LD_LIFO_QUEUE_MAX_SIZE 2000
#define HNS5_TM_LD_LIFO_SHARE_SIZE (HNS5_TM_LD_LIFO_TOTAL_SIZE - TM_MAX_USER_QUEUE_NUM * HNS5_TM_LD_LIFO_QUEUE_MIN_SIZE)
#define HNS5_TM_LD_LIFO_TEMPLATE_ID 0
#define HNS5_TM_LD_LIFO_SHARE_ID 0

static int hns5_tm_cfg_ld_fifo(int dev_id)
{
    uint32_t ret;
    struct hidrv_tm_ld_fifo_share_size size = {
        .ld_share0_size = 4096,
        .ld_share1_size = 512,
        .ld_share2_size = 512,
    };
    struct hidrv_tm_ld_fifo_tmplt_info info = {
        .min_size = HNS5_TM_LD_LIFO_QUEUE_MIN_SIZE,
        .share_size = HNS5_TM_LD_LIFO_QUEUE_MAX_SIZE,
    };

    ret = hidrv_tm_set_ld_fifo_share_size(dev_id, &size);
    if (ret != 0) {
        hns5_log_error("set_ld_fifo_share_size error ret %d\n", ret);
        return ret;
    }
    ret = hidrv_tm_set_ld_fifo_tmplt_info(dev_id, HNS5_TM_LD_LIFO_TEMPLATE_ID, &info);
    if (ret != 0) {
        hns5_log_error("set_ld_fifo_tmplt_info error ret %d\n", ret);
        return ret;
    }

    return 0;
}

static uint32_t hns5_tm_pf_init(uint32_t dev_id, uint32_t init_mode)
{
    (void)init_mode;
    uint32_t ret;
    struct hidrv_tm_pf_cfg config = { 0 };

    config.shap_pkt_len = 0x100;
    config.shap_bypass.bypass_en_value = 0;
    config.sel_mode = HIDRV_TM_TMPLT_SELECT_DIRECT;

    ret = hidrv_tm_pf_setup(dev_id, &config, NULL); // PF init
    if (ret != 0) {
        return ret;
    }
    ret = hns5_tm_cfg_ld_fifo(dev_id);
    if (ret != 0) {
        return ret;
    }

    return 0;
}

static int hns5_cfg_tm_phy_port(int dev_id, uint32_t init_mode)
{
    (void)init_mode;
    int ret;
    uint32_t port_id, port_cnt, i;
    struct hidrv_tm_phys_port_params param = {0};


    port_cnt = g_default_port_num;

    for (i = 0; i < port_cnt; i++) {
        port_id = i;
        ret = hidrv_tm_open_phys_port(dev_id, port_id, &param);
        if (ret != 0) {
            hns5_log_error("hidrv_tm_vf_open_port fail:%d, port:%d\n", ret, port_id);
            return ret;
        }
    }

    return 0;
}

//check
static int hns5_cfg_tm_trunk_port(int dev_id, uint32_t init_mode)
{
    (void)init_mode;
    int ret;
    uint32_t i;
    uint32_t trunk_cnt, trunk_id, bind_port_id;
    struct hidrv_tm_trunk_params trunk_default_cfg = {
        .shaping_type = HIDRV_TM_SHAPING_TYPE_BPS,
        .pshaping = {
            .ir = 10000000000,
            .bs = 0x400000, /* bs_s [4:0] bs_b [9:5] */
        },
        .cshaping = {
            .ir = 10000000000,
            .bs = 0x400000, /* bs_s [4:0] bs_b [9:5] */
        }
    };
    struct hidrv_tm_trunk_params trunk_cfg = { 0 };
    uint32_t mask_index, offset;
    // 1:1 mapping between trunk id and port id
    trunk_cnt = g_default_port_num;
    for (i = 0; i < trunk_cnt; i++) {

        trunk_id = i;
        bind_port_id = i;
        memcpy(&trunk_cfg, &trunk_default_cfg, sizeof(struct hidrv_tm_trunk_params));

        trunk_cfg.bind_param.default_port = bind_port_id;
        mask_index = bind_port_id / 32;
        offset = bind_port_id % 32;
        trunk_cfg.bind_param.phys_port_mask[mask_index] = (1 << offset);
        ret = hidrv_tm_open_trunk(dev_id, trunk_id, &trunk_cfg);
        if (ret) {
            hns5_log_error("hidrv_tm_open_trunk  error %d\n", ret);
            return ret;
        }
    }

    return 0;
}

static int hns5_cfg_tm_vi_port(int dev_id, uint32_t init_mode)
{
    (void)init_mode;
    int ret;
    uint32_t i;
    uint32_t vi_cnt, vi_id, next_node_id;
    struct hidrv_tm_vi_params vi_default_cfg = {
        .shaping_type = HIDRV_TM_SHAPING_TYPE_BPS,
        .sched_type = HIDRV_TM_SCH_TYPE_DWRR,
        .weight = {
            .weight_base = 128,
            .weight_offset = 0,
        },
        .pshaping = {
            .ir = 10000000000,
            .bs = 0x400000, /* bs_s [4:0] bs_b [9:5] */
        },
        .cshaping = {
            .ir = 10000000000,
            .bs = 0x400000, /* bs_s [4:0] bs_b [9:5] */
        }
    };
    struct hidrv_tm_vi_params vi_cfg = { 0 };

    vi_cnt = g_default_port_num;
    for (i = 0; i < vi_cnt; i++) {
        vi_id = i;
        next_node_id = i;
        memcpy(&vi_cfg, &vi_default_cfg, sizeof(struct hidrv_tm_vi_params));
        vi_cfg.next_level_node_id = next_node_id;
        ret = hidrv_tm_open_vi(dev_id, vi_id, &vi_cfg);
        if (ret) {
            hns5_log_error("hidrv_tm_vf_open_vi  error %d\n", ret);
            return ret;
        }
    }

    return 0;
}

//check
static int hns5_cfg_tm_gloc_port(int dev_id, uint32_t init_mode)
{
    (void)init_mode;
    int ret;
    uint32_t i;
    uint32_t gloc_cnt, gloc_id, next_node_id;
    struct hidrv_tm_gloc_params gloc_default_cfg = {
        .shaping_type = HIDRV_TM_SHAPING_TYPE_BPS,
        .sched_type = HIDRV_TM_SCH_TYPE_DWRR,
        .weight = {
            .weight_base = 128,
            .weight_offset = 0,
        },
        .pshaping = {
            .ir = 10000000000,
            .bs = 0x400000, /* bs_s [4:0] bs_b [9:5] */
        },
        .cshaping = {
            .ir = 10000000000,
            .bs = 0x400000, /* bs_s [4:0] bs_b [9:5] */
        }
    };
    struct hidrv_tm_gloc_params gloc_cfg = { 0 };

    gloc_cnt = g_default_port_num;
    for (i = 0; i < gloc_cnt; i++) {
        gloc_id = i;
        next_node_id = i;
        memcpy(&gloc_cfg, &gloc_default_cfg, sizeof(struct hidrv_tm_gloc_params));
        gloc_cfg.next_level_node_id = next_node_id;
        ret = hidrv_tm_open_gloc(dev_id, gloc_id, &gloc_cfg);
        if (ret) {
            hns5_log_error("hidrv_tm_vf_open_gloc  error %d\n", ret);
            return ret;
        }
    }

    return 0;
}

struct hns5_bind_queue_info {
    uint32_t init_mode;
    int loc_id;
    int trunk_id;
    int queue_temple_id;
};


static int hns5_tm_cfg_loc_bind_queue(int dev_id, struct hns5_bind_queue_info *info)
{
    struct hidrv_tm_queue_params queue_cfg = { 0 };
    int ret;
    uint32_t idx, tm_queue_id;
    struct hidrv_tm_queue_params queue_default_cfg = {
        .que_byte_depth = 0x4000, // PD size 512 bits, queue depth 32
        .queue_enable = 1,
        .shaping_type = HIDRV_TM_SHAPING_TYPE_BPS,
        .sched_type = HIDRV_TM_SCH_TYPE_DWRR,
        .cshaping = {
            .ir = 10000000000,
            .bs = 0x400000, /* bs_s [4:0] bs_b [9:5] */
        },
        .weight = {
            .weight_base = 128,
            .weight_offset = 0,
        },
        .drop_enable = 0,
        .drop_template_id = 1,
        .drop_type = HIDRV_TM_QUE_DROP_TYPE_TAIL,
        .yellow_drop_enable = 0,
        .yellow_drop_th = HIDRV_TM_DROP_THLD_12P5,
    };
    for (idx = 0; idx < QUEUE_NUM_PER_PORT; idx++) {
        memcpy(&queue_cfg, &queue_default_cfg, sizeof(struct hidrv_tm_queue_params));
        queue_cfg.next_level_node_id = info->loc_id;
        queue_cfg.trunk = info->trunk_id;
        tm_queue_id = info->loc_id * QUEUE_NUM_PER_PORT + idx; // 8:1 queue-to-port mapping
        queue_cfg.ld_template_id = HNS5_TM_LD_LIFO_TEMPLATE_ID;
        queue_cfg.ld_share_id = HNS5_TM_LD_LIFO_SHARE_ID;
        ret = hidrv_tm_open_queue(dev_id, tm_queue_id, &queue_cfg);
        if (ret != 0) {
            hns5_log_error("hidrv_tm_open_queue %d  error %d\n", tm_queue_id, ret);
            return ret;
        }
    }

    return 0;
}

static int hns5_cfg_tm_loc_port(int dev_id, uint32_t init_mode)
{
    int ret;
    uint32_t loc_cnt, loc_id, bind_gloc_id, i;
    struct hidrv_tm_loc_params loc_cfg = { 0 };
    struct hns5_bind_queue_info queue_info = { 0 };
    struct hidrv_tm_loc_params loc_default_cfg = {
        .shaping_type = HIDRV_TM_SHAPING_TYPE_BPS,
        .sched_type = HIDRV_TM_SCH_TYPE_DWRR,
        .pshaping = {
            .ir = 10000000000,
            .bs = 0x400000, /* bs_s [4:0] bs_b [9:5] */
        },
        .cshaping = {
            .ir = 10000000000,
            .bs = 0x400000, /* bs_s [4:0] bs_b [9:5] */
        },
        .weight = {
            .weight_base = 128,
            .weight_offset = 0,
        },
    };
    loc_cnt = g_default_port_num;

    for (i = 0; i < loc_cnt; i++) {
        loc_id = i * 2;
        bind_gloc_id = i;
        queue_info.trunk_id = bind_gloc_id;
        memcpy(&loc_cfg, &loc_default_cfg, sizeof(struct hidrv_tm_loc_params));

        loc_cfg.next_level_node_id = bind_gloc_id;
        ret = hidrv_tm_open_loc(dev_id, loc_id, &loc_cfg);
        if (ret) {
            hns5_log_error("hidrv_tm_vf_open_loc  error %d\n", ret);
            return ret;
        }
        queue_info.init_mode = init_mode;
        queue_info.loc_id = loc_id;
        ret = hns5_tm_cfg_loc_bind_queue(dev_id, &queue_info);
        if (ret) {
            hns5_log_error("hns5_tm_cfg_loc_bind_queue  error %d\n", ret);
            return ret;
        }
    }

    return 0;
}

#define TM_QUEUE_OVER_FLOW_DDR_BASE (0x20A0000000)
#define TM_QUEUE_OVER_FLOW_DDR_SIZE (0x3000000) // max 0xc0 entries, unit 8*1024*32
static int hns5_tm_vf_init(uint32_t dev_id, uint32_t init_mode)
{
    (void)init_mode;
    int ret;
    struct hidrv_tm_vf_cfg config = {0};

    config.overflow_ddr_base = TM_QUEUE_OVER_FLOW_DDR_BASE;
    config.overflow_ddr_size = TM_QUEUE_OVER_FLOW_DDR_SIZE;

    ret = hidrv_tm_vf_setup(dev_id, &config, NULL);
    if (ret != 0) {
        return ret;
    }

    return 0;
}

int hns5_tm_platform_dev_probe(void)
{
    int ret;
    uint32_t init_mode = 1;
    uint32_t pf_dev_id, vf_dev_id;
    static int inited = 0;

    if (inited != 0) {
        return 0;
    }
    inited = 1;
    g_default_port_num = TM_INIT_PORT_NUM;
    ret = hidrv_tm_get_pf_devid(TM_DEV_ID, TM_PF_ID);
    if (ret < 0) {
        return ret;
    }
    pf_dev_id = (uint32_t)ret;

    ret = hns5_tm_pf_init(pf_dev_id, init_mode);
    if (ret) {
        hns5_log_error("hns5_tm_pf_init fail:%d\n", ret);
        return ret;
    }

    ret = hidrv_tm_get_vf_devid(TM_DEV_ID, TM_PF_ID, TM_VF_ID);
    if (ret < 0) {
        hns5_log_error("hidrv_tm_get_vf_devid fail:%d\n", ret);
        return ret;
    }
    vf_dev_id = (uint32_t)ret;

    ret = hns5_tm_vf_init(vf_dev_id, init_mode);
    if (ret != 0) {
        hns5_log_error("hns5_tm_vf_init fail:%d\n", ret);
        return ret;
    }
    ret = hns5_cfg_tm_phy_port(vf_dev_id, init_mode);
    if (ret) {
        hns5_log_error("hns5_cfg_tm_phy_port fail:%d\n", ret);
        return ret;
    }
    ret = hns5_cfg_tm_trunk_port(vf_dev_id, init_mode);
    if (ret) {
        hns5_log_error("hns5_cfg_tm_trunk_port fail:%d\n", ret);
        return ret;
    }
    ret = hns5_cfg_tm_vi_port(vf_dev_id, init_mode);
    if (ret) {
        hns5_log_error("hns5_cfg_tm_vi_port fail:%d\n", ret);
        return ret;
    }
    ret = hns5_cfg_tm_gloc_port(vf_dev_id, init_mode);
    if (ret) {
        hns5_log_error("hns5_cfg_tm_gloc_port fail:%d\n", ret);
        return ret;
    }
    ret = hns5_cfg_tm_loc_port(vf_dev_id, init_mode);
    if (ret) {
        hns5_log_error("hns5_cfg_tm_loc_port fail:%d\n", ret);
        return ret;
    }
    return 0;
}
