#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_rwlock.h>
#include <rte_string_fns.h>
#include <rte_cycles.h>
#include <rte_kvargs.h>
#include <rte_dev.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_common.h>
#include "hns5_mempool.h"
#include "hidrv_subctrl_api.h"
#include "hidrv_udev.h"
#include "hidrv_map.h"
#include "hidrv_eth_api.h"
#include "hidrv_bmu_vf_api.h"
#include "hidrv_bmu_pf_api.h"
#include "hidrv_ppe_pf_api.h"
#include "ndrv_bmu_pf_api.h"
#include "ndrv_bmu_vf_api.h"
#include "ndrv_ppe_debug_api.h"

#define E_OK (0)
#define E_BMU_GET_VF_HANDLE (-1)
#define E_BMU_GET_PF_HANDLE (-2)
#define E_BMU_PF_INIT_FAIL (-3)
#define E_BMU_VF_INIT_FAIL (-4)
#define E_BMU_GET_POOL_ID_FAIL (-5)
#define E_BMU_GET_HANDLE_FAIL (-6)
#define E_BMU_VF_FREE_FAIL (-7)
#define E_BMU_GET_BUF_CNT_FAIL (-8)
#define E_BMU_PHY_POOL_INIT_FAIL (-9)
#define E_BMU_LOGIC_POOL_INIT_FAIL (-10)
#define E_BMU_INJECT_ADDR_ERR (-11)
#define E_BMU_ALIGN_SIZE_ERR (-12)
#define E_BMU_GET_DEV_ID_ERR (-13)
#define E_BMU_UDEV_MAP_ERR (-14)
#define E_BMU_ALLOC_ERR (-15)
#define E_BMU_GET_BLOCK_SIZE_ERR (-15)

#define HNS5_DISABLE 0
#define HNS5_ENABLE 1
#define MP_TOTAL_ELT_SIZE(mp) ((mp)->header_size + (mp)->elt_size + (mp)->trailer_size)
#define HNS5_BMU_MODULE_INIT_USER 1
#define POOL_MAX 16
#define BMU_QOS_NUM (9)
#define BMU_MAX_BLOCK_NUM 512
#define HNS5_BMU_ALIGN_SIZE 256

struct hns5_bmu_addr_trans_info *g_hns5_bmu_addr_trans_tbl = NULL;

struct device_id_info {
    uint32_t die_id;
    uint32_t pf_id;
    uint32_t vf_id;
};

struct hns5_mp_bmu_dev {
    struct device_id_info id;
    uint32_t pf_devid;
    uint32_t vf_devid;
    handle pfhandle;
    handle vfhandle;
    rte_spinlock_t vf_lock;
    rte_spinlock_t pf_lock;
    uint32_t bmu_block_size;
    uint32_t bmu_module_inited;
};

struct hns5_mp_bmu_dev g_mp_bmu_dev = { 0 };

struct hns5_bmu_qos_wl {
    uint32_t qos_high_wl[BMU_QOS_NUM];
    uint32_t qos_low_wl[BMU_QOS_NUM];
};
struct hns5_bmu_pool_param {
    struct hidrv_bmu_hard_mng_lp_cfg vlp_cfg;
    struct hns5_bmu_qos_wl qos_wl;
    uint32_t lpool_inited;
};

#ifdef HNS5_BMU_POOL_HEAD_DFX_DEBUG
/* BMU debug header */
struct _trace_info {
    uint32_t udf : 6;
    uint32_t flg : 1;
    uint32_t rev : 1;
    uint32_t axi_id : 15;
    uint32_t vmid : 4;
    uint32_t pool : 5;
};

typedef struct {
    /* managed by cpu  */
    uint32_t magic : 27; /* magic */
    uint32_t vlpid : 5;  /* virtual pool id */
    struct _trace_info free;
    struct _trace_info alloc;
    uint32_t timestamp; /* global timestamp, updated on alloc only, in seconds */

    /* managed by software */
    uint32_t udata[12]; /* software-defined fields */
} bmu_buff_hdr_t;

#define BMU_HDR_MAGIC_VALUE_INIT 0x48554d41
#define BMU_HDR_MAGIC_VALUE 0x48554d42
#endif

static int hns5_udev_map_res(const char *dev_base_name, struct device_id_info *id,
    enum hidrv_urm_id module_id, enum hidrv_urm_dev_type dev_type, const char *res_name, struct hidrv_urm_res *res)
{
    struct hidrv_urm_dev_info info;
    int ret;

    info.dev_base_name = dev_base_name;
    info.dev_type = dev_type;
    info.module_id = module_id;
    info.die_id = id->die_id;

    if (dev_type == 0) {
        info.pf.pf_id = id->pf_id;
    } else {
        info.vf.vf_id = id->vf_id;
        info.vf.pf_id = id->pf_id;
    }
    ret = hidrv_udev_map_res(&info, res_name, res);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "hidrv_udev_map_res bmu fail\n");
        return E_BMU_UDEV_MAP_ERR;
    }

    return 0;
}

