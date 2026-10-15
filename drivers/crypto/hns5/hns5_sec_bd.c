/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Huawei Technologies Co., Ltd
 *
 * 128-byte SEC descriptor builder. Mirrors the reference engine's build():
 * the key, IV and message are copied into a single BMU-backed segment (the
 * first segment of the chain; longer messages would chain segments, but the
 * initial DPDK path supports a single segment for messages up to one SEC
 * segment). The SEC engine performs the key schedule on the origin key, so
 * no persistent HW session is needed.
 */

#include "hns5_crypto_pmd_private.h"

#include <rte_memcpy.h>
#include <rte_crypto_sym.h>


static int
copy_in_data(struct hns5_sec_segment *seg, struct rte_crypto_op *op,
	     bool is_cipher, size_t *msg_len)
{
	unsigned char *dst = seg->raw + HNS5_SEC_DATA_OFFSET + HNS5_SEC_PREFIX_BYTES;
	struct rte_mbuf *msrc;
	uint32_t off, len, copied = 0;
	unsigned int i;

	if (is_cipher) {
		off = op->sym->cipher.data.offset;
		len = op->sym->cipher.data.length;
	} else {
		off = op->sym->auth.data.offset;
		len = op->sym->auth.data.length;
	}
	*msg_len = len;

	if (len > HNS5_SEC_SEGMENT_BYTES)
		return -ENOSPC;

	msrc = op->sym->m_src;
	for (i = 0; i < msrc->nb_segs && copied < len; i++) {
		struct rte_mbuf *seg_cur = (i == 0) ? msrc : msrc->next;
		uint32_t seg_off = (i == 0) ? off : 0;
		uint32_t seg_len = rte_pktmbuf_data_len(seg_cur);
		uint32_t take;

		if (seg_off >= seg_len)
			continue;
		take = seg_len - seg_off;
		if (take > len - copied)
			take = len - copied;
		rte_memcpy(dst + copied,
			   rte_pktmbuf_mtod_offset(seg_cur, void *, seg_off), take);
		copied += take;
		if (seg_cur == msrc)
			seg_off = 0;
	}
	if (copied != len)
		return -EINVAL;
	return 0;
}

int
hns5_sec_build_descriptor(struct hns5_sec_runtime *r,
			  struct hns5_session *sess,
			  struct rte_crypto_op *op,
			  struct hns5_sec_segment *seg,
			  sec_alg_s *alg)
{
	size_t msg_len = 0;
	bool is_cipher = (sess->op_type == 0);
	bool is_auth = (sess->op_type == 1);
	bool is_chain = (sess->op_type == 2);
	int rc;

	RTE_SET_USED(r);

	seg->raw = NULL;	/* allocated by caller */
	seg->used = 0;
	seg->pa = 0;

	rc = copy_in_data(seg, op, is_cipher || is_chain, &msg_len);
	if (rc)
		return rc;

	/* Build the mbuf header the SEC POE path expects. Single-segment
	 * chain: this segment is both head and tail. */
	{
		struct hidrv_mbuf *m = (struct hidrv_mbuf *)(void *)
			(seg->raw + HNS5_SEC_MBUF_OFFSET);
		m->buf_addr = seg->raw + HNS5_SEC_BUFFER_OFFSET;
		m->buf_pa = seg->pa + HNS5_SEC_BUFFER_OFFSET;
		m->data_offset = (uint16_t)(HNS5_SEC_DATA_OFFSET - HNS5_SEC_BUFFER_OFFSET);
		m->ref_cnt = 1;
		m->segs_num = 1;
		m->pkt_len = (uint32_t)(msg_len + HNS5_SEC_PREFIX_BYTES);
		m->data_len = (uint16_t)(msg_len + HNS5_SEC_PREFIX_BYTES);
		m->buf_len = HNS5_SEC_MAC_OFFSET - HNS5_SEC_BUFFER_OFFSET;
		m->next = NULL;
	}

	/* Copy key (SEC does the key schedule on the origin key). */
	if ((is_cipher || is_chain) && sess->cipher_keylen)
		rte_memcpy(seg->raw + HNS5_SEC_KEY_OFFSET, sess->key,
			   sess->cipher_keylen);
	/* IV for non-ECB cipher modes sits at the data offset head. */
	if ((is_cipher || is_chain) && sess->cipher_mode != HNS5_SEC_CIPHER_MODE_ECB) {
		uint8_t *iv = rte_crypto_op_ctod_offset(op, uint8_t *,
							sess->iv_offset);
		rte_memcpy(seg->raw + HNS5_SEC_DATA_OFFSET, iv, sess->iv_length);
	}
	/* For HMAC the key is placed at the auth key slot (same MAC area top). */
	if (is_auth && (sess->auth_alg == HNS5_SEC_AUTH_HMAC_SM3 ||
			sess->auth_alg == HNS5_SEC_AUTH_HMAC_SHA256))
		rte_memcpy(seg->raw + HNS5_SEC_KEY_OFFSET, sess->key,
			   sess->akey_len);

	memset(alg, 0, sizeof(*alg));
	alg->scene = HIDRV_SEC_SCENE_IPSEC;
	alg->addr_type = HIDRV_SEC_CHAIN_BUFF;
	alg->cipher_data_in_addr = seg->pa + HNS5_SEC_DATA_OFFSET;
	alg->auth_data_in_addr = seg->pa + HNS5_SEC_DATA_OFFSET;
	alg->cipher_data_in_offset = HNS5_SEC_PREFIX_BYTES;
	alg->auth_data_in_offset = HNS5_SEC_PREFIX_BYTES;
	alg->cipher_key_addr = seg->pa + HNS5_SEC_KEY_OFFSET;
	alg->auth_key_addr = seg->pa + HNS5_SEC_KEY_OFFSET;
	alg->cipher_iv_in_addr = seg->pa + HNS5_SEC_DATA_OFFSET;
	alg->auth_iv_in_addr = seg->pa + HNS5_SEC_DATA_OFFSET;
	alg->cipher_iv_in_offset = 0;
	alg->cipher_iv_gen = HIDRV_SEC_CI_GEN_FROM_ADDR;
	alg->auth_iv_gen = HIDRV_SEC_AI_GEN_FROM_INNER;

	if (is_cipher || is_chain) {
		alg->ac_type = is_chain ? HIDRV_SEC_CIPHER_AUTH : HIDRV_SEC_CIPHER;
		alg->cipher_type = (sess->cipher_dir == RTE_CRYPTO_CIPHER_OP_ENCRYPT)
				   ? HIDRV_SEC_CIPHER_ENCRYPT : HIDRV_SEC_CIPHER_DECRYPT;
		alg->cipher_alg.bits.cipher_alg = sess->cipher_alg;
		alg->cipher_alg.bits.cipher_mode = sess->cipher_mode;
		alg->cipher_alg.bits.cipher_keylen = (sess->cipher_keylen - 16) / 8;
		alg->cipher_gran_sz = (uint32_t)msg_len;
		alg->cipher_data_out_addr = seg->pa + HNS5_SEC_DATA_OFFSET;
	}

	if (is_auth || is_chain) {
		alg->auth_alg.bits.auth_alg = sess->auth_alg;
		alg->auth_alg.bits.auth_keylen = sess->akey_len / 4;
		alg->auth_alg.bits.mac_len = 8;
		alg->auth_gran_sz = (uint32_t)msg_len;
		alg->auth_data_out_addr = seg->pa + HNS5_SEC_MAC_OFFSET;
	}

	seg->used = HNS5_SEC_PREFIX_BYTES + msg_len;
	return 0;
}
