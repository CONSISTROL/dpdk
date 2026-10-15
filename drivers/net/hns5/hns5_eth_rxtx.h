#ifndef _HNS5_ETH_RXTX_H_
#define _HNS5_ETH_RXTX_H_

#include <stdint.h>
#include <stdbool.h>

#include <rte_mbuf.h>

#include "hns5_eth.h"

/**
 * @attention Multiple cores are not allowed to receive packets from the same queue
 */
uint16_t __rte_hot hns5_eth_rx_burst(void *rxq, struct rte_mbuf **rx_pkts, uint16_t nb_pkts);
void hns5_eth_set_default_tx_msg(struct hns5_eth_tx_queue *txq);
/**
 * @attention Multiple cores are not allowed to transmit packets from the same queue
 */
uint16_t __rte_hot hns5_eth_tx_burst(void *txq, struct rte_mbuf **tx_pkts, uint16_t nb_pkts);
bool hns5_get_tm_init_status(void);

#endif /* _HNS5_ETH_RXTX_H_ */
