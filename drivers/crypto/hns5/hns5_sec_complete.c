/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Huawei Technologies Co., Ltd
 *
 * POE completion handling. Mirrors the reference engine's complete(): poll the
 * POE completion virtual queues for a message carrying the segment's physical
 * address, then read the write-back mbuf state. Because POE order_type is 0
 * (in-order), completions arrive in submission order, so dequeue reaps the
 * in-flight ring head.
 */

#include "hns5_crypto_pmd_private.h"

#include <rte_memcpy.h>
#include <rte_crypto_sym.h>
#include "poe_errno.h"

/* Poll POE for a completion whose message carries seg->pa. Returns 0 on
 * success, -ETIMEDOUT if no completion arrived within the budget, -EIO on a
 * mismatched completion. */
int
hns5_sec_reap_completion(struct hns5_sec_runtime *r,
			struct hns5_sec_segment *seg, int *result)
{
	union ndrv_poe_msg msg;
	unsigned int queue = 0;
	uint32_t empty_polls = 0;
	unsigned int rc;

	if (r == NULL || seg == NULL)
		return -EINVAL;

	for (;;) {
		rc = ndrv_poe_vf_get_que_msg(r->poe_vf_handle, r->vcpu,
					     r->completion_group, queue, &msg);
		if (!rc)
			break;
		if (!UDRV_IS_ERR(rc, POE_GET_QUEUE_FAIL)) {
			if (result)
				*result = -EIO;
			return -EIO;
		}
		if (++empty_polls >= HNS5_MAX_RECV_POLLS) {
			if (result)
				*result = -ETIMEDOUT;
			return -ETIMEDOUT;
		}
		queue ^= 1;
	}

	uint64_t pa = ((uint64_t)msg.packet_msg.addr_47_32 << 32) |
		      msg.packet_msg.addr_31_0;
	if (msg.packet_msg.t_tagvalue != (NDRV_POE_ADD_PACKET & 15) ||
	    msg.packet_msg.sourceport != HNS5_SEC_SOURCE_PORT ||
	    msg.packet_msg.order_bindval != 0 ||
	    msg.packet_msg.order_type != 0 ||
	    msg.packet_msg.vqi != (queue ? r->error_vqi : r->completion_vqi) ||
	    pa != seg->pa) {
		if (result)
			*result = -EIO;
		return -EIO;
	}

	__sync_synchronize();
	if (result)
		*result = 0;
	return 0;
}

/* Read the engine write-back and copy results into the op. Returns 0 on
 * success, -EBADMSG on authentication failure, -EIO otherwise. */
int
hns5_sec_extract_result(struct hns5_sec_segment *seg,
			struct hns5_session *sess, struct rte_crypto_op *op)
{
	struct hidrv_mbuf *m = (struct hidrv_mbuf *)(void *)
		(seg->raw + HNS5_SEC_MBUF_OFFSET);
	bool is_cipher = (sess->op_type == 0);
	bool is_auth = (sess->op_type == 1);

	if (hidrv_sec_poe_reflesh_mbuf_result(m) || m->done != 1)
		return -EIO;

	if (m->sec_result || m->err_type || m->warning_type || m->icv >= 2 ||
	    m->csc == 2)
		return -EIO;

	if (is_auth || sess->op_type == 2) {
		uint16_t dlen = sess->digest_len;
		uint8_t *dst = op->sym->auth.digest.data;

		if (sess->auth_op == RTE_CRYPTO_AUTH_OP_GENERATE && dst != NULL &&
		    dlen > 0 && dlen <= 32)
			rte_memcpy(dst, seg->raw + HNS5_SEC_MAC_OFFSET, dlen);
	}

	if (is_cipher || sess->op_type == 2) {
		struct rte_mbuf *mdst = op->sym->m_dst ? op->sym->m_dst :
				  op->sym->m_src;
		uint32_t off = op->sym->cipher.data.offset;
		uint32_t len = op->sym->cipher.data.length;
		unsigned char *src = seg->raw + HNS5_SEC_DATA_OFFSET +
				     HNS5_SEC_PREFIX_BYTES;

		if (mdst != NULL && len <= HNS5_SEC_SEGMENT_BYTES) {
			uint32_t copied = 0;
			struct rte_mbuf *cur = mdst;

			while (cur != NULL && copied < len) {
				uint32_t seg_off = (cur == mdst) ? off : 0;
				uint32_t cap = rte_pktmbuf_data_len(cur) - seg_off;
				uint32_t take;

				if (cap == 0) {
					cur = cur->next;
					continue;
				}
				take = (cap < len - copied) ? cap : (len - copied);
				rte_memcpy(rte_pktmbuf_mtod_offset(cur, void *, seg_off),
					   src + copied, take);
				copied += take;
				cur = cur->next;
			}
		}
	}

	if (!(m->flag & (is_cipher ? 2u : 1u)))
		return -EIO;
	return 0;
}