static handle hns5_bmu_pf_open(struct hns5_mp_bmu_dev *dev)
{
    int ret = E_OK;
    struct ndrv_bmu_pf_open_cfg bmu_open_cfg = { 0 };
    handle bmu_pfhandle;
    struct hidrv_urm_res bmu_reg = { 0 };

    ret = hns5_udev_map_res("bmu_pf", &(dev->id), URM_BMU, HIDRV_URM_PF_DEV, "reg", &bmu_reg);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "%s: udev map res fail, ret\n", __FUNCTION__);
        return NULL;
    }

    rte_spinlock_lock(&dev->pf_lock);
    if (likely(dev->pfhandle != NULL)) {
        bmu_pfhandle = dev->pfhandle;
        ret = E_OK;
        goto unlock;
    }

    bmu_open_cfg.reg.io_base = bmu_reg.vaddr;
    bmu_open_cfg.reg.io_size = (uint32_t)bmu_reg.size;
    bmu_pfhandle = ndrv_bmu_pf_open(&bmu_open_cfg);
    if (bmu_pfhandle == NULL) {
        RTE_LOG(ERR, PMD, "%s: ndrv_bmu_pf_open fail\n", __FUNCTION__);
        ret = E_BMU_GET_PF_HANDLE;
        goto unlock;
    }

unlock:
    rte_spinlock_unlock(&dev->pf_lock);
    return (ret == E_OK) ? bmu_pfhandle : NULL;
}

static handle hns5_bmu_vf_open(struct hns5_mp_bmu_dev *dev)
{
    int ret = E_OK;
    struct ndrv_bmu_vf_open_cfg bmu_open_cfg = { 0 };
    handle bmu_vfhandle;
    struct hidrv_urm_res bmu_reg = { 0 };

    ret = hns5_udev_map_res("bmu_vf", &(dev->id), URM_BMU, HIDRV_URM_VF_DEV, "reg", &bmu_reg);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "%s: hidrv urm get_vf_addr fail, ret\n", __FUNCTION__);
        return NULL;
    }

    rte_spinlock_lock(&dev->vf_lock);
    if (likely(dev->vfhandle != NULL)) {
        bmu_vfhandle = dev->vfhandle;
        ret = E_OK;
        goto unlock;
    }

    bmu_open_cfg.reg.io_base = bmu_reg.vaddr;
    bmu_open_cfg.reg.io_size = (uint32_t)bmu_reg.size;
    bmu_vfhandle = ndrv_bmu_vf_open(&bmu_open_cfg);
    if (bmu_vfhandle == NULL) {
        RTE_LOG(ERR, PMD, "%s: ndrv_bmu_vf_open fail\n", __FUNCTION__);
        ret = E_BMU_GET_VF_HANDLE;
        goto unlock;
    }

unlock:
    rte_spinlock_unlock(&dev->vf_lock);
    return (ret == E_OK) ? bmu_vfhandle : NULL;
}

static __rte_always_inline handle hns5_mp_get_vf_handle(struct hns5_mp_bmu_dev *dev)
{
    handle vfhandle;

    if (unlikely(dev->vfhandle == NULL)) {
        vfhandle = hns5_bmu_vf_open(dev);
        if (vfhandle == NULL) {
            RTE_LOG(ERR, PMD, "[hns5_bmu] hns5_bmu_vf_open err\n");
            return NULL;
        }
        dev->vfhandle = vfhandle;
        return vfhandle;
    }
    return dev->vfhandle;
}

static __rte_always_inline handle hns5_mp_get_pf_handle(struct hns5_mp_bmu_dev *dev)
{
    handle pfhandle;

    if (unlikely(dev->pfhandle == NULL)) {
        pfhandle = hns5_bmu_pf_open(dev);
        if (pfhandle == NULL) {
            RTE_LOG(ERR, PMD, "[hns5_bmu] hns5_bmu_pf_open err\n");
            return NULL;
        }
        dev->pfhandle = pfhandle;
        RTE_LOG(INFO, PMD, "[hns5_bmu] hns5_mp_get_pf_handle: FIRST get pfhandle=%p, die_id=%u, pf_id=%u\n",
            pfhandle, dev->id.die_id, dev->id.pf_id);
        return pfhandle;
    }
    RTE_LOG(INFO, PMD, "[hns5_bmu] hns5_mp_get_pf_handle: REUSE cached pfhandle=%p\n", dev->pfhandle);
    return dev->pfhandle;
}

static int hns5_get_mempool_block_size(uint32_t *block_size)
{
    enum ndrv_bmu_pf_blk_size size;
    handle pfhandle;
    uint32_t ret;

    if (likely(g_mp_bmu_dev.bmu_block_size != 0)) {
        *block_size = g_mp_bmu_dev.bmu_block_size;
        return 0;
    }
    pfhandle = hns5_mp_get_pf_handle(&g_mp_bmu_dev);
    if (pfhandle == NULL) {
        return E_BMU_GET_HANDLE_FAIL;
    }
    ret = ndrv_bmu_pf_get_blk_size(pfhandle, &size);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] ndrv bmu pf get blk size err ret %u\n", ret);
        return ret;
    }
    switch (size) {
        case NDRV_BMU_BLKSIZE_4M:
            *block_size = 0x400000;
            break;
        case NDRV_BMU_BLKSIZE_8M:
            *block_size = 0x800000;
            break;
        case NDRV_BMU_BLKSIZE_2M:
            *block_size = 0x200000;
            break;
        default:
            RTE_LOG(ERR, PMD, "[hns5_bmu] blk size err is not support\n");
            return -EINVAL;
            break;
    }
    g_mp_bmu_dev.bmu_block_size = *block_size;

    return 0;
}

