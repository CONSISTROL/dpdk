/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Huawei Technologies Co., Ltd
 *
 * DPDK sym session handling. The SEC engine is sessionless at the HW level
 * (keys/IVs/MAC are supplied per descriptor), so the session only holds the
 * software state needed to build a descriptor: algorithm/mode identifiers,
 * key length and the raw key.
 */

#include "hns5_crypto_pmd_private.h"
#include <rte_crypto_sym.h>
#include <rte_ether.h>

static int
cipher_setup(struct hns5_session *sess, const struct rte_crypto_cipher_xform *x)
{
	switch (x->algo) {
	case RTE_CRYPTO_CIPHER_SM4_CBC:
		sess->cipher_alg = HNS5_SEC_CIPHER_ALG_SM4;
		sess->cipher_mode = HNS5_SEC_CIPHER_MODE_CBC;
		break;
	case RTE_CRYPTO_CIPHER_AES_CBC:
		sess->cipher_alg = HNS5_SEC_CIPHER_ALG_AES;
		sess->cipher_mode = HNS5_SEC_CIPHER_MODE_CBC;
		break;
	case RTE_CRYPTO_CIPHER_AES_ECB:
		sess->cipher_alg = HNS5_SEC_CIPHER_ALG_AES;
		sess->cipher_mode = HNS5_SEC_CIPHER_MODE_ECB;
		break;
	default:
		HNS5_LOG(ERR, "unsupported cipher algo %u", x->algo);
		return -ENOTSUP;
	}
	if (x->key.length != 16 && x->key.length != 24 && x->key.length != 32) {
		HNS5_LOG(ERR, "bad cipher key length %u", x->key.length);
		return -EINVAL;
	}
	sess->cipher_keylen = (uint8_t)x->key.length;
	sess->cipher_dir = (uint8_t)x->op;
	sess->iv_offset = (uint16_t)x->iv.offset;
	sess->iv_length = (uint16_t)x->iv.length;
	if (x->key.data != NULL)
		rte_memcpy(sess->key, x->key.data, x->key.length);
	return 0;
}

static int
auth_setup(struct hns5_session *sess, const struct rte_crypto_auth_xform *x)
{
	bool hmac = (x->key.length != 0);

	switch (x->algo) {
	case RTE_CRYPTO_AUTH_SM3:
		sess->auth_alg = hmac ? HNS5_SEC_AUTH_HMAC_SM3 : HNS5_SEC_AUTH_SM3;
		break;
	case RTE_CRYPTO_AUTH_SM3_HMAC:
		sess->auth_alg = HNS5_SEC_AUTH_HMAC_SM3;
		break;
	case RTE_CRYPTO_AUTH_SHA256:
		sess->auth_alg = hmac ? HNS5_SEC_AUTH_HMAC_SHA256 : HNS5_SEC_AUTH_SHA_256;
		break;
	default:
		HNS5_LOG(ERR, "unsupported auth algo %u", x->algo);
		return -ENOTSUP;
	}
	if (hmac) {
		if (x->key.length > sizeof(sess->key))
			return -EINVAL;
		sess->akey_len = (uint16_t)x->key.length;
		rte_memcpy(sess->key, x->key.data, x->key.length);
	}
	sess->auth_op = (uint8_t)x->op;
	sess->digest_len = (uint16_t)x->digest_length;
	return 0;
}

int
hns5_session_configure(struct rte_crypto_sym_xform *xform, void *priv)
{
	struct hns5_session *sess = priv;
	struct rte_crypto_sym_xform *xf = xform;

	if (sess == NULL || xf == NULL)
		return -EINVAL;
	memset(sess, 0, sizeof(*sess));

	if (xf->type == RTE_CRYPTO_SYM_XFORM_CIPHER && xf->next == NULL) {
		sess->op_type = 0;
		return cipher_setup(sess, &xf->cipher);
	}
	if (xf->type == RTE_CRYPTO_SYM_XFORM_AUTH && xf->next == NULL) {
		sess->op_type = 1;
		return auth_setup(sess, &xf->auth);
	}
	/* Chain: cipher-then-auth (or auth-then-cipher). */
	if (xf->type == RTE_CRYPTO_SYM_XFORM_CIPHER && xf->next != NULL &&
	    xf->next->type == RTE_CRYPTO_SYM_XFORM_AUTH) {
		int rc;

		sess->op_type = 2;
		rc = cipher_setup(sess, &xf->cipher);
		if (rc)
			return rc;
		return auth_setup(sess, &xf->next->auth);
	}
	HNS5_LOG(ERR, "unsupported xform chain");
	return -ENOTSUP;
}

void
hns5_session_clear(void *priv)
{
	if (priv != NULL)
		memset(priv, 0, sizeof(struct hns5_session));
}
