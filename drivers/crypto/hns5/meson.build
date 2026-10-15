# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024 Huawei Technologies Co., Ltd

if not is_linux
    build = false
    reason = 'only supported on Linux'
    subdir_done()
endif

deps += ['bus_vdev']

# S920L18_SDK vendor headers (hidrv_sec/bmu/poe_vf, ndrv_poe_vf, native, ppe,
# mag, tm, ...). The SDK is expected alongside this DPDK checkout; adjust the
# path below if it differs.
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

# Pre-built Hi1231 BSP library implementing the hidrv_* SEC/QM/POE/BMU API.
# It is shipped by the S920L18 board image (e.g. /opt/extralib/libhidrv_api.so)
# and is NOT buildable from this checkout. When absent, the PMD is skipped.
lib = cc.find_library('hidrv_api', required: false)
if not lib.found()
    build = false
    reason = 'missing dependency libhidrv_api (Hi1231 board BSP library)'
    subdir_done()
endif

ext_deps += lib

sources = files(
        'hns5_crypto_log.c',
        'hns5_crypto_pmd.c',
        'hns5_crypto_capabilities.c',
        'hns5_crypto_runtime.c',
        'hns5_sec_platform.c',
        'hns5_sec_bd.c',
        'hns5_sec_complete.c',
        'hns5_crypto_session.c',
        'hns5_crypto_enqdeq.c',
)

includes += include_directories('.')