static int32_t hns5_bmu_add_addr_trans(uint64_t virt, uint64_t phys, uint32_t size, uint32_t pool_id)
{
    uint32_t num;
    uint32_t blk_size;
    int ret;

    ret = hns5_get_mempool_block_size(&blk_size);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] get_mempool_block_size fail %d\n", ret);
        return ret;
    }

    if (unlikely(g_hns5_bmu_addr_trans_tbl == NULL)) {
        const struct rte_memzone *mz;
        mz = rte_memzone_lookup(HNS5_BMU_ADDR_TRANS_NAME);
        if(mz == NULL) {
            mz = rte_memzone_reserve(HNS5_BMU_ADDR_TRANS_NAME, sizeof(struct hns5_bmu_addr_trans_info), rte_socket_id(), 0);
            if(mz == NULL) {
                RTE_LOG(ERR, PMD, "[hns5_bmu] reserve mem failed for HNS5_BMU_ADD_TRANS_TBL\n");
                return -ENOMEM;
            }
            memset(mz->addr, 0, sizeof(struct hns5_bmu_addr_trans_info));
        }
        g_hns5_bmu_addr_trans_tbl = (struct hns5_bmu_addr_trans_info *)(mz->addr);
    }

    if ((g_hns5_bmu_addr_trans_tbl->current_blk_num + (size / blk_size)) >= BMU_MAX_BLOCK_NUM) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] bmu rearch the max block num(%d)\n", g_hns5_bmu_addr_trans_tbl->current_blk_num);
        return -EINVAL;
    }

    rte_rwlock_write_lock(&(g_hns5_bmu_addr_trans_tbl->lock));
    num = g_hns5_bmu_addr_trans_tbl->num;
    if (num >= HNS5_BMU_ADDR_TRANS_MAX) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] HNS5_BMU_ADD_TRANS_TBL is to more, %d\n", num);
        rte_rwlock_write_unlock(&(g_hns5_bmu_addr_trans_tbl->lock));
        return -EINVAL;
    }

    g_hns5_bmu_addr_trans_tbl->tbl[num].phys = phys;
    g_hns5_bmu_addr_trans_tbl->tbl[num].virt = virt;
    g_hns5_bmu_addr_trans_tbl->tbl[num].size = size;
    g_hns5_bmu_addr_trans_tbl->tbl[num].pool_id = pool_id;
    g_hns5_bmu_addr_trans_tbl->num++;

    g_hns5_bmu_addr_trans_tbl->current_blk_num += size / blk_size;

    rte_rwlock_write_unlock(&(g_hns5_bmu_addr_trans_tbl->lock));

    return 0;
}

#if (HNS5_BMU_MODULE_INIT_USER != 0)
static int hns5_bmu_pf_init(struct hns5_mp_bmu_dev *dev)
{
    int ret;
    struct hidrv_bmu_pf_ext_para ext_para = {0};
    struct hidrv_bmu_pf_cfg cfg = {
        .endian = HIDRV_BMU_PF_ENDIAN_LITTLE,
        .mb_buf_rls_overflow_addr = 0,
    };
    uint32_t vf_id = dev->id.vf_id;

    cfg.blk_size = HIDRV_BMU_BLKSIZE_4M;
    cfg.vf_num = 1;
    cfg.pool[0].vf_id = vf_id;
    cfg.pool[0].lp_num = POOL_MAX;
    cfg.pool[0].pp_num = POOL_MAX;
    ret = hidrv_bmu_pf_setup(dev->pf_devid, &cfg, &ext_para);
    if (ret) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] hidrv_bmu_pf_setup fail %d\n", ret);
        return ret;
    }

    return 0;
}

static int hns5_bmu_vf_endian_transfer(enum hidrv_ppe_endian_type endian_mode)
{
    switch (endian_mode) {
        case HIDRV_PPE_32LE:
            return HIDRV_BMU_VF_ENDIAN_LITTLE;
        case HIDRV_PPE_32BE:
            return HIDRV_BMU_VF_ENDIAN_BIG_32BIT;
        case HIDRV_PPE_64BE:
            return HIDRV_BMU_VF_ENDIAN_BIG_64BIT;
        default:
            return HIDRV_BMU_VF_ENDIAN_INVALID;
    }
}

