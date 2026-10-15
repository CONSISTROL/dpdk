/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Huawei Technologies Co., Ltd
 *
 * DPDK vdev front-end. Probe acquires the shared Hi1231 HugeTLB/BMU runtime
 * and brings up the SEC PF/VF + POE submit/completion groups; each queue pair
 * keeps its own in-flight ring.
 */

#include "hns5_crypto_pmd_private.h"

#include <rte_string_fns.h>
#include <rte_malloc.h>

static uint8_t hns5_cryptodev_driver_id;

static void
hns5_dev_info_get(struct rte_cryptodev *dev,
		  struct rte_cryptodev_info *info)
{
	struct hns5_dev_private *priv = dev->data->dev_private;

	RTE_SET_USED(dev);
	if (info == NULL)
		return;

	info->driver_id = hns5_cryptodev_driver_id;
	info->feature_flags = dev->feature_flags;
	info->capabilities = hns5_caps_get();
	info->max_nb_queue_pairs = priv->max_qps;
	info->sym.max_nb_sessions = 0;
}

static int
hns5_dev_config(struct rte_cryptodev *dev __rte_unused,
		struct rte_cryptodev_config *config __rte_unused)
{
	return 0;
}

static int
hns5_dev_start(struct rte_cryptodev *dev __rte_unused)
{
	return 0;
}

static void
hns5_dev_stop(struct rte_cryptodev *dev __rte_unused)
{
}

static int
hns5_dev_close(struct rte_cryptodev *dev)
{
	struct hns5_dev_private *priv = dev->data->dev_private;

	if (priv == NULL)
		return 0;

	if (priv->sec.in_use) {
		HNS5_LOG(ERR, "device closed with in-flight work; refusing teardown");
		return -EBUSY;
	}

	if (priv->sec.sec_vf_started || priv->sec.sec_pf_started)
		hns5_sec_platform_close(&priv->sec);
	if (priv->common_runtime_acquired)
		hns5_runtime_release(priv->sec.common);
	return 0;
}

static void
hns5_stats_get(struct rte_cryptodev *dev, struct rte_cryptodev_stats *stats)
{
	struct hns5_dev_private *priv = dev->data->dev_private;
	uint16_t i;

	memset(stats, 0, sizeof(*stats));
	for (i = 0; i < priv->nb_used_qps; i++) {
		if (priv->qps[i] == NULL)
			continue;
		stats->enqueued_count += priv->qps[i]->stats.enqueued_count;
		stats->dequeued_count += priv->qps[i]->stats.dequeued_count;
		stats->enqueue_err_count += priv->qps[i]->stats.enqueue_err_count;
		stats->dequeue_err_count += priv->qps[i]->stats.dequeue_err_count;
	}
}

static void
hns5_stats_reset(struct rte_cryptodev *dev)
{
	struct hns5_dev_private *priv = dev->data->dev_private;
	uint16_t i;

	for (i = 0; i < priv->nb_used_qps; i++)
		if (priv->qps[i] != NULL)
			memset(&priv->qps[i]->stats, 0,
			       sizeof(priv->qps[i]->stats));
}

static int
hns5_qp_setup(struct rte_cryptodev *dev, uint16_t qp_id,
	      const struct rte_cryptodev_qp_conf *qp_conf,
	      int socket_id __rte_unused)
{
	struct hns5_dev_private *priv = dev->data->dev_private;
	struct hns5_qp *qp;

	if (qp_id >= priv->max_qps)
		return -EINVAL;

	qp = rte_zmalloc_socket("hns5 qp", sizeof(*qp), 0, socket_id);
	if (qp == NULL)
		return -ENOMEM;

	qp->depth = qp_conf->nb_descriptors;
	qp->ring = rte_zmalloc_socket("hns5 inflight",
				      sizeof(struct hns5_inflight_op) * qp->depth,
				      0, socket_id);
	if (qp->ring == NULL) {
		rte_free(qp);
		return -ENOMEM;
	}

	qp->sec = &priv->sec;
	rte_spinlock_init(&qp->lock);
	snprintf(qp->name, sizeof(qp->name), "hns5_qp_%u", qp_id);
	priv->qps[qp_id] = qp;
	if (qp_id + 1 > priv->nb_used_qps)
		priv->nb_used_qps = qp_id + 1;
	return 0;
}

static int
hns5_qp_release(struct rte_cryptodev *dev, uint16_t qp_id)
{
	struct hns5_dev_private *priv = dev->data->dev_private;
	struct hns5_qp *qp;

	if (qp_id >= priv->max_qps)
		return 0;
	qp = priv->qps[qp_id];
	if (qp == NULL)
		return 0;
	if (qp->inflight != 0)
		HNS5_LOG(ERR, "releasing qp %u with %u ops in flight", qp_id, qp->inflight);
	rte_free(qp->ring);
	rte_free(qp);
	priv->qps[qp_id] = NULL;
	return 0;
}

static unsigned int
hns5_sym_session_get_size(struct rte_cryptodev *dev __rte_unused)
{
	return sizeof(struct hns5_session);
}

static int
hns5_sym_session_configure(struct rte_cryptodev *dev __rte_unused,
			   struct rte_crypto_sym_xform *xform,
			   struct rte_cryptodev_sym_session *sess)
{
	return hns5_session_configure(xform, CRYPTODEV_GET_SYM_SESS_PRIV(sess));
}

