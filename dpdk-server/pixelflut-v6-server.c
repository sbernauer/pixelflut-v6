#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include <rte_common.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_launch.h>
#include <rte_cycles.h>

#define MAX_PORTS 32
#define MAX_CORES 128
#define MAX_CORES_PER_PORT 16
#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32
#define MAX_QUEUES_PER_CORE 64

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

static void init_port(uint16_t port_id) {
    struct port_config *cfg = &ports[port_id];
    if (cfg->nb_queues == 0) return;

    struct rte_eth_conf port_conf = { 0 };

    if (rte_eth_dev_configure(port_id, cfg->nb_queues, 0, &port_conf) < 0)
        rte_exit(EXIT_FAILURE, "Port %u configure failed\n", port_id);

    for (uint16_t q = 0; q < cfg->nb_queues; q++) {
        if (rte_eth_rx_queue_setup(port_id, q, 1024,
            rte_eth_dev_socket_id(port_id), NULL, mbuf_pool) < 0) {
            rte_exit(EXIT_FAILURE, "RX queue setup failed for port %u, queue %u\n", port_id, q);
        }
    }

    if (rte_eth_dev_start(port_id) < 0)
        rte_exit(EXIT_FAILURE, "Port %u start failed\n", port_id);

    rte_eth_promiscuous_enable(port_id);
}

static int lcore_main(void *arg) {
    uint16_t core_id = rte_lcore_id();
    struct core_work *cw = &core_tasks[core_id];
    struct rte_mbuf *bufs[BURST_SIZE];

    printf("[DEBUG] Core %d will handle %d queues\n", core_id, cw->count);

    while (1) {
        for (uint16_t i = 0; i < cw->count; i++) {
            uint16_t port = cw->tasks[i].port;
            uint16_t queue = cw->tasks[i].queue;
            uint16_t nb_rx = rte_eth_rx_burst(port, queue, bufs, BURST_SIZE);
            rx_counters[port][queue] += nb_rx;
            for (uint16_t j = 0; j < nb_rx; j++) {
                rte_pktmbuf_free(bufs[j]);
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
        sleep(1);
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

    const struct option longopts[] = {
        { "port-core-mapping", required_argument, NULL, 'p' }, {0,0,0,0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:", longopts, NULL)) != -1) {
        if (opt == 'p') {
            parse_port_core_map(optarg);
        } else {
            rte_exit(EXIT_FAILURE, "Unknown option\n");
        }
    }

    if (mapped_ports == 0)
        rte_exit(EXIT_FAILURE, "No port mappings provided\n");

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
            rte_eal_remote_launch(lcore_main, NULL, core_id);
        }
    }

    stats_loop(NULL);
    rte_eal_mp_wait_lcore();
    return 0;
}
