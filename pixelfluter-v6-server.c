#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <locale.h>
#include <sys/time.h>
#include <argp.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

#define STATS_INTERVAL_MS 1000

static struct argp_option options[] = {
    {"width",  'w', "pixels", 0,  "Width of the drawing surface in pixels"  },
    {"height", 'h', "pixels", 0,  "Height of the drawing surface in pixels" },
};
struct arguments {
    int width;
    int height;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
  // Get the input argument from argp_parse, which we know is a pointer to our arguments structure
  struct arguments *arguments = state->input;

  switch (key)
    {
    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

const char *argp_program_version = "pixelfluter-v6-server 0.1.0";
static char doc[] = "Fast pixelflut v6 or pingxelflut server using DPDK";
static char args_doc[] = "";
static struct argp argp = { options, parse_opt, args_doc, doc };

// Main functional part of port initialization
static inline int port_init(uint16_t port, struct rte_mempool *mbuf_pool) {
    struct rte_eth_conf port_conf;
    const uint16_t rx_rings = 1, tx_rings = 1;
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    int retval;
    uint16_t q;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_txconf txconf;

    if (!rte_eth_dev_is_valid_port(port))
        return -1;

    memset(&port_conf, 0, sizeof(struct rte_eth_conf));

    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0) {
        printf("Error during getting device (port %u) info: %s\n",
                port, strerror(-retval));
        return retval;
    }

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        port_conf.txmode.offloads |=
            RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    // Configure the Ethernet device
    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0)
        return retval;

    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0)
        return retval;

    // Allocate and set up 1 RX queue per Ethernet port
    for (q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
                rte_eth_dev_socket_id(port), NULL, mbuf_pool);
        if (retval < 0)
            return retval;
    }

    txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;
    // Allocate and set up 1 TX queue per Ethernet port
    for (q = 0; q < tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(port, q, nb_txd,
                rte_eth_dev_socket_id(port), &txconf);
        if (retval < 0)
            return retval;
    }

    // Starting Ethernet port
    retval = rte_eth_dev_start(port);
    if (retval < 0)
        return retval;

    // Display the port MAC address
    struct rte_ether_addr addr;
    retval = rte_eth_macaddr_get(port, &addr);
    if (retval != 0)
        return retval;

    printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
               " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
            port, RTE_ETHER_ADDR_BYTES(&addr));

    // Enable RX in promiscuous mode for the Ethernet device
    retval = rte_eth_promiscuous_enable(port);
    if (retval != 0)
        return retval;

    return 0;
}

struct main_thread_args {
    struct rte_mempool *mbuf_pool;
    int port_id;
};

 /* Basic forwarding application lcore. 8< */
static __rte_noreturn void lcore_main(struct main_thread_args *args) {
    // Read args
    struct rte_mempool *mbuf_pool = args->mbuf_pool;
    int port_id = args->port_id;

    uint16_t port, nb_tx;

    uint32_t stats_loop_counter = 0;
    struct timeval last_stats_report, now;
    long elapsed_millis;

    gettimeofday(&last_stats_report, NULL);

    // Check that the port is on the same NUMA node as the polling thread for best performance
    RTE_ETH_FOREACH_DEV(port)
        if (rte_eth_dev_socket_id(port) >= 0 && rte_eth_dev_socket_id(port) != (int)rte_socket_id())
            printf("WARNING, port %u is on remote NUMA node to polling thread.\n"
                "\tPerformance will not be optimal.\n", port);

    for (;;) { }

    // Closing and releasing resources
    // rte_flow_flush(port_id, &error);
    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);
}

// The main function, which does initialization and calls the per-lcore functions
int main(int argc, char *argv[]) {
    // Initializion the Environment Abstraction Layer (EAL)
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

    argc -= ret;
    argv += ret;

    // Parse command arguments (after the EAL ones)
    struct arguments arguments = {0};
    arguments.width = 1920;
    arguments.height = 1080;
    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    int err = 0;

    struct rte_mempool *mbuf_pool;
    unsigned nb_ports;
    uint16_t portid;

    nb_ports = rte_eth_dev_count_avail();
    printf("Detected %u ports\n", nb_ports);
    if (nb_ports != 1)
        rte_exit(EXIT_FAILURE, "Error: currently only a single port is supported, you have %d ports\n", nb_ports);

    // Allocates mempool to hold the mbufs
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    // Initializing all ports
    RTE_ETH_FOREACH_DEV(portid)
        if (port_init(portid, mbuf_pool) != 0)
            rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n",
                    portid);

    if (rte_lcore_count() > 1)
        printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");

    // Call lcore_main on the main core only. Called on single lcore
    struct main_thread_args args;
    args.mbuf_pool = mbuf_pool;
    // FIXME: Currently this only works with a single port
    args.port_id = 0;

    lcore_main(&args);

    // Clean up the EAL
    rte_eal_cleanup();

    return 0;
}
