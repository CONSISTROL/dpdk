# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2017 Cavium, Inc
# Copyright(c) 2024 Huawei Technologies Co., Ltd

if not is_linux or not dpdk_conf.get('RTE_ARCH_64')
    build = false
    reason = 'only supported on 64-bit Linux'
    subdir_done()
endif

deps += ['mbuf', 'bus_pci']

# S920L18_SDK vendor headers (shared with net/hns5)
hns5_sdk = '../../../../S920L18_SDK/src/kernel/uDriver/udrv'
includes += include_directories(hns5_sdk + '/include/hidrv_api')
includes += include_directories(hns5_sdk + '/include/nativedrv_api')
includes += include_directories(hns5_sdk + '/hidrv/include')
includes += include_directories(hns5_sdk + '/include/module_type_api/bd_v400')
includes += include_directories(hns5_sdk + '/nativedrv/modules/poe')
includes += include_directories(hns5_sdk + '/nativedrv/modules/bmu')
includes += include_directories(hns5_sdk + '/nativedrv/modules/sec')
includes += include_directories(hns5_sdk + '/nativedrv/modules/include')
includes += include_directories(hns5_sdk + '/include/module_type_api')
includes += include_directories(hns5_sdk + '/include/module_type_api/bd_v100')
includes += include_directories(hns5_sdk + '/include/module_type_api/bd_v200')

# Pre-built Hi1231 BSP library.
lib = cc.find_library('hidrv_api', required: false)
if not lib.found()
    build = false
    reason = 'missing dependency libhidrv_api (Hi1231 board BSP library)'
    subdir_done()
endif
ext_deps += lib

sources = files(
    'hns5_mempool.c',
)
