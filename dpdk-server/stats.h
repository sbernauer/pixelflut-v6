#ifndef _STATS_H_
#define _STATS_H_

#include <rte_ethdev.h>

struct port_stats {
    struct rte_ether_addr mac_addr;
    struct rte_eth_stats stats;
};

#endif
