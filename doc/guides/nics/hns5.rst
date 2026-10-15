..  SPDX-License-Identifier: BSD-3-Clause
    Copyright(c) 2024 Huawei Technologies Co., Ltd

HNS5 Poll Mode Driver
======================

The ``hns5`` poll mode driver provides a full network data path for the
Hi1231 (Kunpeng 920L18) SoC integrated NIC, plus a matching mempool driver
(``mempool/hns5``) backed by the hardware BMU. It is adapted from the
``hns5_20_11_En`` patch (DPDK 20.11) to 22.11, and drives the PPE/POE/MAG/TM
sub-blocks through the board BSP library ``libhidrv_api`` and the
S920L18_SDK vendor headers.

The driver binds as a virtual device (``net_hns5``): the SoC network engine
is a platform device rather than a discoverable PCI function. A 32 MiB
HugeTLB pool is managed by the BMU-based mempool; the Rx/Tx paths build and
harvest hardware descriptors (PPE/POE), and flow/rss/fdir/tm/stats control
planes program the firmware through the vendor API.

Vendor headers
--------------

The driver consumes the S920L18_SDK ``uDriver`` headers: ``hidrv_api``
(hidrv_ppe_pf/vf_api, hidrv_mag_api, hidrv_tm_pf_api, hidrv_poe_pf_api, ...),
``nativedrv_api`` (ndrv_poe/ppe/mag_pf_api, native, ppe_type_api,
node_code_api, ...), ``hidrv`` (hidrv_udev/map/subctrl_api) and
``module_type_api/bd_v400`` (ndrv_ppe_bd_type, ...). The meson build points
``include_directories`` at the SDK checkout path; adjust the path in
``drivers/net/hns5/meson.build`` if the SDK layout differs.

Build and run
-------------

#. Ensure the board BSP provides ``libhidrv_api`` and the S920L18_SDK headers
   are checked out alongside the DPDK tree.

#. Build DPDK. The driver builds automatically when ``libhidrv_api`` is found
   and is skipped otherwise:

   .. code-block:: console

      cd dpdk
      meson setup build
      ninja -C build drivers/librte_net_hns5.so.23.0

#. On a host without ``libhidrv_api`` the net/mempool drivers are skipped; a
   link-verification stub library may be placed on the library path to exercise
   compilation, but it must never be used to run.

#. On the board, prepare the 32 MiB hugetlbfs used by the BMU mempool and run:

   .. code-block:: console

      mkdir -p /mnt/huge32m
      mount -t hugetlbfs none /mnt/huge32m -o pagesize=32M,mode=0700
      ./build/app/dpdk-test --vdev=net_hns5 --log-level=6

Limitations
-----------

* Board run verification on S920L18 is pending; source-level compilation
  against DPDK 22.11 plus the SDK headers is the host-side boundary.
* The patch was authored for DPDK 20.11; the 22.11 port covers the ethdev /
  flow / mempool / mbuf API changes but has not been board-tested.