static void
hns5_sym_session_clear(struct rte_cryptodev *dev __rte_unused,
		       struct rte_cryptodev_sym_session *sess)
{
	hns5_session_clear(CRYPTODEV_GET_SYM_SESS_PRIV(sess));
}

static struct rte_cryptodev_ops hns5_crypto_pmd_ops = {
	.dev_configure		= hns5_dev_config,
	.dev_start		= hns5_dev_start,
	.dev_stop		= hns5_dev_stop,
	.dev_close		= hns5_dev_close,
	.stats_get		= hns5_stats_get,
	.stats_reset		= hns5_stats_reset,
	.dev_infos_get		= hns5_dev_info_get,
	.queue_pair_setup	= hns5_qp_setup,
	.queue_pair_release	= hns5_qp_release,
	.sym_session_get_size	= hns5_sym_session_get_size,
	.sym_session_configure	= hns5_sym_session_configure,
	.sym_session_clear	= hns5_sym_session_clear,
};

static uint16_t
hns5_crypto_enqueue_burst(void *qp, struct rte_crypto_op **ops, uint16_t nb_ops)
{
	return hns5_enqueue_burst((struct hns5_qp *)qp, ops, nb_ops);
}

static uint16_t
hns5_crypto_dequeue_burst(void *qp, struct rte_crypto_op **ops, uint16_t nb_ops)
{
	return hns5_dequeue_burst((struct hns5_qp *)qp, ops, nb_ops);
}

int
hns5_crypto_probe(struct rte_vdev_device *vdev)
{
	struct rte_cryptodev_pmd_init_params init_params = {
		.name = "",
		.private_data_size = sizeof(struct hns5_dev_private),
		.max_nb_queue_pairs = 1,
	};
	struct hns5_dev_private *priv;
	struct rte_cryptodev *dev;
	const char *name;
	const char *pool = NULL;
	int rc;

	name = rte_vdev_device_name(vdev);
	if (name == NULL)
		return -EINVAL;

	dev = rte_cryptodev_pmd_create(name, &vdev->device, &init_params);
	if (dev == NULL) {
		HNS5_LOG(ERR, "cryptodev create failed for %s", name);
		return -ENODEV;
	}

	dev->dev_ops = &hns5_crypto_pmd_ops;
	dev->driver_id = hns5_cryptodev_driver_id;
	dev->enqueue_burst = hns5_crypto_enqueue_burst;
	dev->dequeue_burst = hns5_crypto_dequeue_burst;
	dev->feature_flags = RTE_CRYPTODEV_FF_HW_ACCELERATED |
			     RTE_CRYPTODEV_FF_SYMMETRIC_CRYPTO |
			     RTE_CRYPTODEV_FF_SYM_OPERATION_CHAINING |
			     RTE_CRYPTODEV_FF_IN_PLACE_SGL |
			     RTE_CRYPTODEV_FF_OOP_SGL_IN_LB_OUT;

	priv = dev->data->dev_private;
	memset(priv, 0, sizeof(*priv));
	priv->sec.sec_pf = -1;
	priv->sec.sec_vf = -1;
	priv->sec.poe_pf = -1;
	priv->sec.poe_vf = -1;

	rc = hns5_runtime_acquire(&priv->sec.common, pool);
	if (rc) {
		HNS5_LOG(ERR, "HugeTLB/BMU runtime acquire failed: %d", rc);
		goto destroy;
	}
	priv->common_runtime_acquired = true;

	rc = hns5_sec_platform_inspect(&priv->sec);
	if (rc) {
		HNS5_LOG(ERR, "SEC platform inspect failed: %d", rc);
		goto release_runtime;
	}

	rc = hns5_sec_platform_start(&priv->sec);
	if (rc) {
		HNS5_LOG(ERR, "SEC platform start failed: %d", rc);
		goto release_runtime;
	}

	priv->max_qps = 1;
	priv->qps = rte_calloc("hns5 qps", priv->max_qps, sizeof(struct hns5_qp *), 0);
	if (priv->qps == NULL) {
		rc = -ENOMEM;
		goto close_platform;
	}

	rte_cryptodev_pmd_probing_finish(dev);
	return 0;

close_platform:
	hns5_sec_platform_close(&priv->sec);
release_runtime:
	hns5_runtime_release(priv->sec.common);
destroy:
	rte_cryptodev_pmd_destroy(dev);
	return rc;
}

int
hns5_crypto_remove(struct rte_vdev_device *vdev)
{
	struct rte_cryptodev *dev;
	const char *name;

	if (vdev == NULL)
		return -EINVAL;
	name = rte_vdev_device_name(vdev);
	dev = rte_cryptodev_pmd_get_named_dev(name);
	if (dev == NULL)
		return -ENODEV;

	rte_cryptodev_pmd_destroy(dev);
	return 0;
}

static struct rte_vdev_driver hns5_crypto_vdev_drv = {
	.probe = hns5_crypto_probe,
	.remove = hns5_crypto_remove,
};

static struct cryptodev_driver hns5_crypto_drv;

RTE_PMD_REGISTER_VDEV(HNS5_CRYPTO_DRIVER_NAME, hns5_crypto_vdev_drv);
RTE_PMD_REGISTER_CRYPTO_DRIVER(hns5_crypto_drv, hns5_crypto_vdev_drv.driver,
			       hns5_cryptodev_driver_id);
RTE_PMD_REGISTER_PARAM_STRING(HNS5_CRYPTO_DRIVER_NAME, "");
