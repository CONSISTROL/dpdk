..  SPDX-License-Identifier: BSD-3-Clause
    Copyright(c) 2024 Huawei Technologies Co., Ltd

HNS5 Crypto Poll Mode Driver
============================

The HNS5 crypto poll mode driver provides symmetric crypto offload for the SEC
(Symmetric Encrypt Engine) accelerator integrated in the Hi1231 (Kunpeng
920L18) SoC.

Unlike a pure QM lookaside path, the SEC engine is driven in POE (Packet Order
Engine) mode through the Hi1231 board BSP library ``libhidrv_api``: the SEC
PF/VF are set up together with POE submit/completion groups, and DMA buffers
are allocated from a file-backed 32 MiB HugeTLB pool managed by the BMU. A
fresh 128-byte SEC descriptor is built per request from the DPDK sym xform
chain; the engine performs the key schedule on the origin key, so no
persistent HW session is needed (the DPDK session is pure software state).

The hardware access path is adapted from the Hi1231 OpenSSL engine reference.
It is **board-specific**: ``libhidrv_api``, the SEC/BMU/POE ``/dev`` devices
and a 32 MiB hugetlbfs mount are only present on the S920L18 board image.
On a host without the BSP the driver is skipped at build time.

Features
--------

HNS5 crypto PMD has support for:

Cipher algorithms:

* ``RTE_CRYPTO_CIPHER_SM4_CBC``
* ``RTE_CRYPTO_CIPHER_AES_CBC``
* ``RTE_CRYPTO_CIPHER_AES_ECB``

Hash algorithms:

* ``RTE_CRYPTO_AUTH_SM3``
* ``RTE_CRYPTO_AUTH_SHA256``
* ``RTE_CRYPTO_AUTH_SHA256_HMAC``

Chained cipher-then-auth operations are supported.

Device and queue pair model
---------------------------

The PMD registers as a virtual device (``crypto_hns5``) since the SEC engine
is a SoC platform device rather than a discoverable PCI function. One SEC VF
is set up per device, with a single in-order queue pair. Each queue pair keeps
a fixed-depth in-flight ring: ``enqueue`` builds descriptors and submits them
through ``hidrv_send_sec``, and ``dequeue`` polls the POE completion queue
(in-order, so the ring head is reaped next) and copies results back into the
DPDK mbufs.

DMA memory and recovery
-----------------------

A 32 MiB HugeTLB pool is opened under a canonical owner lock. Its physical
address is read from ``/proc/self/pagemap`` (the SMMU is configured in
passthrough mode). Once hardware has been touched, the pool is retained unless
every teardown and reset step completes; a later process therefore never
reuses live DMA memory. After an uncertain teardown the engine is poisoned
and must be reset through the BSP flow before reuse.

Build and run
-------------

#. Ensure the board BSP provides ``libhidrv_api`` (e.g.
   ``/opt/extralib/libhidrv_api.so``) and the matching SDK headers.

#. Place the Hi1231 vendor headers so the driver meson file can find them. By
   default the headers are expected at ``<workdir>/hi1231/vendor/hi1231/include``
   alongside the DPDK checkout; adjust the path in
   ``drivers/crypto/hns5/meson.build`` otherwise.

#. Build DPDK. The driver builds automatically when ``libhidrv_api`` is found
   and is skipped otherwise:

   .. code-block:: console

      cd dpdk
      meson setup build
      ninja -C build

#. Prepare the 32 MiB hugetlbfs mount used by the SEC pool:

   .. code-block:: console

      mkdir -p /mnt/huge32m
      mount -t hugetlbfs none /mnt/huge32m -o pagesize=32M,mode=0700

#. Run a crypto application:

   .. code-block:: console

      sudo ./build/app/dpdk-test --vdev=crypto_hns5 --log-level=6
      RTE>>cryptodev_sm4_autotest

Limitations
-----------

* Board verification on S920L18 is pending; source-level compilation against
  DPDK 22.11 plus the vendor headers is the host-side boundary.
* Single queue pair per device.
* Messages up to a single SEC segment (``SEC_SEGMENT_BYTES``); longer
  messages are not yet chunked across multiple segments.
