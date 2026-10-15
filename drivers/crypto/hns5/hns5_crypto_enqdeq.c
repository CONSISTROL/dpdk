/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Huawei Technologies Co., Ltd
 *
 * Asynchronous enqueue/dequeue. enqueue builds a descriptor per op and submits
 * it through hidrv_send_sec, recording the op+segment in the in-flight ring.
 * dequeue polls POE for the ring head completion (POE is in-order, so
 * completions arrive in submission order), extracts the result and returns
 * the completed ops.
 */

#include "hns5_crypto_pmd_private.h"

#include <rte_crypto_sym.h>

static inline uint16_t
ring_next(uint16_t cur, uint16_t depth)
{
	return (uint16_t)((cur + 1) % depth);
}

uint16_t
hns5_enqueue_burst(struct hns5_qp *qp, struct rte_crypto_op **ops, uint16_t nb_ops)
{
	struct hns5_sec_runtime *sec;
	uint16_t i;
	uint16_t enqueued = 0;
	int cancel;

	if (qp == NULL || ops == NULL || nb_ops == 0)
		return 0;
	sec = qp->sec;
	if (sec == NULL || sec->common == NULL || sec->common->poisoned)
		return 0;

	if (hns5_runtime_lock(sec->common, &cancel))
		return 0;

	for (i = 0; i < nb_ops; i++) {
		struct rte_crypto_op *op = ops[i];
		struct hns5_session *sess;
		struct hns5_inflight_op *slot;
		sec_alg_s alg;
		uint64_t pa = 0;
		int rc;

		if (qp->inflight == qp->depth)
			break;

		sess = (struct hns5_session *)CRYPTODEV_GET_SYM_SESS_PRIV(op->sym->session);
		if (sess == NULL) {
			op->status = RTE_CRYPTO_OP_STATUS_INVALID_ARGS;
			continue;
		}

		slot = &qp->ring[qp->prod_head];
		slot->seg.raw = hns5_bmu_alloc(sec->common, &pa);
		if (slot->seg.raw == NULL) {
			op->status = RTE_CRYPTO_OP_STATUS_NOT_PROCESSED;
			break;
		}
		slot->seg.pa = pa;
		memset(slot->seg.raw, 0, HNS5_BUFFER_BYTES);

		rc = hns5_sec_build_descriptor(sec, sess, op, &slot->seg, &alg);
		if (rc) {
			hns5_bmu_free(sec->common, slot->seg.raw);
			op->status = RTE_CRYPTO_OP_STATUS_INVALID_ARGS;
			continue;
		}

		slot->op = op;
		slot->used = true;
		__sync_synchronize();

		if (hidrv_send_sec((uint32_t)sec->sec_vf, sec->vcpu,
				   slot->seg.raw + HNS5_SEC_MBUF_OFFSET, &alg)) {
			hns5_bmu_free(sec->common, slot->seg.raw);
			slot->used = false;
			op->status = RTE_CRYPTO_OP_STATUS_ERROR;
			hns5_runtime_poison(sec->common,
				"SEC submit failed; DMA ownership uncertain");
			break;
		}

		sec->in_use = true;
		qp->prod_head = ring_next(qp->prod_head, qp->depth);
		qp->inflight++;
		enqueued++;
	}

	qp->stats.enqueued_count += enqueued;
	hns5_runtime_unlock(sec->common, cancel);
	return enqueued;
}

uint16_t
hns5_dequeue_burst(struct hns5_qp *qp, struct rte_crypto_op **ops, uint16_t nb_ops)
{
	struct hns5_sec_runtime *sec;
	uint16_t dequeued = 0;
	int cancel;

	if (qp == NULL || ops == NULL || nb_ops == 0)
		return 0;
	sec = qp->sec;
	if (sec == NULL || sec->common == NULL)
		return 0;

	if (hns5_runtime_lock(sec->common, &cancel))
		return 0;

	while (dequeued < nb_ops && qp->inflight > 0) {
		struct hns5_inflight_op *slot = &qp->ring[qp->cons_tail];
		struct rte_crypto_op *op;
		struct hns5_session *sess;
		int rc;

		if (!slot->used)
			break;

		rc = hns5_sec_reap_completion(sec, &slot->seg, NULL);
		if (rc == -ETIMEDOUT)
			break;	/* no completion yet */
		if (rc == -EIO)
			break;	/* completion mismatch; ownership uncertain */

		op = slot->op;
		sess = (struct hns5_session *)CRYPTODEV_GET_SYM_SESS_PRIV(op->sym->session);
		rc = hns5_sec_extract_result(&slot->seg, sess, op);
		hns5_bmu_free(sec->common, slot->seg.raw);
		slot->used = false;
		qp->cons_tail = ring_next(qp->cons_tail, qp->depth);
		qp->inflight--;

		if (rc == 0)
			op->status = RTE_CRYPTO_OP_STATUS_SUCCESS;
		else if (rc == -EBADMSG)
			op->status = RTE_CRYPTO_OP_STATUS_AUTH_FAILED;
		else
			op->status = RTE_CRYPTO_OP_STATUS_ERROR;

		ops[dequeued++] = op;
	}

	if (dequeued != 0)
		qp->stats.dequeued_count += dequeued;

	if (qp->inflight == 0)
		sec->in_use = false;

	hns5_runtime_unlock(sec->common, cancel);
	return dequeued;
}
