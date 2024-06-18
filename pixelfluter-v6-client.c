/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2015 Intel Corporation
 */

#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <locale.h>
#include <sys/time.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#include "image.h"

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

#define MAX(x, y) (((x) > (y)) ? (x) : (y))

#define STATS_INTERVAL_MS 1000

/* pixelfluter_v6.c: Pixelflut v6 client. */

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */

/* Main functional part of port initialization. 8< */
static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
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

    /* Configure the Ethernet device. */
    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0)
        return retval;

    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0)
        return retval;

    /* Allocate and set up 1 RX queue per Ethernet port. */
    for (q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
                rte_eth_dev_socket_id(port), NULL, mbuf_pool);
        if (retval < 0)
            return retval;
    }

    txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;
    /* Allocate and set up 1 TX queue per Ethernet port. */
    for (q = 0; q < tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(port, q, nb_txd,
                rte_eth_dev_socket_id(port), &txconf);
        if (retval < 0)
            return retval;
    }

    /* Starting Ethernet port. 8< */
    retval = rte_eth_dev_start(port);
    /* >8 End of starting of ethernet port. */
    if (retval < 0)
        return retval;

    /* Display the port MAC address. */
    struct rte_ether_addr addr;
    retval = rte_eth_macaddr_get(port, &addr);
    if (retval != 0)
        return retval;

    printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
               " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
            port, RTE_ETHER_ADDR_BYTES(&addr));

    /* Enable RX in promiscuous mode for the Ethernet device. */
    retval = rte_eth_promiscuous_enable(port);
    /* End of setting RX port in promiscuous mode. */
    if (retval != 0)
        return retval;

    return 0;
}
/* >8 End of main functional part of port initialization. */

/*
 * The lcore main. This is the main thread that does the work, reading from
 * an input port and writing to an output port.
 */

struct main_thread_args {
    struct fluter_image *fluter_image;
    struct rte_mempool *mbuf_pool;
    int port_id;
};

 /* Basic forwarding application lcore. 8< */
