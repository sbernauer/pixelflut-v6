#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <locale.h>
#include <sys/time.h>
#include <argp.h>
#include <unistd.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#include "framebuffer.h"
#include "stats.h"

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

#define STATS_INTERVAL_MS 100

#define MSG_SIZE_REQUEST 0xaa
#define MSG_SIZE_RESPONSE 0xbb
#define MSG_SET_PIXEL 0xcc

static struct argp_option options[] = {
    {"width",  'w', "pixels", 0,  "Width of the drawing surface in pixels (default 1920)" },
    {"height", 'h', "pixels", 0,  "Height of the drawing surface in pixels (default 1080)"},
    {"shared-memory-name", 's', "name", 0, "Name of the shared memory. Usually it will be created at /dev/shm/<name> (default pixelflut)"},
    {0}
};

struct arguments {
    uint16_t width;
    uint16_t height;
    char* shared_memory_name;
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
    case 's':
      arguments->shared_memory_name = arg;
      break;

    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

const char *argp_program_version = "pixelflut-v6-server 0.1.0";
static char doc[] = "Fast pixelflut v6 or pingxelflut server using DPDK";
static char args_doc[] = "";
static struct argp argp = { options, parse_opt, args_doc, doc };

int find_free_stats_slot(struct framebuffer* fb, struct rte_ether_addr* mac_addr) {
    for (int slot = 0; slot < MAX_PORTS; slot++) {
        if (rte_is_same_ether_addr(&fb->port_stats[slot].mac_addr, mac_addr)) {
            printf("Found slot %d with my MAC address, using that\n", slot);
            return slot;
        }

        if (rte_is_zero_ether_addr(&fb->port_stats[slot].mac_addr)) {
            // All full slots have been checked before, so we can now assume this mac address does not have a slot yet
            printf("Found empty slot %d, using that\n", slot);
            rte_ether_addr_copy(mac_addr, &fb->port_stats[slot].mac_addr);
            return slot;
        }
    }

    // No free slot found
    return -1;
}

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
static void lcore_main(struct main_thread_args *args) {
    // Read args
    struct rte_mempool *mbuf_pool = args->mbuf_pool;
    int port_id = args->port_id;
    struct framebuffer* fb = args->fb;

    int err;
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

    struct rte_ether_addr mac_addr;
    if((err = rte_eth_macaddr_get(port_id, &mac_addr))) {
		fprintf(stderr, "Failed to read MAC address of port %d: %s\n", port_id, strerror(err));
        return;
	}
    printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8" %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
        port_id, RTE_ETHER_ADDR_BYTES(&mac_addr));

    int stat_slot = find_free_stats_slot(fb, &mac_addr);
    if(stat_slot == -1) {
		fprintf(stderr, "Failed to find free statistics slot. You compiled with support for maximum %d ports, \
            please increase MAX_PORTS\n", MAX_PORTS);
        return;
	}
    struct rte_eth_stats* eth_stats = &fb->port_stats[stat_slot].stats;

    bool was_pingxelflut;
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ipv4_hdr;
    struct rte_ipv6_hdr *ipv6_hdr;
    struct rte_icmp_hdr *icmp_hdr;

    uint8_t msg_kind;
    uint16_t x, y;
    uint32_t rgba, icmp_payload_len;

