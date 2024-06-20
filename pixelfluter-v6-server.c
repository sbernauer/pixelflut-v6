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

#include "framebuffer.h"

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

#define STATS_INTERVAL_MS 1000

static struct argp_option options[] = {
    {"width",  'w', "pixels", 0,  "Width of the drawing surface in pixels" },
    {"height", 'h', "pixels", 0,  "Height of the drawing surface in pixels"},
    {0}
};
struct arguments {
    uint16_t width;
    uint16_t height;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
  // Get the input argument from argp_parse, which we know is a pointer to our arguments structure
  struct arguments *arguments = state->input;

  switch (key)
    {
    case 'w':
      arguments->width = (uint16_t) strtol(arg, NULL, 10);
      break;
    case 'h':
      arguments->height = (uint16_t) strtol(arg, NULL, 10);
      break;

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
        printf("Error during getting device (port %u) info: %s\n", port, strerror(-retval));
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

    printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8" %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
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
    struct framebuffer* fb;
};

 /* Basic forwarding application lcore. 8< */
static __rte_noreturn void lcore_main(struct main_thread_args *args) {
    // Read args
    struct rte_mempool *mbuf_pool = args->mbuf_pool;
    int port_id = args->port_id;
    struct framebuffer* fb = args->fb;

    uint16_t port, nb_rx;

    uint32_t stats_loop_counter = 0;
    struct timeval last_stats_report, now;
    long elapsed_millis;

    gettimeofday(&last_stats_report, NULL);

    // Check that the port is on the same NUMA node as the polling thread for best performance
    RTE_ETH_FOREACH_DEV(port)
        if (rte_eth_dev_socket_id(port) >= 0 && rte_eth_dev_socket_id(port) != (int)rte_socket_id())
            printf("WARNING, port %u is on remote NUMA node to polling thread.\n"
                "\tPerformance will not be optimal.\n", port);

    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv6_hdr *ipv6_hdr;

    uint16_t x, y;
    uint32_t rgba;

    struct rte_mbuf * pkt[BURST_SIZE];
    int i;
    for (;;) {
        nb_rx = rte_eth_rx_burst(port_id, /* queue_id */ 0, pkt, BURST_SIZE);

        // printf("Received %u packets\n", nb_rx);
        // rte_delay_ms(100);
        for (i = 0; i < nb_rx; i++) {
            eth_hdr = rte_pktmbuf_mtod(pkt[i], struct rte_ether_hdr *);
            if (eth_hdr->ether_type == htons(RTE_ETHER_TYPE_IPV6)) {
                ipv6_hdr = rte_pktmbuf_mtod_offset(pkt[i], struct rte_ipv6_hdr*, sizeof(struct rte_ether_hdr));
                x = ((uint16_t)ipv6_hdr->dst_addr[8] << 8) + (uint16_t)ipv6_hdr->dst_addr[9];
                y = ((uint16_t)ipv6_hdr->dst_addr[10] << 8) + (uint16_t)ipv6_hdr->dst_addr[11];
                rgba = ((uint32_t)ipv6_hdr->dst_addr[12] << 24) + ((uint32_t)ipv6_hdr->dst_addr[13] << 16) + ((uint32_t)ipv6_hdr->dst_addr[14] << 8);
            }

            // printf("Got packet with (%u, %u) and rgba %010x\n", x, y, rgba);
            fb_set(fb, x, y, rgba);

            rte_pktmbuf_free(pkt[i]);
        }

        // I assume reading the system time is a expensive operation, so let's not do that every loop...
        stats_loop_counter++;
        if (unlikely(stats_loop_counter > 10000)) {
            stats_loop_counter = 0;

            gettimeofday(&now, NULL);
            elapsed_millis = (now.tv_sec - last_stats_report.tv_sec) * 1000.0;
            elapsed_millis += (now.tv_usec - last_stats_report.tv_usec) / 1000.0;

            if (elapsed_millis >= STATS_INTERVAL_MS) {
                last_stats_report = now;

                struct rte_eth_stats eth_stats;
                rte_eth_stats_get(port_id, &eth_stats);
                setlocale(LC_NUMERIC, "");
                printf("Total number of packets for port %u: send %'lu packets (%'lu bytes), "
                    "received %'lu packets (%'lu bytes), dropped rx %'lu, ierrors %'lu, rx_nombuf %'lu, q_ipackets %'lu\n",
                    port_id, eth_stats.opackets, eth_stats.obytes, eth_stats.ipackets, eth_stats.ibytes, eth_stats.imissed,
                    eth_stats.ierrors, eth_stats.rx_nombuf, eth_stats.q_ipackets[0]);
            }
        }
    }

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

    struct framebuffer* fb;
    if((err = fb_alloc(&fb, arguments.width, arguments.height))) {
		fprintf(stderr, "Failed to allocate framebuffer: %s\n", strerror(-err));
        return err;
	}

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
    args.fb = fb;

    lcore_main(&args);

    // Clean up the EAL
    rte_eal_cleanup();

    return 0;
}