static int hns5_bmu_vf_init(uint32_t vf_devid)
{
    struct hidrv_ppe_endian_cfg endian_cfg = { 0 };
    struct hidrv_bmu_vf_ext_para ext_para = {0};
    struct hidrv_bmu_vf_cfg cfg = { 0 };
    int ret;

    ret = hidrv_ppe_pf_get_tnl_endian_cfg(hidrv_ppe_get_pf_devid(0, 0), 0, &endian_cfg);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "hidrv_ppe_pf_get_tnl_endian_cfg failed. ret:%d\n", ret);
        return ret;
    }
    cfg.endian = hns5_bmu_vf_endian_transfer(endian_cfg.rx_bd);
    ret = hidrv_bmu_vf_setup(vf_devid, &cfg, &ext_para);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "hidrv_bmu_vf_setup failed. ret:%d\n", ret);
        return ret;
    }

    return 0;
}
#endif

static int get_mempool_id(struct rte_mempool *mp, uint32_t *pool_id)
{
    uint32_t id;
    int ret;

    if (mp == NULL) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] Invalid mem pool \n");
        return -EINVAL;
    }

    ret = sscanf(mp->name, "bmu_pool%u", &id);
    if (ret != 1) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] get_mempool_id sscanf fail\n");
        return -EINVAL;
    }
    *pool_id = id;

    return 0;
}

#define SIZE_1K 1024
#define BMU_BUFF_SIZE_STEP 256

static uint32_t hns5_mp_get_buffer_pad_size(const struct rte_mempool *mp)
{
    uint32_t total_elt_sz, hw_pad_size;
    size_t value;

    total_elt_sz = MP_TOTAL_ELT_SIZE(mp);
    // For BMU perf, buffer size must be a multiple of 256B; nKB+256B or nKB-256B performs better, and 128B-aligned data_room start performs better;
    value = (BUM_HARD_HEAD_LEN + total_elt_sz) % SIZE_1K;
    if (value <= BMU_BUFF_SIZE_STEP) {
        hw_pad_size = (BMU_BUFF_SIZE_STEP - value);
    } else if (value <= (SIZE_1K - BMU_BUFF_SIZE_STEP)) {
        hw_pad_size = SIZE_1K - BMU_BUFF_SIZE_STEP - value;
    } else {
        hw_pad_size = SIZE_1K + BMU_BUFF_SIZE_STEP - value;
    }

    return hw_pad_size;
}

static uint32_t hns5_mp_get_buffer_total_size(const struct rte_mempool *mp)
{
    return BUM_HARD_HEAD_LEN + MP_TOTAL_ELT_SIZE(mp) + hns5_mp_get_buffer_pad_size(mp);
}

static int hns5_align_mp_size(struct rte_mempool *mp)
{
    uint32_t block_size, bmu_buf_size, elt_num_per_block;
    int ret;

    bmu_buf_size = hns5_mp_get_buffer_total_size(mp);
    // Align pool buffer count to whole blocks
    ret = hns5_get_mempool_block_size(&block_size);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] get_mempool_block_size fail %d\n", ret);
        return ret;
    }
    elt_num_per_block = block_size / bmu_buf_size;
    if (mp->size % elt_num_per_block != 0) { // mp->size not block-aligned, print warning, no return
        RTE_LOG(INFO, PMD, "[hns5_bmu] mp->size %u is not an integer multiple of the block's buffer size %u.\n", mp->size, elt_num_per_block);
    }
    mp->size = (mp->size + elt_num_per_block - 1) / elt_num_per_block * elt_num_per_block;

    return ret;
}

static int hns5_bmu_create_pool(struct rte_mempool *mp)
{
    int ret;
    uint32_t pool_id, pf_devid, vf_devid;
    struct hns5_mp_bmu_dev *dev = &g_mp_bmu_dev;

    if (dev->bmu_module_inited == 0) {
        pf_devid = hidrv_bmu_get_pf_devid(dev->id.die_id, dev->id.pf_id);
        if (pf_devid < 0) {
            RTE_LOG(ERR, PMD, "[hns5_bmu] hidrv_bmu_get_pf_devid fail %d\n", pf_devid);
            return E_BMU_GET_DEV_ID_ERR;
        }
        dev->pf_devid = (uint32_t)pf_devid;

#if (HNS5_BMU_MODULE_INIT_USER != 0)
        ret = hns5_bmu_pf_init(dev);
        if (ret != 0) {
            RTE_LOG(ERR, PMD, "[hns5_bmu] hns5_bmu_pf_init fail %d\n", ret);
            return E_BMU_PF_INIT_FAIL;
        }
#endif
        vf_devid = hidrv_bmu_get_vf_devid(dev->id.die_id, dev->id.pf_id, dev->id.vf_id);
        if (vf_devid < 0) {
            RTE_LOG(ERR, PMD, "[hns5_bmu] hidrv_bmu_get_vf_devid fail %d\n", vf_devid);
            return E_BMU_GET_DEV_ID_ERR;
        }
        dev->vf_devid = (uint32_t)vf_devid;

#if (HNS5_BMU_MODULE_INIT_USER != 0)
        // Reset BMU VF to prevent last run's memory leak affecting this run
        {
            handle vfhandle;
            vfhandle = hns5_mp_get_vf_handle(dev);
            if (vfhandle == NULL) {
                RTE_LOG(ERR, PMD, "[hns5_bmu] hns5_mp_get_vf_handle fail\n");
                return E_BMU_GET_HANDLE_FAIL;
            }
            (void)ndrv_bmu_vf_reset(vfhandle);
        }
        ret = hns5_bmu_vf_init(vf_devid);
        if (ret != 0) {
            RTE_LOG(ERR, PMD, "[hns5_bmu] hns5_bmu_vf_init fail %d\n", ret);
            return E_BMU_VF_INIT_FAIL;
        }
#endif

        dev->bmu_module_inited = 1;
    }

    ret = hns5_align_mp_size(mp);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] hns5_align_mp_size fail %d\n", ret);
        return E_BMU_ALIGN_SIZE_ERR;
    }

    ret = get_mempool_id(mp, &pool_id);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] get_mempool_id fail %d\n", ret);
        return E_BMU_GET_POOL_ID_FAIL;
    }
    mp->pool_id = pool_id;
    mp->pool_config = rte_zmalloc("pool_cfg", sizeof(struct hns5_bmu_pool_param), 0);
    if (NULL == mp->pool_config) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] pool%u config malloc fail\n", pool_id);
        return -ENOMEM;
    }
    memset(mp->pool_config, 0, sizeof(struct hns5_bmu_pool_param));
    RTE_LOG(INFO, PMD, "[hns5_bmu] hns5 bmu create pool success\n");

    return 0;
}

