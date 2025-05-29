#include <sys/time.h>
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <argp.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>

#include "framebuffer.h"
#include "stats.h"

#define NUM_RX_QUEUES 2
#define NUM_RX_DESC 1024
#define BURST_SIZE 32
#define MEMPOOL_CACHE_SIZE 256

#define STATS_INTERVAL_MS 250

// pingxelflut protocol constants
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

static volatile bool force_quit = false;
static uint64_t packet_counts[NUM_RX_QUEUES] = {0};

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        force_quit = true;
    }
}

struct lcore_params {
    uint16_t port_id;
    uint16_t queue_id;
    struct framebuffer* fb;
    int stats_slot;
    bool update_stats;
};

static int lcore_rx_loop(void *arg) {
    struct lcore_params *params = (struct lcore_params *)arg;
    const uint16_t port_id = params->port_id;
    const uint16_t queue_id = params->queue_id;
    struct framebuffer* fb = params->fb;
    int stats_slot = params->stats_slot;
    bool update_stats = params->update_stats;

    // Check that the port is on the same NUMA node as the polling thread for best performance
    uint16_t port;
    RTE_ETH_FOREACH_DEV(port)
        if (rte_eth_dev_socket_id(port) >= 0 && rte_eth_dev_socket_id(port) != (int)rte_socket_id())
            printf("WARNING, port %u is on remote NUMA node to polling thread.\n"
                "\tPerformance will not be optimal.\n", port);

    // Statistics stuff
    struct rte_eth_stats* eth_stats = &fb->port_stats[stats_slot].stats;

    uint64_t hz = rte_get_timer_hz(); // Timer frequency in Hz
    uint64_t interval = hz / 1000 * STATS_INTERVAL_MS; // 100 ms (or whatever) in timer cycles
    uint64_t last_stats_report;

    // Actual packet processing starts
    struct rte_mbuf *pkt[BURST_SIZE];

    bool was_pingxelflut;
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ipv4_hdr;
    struct rte_ipv6_hdr *ipv6_hdr;
    struct rte_icmp_hdr *icmp_hdr;

    uint8_t msg_kind;
    uint16_t x, y;
    uint32_t rgba, icmp_payload_len;

    while (!force_quit) {
        uint16_t nb_rx = rte_eth_rx_burst(port_id, queue_id, pkt, BURST_SIZE);
        packet_counts[queue_id] += nb_rx;

        for (uint16_t i = 0; i < nb_rx; i++) {
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
                    // Note: In older(?) DPDK versions the constant was called RTE_ICMP6_ECHO_REQUEST
                    if (icmp_hdr->icmp_type == RTE_IP_ICMP_ECHO_REQUEST && icmp_hdr->icmp_code == 0) {
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

        uint64_t now = rte_get_timer_cycles();
        if ((now - last_stats_report) >= interval) {
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

    return 0;
}

static int stats_loop(__rte_unused void *arg) {
    while (!force_quit) {
        sleep(5);
        printf("Queue counters: ");
        for (int i = 0; i < NUM_RX_QUEUES; i++) {
            printf("%ld ", packet_counts[i]);
        }
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv) {
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "EAL init failed\n");

    argc -= ret;
    argv += ret;

    // Parse command arguments (after the EAL ones)
    struct arguments arguments = {0};
    arguments.width = 1920;
    arguments.height = 1080;
    arguments.shared_memory_name = "/pixelflut";
    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    // Determine port to use
    if (rte_eth_dev_count_avail() != 1)
        rte_exit(EXIT_FAILURE, "Exactly one port is required. Please start multiple pixelflut-v6-server instances if you have multiple ports. Or submit a PR to improve this :)\n");
    uint16_t port_id = 0;

    // Create framebuffer
    struct framebuffer* fb;
    ret = create_fb(&fb, arguments.width, arguments.height, arguments.shared_memory_name);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Failed to allocate framebuffer\n");

    // Statistic stuff
    struct rte_ether_addr mac_addr;
    ret = rte_eth_macaddr_get(port_id, &mac_addr);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Failed to read MAC address of port\n");

    printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8" %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n", port_id, RTE_ETHER_ADDR_BYTES(&mac_addr));

    int stats_slot = find_free_stats_slot(fb, &mac_addr);
    if (stats_slot == -1)
        rte_exit(EXIT_FAILURE, "Failed to find free statistics slot, please increase MAX_PORTS\n");

    // Create mbuf pool
    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL",
        8192, MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!mbuf_pool)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    // Port configuration
    // HASHING of all kinds of shit, all traffic ends up in queue 0
    // struct rte_eth_conf port_conf = {
    //     .rxmode = {
    //         .mq_mode = RTE_ETH_MQ_RX_RSS,
    //     },
    //     .rx_adv_conf.rss_conf = {
    //         .rss_key = NULL,
    //         .rss_hf = RTE_ETH_RSS_IPV4 |
    //                   RTE_ETH_RSS_NONFRAG_IPV4_TCP |
    //                   RTE_ETH_RSS_NONFRAG_IPV4_UDP,
    //     }
    // };

    // ChatGPT said "Intel NICs like the 82599 may then use round-robin among RX queues (if RSS is disabled and no Flow Director)."
    // Behavior is NIC/driver specific, not guaranteed
    // Well, turns out everything still lands in queue 0...
    // struct rte_eth_conf port_conf = {
    //     .rxmode = {
    //         .mq_mode = RTE_ETH_MQ_RX_NONE,
    //     }
    // };
    
    // As we mostly only care about IPv6 we don't need to think about IPv4 hashing.
    // My understanding is that this now hashes
    // 1. source IP
    // 2. destination IP
    // As the destination IP has a gigantic cardinality (Pixelflut v6 höhö), this should distribute
    // the traffic very evenly (regardless of the source IPs).
    struct rte_eth_conf port_conf = {
        .rxmode = {
            .mq_mode = RTE_ETH_MQ_RX_RSS,
        },
        .rx_adv_conf.rss_conf = {
            .rss_key = NULL,
            .rss_hf = RTE_ETH_RSS_IPV6
        }
    };

    ret = rte_eth_dev_configure(port_id, NUM_RX_QUEUES, 0, &port_conf);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Cannot configure port\n");

    // Setup RX queues
    for (int q = 0; q < NUM_RX_QUEUES; q++) {
        ret = rte_eth_rx_queue_setup(port_id, q, NUM_RX_DESC, rte_socket_id(), NULL, mbuf_pool);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "Cannot setup RX queue %d\n", q);
    }

    // Start device
    ret = rte_eth_dev_start(port_id);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Cannot start port\n");

    // Enable promiscuous mode
    rte_eth_promiscuous_enable(port_id);
    printf("Promiscuous mode enabled on port %u\n", port_id);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    struct lcore_params lcore_args[NUM_RX_QUEUES];
    unsigned main_lcore_idx = rte_get_main_lcore();
    unsigned lcore_idx = main_lcore_idx;

    // Launch queues 1..n (skip 0) on available workers
    for (int q = 1; q < NUM_RX_QUEUES; q++) {
        lcore_idx = rte_get_next_lcore(lcore_idx, 1, 0);
        if (lcore_idx == RTE_MAX_LCORE)
            rte_exit(EXIT_FAILURE, "Not enough lcores for queue %d, need more lcores!\n", q);

        lcore_args[q].port_id = port_id;
        lcore_args[q].queue_id = q;
        lcore_args[q].fb = fb;
        lcore_args[q].stats_slot = stats_slot;
        lcore_args[q].update_stats = false; // Only the main thread updates the stats

        printf("Core %u: Handles queue %u\n", lcore_idx, q);
        rte_eal_remote_launch(lcore_rx_loop, &lcore_args[q], lcore_idx);
    }

    lcore_idx = rte_get_next_lcore(lcore_idx, 1, 0);
    if (lcore_idx == RTE_MAX_LCORE)
        rte_exit(EXIT_FAILURE, "Not enough lcores for stats thread, need more lcores\n");

    printf("Core %u: Handles statistics loop\n", lcore_idx);
    rte_eal_remote_launch(stats_loop, NULL, lcore_idx);

    // Queue 0 handled by main thread
    lcore_args[0].port_id = port_id;
    lcore_args[0].queue_id = 0;
    lcore_args[0].fb = fb;
    lcore_args[0].stats_slot = stats_slot;
    lcore_args[0].update_stats = true; // Only the main thread updates the stats
    printf("Core %d: Handles queue 0\n", main_lcore_idx);
    lcore_rx_loop(&lcore_args[0]);

    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);
    rte_eal_cleanup();

    // TODO: Close shared memory again

    return 0;
}