static __rte_noreturn void
lcore_main(struct main_thread_args *args)
{
    // Read args
    struct fluter_image *fluter_image = args->fluter_image;
    struct rte_mempool *mbuf_pool = args->mbuf_pool;
    int port_id = args->port_id;

    int width = fluter_image->width;
    int height = fluter_image->height;

    uint16_t port, nb_tx;

    uint32_t stats_loop_counter = 0;
    struct timeval last_stats_report, now;
    long elapsed_millis;

    gettimeofday(&last_stats_report, NULL);

    /*
     * Check that the port is on the same NUMA node as the polling thread
     * for best performance.
     */
    RTE_ETH_FOREACH_DEV(port)
        if (rte_eth_dev_socket_id(port) >= 0 && rte_eth_dev_socket_id(port) != (int)rte_socket_id())
            printf("WARNING, port %u is on remote NUMA node to polling thread.\n"
                "\tPerformance will not be optimal.\n", port);

    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv6_hdr *ipv6_hdr;
    uint16_t pkt_size = MAX(
        64, // The minimum packet size sent/received through Ethernet is always 64 bytes according to Ethernet specification
        sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv6_hdr) + 8 /* UDP payload */
    );

    struct rte_ether_addr dst_addr;
    dst_addr.addr_bytes[0] = 0x14;
    dst_addr.addr_bytes[1] = 0xa0;
    dst_addr.addr_bytes[2] = 0xf8;
    dst_addr.addr_bytes[3] = 0x8b;
    dst_addr.addr_bytes[4] = 0x1e;
    dst_addr.addr_bytes[5] = 0xe4;

    struct rte_ether_addr src_addr;
    src_addr.addr_bytes[0] = 0x14;
    src_addr.addr_bytes[1] = 0xa0;
    src_addr.addr_bytes[2] = 0xf8;
    src_addr.addr_bytes[3] = 0x8b;
    src_addr.addr_bytes[4] = 0x1e;
    src_addr.addr_bytes[5] = 0xe3;

    int x = 0;
    int y = 0;

    printf("\nCore %u sending %u byte packets on port %u. [Ctrl+C to quit]\n", rte_lcore_id(), pkt_size, port_id);

    struct rte_mbuf * pkt[BURST_SIZE];
    int i;
    for (;;) {
        for(i = 0; i < BURST_SIZE; i++) {
            pkt[i] = rte_pktmbuf_alloc(mbuf_pool);

            eth_hdr = rte_pktmbuf_mtod(pkt[i], struct rte_ether_hdr*);
            eth_hdr->dst_addr = dst_addr;
            eth_hdr->src_addr = src_addr;
            eth_hdr->ether_type = htons(RTE_ETHER_TYPE_IPV6);
            ipv6_hdr = rte_pktmbuf_mtod_offset(pkt[i], struct rte_ipv6_hdr*, sizeof(struct rte_ether_hdr));

            ipv6_hdr->vtc_flow = htonl(6 << 28); // IP version 6
            ipv6_hdr->hop_limits = 0xff;
            ipv6_hdr->proto = 0x11; // UDP
            ipv6_hdr->payload_len = htonl(8);

            ipv6_hdr->src_addr[0] = 0xfe;
            ipv6_hdr->src_addr[1] = 0x80;
            ipv6_hdr->src_addr[2] = 0;
            ipv6_hdr->src_addr[3] = 0;
            ipv6_hdr->src_addr[4] = 0;
            ipv6_hdr->src_addr[5] = 0;
            ipv6_hdr->src_addr[6] = 0;
            ipv6_hdr->src_addr[7] = 0;
            ipv6_hdr->src_addr[8] = 0;
            ipv6_hdr->src_addr[9] = 0;
            ipv6_hdr->src_addr[10] = 0;
            ipv6_hdr->src_addr[11] = 0;
            ipv6_hdr->src_addr[12] = 0;
            ipv6_hdr->src_addr[13] = 0;
            ipv6_hdr->src_addr[14] = 0;
            ipv6_hdr->src_addr[15] = 0x01;

            // Destination /64 IPv6 network
            ipv6_hdr->dst_addr[0] = 0xfe;
            ipv6_hdr->dst_addr[1] = 0x80;
            ipv6_hdr->dst_addr[2] = 0;
            ipv6_hdr->dst_addr[3] = 0;
            ipv6_hdr->dst_addr[4] = 0;
            ipv6_hdr->dst_addr[5] = 0;
            ipv6_hdr->dst_addr[6] = 0;
            ipv6_hdr->dst_addr[7] = 0;

            // X Coordinate
            ipv6_hdr->dst_addr[8] = x >> 8;
            ipv6_hdr->dst_addr[9] = x;

            // Y Coordinate
            ipv6_hdr->dst_addr[10] = y >> 8;
            ipv6_hdr->dst_addr[11] = y;

            // Color in rgb
            ipv6_hdr->dst_addr[12] = fluter_image->pixels[4 * (y * width + x) + 0];
            ipv6_hdr->dst_addr[13] = fluter_image->pixels[4 * (y * width + x) + 1];
            ipv6_hdr->dst_addr[14] = fluter_image->pixels[4 * (y * width + x) + 2];
            ipv6_hdr->dst_addr[15] = 0;

            // FIXME: Don't keep writing past the destination address, altough this should be well-defined behavior (?).
            // UDP Header
            ipv6_hdr->dst_addr[16] = 0; // Source port in UDP
            ipv6_hdr->dst_addr[17] = 13;
            ipv6_hdr->dst_addr[18] = 0; // Destination port in UDP
            ipv6_hdr->dst_addr[19] = 42;
            ipv6_hdr->dst_addr[20] = 0; // Length
            ipv6_hdr->dst_addr[21] = 0;
            ipv6_hdr->dst_addr[22] = 0; // Checksum (manditory at IPv6)
            ipv6_hdr->dst_addr[23] = 0;

            pkt[i]->data_len = pkt_size;
            pkt[i]->pkt_len = pkt_size;

            x++;
            if (x >= width) {
                x = 0;
                y++;
                if (y >= height) {
                    y = 0;
                }
            }
        }

        do {
            nb_tx = rte_eth_tx_burst(port_id, /* queue_id */ 0, pkt, BURST_SIZE);
            // printf("Send %u packets to port %u\n", nb_tx, port_id);
        } while(nb_tx == 0);


        if (unlikely(nb_tx < BURST_SIZE)) {
            printf("ERROR: Couldn't send %u packets.\n", BURST_SIZE - nb_tx);
            uint16_t buf;

            for (buf = nb_tx; buf < BURST_SIZE; buf++)
                rte_pktmbuf_free(pkt[buf]);
        }

        for(i=0;i<BURST_SIZE;i++)
            rte_pktmbuf_free(pkt[i]);

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

    /* closing and releasing resources */
    // rte_flow_flush(port_id, &error);
    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);

    // for (;;) {
    //     /*
    //      * Receive packets on a port and forward them on the paired
    //      * port. The mapping is 0 -> 1, 1 -> 0, 2 -> 3, 3 -> 2, etc.
    //      */
    //     RTE_ETH_FOREACH_DEV(port) {

    //         /* Get burst of RX packets, from first port of pair. */
    //         struct rte_mbuf *bufs[BURST_SIZE];
    //         const uint16_t nb_rx = rte_eth_rx_burst(port, 0,
    //                 bufs, BURST_SIZE);

    //         if (unlikely(nb_rx == 0))
    //             continue;

    //         /* Send burst of TX packets, to second port of pair. */
    //         const uint16_t nb_tx = rte_eth_tx_burst(port ^ 1, 0,
    //                 bufs, nb_rx);

    //         /* Free any unsent packets. */
    //         if (unlikely(nb_tx < nb_rx)) {
    //             uint16_t buf;
    //             for (buf = nb_tx; buf < nb_rx; buf++)
    //                 rte_pktmbuf_free(bufs[buf]);
    //         }
    //     }
    // }
    /* >8 End of loop. */
}
/* >8 End Basic forwarding application lcore. */

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */
int
main(int argc, char *argv[])
{
    int err = 0;

    char* file_name = "red.jpg";
    // char* file_name = "/home/sbernauer/private/pixelflut/sturmflut/red.jpg";
    // char* file_name = "/home/sbernauer/Desktop/Screenshots/Screenshot_20230720_114553.png";

    struct fluter_image* fluter_image;
    if((err = load_image(&fluter_image, file_name))) {
		fprintf(stderr, "Failed to load image from %s: %s\n", file_name, strerror(-err));
		return err;
	}

    struct rte_mempool *mbuf_pool;
    unsigned nb_ports;
    uint16_t portid;

    /* Initializion the Environment Abstraction Layer (EAL). 8< */
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
    /* >8 End of initialization the Environment Abstraction Layer (EAL). */

    argc -= ret;
    argv += ret;

    /* Check that there is an even number of ports to send/receive on. */
    nb_ports = rte_eth_dev_count_avail();
    printf("Detected %u ports\n", nb_ports);
    if (nb_ports != 1)
        rte_exit(EXIT_FAILURE, "Error: currently only a single port is supported, you have %d ports\n", nb_ports);

    /* Allocates mempool to hold the mbufs. 8< */
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    printf("mbuf created\n");
    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
    /* >8 End of allocating mempool to hold mbuf. */

    /* Initializing all ports. 8< */
    RTE_ETH_FOREACH_DEV(portid)
        if (port_init(portid, mbuf_pool) != 0)
            rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n",
                    portid);
    /* >8 End of initializing all ports. */

    if (rte_lcore_count() > 1)
        printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");

    /* Call lcore_main on the main core only. Called on single lcore. 8< */
    struct main_thread_args args;
    args.fluter_image = fluter_image;
    args.mbuf_pool = mbuf_pool;
    // FIXME: Currently this only works with a single port
    args.port_id = 0;

    lcore_main(&args);
    /* >8 End of called on single lcore. */

    /* clean up the EAL */
    rte_eal_cleanup();

    return 0;
}
