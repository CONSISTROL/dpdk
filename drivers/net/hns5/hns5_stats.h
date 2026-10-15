#ifndef _HNS5_STATS_H_
#define _HNS5_STATS_H_

#include <rte_ethdev.h>
#include <rte_common.h>
#include "ndrv_mag_api.h"

int hns5_eth_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats);
int hns5_eth_xstats_get(struct rte_eth_dev *dev, struct rte_eth_xstat *xstats, unsigned n);
int hns5_eth_xstats_get_names(__rte_unused struct rte_eth_dev *dev,
                                                struct rte_eth_xstat_name *xstats_names,
                                                __rte_unused unsigned size);
int hns5_eth_stats_reset(struct rte_eth_dev *dev);
int hns5_eth_xstats_reset(struct rte_eth_dev *dev);

#endif /* _HNS5_STATS_H_ */
