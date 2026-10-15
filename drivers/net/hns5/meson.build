# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2017 Cavium, Inc
# Copyright(c) 2024 Huawei Technologies Co., Ltd

if not is_linux or not dpdk_conf.get('RTE_ARCH_64')
    build = false
    reason = 'only supported on 64-bit Linux'
    subdir_done()
endif

deps += ['mbuf', 'bus_pci', 'mempool_hns5']

# S920L18_SDK vendor headers (hidrv_* / ndrv_* / native / ppe / poe / mag / tm)
hns5_sdk = '../../../../S920L18_SDK/src/kernel/uDriver/udrv'
includes += include_directories(hns5_sdk + '/include/hidrv_api')
includes += include_directories(hns5_sdk + '/include/nativedrv_api')
includes += include_directories(hns5_sdk + '/hidrv/include')
includes += include_directories(hns5_sdk + '/include/module_type_api/bd_v400')
includes += include_directories('../../mempool/hns5')

# Pre-built Hi1231 BSP library (hidrv_* SEC/PPE/POE/MAG/TM API). Stub may be
# placed on the library path for host link verification.
lib = cc.find_library('hidrv_api', required: false)
if not lib.found()
    build = false
    reason = 'missing dependency libhidrv_api (Hi1231 board BSP library)'
    subdir_done()
endif
ext_deps += lib

sources = files(
    'hns5_eth_rxtx.c',
    'hns5_eth.c',
    'hns5_poe.c',
    'hns5_ppe.c',
    'hns5_mag.c',
    'hns5_flow.c',
    'hns5_fdir.c',
    'hns5_stats.c',
    'hns5_tm.c',
    'hns5_rss.c',
    'hns5_lsw.c',
)