static void hns5_bmu_free_pool(struct rte_mempool* mp)
{
    if (mp->pool_config == NULL) {
        return;
    }
    rte_free(mp->pool_config);
    mp->pool_config = NULL;

    return;
}

#define HNS5_BMU_MAGIC_TIMESTAMP 0x5a5a5a5a
static int hns5_bmu_free_bulk(struct rte_mempool *mp __rte_unused, void * const *obj_table, unsigned int n)
{
    unsigned int i;
    int ret;
    struct rte_mempool_objhdr* hdr;
    uint32_t udf = hns5_get_lcore_id();
    handle vfhandle;

    vfhandle = hns5_mp_get_vf_handle(&g_mp_bmu_dev);
    if (vfhandle == NULL) {
        return E_BMU_GET_HANDLE_FAIL;
    }

    // RTE_LOG(INFO, PMD, "[hns5_bmu] free_bulk called, n=%u, mp=%s\n", n, mp->name);

    for (i = 0; i < n; i++) {
#ifdef HNS5_BMU_POOL_HEAD_DFX_DEBUG
        bmu_buff_hdr_t *bmu_hdr;
        bmu_hdr = (bmu_buff_hdr_t *)RTE_PTR_SUB(obj_table[i], MBUFF_FROM_BMU_OFFSET);
        if (bmu_hdr->udata[0] == BMU_HDR_MAGIC_VALUE_INIT) {
            bmu_hdr->udata[0] = BMU_HDR_MAGIC_VALUE;
            bmu_hdr->udata[3]++;
            continue;
        }
        if (bmu_hdr->udata[0] != BMU_HDR_MAGIC_VALUE) {
            RTE_LOG(ERR, PMD, "[hns5_bmu] bmu free failed, Invalid buf %p\n", obj_table[i]);
            continue;
        }
        bmu_hdr->udata[2]++;
        bmu_hdr->timestamp = HNS5_BMU_MAGIC_TIMESTAMP;
#endif
        hdr = (struct rte_mempool_objhdr *)RTE_PTR_SUB(obj_table[i], sizeof(struct rte_mempool_objhdr));
        ret = ndrv_bmu_vf_free(vfhandle, hdr->iova - MBUFF_FROM_PPE_OFFSET, udf);
        if (ret != 0) {
            RTE_LOG(ERR, PMD, "[hns5_bmu] ndrv_bmu_vf_free fail ret: %d\n", ret);
            return E_BMU_VF_FREE_FAIL;
        }
    }

    return 0;
}

