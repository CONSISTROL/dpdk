/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Huawei Technologies Co., Ltd
 *
 * SEC PF/VF + POE setup, adapted from the Hi1231 reference sec_platform.c.
 * Brings up POE submit/completion groups, binds the reserved vCPU1 to the
 * completion group, sets the SEC PF/VF into POE mode, and verifies the POE
 * routing. Ownership markers in the common runtime keep DMA memory from being
 * reused after an uncertain teardown.
 */

#include "hns5_crypto_pmd_private.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define POE_DEFAULT_BP_THRESHOLD 0x3fffffU
#define POE_GROUP_LIMIT 64U
#define POE_MASK_WORD_BITS 32U
#define POE_MASK_WORDS 2U
#define POE_VCPU_LIMIT 64U

struct poe_acc_endpoint {
	enum ndrv_mod_id module;
	unsigned int index;
	const char *name;
};

static const struct poe_acc_endpoint poe_acc_endpoints[] = {
	{ NDRV_SEC, 0U, "SEC0" },
	{ NDRV_SA, 0U, "SA0" },
	{ NDRV_TRING, 0U, "TRING0" },
	{ NDRV_SPPE_PKIO, 0U, "SPPE_PKIO0" },
	{ NDRV_SPPE_PKIO, 1U, "SPPE_PKIO1" },
};