    struct rte_mbuf * pkt[BURST_SIZE];
    int i;
    for (;;) {
        nb_rx = rte_eth_rx_burst(port_id, /* queue_id */ 0, pkt, BURST_SIZE);
        for (i = 0; i < nb_rx; i++) {
            eth_hdr = rte_pktmbuf_mtod(pkt[i], struct rte_ether_hdr *);

            if (eth_hdr->ether_type == htons(RTE_ETHER_TYPE_IPV4)) {
                ipv4_hdr = rte_pktmbuf_mtod_offset(pkt[i], struct rte_ipv4_hdr*, sizeof(struct rte_ether_hdr));
                if (ipv4_hdr->next_proto_id == IPPROTO_ICMP) {
                    icmp_hdr = rte_pktmbuf_mtod_offset(pkt[i], struct rte_icmp_hdr*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
                    if (icmp_hdr->icmp_type == RTE_IP_ICMP_ECHO_REQUEST && icmp_hdr->icmp_code == 0) {
                        msg_kind = *rte_pktmbuf_mtod_offset(pkt[i], uint8_t*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_icmp_hdr));
                        if (msg_kind == MSG_SET_PIXEL) {
                            x = ntohs(*rte_pktmbuf_mtod_offset(pkt[i], uint16_t*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_icmp_hdr) + 1));
                            y = ntohs(*rte_pktmbuf_mtod_offset(pkt[i], uint16_t*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_icmp_hdr) + 3));

                            icmp_payload_len = pkt[i]->pkt_len - sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv4_hdr) - sizeof(struct rte_icmp_hdr);
                            // Packet is only sending rgb
                            if (icmp_payload_len == 8) {
                                rgba = *rte_pktmbuf_mtod_offset(pkt[i], uint32_t*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_icmp_hdr) + 5);
                                fb_set(fb, x, y, rgba);
                            // Packet is sending rgba
                            } else if (icmp_payload_len == 9) {
                                // TODO: Implement alpha in SET_PIXEL command
                            }
                        }
                    }
                }
            } else if (eth_hdr->ether_type == htons(RTE_ETHER_TYPE_IPV6)) {
                ipv6_hdr = rte_pktmbuf_mtod_offset(pkt[i], struct rte_ipv6_hdr*, sizeof(struct rte_ether_hdr));

                // As we support both (pingxelflut (ICMP) and pixelflut v6 traffic, we first detect if it's pingxelflut
                // and only use pixelflut v6 in case it is not)
                was_pingxelflut = false;

                if (ipv6_hdr->proto == 58 /* ICMPv6 */) {
                    icmp_hdr = rte_pktmbuf_mtod_offset(pkt[i], struct rte_icmp_hdr*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv6_hdr));
                    // Note: In older(?) DPDK versions the constant was called RTE_IP_ICMP_ECHO_REQUEST
                    if (icmp_hdr->icmp_type == RTE_ICMP6_ECHO_REQUEST && icmp_hdr->icmp_code == 0) {
                        msg_kind = *rte_pktmbuf_mtod_offset(pkt[i], uint8_t*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv6_hdr) + sizeof(struct rte_icmp_hdr));
                        if (msg_kind == MSG_SET_PIXEL) {
                            was_pingxelflut = true;

                            x = ntohs(*rte_pktmbuf_mtod_offset(pkt[i], uint16_t*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv6_hdr) + sizeof(struct rte_icmp_hdr) + 1));
                            y = ntohs(*rte_pktmbuf_mtod_offset(pkt[i], uint16_t*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv6_hdr) + sizeof(struct rte_icmp_hdr) + 3));

                            icmp_payload_len = pkt[i]->pkt_len - sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv6_hdr) - sizeof(struct rte_icmp_hdr);
                            // Packet is only sending rgb
                            if (icmp_payload_len == 8) {
                                rgba = *rte_pktmbuf_mtod_offset(pkt[i], uint32_t*, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv6_hdr) + sizeof(struct rte_icmp_hdr) + 5);
                                fb_set(fb, x, y, rgba);
                            // Packet is sending rgba
                            } else if (icmp_payload_len == 9) {
                                // TODO: Implement alpha in SET_PIXEL command
                            }
                        } else if (msg_kind == MSG_SIZE_REQUEST) {
                            was_pingxelflut = true;
                            // TODO: Implement reading of screen size flow
                        } else if (msg_kind == MSG_SIZE_RESPONSE) {
                            was_pingxelflut = true;
                        }
                    }
                }

                if (!was_pingxelflut) {
                    x = ((uint16_t)ipv6_hdr->dst_addr[8] << 8) | (uint16_t)ipv6_hdr->dst_addr[9];
                    y = ((uint16_t)ipv6_hdr->dst_addr[10] << 8) | (uint16_t)ipv6_hdr->dst_addr[11];
                    rgba = (uint32_t)ipv6_hdr->dst_addr[12] << 0 | (uint32_t)ipv6_hdr->dst_addr[13] << 8 | (uint32_t)ipv6_hdr->dst_addr[14] << 16;
                    // rgba = 0x00ff0000; // blue
                    // rgba = 0x0000ff00; // green
                    // rgba = 0x000000ff; // red
                    // printf("x: %d, y: %d, rgba: %08x\n", x, y, rgba);

                    fb_set(fb, x, y, rgba);
                }
            }

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

                rte_eth_stats_get(port_id, eth_stats);
                // Printing commented out to not flood screen. They are printed as a nice table by pixel-fluter instead.
                // setlocale(LC_NUMERIC, "");
                // printf("Total number of packets for port %u: send %'lu packets (%'lu bytes), "
                //     "received %'lu packets (%'lu bytes), dropped rx %'lu, ierrors %'lu, rx_nombuf %'lu, q_ipackets %'lu\n",
                //     port_id, eth_stats->opackets, eth_stats->obytes, eth_stats->ipackets, eth_stats->ibytes, eth_stats->imissed,
                //     eth_stats->ierrors, eth_stats->rx_nombuf, eth_stats->q_ipackets[0]);
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
    // Initialize the Environment Abstraction Layer (EAL)
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

    argc -= ret;
    argv += ret;

    // Parse command arguments (after the EAL ones)
    struct arguments arguments = {0};
    arguments.width = 1920;
    arguments.height = 1080;
    arguments.shared_memory_name = "/pixelflut";
    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    int err = 0;

    struct framebuffer* fb;
    if((err = create_fb(&fb, arguments.width, arguments.height, arguments.shared_memory_name))) {
		fprintf(stderr, "Failed to allocate framebuffer: %s\n", strerror(err));
        return err;
	}

    // uint32_t rgb;
    // srand(time(NULL));
    // while (true) {
    //     rgb = rand();
    //     for (int x = 0; x < arguments.width / 2; x++) {
    //         for (int y = 0; y < arguments.height / 1.5; y++) {
    //             fb_set(fb, x, y, rgb);
    //         }
    //     }
    //     printf("Painted random color\n");
    //     usleep(500000);
    // }

    struct rte_mempool *mbuf_pool;
    unsigned nb_ports;
    uint16_t port_id;

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
    RTE_ETH_FOREACH_DEV(port_id)
        if (port_init(port_id, mbuf_pool) != 0)
            rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n",
                    port_id);

    if (rte_lcore_count() > 1) {
        printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");
    }

    // Call lcore_main on the main core only. Called on single lcore
    struct main_thread_args args;
    args.mbuf_pool = mbuf_pool;
    // FIXME: Currently this only works with a single port
    args.port_id = 0;
    args.fb = fb;

    lcore_main(&args);

    // Clean up the EAL
    rte_eal_cleanup();

    // TODO: Close shared memory again

    return 0;
}