#define BMU_MIN_VALID_ADDR 256
static int hns5_bmu_alloc_bulk(struct rte_mempool *mp, void **obj_table, unsigned int count)
{
    uint32_t i, j;
    struct rte_mempool_objhdr* hdr;
    uint64_t paddr = 0;
    uint32_t udf = hns5_get_lcore_id();
    void *obj = NULL;
    handle vfhandle;

    vfhandle = hns5_mp_get_vf_handle(&g_mp_bmu_dev);
    if (vfhandle == NULL) {
        return E_BMU_GET_HANDLE_FAIL;
    }

    for (i = 0; i < count; i++) {
        paddr = ndrv_bmu_vf_alloc(vfhandle, mp->pool_id, udf);
        if (paddr < BMU_MIN_VALID_ADDR) {
            RTE_LOG(ERR, PMD, "[hns5_bmu] bmu alloc fail, pool id:%ld\n", mp->pool_id);
            goto fail_do;
        }
        obj = (void *)hns5_bmu_phys2virt(paddr + MBUFF_FROM_PPE_OFFSET);
        if (obj == NULL) {
            RTE_LOG(ERR, PMD, "[hns5_bmu] bmu alloc phys2virt fail, pool id:%ld paddr:0x%lx\n", mp->pool_id, paddr);
            goto fail_do_cur;
        }
#ifdef HNS5_BMU_POOL_HEAD_DFX_DEBUG
        bmu_buff_hdr_t *bmu_hdr;
        bmu_hdr = (bmu_buff_hdr_t *)RTE_PTR_SUB(obj_table[i], MBUFF_FROM_BMU_OFFSET);
        if (bmu_hdr->udata[0] == BMU_HDR_MAGIC_VALUE_INIT) {
            bmu_hdr->udata[0] = BMU_HDR_MAGIC_VALUE;
        }
        if (bmu_hdr->udata[0] != BMU_HDR_MAGIC_VALUE) {
            RTE_LOG(ERR, PMD, "[hns5_bmu] bmu alloc a error buf, pool id:%ld, Invalid buf %p, bmu_hdr:%p, magic:0x%x\n",
                mp->pool_id, obj_table[i], (void *)bmu_hdr, bmu_hdr->udata[0]);
            goto fail_do_cur;
        }
#endif
        obj_table[i] = obj;
    }

    return 0;

fail_do_cur:
    ndrv_bmu_vf_free(vfhandle, paddr, udf);

fail_do:

    for (j = 0; j < i; j++) {
        hdr = (struct rte_mempool_objhdr *)RTE_PTR_SUB(obj_table[j], sizeof(struct rte_mempool_objhdr));
        ndrv_bmu_vf_free(vfhandle, hdr->iova - MBUFF_FROM_PPE_OFFSET, udf);
    }

    return E_BMU_ALLOC_ERR;
}

static uint32_t hns5_bmu_get_count(const struct rte_mempool *mp)
{
    int ret;
    uint32_t free_buffer_num;
    handle vfhandle;

    vfhandle = hns5_mp_get_vf_handle(&g_mp_bmu_dev);
    if (vfhandle == NULL) {
        return E_BMU_GET_HANDLE_FAIL;
    }

    ret = ndrv_bmu_vf_get_vlp_buf_cnt(vfhandle, mp->pool_id, &free_buffer_num);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] bmu_vf_get_vlp_buf_cnt fail, ret %d\n", ret);
        return E_BMU_GET_BUF_CNT_FAIL;
    }

    return free_buffer_num;
}

#ifdef HNS5_BMU_POOL_HEAD_DFX_DEBUG
static int bmu_buffer_add_soft_magic(const struct rte_mempool *mp, char *vaddr, size_t len)
{
    uint32_t block_size;
    uint32_t buffer_size = hns5_mp_get_buffer_total_size(mp);
    bmu_buff_hdr_t *bmu_hdr = NULL;
    uint32_t block_num, buf_num_per_block;
    int ret;

    ret = hns5_get_mempool_block_size(&block_size);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] get_mempool_block_size fail %d\n", ret);
        return ret;
    }
    block_num = len / block_size;
    buf_num_per_block = block_size / buffer_size;
    for (uint32_t i = 0; i < block_num; i++) {
        for (uint32_t j = 0; j < buf_num_per_block; j++) {
            bmu_hdr = (bmu_buff_hdr_t *)((char *)vaddr + i * block_size + j * buffer_size);
            bmu_hdr->udata[0] = BMU_HDR_MAGIC_VALUE_INIT;
            bmu_hdr->udata[1] = buffer_size - BMU_BD_LEN;
        }
    }

    return 0;
}
#endif

static int hns5_mp_phy_pool_inject_block(struct rte_mempool *mp __rte_unused, uint32_t pool_id, void *vaddr, rte_iova_t iova, size_t len)
{
    int ret;
    uint32_t blk_size;
    uint32_t vpp_id;
    uint32_t vfid = 0;
    uint32_t u_ret;
    handle vfhandle;

    ret = hns5_get_mempool_block_size(&blk_size);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] get_mempool_block_size fail %d\n", ret);
        return ret;
    }

    /* Record addr translation info (software table, not limited to 16 hw entries) */
    ret = hns5_bmu_add_addr_trans((uint64_t)vaddr, iova, len, pool_id);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] BMU add_block add trans tbl fail, ret:%d, vaddr:%p, iova:%p, size:%ld\n", ret, (void *)vaddr, (void *)iova, len);
        return ret;
    }

    vfhandle = hns5_mp_get_vf_handle(&g_mp_bmu_dev);
    if (vfhandle == NULL) {
        return E_BMU_GET_HANDLE_FAIL;
    }

    u_ret = ndrv_bmu_vf_get_pp_map_vpp(vfhandle, pool_id, &vfid, &vpp_id);
    if (u_ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] ndrv_bmu_vf_get_pp_map_vpp fail, ret:%u, pool_id:%u\n", u_ret, pool_id);
        return E_BMU_PHY_POOL_INIT_FAIL;
    }

    RTE_LOG(INFO, PMD, "[hns5_bmu] inject phy pool, pool_id=%u, vpp_id=%u, iova=0x%lx, len=%zu, blocks=%zu\n",
        pool_id, vpp_id, iova, len, len / blk_size);

    u_ret = ndrv_bmu_vf_add_vpp_blk(vfhandle, vpp_id, iova, len);
    if (u_ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] ndrv_bmu_vf_add_vpp_blk fail, ret = 0x%x\n", u_ret);
        return E_BMU_PHY_POOL_INIT_FAIL;
    }

