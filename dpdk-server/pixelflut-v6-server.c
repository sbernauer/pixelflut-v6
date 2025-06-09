#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <argp.h>

#include <rte_common.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_launch.h>
#include <rte_cycles.h>

#include "framebuffer.h"
#include "stats.h"

#define MAX_PORTS 32
#define MAX_CORES 128
#define MAX_CORES_PER_PORT 16
#define MAX_QUEUES_PER_CORE 64

#define NUM_RX_DESC 1024
#define BURST_SIZE 32
#define NUM_MBUFS 8192
#define MBUF_CACHE_SIZE 256

// pingxelflut protocol constants
#define MSG_SIZE_REQUEST 0xaa
#define MSG_SIZE_RESPONSE 0xbb
#define MSG_SET_PIXEL 0xcc

static struct argp_option options[] = {
    {"width",  'w', "pixels", 0,  "Width of the drawing surface in pixels (default 1920)" },
    {"height", 'h', "pixels", 0,  "Height of the drawing surface in pixels (default 1080)"},
    {"shared-memory-name", 's', "name", 0, "Name of the shared memory. Usually it will be created at /dev/shm/<name> (default pixelflut)"},
    {"port-core-mapping", 'c', "mapping", 0, "Mapping of NIC ports to CPU cores. Format is '<port1>:<core1> <port2>:<core2>,<core3>', e.g. '0:1' or '0:1,2,3,4 1:5,6,7,8'"},
    {0}
};

struct arguments {
    uint16_t width;
    uint16_t height;
    char* shared_memory_name;
    char* port_core_mapping;
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
        case 'c':
            arguments->port_core_mapping = arg;
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

struct port_config {
    uint16_t port_id;
    uint16_t nb_queues;
    uint16_t cores[MAX_CORES_PER_PORT];
};

struct core_work {
    uint16_t count;
    struct {
        uint16_t port;
        uint16_t queue;
    } tasks[MAX_QUEUES_PER_CORE];
    struct framebuffer* fb;
};

static struct port_config ports[MAX_PORTS];
static struct core_work core_tasks[MAX_CORES];
static uint16_t total_ports = 0;
static uint16_t mapped_ports = 0;

static struct rte_mempool *mbuf_pool;
static uint64_t rx_counters[MAX_PORTS][MAX_CORES_PER_PORT];

static void parse_port_core_map(const char *arg) {
    char *copy = strdup(arg);
    char *saveptr1 = NULL;
    char *token = strtok_r(copy, " ", &saveptr1);

    while (token) {
        int port;
        if (sscanf(token, "%d:", &port) != 1 || port >= total_ports) {
            rte_exit(EXIT_FAILURE, "Invalid port spec '%s' for port %d. Valid range of ports: 0..%d\n", token, port, total_ports - 1);
        }

        struct port_config *p = &ports[port];
        if (p->nb_queues > 0)
            rte_exit(EXIT_FAILURE, "Duplicate mapping for port %d\n", port);

        char *cores = strchr(token, ':');
        if (!cores || *(++cores) == '\0') {
            rte_exit(EXIT_FAILURE, "No cores specified for port %d\n", port);
        }

        char *saveptr2 = NULL;
        char *ctok = strtok_r(cores, ",", &saveptr2);
        while (ctok) {
            int core = atoi(ctok);
            if (p->nb_queues >= MAX_CORES_PER_PORT)
                rte_exit(EXIT_FAILURE, "Too many cores for port %d\n", port);

            if (core == 0)
                rte_exit(EXIT_FAILURE, "Im sorry, but core 0 is reserved for the main (stats) loop, use a different one");

            p->cores[p->nb_queues++] = core;
            ctok = strtok_r(NULL, ",", &saveptr2);
        }

        mapped_ports++;
        token = strtok_r(NULL, " ", &saveptr1);
    }

    free(copy);
}

static void check_and_enable_lcores(void) {
    for (uint16_t p = 0; p < MAX_PORTS; p++) {
        for (uint16_t q = 0; q < ports[p].nb_queues; q++) {
            uint16_t core = ports[p].cores[q];
            if (!rte_lcore_is_enabled(core)) {
                rte_exit(EXIT_FAILURE, "Core %u is not enabled (used for port %u)\n", core, p);
            }
        }
    }
}

static void build_core_task_map(void) {
    for (uint16_t p = 0; p < MAX_PORTS; p++) {
        for (uint16_t q = 0; q < ports[p].nb_queues; q++) {
            uint16_t core = ports[p].cores[q];
            struct core_work *cw = &core_tasks[core];
            if (cw->count >= MAX_QUEUES_PER_CORE) {
                rte_exit(EXIT_FAILURE, "Core %u assigned too many queues\n", core);
            }
            cw->tasks[cw->count].port = p;
            cw->tasks[cw->count].queue = q;
            cw->count++;
        }
    }
}

static void print_assignment(void) {
    printf("\nDPDK Port/Core Assignment:\n");
    printf("+--------+----------+--------+\n");
    printf("| PortID | Queue ID | CoreID |\n");
    printf("+--------+----------+--------+\n");
    for (uint16_t p = 0; p < MAX_PORTS; p++) {
        for (uint16_t q = 0; q < ports[p].nb_queues; q++) {
            printf("|  %4u  |   %4u   |  %4u  |\n", p, q, ports[p].cores[q]);
        }
    }
    printf("+--------+----------+--------+\n\n");
}

int disable_pause_frames(uint16_t port_id) {
    struct rte_eth_fc_conf fc_conf;

    memset(&fc_conf, 0, sizeof(fc_conf));

    // rte_eth_fc_mode enum:
    // RTE_ETH_FC_NONE      Disable flow control.
    // RTE_ETH_FC_RX_PAUSE  Rx pause frame, enable flowctrl on Tx side.
    // RTE_ETH_FC_TX_PAUSE  Tx pause frame, enable flowctrl on Rx side.
    // RTE_ETH_FC_FULL      Enable flow control on both sides.
    fc_conf.mode = RTE_ETH_FC_NONE;

    int ret = rte_eth_dev_flow_ctrl_set(port_id, &fc_conf);
    if (ret < 0) {
        printf("Failed to disable flow control on port %u: %s\n", port_id, rte_strerror(-ret));
        return ret;
    }

    printf("Flow control (pause frames) disabled on port %u\n", port_id);
    return 0;
}

static void init_port(uint16_t port_id) {
    struct port_config *cfg = &ports[port_id];
    if (cfg->nb_queues == 0) return;

    struct rte_eth_conf port_conf = { 0 };

    if (rte_eth_dev_configure(port_id, cfg->nb_queues, 0, &port_conf) < 0)
        rte_exit(EXIT_FAILURE, "Port %u configure failed\n", port_id);

    for (uint16_t q = 0; q < cfg->nb_queues; q++) {
        if (rte_eth_rx_queue_setup(port_id, q, NUM_RX_DESC,
            rte_eth_dev_socket_id(port_id), NULL, mbuf_pool) < 0) {
            rte_exit(EXIT_FAILURE, "RX queue setup failed for port %u, queue %u\n", port_id, q);
        }
    }

    if (rte_eth_dev_start(port_id) < 0)
        rte_exit(EXIT_FAILURE, "Port %u start failed\n", port_id);

    rte_eth_promiscuous_enable(port_id);
    disable_pause_frames(port_id);
}

static int lcore_main(void *arg) {
    uint16_t core_id = rte_lcore_id();
    struct core_work *core_work = &core_tasks[core_id];
    struct framebuffer* fb = core_work->fb;

    printf("[DEBUG] Core %d will handle %d queues\n", core_id, core_work->count);

    // Check that the port is on the same NUMA node as the polling thread for best performance
    uint16_t port;
    RTE_ETH_FOREACH_DEV(port)
        if (rte_eth_dev_socket_id(port) >= 0 && rte_eth_dev_socket_id(port) != (int)rte_socket_id())
            printf("WARNING, port %u is on remote NUMA node to polling thread.\n"
                "\tPerformance will not be optimal.\n", port);

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

    while (1) {
        for (uint16_t i = 0; i < core_work->count; i++) {
            uint16_t port = core_work->tasks[i].port;
            uint16_t queue = core_work->tasks[i].queue;
            uint16_t nb_rx = rte_eth_rx_burst(port, queue, pkt, BURST_SIZE);
            rx_counters[port][queue] += nb_rx;

            for (uint16_t i = 0; i < nb_rx; i++) {
                eth_hdr = rte_pktmbuf_mtod(pkt[i], struct rte_ether_hdr *);

                // Let's handle pixelflut v6 traffic first, I assume that is a bit more performance-focused
                if (eth_hdr->ether_type == htons(RTE_ETHER_TYPE_IPV6)) {
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
                        // printf("[DEBUG] x: %d, y: %d, rgba: %08x\n", x, y, rgba);

                        fb_set(fb, x, y, rgba);
                    }
                } else if (eth_hdr->ether_type == htons(RTE_ETHER_TYPE_IPV4)) {
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
                }

                rte_pktmbuf_free(pkt[i]);
            }
        }
    }
    return 0;
}

