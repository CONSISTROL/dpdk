/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Huawei Technologies Co., Ltd
 *
 * Capabilities advertised by the SEC engine. The set mirrors the subset the
 * reference Hi1231 SEC engine exposes and is constrained to algorithms whose
 * rte_crypto enums already exist in DPDK 22.11.
 */

#include "hns5_crypto_pmd_private.h"

static const struct rte_cryptodev_capabilities hns5_capabilities[] = {
	{	/* SM4 CBC */
		.op = RTE_CRYPTO_OP_TYPE_SYMMETRIC,
		{.sym = {
			.xform_type = RTE_CRYPTO_SYM_XFORM_CIPHER,
			{.cipher = {
				.algo = RTE_CRYPTO_CIPHER_SM4_CBC,
				.block_size = 16,
				.key_size = { .min = 16, .max = 16, .increment = 0 },
				.iv_size = { .min = 16, .max = 16, .increment = 0 }
			}, }
		}, }
	},
	{	/* AES-128/192/256 CBC */
		.op = RTE_CRYPTO_OP_TYPE_SYMMETRIC,
		{.sym = {
			.xform_type = RTE_CRYPTO_SYM_XFORM_CIPHER,
			{.cipher = {
				.algo = RTE_CRYPTO_CIPHER_AES_CBC,
				.block_size = 16,
				.key_size = { .min = 16, .max = 32, .increment = 8 },
				.iv_size = { .min = 16, .max = 16, .increment = 0 }
			}, }
		}, }
	},
	{	/* AES-128/192/256 ECB */
		.op = RTE_CRYPTO_OP_TYPE_SYMMETRIC,
		{.sym = {
			.xform_type = RTE_CRYPTO_SYM_XFORM_CIPHER,
			{.cipher = {
				.algo = RTE_CRYPTO_CIPHER_AES_ECB,
				.block_size = 16,
				.key_size = { .min = 16, .max = 32, .increment = 8 },
				.iv_size = { .min = 0, .max = 0, .increment = 0 }
			}, }
		}, }
	},
	{	/* SM3 */
		.op = RTE_CRYPTO_OP_TYPE_SYMMETRIC,
		{.sym = {
			.xform_type = RTE_CRYPTO_SYM_XFORM_AUTH,
			{.auth = {
				.algo = RTE_CRYPTO_AUTH_SM3,
				.block_size = 64,
				.key_size = { .min = 0, .max = 0, .increment = 0 },
				.digest_size = { .min = 32, .max = 32, .increment = 0 },
				.iv_size = { .min = 0, .max = 0, .increment = 0 }
			}, }
		}, }
	},
	{	/* SM3-HMAC */
		.op = RTE_CRYPTO_OP_TYPE_SYMMETRIC,
		{.sym = {
			.xform_type = RTE_CRYPTO_SYM_XFORM_AUTH,
			{.auth = {
				.algo = RTE_CRYPTO_AUTH_SM3_HMAC,
				.block_size = 64,
				.key_size = { .min = 1, .max = 64, .increment = 1 },
				.digest_size = { .min = 32, .max = 32, .increment = 0 },
				.iv_size = { .min = 0, .max = 0, .increment = 0 }
			}, }
		}, }
	},
	{	/* SHA-256 */
		.op = RTE_CRYPTO_OP_TYPE_SYMMETRIC,
		{.sym = {
			.xform_type = RTE_CRYPTO_SYM_XFORM_AUTH,
			{.auth = {
				.algo = RTE_CRYPTO_AUTH_SHA256,
				.block_size = 64,
				.key_size = { .min = 0, .max = 0, .increment = 0 },
				.digest_size = { .min = 32, .max = 32, .increment = 0 },
				.iv_size = { .min = 0, .max = 0, .increment = 0 }
			}, }
		}, }
	},
	RTE_CRYPTODEV_END_OF_CAPABILITIES_LIST()
};

const struct rte_cryptodev_capabilities *
hns5_caps_get(void)
{
	return hns5_capabilities;
}