#ifdef HNS5_BMU_POOL_HEAD_DFX_DEBUG
    ret = bmu_buffer_add_soft_magic(mp, vaddr, len);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] cfg_bmu_magic_check fail, ret%d\n", ret);
        return ret;
    }
#endif

    return 0;
}

static int hns5_logic_pool_init(struct rte_mempool *mp, uint32_t pool_id)
{
    uint32_t buffer_size, buf_num_per_block;
    struct hidrv_bmu_ppe_qos_thld qos_thld;
    uint32_t i;
    int ret;
    uint32_t qos_high_wl[BMU_QOS_NUM] = { 10 << 6, 8 << 6, 6 << 6, 4 << 6, 3 << 6, 2 << 6, 1 << 6, 0, 0 };
    uint32_t qos_low_wl[BMU_QOS_NUM] = { 10 << 6, 8 << 6, 6 << 6, 4 << 6, 3 << 6, 2 << 6, 1 << 6, 0, 0 };
    struct hidrv_bmu_hard_mng_lp_cfg vlp_cfg = { 0 };
    struct hns5_bmu_pool_param *pool_config = (struct hns5_bmu_pool_param *)(mp->pool_config);
    uint32_t blk_size;

    ret = hns5_get_mempool_block_size(&blk_size);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] get_mempool_block_size fail %d\n", ret);
        return ret;
    }

    buffer_size = hns5_mp_get_buffer_total_size(mp);
    if ((buffer_size % HNS5_BMU_ALIGN_SIZE) != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] BMU name %s buff size must be can sz 256 equal 0, now it %d(%d,%d,%d) %d %d\n",
            mp->name, buffer_size, mp->header_size, mp->elt_size, mp->trailer_size, BUM_HARD_HEAD_LEN, HNS5_BMU_ALIGN_SIZE);
        return -EINVAL;
    }
    buf_num_per_block = blk_size / buffer_size;

    RTE_LOG(INFO, PMD, "[hns5_bmu] pool_id=%u, buffer_size=%u, blk_size=%u, buf_num_per_block=%u, mp->size=%u, max_block_num=%u\n",
        pool_id, buffer_size, blk_size, buf_num_per_block, mp->size, mp->size / buf_num_per_block);

    vlp_cfg.buffer_size = buffer_size;
    vlp_cfg.buffer_max = 0xFFFFFF; // Enable BMU fixed-length mode
    vlp_cfg.buffer_min = 0xFFFFFF; // Enable BMU fixed-length mode
    vlp_cfg.max_block_num = mp->size / buf_num_per_block;
    vlp_cfg.min_block_num = 0;
    vlp_cfg.head_enable = HNS5_ENABLE;
    vlp_cfg.info.magic_num_check_enable = HNS5_ENABLE;
    vlp_cfg.info.magic_num_inject_enable = HNS5_ENABLE;
    vlp_cfg.info.lp_store_buf_tag_enable = HNS5_DISABLE;
    vlp_cfg.info.lp_load_buf_tag_enable = HNS5_DISABLE;

    ret = hidrv_bmu_init_hard_mng_lp(g_mp_bmu_dev.vf_devid, pool_id, &vlp_cfg); // Init logical pool buffer size
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] hidrv bmu init hard mng lp failed, ret=%d, lpid:%d", ret, pool_id);
        return ret;
    }

    for (i = 0; i < BMU_QOS_NUM; i++) {
        qos_thld.qos_high = qos_high_wl[i];
        qos_thld.qos_low = qos_low_wl[i];
        ret = hidrv_bmu_vf_set_ppe_qos_thld(g_mp_bmu_dev.vf_devid, pool_id, i, &qos_thld);
        if (ret != 0) {
            RTE_LOG(ERR, PMD, "[hns5_bmu] hidrv bmu vf set ppe qos thld failed, ret=%d", ret);
            return ret;
        }
    }

    memcpy(&pool_config->qos_wl.qos_high_wl, &qos_high_wl, sizeof(uint32_t) * BMU_QOS_NUM);
    memcpy(&pool_config->qos_wl.qos_low_wl, &qos_low_wl, sizeof(uint32_t) * BMU_QOS_NUM);
    memcpy(&pool_config->vlp_cfg, &vlp_cfg, sizeof(struct hidrv_bmu_hard_mng_lp_cfg)); // Use secure function

    return 0;
}

