/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Huawei Technologies Co., Ltd
 *
 * hns5 crypto PMD: lookaside symmetric crypto on the Hi1231 SEC engine.
 *
 * The hardware access path is adapted from the Hi1231 OpenSSL engine, which
 * drives the SEC engine in POE (Packet Order Engine) mode through the board
 * BSP library libhidrv_api: a single SEC PF/VF is set up, submit/completion
 * POE groups are opened, DMA buffers are allocated from a file-backed 32 MiB
 * HugeTLB pool managed by the BMU, and a fresh 128-byte SEC descriptor is
 * built per request. Unlike the reference engine (synchronous single
 * request), the DPDK PMD keeps an in-flight ring per queue pair so that
 * enqueue submits bursts and dequeue polls POE completions asynchronously.
 *
 * Board BSP and SEC device are NOT present on a development host; this PMD
 * links libhidrv_api (skipped when absent) and only runs on the S920L18
 * image. Source-level checks against DPDK 22.11 plus the vendor headers are
 * the host-side verification boundary.
 */
#ifndef _HNS5_CRYPTO_PMD_PRIVATE_H_
#define _HNS5_CRYPTO_PMD_PRIVATE_H_

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/types.h>

#include <rte_log.h>
#include <rte_cryptodev.h>
#include <cryptodev_pmd.h>
#include <bus_vdev_driver.h>

/* Hi1231 board SDK vendor headers (read-only ABI). */
#include "hidrv_sec.h"
#include "hidrv_sec_vf_api.h"
#include "hidrv_sec_pf_api.h"
#include "hidrv_poe_vf_api.h"
#include "hidrv_poe_pf_api.h"
#include "hidrv_poe_xtp.h"
#include "hidrv_bmu_vf_api.h"
#include "hidrv_mbuf_api.h"
#include "ndrv_poe_vf_api.h"
#include "ndrv_poe_pf_api.h"

#define HNS5_CRYPTO_DRIVER_NAME crypto_hns5
#define HNS5_CRYPTO_LOG_PREFIX "crypto/hns5"