#define CHECK(call)										\
	do {											\
		if ((call) != 0) {								\
			HNS5_LOG(ERR, "%s failed", #call);					\
			return -EIO;								\
		}										\
	} while (0)

static bool
poe_mask_is_empty(const unsigned int *mask, unsigned int words)
{
	unsigned int i;

	for (i = 0; i < words; ++i)
		if (mask[i] != 0U)
			return false;
	return true;
}

static int
check_group(struct hns5_sec_runtime *p, unsigned int group)
{
	struct ndrv_poe_pgrp_map_info map = { 0 };
	struct ndrv_poe_vf_grp_stats stats = { 0 };
	unsigned int enabled;

	CHECK(ndrv_poe_pf_get_pgrp_map(p->poe_pf_handle, group, &map));
	if (map.vfid != 0U || map.vgrp != group)
		return -1;
	CHECK(ndrv_poe_vf_get_grp_out_enable(p->poe_vf_handle, group, &enabled));
	CHECK(ndrv_poe_vf_get_grp_stats(p->poe_vf_handle, group, &stats));
	if (stats.cur_pkt_cnt != 0U)
		return -1;
	return 0;
}

static int
check_submit_group_unbound(struct hns5_sec_runtime *p,
			   const struct ndrv_poe_pf_grp_range *range)
{
	unsigned int mask[POE_MASK_WORDS] = { 0 };
	unsigned int words = (range->acc_grp_num + POE_MASK_WORD_BITS - 1U) /
			     POE_MASK_WORD_BITS;
	unsigned int local = p->submit_group - range->acc_grp_start;
	size_t i;

	if (words == 0U || words > POE_MASK_WORDS)
		return -EIO;
	for (i = 0; i < sizeof(poe_acc_endpoints) / sizeof(poe_acc_endpoints[0]); ++i) {
		const struct poe_acc_endpoint *endpoint = &poe_acc_endpoints[i];
		unsigned int rc;

		memset(mask, 0, sizeof(mask));
		rc = ndrv_poe_vf_get_acc_bind_grp(p->poe_vf_handle, endpoint->module,
						  endpoint->index, mask, words);
		if (rc != 0U)
			return -1;
		if ((mask[local / POE_MASK_WORD_BITS] &
		     (1U << (local % POE_MASK_WORD_BITS))) != 0U)
			return -1;
	}
	return 0;
}

static int
select_reserved_vcpu(struct hns5_sec_runtime *p,
		      const struct ndrv_poe_pf_grp_range *range)
{
	bool vcpu_free[POE_VCPU_LIMIT] = { false };
	struct ndrv_poe_lcpu_map_info cpu = { 0 };
	unsigned int mask[POE_MASK_WORDS] = { 0 };
	unsigned int words = (range->cpu_grp_num + POE_MASK_WORD_BITS - 1U) /
			     POE_MASK_WORD_BITS;
	unsigned int local = p->completion_group - range->cpu_grp_start;
	unsigned int vcpu_count = ndrv_poe_get_lcpu_num();
	unsigned int i;

	if (words == 0U || words > POE_MASK_WORDS || vcpu_count == 0U ||
	    vcpu_count > POE_VCPU_LIMIT || HNS5_SEC_ENGINE_VCPU >= vcpu_count)
		return -EIO;
	for (i = 0; i < vcpu_count; ++i) {
		unsigned int rc;

		memset(mask, 0, sizeof(mask));
		rc = ndrv_poe_vf_get_vcpu_bind_grp(p->poe_vf_handle, i, mask, words);
		if (rc != 0U)
			return -1;
		if ((mask[local / POE_MASK_WORD_BITS] &
		     (1U << (local % POE_MASK_WORD_BITS))) != 0U)
			return -1;
		vcpu_free[i] = poe_mask_is_empty(mask, words);
	}
	for (i = 0; i < vcpu_count; ++i) {
		CHECK(ndrv_poe_pf_get_lcpu_map(p->poe_pf_handle, i, &cpu));
		if (cpu.vfid != 0U || cpu.vcpu != HNS5_SEC_ENGINE_VCPU)
			continue;
		if (cpu.vcpu >= vcpu_count)
			return -1;
		if (!vcpu_free[cpu.vcpu])
			return -EIO;
		p->vcpu = cpu.vcpu;
		return 0;
	}
	return -EIO;
}

int
hns5_sec_platform_inspect(struct hns5_sec_runtime *p)
{
	struct ndrv_poe_pf_grp_range range = { 0 };
	int submit_status, completion_status, cpu_status, acc_status;

	p->poe_pf = hidrv_poe_get_pf_devid(0, 0);
	p->poe_vf = hidrv_poe_get_vf_devid(0, 0, 0);
	p->sec_pf = hidrv_sec_get_pf_devid(0, 0);
	p->sec_vf = hidrv_sec_get_vf_devid(0, 0, 0, HIDRV_SEC_VF_MODE_POE);
	if (p->poe_pf < 0 || p->poe_vf < 0 || p->sec_pf < 0 || p->sec_vf < 0)
		return -EIO;

	p->poe_pf_handle = hidrv_poe_pf_get_ndrv_handle(p->poe_pf);
	p->poe_vf_handle = hidrv_poe_vf_get_ndrv_handle(p->poe_vf);
	if (p->poe_pf_handle == NULL || p->poe_vf_handle == NULL)
		return -EIO;

	CHECK(ndrv_poe_pf_get_grp_range(p->poe_pf_handle, &range));
	if (!range.cpu_grp_num || !range.acc_grp_num ||
	    range.cpu_grp_start >= POE_GROUP_LIMIT ||
	    range.acc_grp_start >= POE_GROUP_LIMIT ||
	    range.cpu_grp_num > POE_GROUP_LIMIT - range.cpu_grp_start ||
	    range.acc_grp_num > POE_GROUP_LIMIT - range.acc_grp_start ||
	    (range.cpu_grp_start < range.acc_grp_start + range.acc_grp_num &&
	     range.acc_grp_start < range.cpu_grp_start + range.cpu_grp_num))
		return -EIO;
	if (HNS5_SEC_ENGINE_COMPLETION_GRP < range.cpu_grp_start ||
	    HNS5_SEC_ENGINE_COMPLETION_GRP >= range.cpu_grp_start + range.cpu_grp_num)
		return -EIO;
	p->completion_group = HNS5_SEC_ENGINE_COMPLETION_GRP;
	p->submit_group = range.acc_grp_start;
	p->completion_group_local = p->completion_group - range.cpu_grp_start;
	p->submit_group_local = p->submit_group - range.acc_grp_start;
	submit_status = check_group(p, p->submit_group);
	completion_status = check_group(p, p->completion_group);
	if (submit_status != 0 || completion_status != 0)
		return -1;
	cpu_status = select_reserved_vcpu(p, &range);
	acc_status = check_submit_group_unbound(p, &range);
	if (cpu_status != 0 || acc_status != 0)
		return -1;
	return 0;
}

static int
setup_poe(struct hns5_sec_runtime *p)
{
	struct hidrv_poe_grp_cfg group = {
		.order_en = 0U,
		.preload_en = 0U,
		.flow_en = 1U,
		.int_th = 0U,
		.bp_ppe_th = POE_DEFAULT_BP_THRESHOLD
	};
	struct hidrv_poe_group_bind_mask mask = { { 0 } };
	struct hidrv_poe_group_bind_mask old = { { 0 } };
	unsigned int enabled, flow, bp;
	size_t i;

	CHECK(hidrv_poe_get_cpu_bind_group_mask(p->poe_vf, p->vcpu, &old));
	for (i = 0; i < sizeof(old.group_mask) / sizeof(old.group_mask[0]); ++i)
		if (old.group_mask[i])
			return -EIO;
	p->complete_open = true;
	CHECK(hidrv_poe_open_group(p->poe_vf, p->completion_group, &group));
	p->submit_open = true;
	CHECK(hidrv_poe_open_group(p->poe_vf, p->submit_group, &group));

	CHECK(ndrv_poe_vf_get_grp_out_enable(p->poe_vf_handle,
			p->completion_group, &enabled));
	CHECK(ndrv_poe_vf_get_grp_overflow_enable(p->poe_vf_handle,
			p->completion_group, &flow));
	CHECK(ndrv_poe_vf_get_grp_bp_th(p->poe_vf_handle,
			p->completion_group, &bp));
	if (enabled != 1U || flow != 1U || bp != POE_DEFAULT_BP_THRESHOLD)
		return -EIO;
	CHECK(ndrv_poe_vf_get_grp_out_enable(p->poe_vf_handle,
			p->submit_group, &enabled));
	CHECK(ndrv_poe_vf_get_grp_overflow_enable(p->poe_vf_handle,
			p->submit_group, &flow));
	CHECK(ndrv_poe_vf_get_grp_bp_th(p->poe_vf_handle, p->submit_group, &bp));
	if (enabled != 1U || flow != 1U || bp != POE_DEFAULT_BP_THRESHOLD)
		return -EIO;

	mask.group_mask[p->completion_group_local / 64U] = UINT64_C(1) <<
		(p->completion_group_local % 64U);
	p->cpu_bound = true;
	CHECK(hidrv_poe_set_cpu_bind_group_mask(p->poe_vf, p->vcpu, &mask));
	return 0;
}

static int
setup_sec(struct hns5_sec_runtime *p)
{
	struct hidrv_sec_pf_conf pf = { 0 };
	struct hidrv_sec_vf_conf vf = { 0 };
	struct hidrv_poe_group_bind_mask cpu_mask = { { 0 } };
	struct hidrv_poe_group_bind_mask sec_mask = { { 0 } };
	uint64_t expected;
	size_t i;
	int rc;

	CHECK(ndrv_poe_vf_cvt_que_to_vqi(p->poe_vf_handle, p->submit_group, 0,
			&p->submit_vqi));
	CHECK(ndrv_poe_vf_cvt_que_to_vqi(p->poe_vf_handle, p->completion_group, 0,
			&p->completion_vqi));
	CHECK(ndrv_poe_vf_cvt_que_to_vqi(p->poe_vf_handle, p->completion_group, 1,
			&p->error_vqi));
	pf.mode = HIDRV_SEC_PF_MODE_POE;
	pf.sec_bd_endian = HIDRV_SEC_ENDIAN_32_LITTLE;

	rc = hidrv_sec_pf_setup((uint32_t)p->sec_pf, &pf, NULL);
	if (rc != 0) {
		hns5_runtime_poison(p->common, "SEC PF init failed");
		return -1;
	}
	p->sec_pf_started = true;
	vf.mode = HIDRV_SEC_VF_MODE_POE;
	vf.poe_conf.get_packet_group = p->submit_group;
	vf.poe_conf.get_packet_vqi = p->submit_vqi;
	vf.poe_conf.add_packet_group = p->completion_group;
	vf.poe_conf.add_packet_vqi = p->completion_vqi;
	vf.poe_conf.add_err_packet_vqi = p->error_vqi;

	rc = hidrv_sec_vf_setup((uint32_t)p->sec_vf, &vf, NULL);
	if (rc != 0) {
		hns5_runtime_poison(p->common, "SEC VF init failed");
		return -1;
	}
	p->sec_vf_started = true;

	CHECK(hidrv_poe_get_cpu_bind_group_mask(p->poe_vf, p->vcpu, &cpu_mask));
	CHECK(hidrv_poe_get_acc_bind_group_mask(p->poe_vf, HIDRV_POE_ACC_SEC, 0,
			&sec_mask));
	for (i = 0; i < sizeof(cpu_mask.group_mask) / sizeof(cpu_mask.group_mask[0]); ++i) {
		expected = (i == p->completion_group_local / 64U)
			   ? UINT64_C(1) << (p->completion_group_local % 64U)
			   : 0U;
		if (cpu_mask.group_mask[i] != expected)
			return -EIO;
	}
	if ((sec_mask.group_mask[p->submit_group_local / 64U] &
	     (UINT64_C(1) << (p->submit_group_local % 64U))) == 0U)
		return -EIO;
	return 0;
}

int
hns5_sec_platform_start(struct hns5_sec_runtime *r)
{
	const struct hidrv_mbuf_params layout = { 128, 128, 128 };

	if (r->common == NULL)
		return -EINVAL;
	hidrv_mbuf_set_config(&layout);
	if (setup_poe(r) || setup_sec(r)) {
		hns5_runtime_poison(r->common, "SEC/POE partial initialization");
		return -EIO;
	}
	return 0;
}

int
hns5_sec_platform_close(struct hns5_sec_runtime *r)
{
	struct hidrv_poe_group_bind_mask empty = { { 0 } };
	int rc = 0;

	if (r == NULL)
		return 0;

	if (r->in_use)
		return -EBUSY;

	if (r->sec_vf_started && hidrv_sec_vf_unsetup((uint32_t)r->sec_vf))
		rc = -EIO;
	if (r->sec_pf_started && hidrv_sec_pf_unsetup((uint32_t)r->sec_pf))
		rc = -EIO;
	if (r->cpu_bound &&
	    hidrv_poe_set_cpu_bind_group_mask(r->poe_vf, r->vcpu, &empty))
		rc = -EIO;
	if (r->submit_open && hidrv_poe_close_group(r->poe_vf, r->submit_group))
		rc = -EIO;
	if (r->complete_open && hidrv_poe_close_group(r->poe_vf, r->completion_group))
		rc = -EIO;
	if (rc)
		hns5_runtime_poison(r->common, "SEC close incomplete");
	r->sec_vf_started = false;
	r->sec_pf_started = false;
	r->cpu_bound = false;
	r->submit_open = false;
	r->complete_open = false;
	return rc;
}