static int stats_loop(__rte_unused void *arg) {
    while (1) {
        printf("\n[RX Stats]\n");
        for (uint16_t p = 0; p < MAX_PORTS; p++) {
            for (uint16_t q = 0; q < ports[p].nb_queues; q++) {
                printf("Port %u Queue %u: %lu pkts\n", p, q, rx_counters[p][q]);
            }
        }
        fflush(stdout);
        sleep(2);
    }
    return 0;
}

int main(int argc, char **argv) {
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "EAL init failed\n");

    argc -= ret;
    argv += ret;

    total_ports = rte_eth_dev_count_avail();
    if (total_ports == 0)
        rte_exit(EXIT_FAILURE, "No Ethernet ports found\n");

    // Parse command arguments (after the EAL ones)
    struct arguments arguments = {0};
    arguments.width = 1920;
    arguments.height = 1080;
    arguments.shared_memory_name = "/pixelflut";
    arguments.port_core_mapping = "";
    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    parse_port_core_map(arguments.port_core_mapping);
    if (mapped_ports == 0)
        rte_exit(EXIT_FAILURE, "No port mappings provided, use --port-core-mapping for that. See --help for details\n");

    // Create framebuffer
    struct framebuffer* fb;
    ret = create_fb(&fb, arguments.width, arguments.height, arguments.shared_memory_name);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Failed to allocate framebuffer\n");

    check_and_enable_lcores();
    build_core_task_map();
    print_assignment();

    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * rte_lcore_count(),
                                        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!mbuf_pool)
        rte_exit(EXIT_FAILURE, "mbuf_pool create failed\n");

    for (uint16_t p = 0; p < total_ports; p++)
        init_port(p);

    unsigned int core_id;
    RTE_LCORE_FOREACH_WORKER(core_id) {
        if (core_tasks[core_id].count > 0) {
            core_tasks[core_id].fb = fb;
            rte_eal_remote_launch(lcore_main, NULL, core_id);
        }
    }

    stats_loop(NULL);
    rte_eal_mp_wait_lcore();
    return 0;
}