extern int hns5_logtype;
#define HNS5_LOG(level, ...) \
	rte_log(RTE_LOG_ ## level, hns5_logtype, HNS5_CRYPTO_LOG_PREFIX ": " __VA_ARGS__)

/* BMU buffer layout, identical to the reference engine. The 64 KiB buffer is
 * split into fixed slots: mbuf at offset 192, data at 576, MAC at 65280 and
 * key at 65408. Offsets are kept consistent with the SEC descriptor builder.
 */
#define HNS5_SEC_MBUF_OFFSET       192u
#define HNS5_SEC_BUFFER_OFFSET     448u
#define HNS5_SEC_DATA_OFFSET       576u
#define HNS5_SEC_PREFIX_BYTES      128u
#define HNS5_SEC_MAC_OFFSET        65280u
#define HNS5_SEC_KEY_OFFSET        65408u
#define HNS5_BUFFER_BYTES         65536u
#define HNS5_POOL_BYTES           (32u * 1024u * 1024u)
#define HNS5_QUEUE_RESERVE_BYTES  (4u * 1024u * 1024u)
#define HNS5_BMU_BLOCKS           7u
#define HNS5_SEC_MAX_SEGMENTS     274u
#define HNS5_SEC_MESSAGE_MAX      0xfffe00u
#define HNS5_SEC_CIPHER_CHUNK     262144u
#define HNS5_SEC_SEGMENT_BYTES    61440u
#define HNS5_MAX_RECV_POLLS       2000000000u
#define HNS5_SEC_SOURCE_PORT      0x60u

#define HNS5_SEC_CIPHER_ALG_SM4   0x3u
#define HNS5_SEC_CIPHER_ALG_AES   0x2u
#define HNS5_SEC_CIPHER_MODE_ECB  0x0u
#define HNS5_SEC_CIPHER_MODE_CBC  0x1u
#define HNS5_SEC_CIPHER_MODE_OFB  0x3u
#define HNS5_SEC_CIPHER_MODE_CTR  0x4u
/* SEC auth algorithm identifiers (vendor ABI). */
#define HNS5_SEC_AUTH_SM3         0xBu
#define HNS5_SEC_AUTH_HMAC_SM3    0xCu
#define HNS5_SEC_AUTH_SHA_256     0x1u
#define HNS5_SEC_AUTH_HMAC_SHA256 0xFu

/* POE routing, matching the reference platform defaults. */
#define HNS5_SEC_ENGINE_VCPU            1u
#define HNS5_SEC_ENGINE_COMPLETION_GRP 12u

/* ------------------------------------------------------------------ */
/* Common runtime: HugeTLB pool + BMU ownership, per process.          */
/* ------------------------------------------------------------------ */

struct hns5_pool {
	void *va;
	uint64_t pa;
	int file_fd;
	int lock_fd;
	int dir_fd;
	char *path;
	bool touched;
	bool recovered;
};

struct hns5_runtime {
	struct hns5_pool pool;
	int bmu_vf;
	bool poisoned;
	pid_t owner_pid;
	pthread_mutex_t queue_lock;
};

/* Per-request BMU-backed DMA segment. */
struct hns5_sec_segment {
	unsigned char *raw;
	uint64_t pa;
	size_t used;
};

/* ------------------------------------------------------------------ */
/* SEC engine runtime: device handles and POE routing.                  */
/* ------------------------------------------------------------------ */

struct hns5_sec_runtime {
	struct hns5_runtime *common;
	int poe_pf;
	int poe_vf;
	int sec_pf;
	int sec_vf;
	unsigned int vcpu;
	unsigned int submit_group;
	unsigned int completion_group;
	unsigned int submit_group_local;
	unsigned int completion_group_local;
	unsigned int submit_vqi;
	unsigned int completion_vqi;
	unsigned int error_vqi;
	void *poe_pf_handle;
	void *poe_vf_handle;
	bool submit_open;
	bool complete_open;
	bool cpu_bound;
	bool sec_pf_started;
	bool sec_vf_started;
	bool in_use;		/* device has at least one in-flight op */
};

/* ------------------------------------------------------------------ */
/* DPDK session: software state, no persistent HW session (per-WQE key). */
/* ------------------------------------------------------------------ */

struct hns5_session {
	uint8_t cipher_alg;	/* SEC_CIPHER_ALG_* */
	uint8_t cipher_mode;	/* SEC_CIPHER_MODE_*  */
	uint8_t cipher_keylen;	/* bytes */
	uint8_t cipher_dir;	/* RTE_CRYPTO_CIPHER_OP_* */

	uint8_t auth_alg;	/* SEC_AUTH_* */
	uint8_t mac_len;	/* bytes */
	uint16_t akey_len;	/* bytes */
	uint8_t auth_op;	/* RTE_CRYPTO_AUTH_OP_* */
	uint16_t digest_len;	/* bytes, from xform auth */
	uint16_t iv_offset;	/* IV offset within op private data */
	uint16_t iv_length;	/* IV length, bytes */
	bool is_aead;		/* GCM: cipher==auth combined */

	uint8_t op_type;	/* 0 cipher, 1 auth, 2 chain cipher-then-auth */
	uint8_t key[HNS5_BUFFER_BYTES - HNS5_SEC_KEY_OFFSET];
};

/* ------------------------------------------------------------------ */
/* Queue pair: in-flight ring mapping submitted ops to POE completions. */
/* ------------------------------------------------------------------ */

struct hns5_inflight_op {
	struct rte_crypto_op *op;
	struct hns5_sec_segment seg;
	bool used;
};

struct hns5_qp {
	struct hns5_sec_runtime *sec;
	struct hns5_inflight_op *ring;
	uint16_t depth;		/* ring capacity */
	uint16_t prod_head;	/* next submit slot */
	uint16_t cons_tail;	/* next reap slot (FIFO, POE order=1) */
	uint16_t inflight;
	rte_spinlock_t lock;
	struct rte_cryptodev_stats stats;
	char name[RTE_CRYPTODEV_NAME_MAX_LEN];
};

struct hns5_dev_private {
	struct hns5_sec_runtime sec;
	struct hns5_qp **qps;
	uint16_t max_qps;
	uint16_t nb_used_qps;
	bool common_runtime_acquired;
};

/* hns5_crypto_session.c */
int hns5_session_configure(struct rte_crypto_sym_xform *xform, void *priv);
void hns5_session_clear(void *priv);

/* hns5_crypto_enqdeq.c */
uint16_t hns5_enqueue_burst(struct hns5_qp *qp, struct rte_crypto_op **ops,
			    uint16_t nb_ops);
uint16_t hns5_dequeue_burst(struct hns5_qp *qp, struct rte_crypto_op **ops,
			    uint16_t nb_ops);

/* hns5_crypto_runtime.c (common HugeTLB/BMU layer) */
int hns5_runtime_acquire(struct hns5_runtime **out, const char *path);
int hns5_runtime_release(struct hns5_runtime *r);
void hns5_runtime_poison(struct hns5_runtime *r, const char *reason);
int hns5_runtime_lock(struct hns5_runtime *r, int *cancel_state);
void hns5_runtime_unlock(struct hns5_runtime *r, int cancel_state);
void *hns5_bmu_alloc(struct hns5_runtime *r, uint64_t *pa);
int hns5_bmu_free(struct hns5_runtime *r, void *buffer);
int hns5_pool_mark_hardware(struct hns5_pool *pool);
int hns5_bmu_reset(void);

/* hns5_sec_platform.c (SEC PF/VF + POE setup, mirrors sec_platform.c) */
int hns5_sec_platform_inspect(struct hns5_sec_runtime *r);
int hns5_sec_platform_start(struct hns5_sec_runtime *r);
int hns5_sec_platform_close(struct hns5_sec_runtime *r);

/* hns5_sec_bd.c (128-byte descriptor build from session+op) */
int hns5_sec_build_descriptor(struct hns5_sec_runtime *r,
			      struct hns5_session *sess,
			      struct rte_crypto_op *op,
			      struct hns5_sec_segment *seg,
			      sec_alg_s *alg);

/* hns5_sec_complete.c (POE completion poll + write-back parse) */
int hns5_sec_reap_completion(struct hns5_sec_runtime *r,
			     struct hns5_sec_segment *seg, int *result);

/* Extract the engine result from a completed segment into the DPDK op. */
int hns5_sec_extract_result(struct hns5_sec_segment *seg,
			    struct hns5_session *sess, struct rte_crypto_op *op);

/* hns5_crypto_capabilities.c */
const struct rte_cryptodev_capabilities *hns5_caps_get(void);

/* hns5_crypto_pmd.c */
int hns5_crypto_probe(struct rte_vdev_device *vdev);
int hns5_crypto_remove(struct rte_vdev_device *vdev);

#endif /* _HNS5_CRYPTO_PMD_PRIVATE_H_ */