static int hns5_bmu_mbuf_populate(struct rte_mempool *mp, unsigned int max_objs,
        void *vaddr, rte_iova_t iova, size_t len,
        rte_mempool_populate_obj_cb_t *obj_cb, void *obj_cb_arg)
{
    size_t total_elt_sz;
    size_t off;
    size_t off_to_align;
    unsigned int i;
    void *mbuf;
    uint32_t pool_id = mp->pool_id;
    int ret;
    struct hns5_bmu_pool_param *pool_config = (struct hns5_bmu_pool_param *)(mp->pool_config);
    uint32_t align;

    ret = hns5_get_mempool_block_size(&align);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] get_mempool_block_size fail %d\n", ret);
        return ret;
    }

    // Round down memory size to multiple of 4M
    RTE_LOG(INFO, PMD, "[hns5_bmu] except mem iova:0x%lx, len:0x%zx\n", iova, len);
    len = RTE_ALIGN_FLOOR(len, align);
    RTE_LOG(INFO, PMD, "[hns5_bmu] inject mem iova:0x%lx, len:0x%zx\n", iova, len);

    if (iova % align != 0 || (uint64_t)vaddr % align != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] mbuf_populate addr err iova:0x%lx\n", iova);
        return E_BMU_INJECT_ADDR_ERR;
    }

    ret = hns5_mp_phy_pool_inject_block(mp, pool_id, vaddr, iova, len);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] hns5_mp_phy_pool_inject_block fail %d\n", ret);
        return E_BMU_PHY_POOL_INIT_FAIL;
    }

    total_elt_sz = hns5_mp_get_buffer_total_size(mp);

    for (off = 0, i = 0; off + total_elt_sz <= len && i < max_objs; i++) {
        off += (BUM_HARD_HEAD_LEN + mp->header_size);
        mbuf = (char *)vaddr + off;
        obj_cb(mp, obj_cb_arg, mbuf, (iova == RTE_BAD_IOVA) ? RTE_BAD_IOVA : (iova + off)); // mempool_add_elem
        off += mp->elt_size + mp->trailer_size + hns5_mp_get_buffer_pad_size(mp);
        // If a block is not a multiple of the BMU buffer size, skip the extra space
        if (align != 0) {
            off_to_align = (align - (off % align));
            if (off_to_align < total_elt_sz) {
                off += off_to_align;
            }
        }
    }

    if ((mp->populated_size >= mp->size) && (pool_config->lpool_inited == 0)) {
        ret = hns5_logic_pool_init(mp, pool_id);
        if (ret != 0) {
            RTE_LOG(ERR, PMD, "[hns5_bmu] hns5_logic_pool_init fail %d\n", ret);
            return E_BMU_LOGIC_POOL_INIT_FAIL;
        }
        pool_config->lpool_inited = 1;
    }

    return i;
}

static ssize_t hns5_bmu_calc_mem_size(const struct rte_mempool *mp,
                     uint32_t obj_num, __rte_unused uint32_t pg_shift,
                     size_t *min_chunk_size, size_t *align)
{
    size_t mem_size;
    int ret;
    uint32_t obj_per_block;
    uint32_t total_elt_sz, block_size;

    ret = hns5_get_mempool_block_size(&block_size);
    if (ret != 0) {
        RTE_LOG(ERR, PMD, "[hns5_bmu] get_mempool_block_size fail %d\n", ret);
        return E_BMU_GET_BLOCK_SIZE_ERR;
    }
    *align = block_size;
    *min_chunk_size = block_size;
    total_elt_sz = hns5_mp_get_buffer_total_size(mp);
    obj_per_block = block_size / total_elt_sz;
    mem_size = (obj_num + obj_per_block - 1) / obj_per_block * block_size; // RTE_ALIGN_CEIL(obj_num, obj_per_block) * block_size

    return mem_size;
}

struct rte_mempool_ops hns5_bmu_mpool_ops = {
    .name = "hns5_bmu",
    .alloc = hns5_bmu_create_pool,
    .calc_mem_size = hns5_bmu_calc_mem_size,
    .free = hns5_bmu_free_pool,
    .enqueue = hns5_bmu_free_bulk,
    .dequeue = hns5_bmu_alloc_bulk,
    .get_count = hns5_bmu_get_count,
    .populate = hns5_bmu_mbuf_populate,
};

RTE_MEMPOOL_REGISTER_OPS(hns5_bmu_mpool_ops);

void __rte_internal hns5_mp_clean_addr_trans_tbl(void)
{
    handle vfhandle;
    unsigned long long blk_addr;
    uint32_t i;
    int ret;

    if (g_hns5_bmu_addr_trans_tbl == NULL) {
        return;
    }

    vfhandle = hns5_mp_get_vf_handle(&g_mp_bmu_dev);
    if (vfhandle == NULL) {
        return;
    }

    rte_rwlock_write_lock(&g_hns5_bmu_addr_trans_tbl->lock);
    for (i = 0; i < g_hns5_bmu_addr_trans_tbl->num; i++) {
        blk_addr = g_hns5_bmu_addr_trans_tbl->tbl[i].phys;
        ret = ndrv_bmu_vf_del_vpp_blk(vfhandle, g_hns5_bmu_addr_trans_tbl->tbl[i].pool_id, &blk_addr);
        if (ret != 0) {
            RTE_LOG(ERR, PMD, "[hns5_bmu] del vpp blk fail, pool_id:%u, ret:%d\n",
                g_hns5_bmu_addr_trans_tbl->tbl[i].pool_id, ret);
        }
    }
    g_hns5_bmu_addr_trans_tbl->num = 0;
    g_hns5_bmu_addr_trans_tbl->current_blk_num = 0;
    rte_rwlock_write_unlock(&g_hns5_bmu_addr_trans_tbl->lock);
}
