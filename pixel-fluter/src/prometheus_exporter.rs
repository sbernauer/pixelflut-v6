use std::time::Duration;

use anyhow::Context;
use prometheus_exporter::prometheus::{IntGaugeVec, register_int_gauge_vec};
use tokio::time::interval;

use crate::statistics::Statistics;

pub struct PrometheusExporter<'a> {
    current_statistics: &'a Statistics,

    metric_received_packets: IntGaugeVec,
    metric_transmitted_packets: IntGaugeVec,
    metric_dropped_packets: IntGaugeVec,
    metric_input_error_packets: IntGaugeVec,
    metric_output_error_packets: IntGaugeVec,

    metric_received_bytes: IntGaugeVec,
    metric_transmitted_bytes: IntGaugeVec,

    metric_rx_mbuf_allocation_failures: IntGaugeVec,

    metric_received_packets_per_queue: IntGaugeVec,
    metric_transmitted_packets_per_queue: IntGaugeVec,
    metric_dropped_packets_per_queue: IntGaugeVec,

    metric_received_bytes_per_queue: IntGaugeVec,
    metric_transmitted_bytes_per_queue: IntGaugeVec,
}

impl<'a> PrometheusExporter<'a> {
    pub fn new(current_statistics: &'a Statistics) -> anyhow::Result<Self> {
        Ok(Self {
            current_statistics,

            // Descriptions copied from the struct `PortStats` (which in turn copies from DPDK)

            // Packet stats
            metric_received_packets: register_int_gauge_vec!(
                "pixelflut_v6_received_packets",
                "Total number of successfully received packets",
                &["mac"],
            )?,
            metric_transmitted_packets: register_int_gauge_vec!(
                "pixelflut_v6_transmitted_packets",
                "Total number of successfully transmitted packets",
                &["mac"],
            )?,
            metric_dropped_packets: register_int_gauge_vec!(
                "pixelflut_v6_dropped_packets",
                "Total of Rx packets dropped by the HW, because there are no available buffer (i.e. Rx queues are full)",
                &["mac"],
            )?,
            metric_input_error_packets: register_int_gauge_vec!(
                "pixelflut_v6_input_error_packets",
                "Total number of erroneous received packets",
                &["mac"],
            )?,
            metric_output_error_packets: register_int_gauge_vec!(
                "pixelflut_v6_output_error_packets",
                "Total number of failed transmitted packets",
                &["mac"],
            )?,

            // Byte stats
            metric_received_bytes: register_int_gauge_vec!(
                "pixelflut_v6_received_bytes",
                "Total number of successfully received bytes",
                &["mac"],
            )?,
            metric_transmitted_bytes: register_int_gauge_vec!(
                "pixelflut_v6_transmitted_bytes",
                "Total number of successfully transmitted bytes",
                &["mac"],
            )?,

            // Some random metric
            metric_rx_mbuf_allocation_failures: register_int_gauge_vec!(
                "pixelflut_v6_rx_mbuf_allocation_failures",
                "Total number of Rx mbuf allocation failures",
                &["mac"],
            )?,

            // Queue level stats
            metric_received_packets_per_queue: register_int_gauge_vec!(
                "pixelflut_v6_received_packets_per_queue",
                "Total number of queue Rx packets",
                &["mac", "queue"],
            )?,
            metric_transmitted_packets_per_queue: register_int_gauge_vec!(
                "pixelflut_v6_transmitted_packets_per_queue",
                "Total number of queue Tx packets",
                &["mac", "queue"],
            )?,
            metric_dropped_packets_per_queue: register_int_gauge_vec!(
                "pixelflut_v6_dropped_packets_per_queue",
                "Total number of queue packets received that are dropped",
                &["mac", "queue"],
            )?,

            metric_received_bytes_per_queue: register_int_gauge_vec!(
                "pixelflut_v6_received_bytes_per_queue",
                "Total number of successfully received queue bytes",
                &["mac", "queue"],
            )?,
            metric_transmitted_bytes_per_queue: register_int_gauge_vec!(
                "pixelflut_v6_transmitted_bytes_per_queue",
                "Total number of successfully transmitted queue bytes",
                &["mac", "queue"],
            )?,
        })
    }

    pub async fn run(&self) -> anyhow::Result<()> {
        let listen_addr = "[::]:9102".parse().unwrap();
        prometheus_exporter::start(listen_addr).context("bind Prometheus exporter")?;

        let mut interval = interval(Duration::from_secs(1));

        let stats = self.current_statistics;
        loop {
            interval.tick().await;

            for stats in &stats.port_stats {
                if stats.mac_addr.is_nil() {
                    // Only export slots that have actual statistics
                    continue;
                }
                let mac = stats.mac_addr.to_string();

                self.metric_received_packets
                    .with_label_values(&[&mac])
                    .set(stats.ipackets.try_into().expect("convert ipackets to i64"));
                self.metric_transmitted_packets
                    .with_label_values(&[&mac])
                    .set(stats.opackets.try_into().expect("convert opackets to i64"));
                self.metric_dropped_packets
                    .with_label_values(&[&mac])
                    .set(stats.imissed.try_into().expect("convert imissed to i64"));
                self.metric_input_error_packets
                    .with_label_values(&[&mac])
                    .set(stats.ierrors.try_into().expect("convert ierrors to i64"));
                self.metric_output_error_packets
                    .with_label_values(&[&mac])
                    .set(stats.oerrors.try_into().expect("convert oerrors to i64"));

                self.metric_received_bytes
                    .with_label_values(&[&mac])
                    .set(stats.ibytes.try_into().expect("convert ibytes to i64"));
                self.metric_transmitted_bytes
                    .with_label_values(&[&mac])
                    .set(stats.obytes.try_into().expect("convert obytes to i64"));

                self.metric_rx_mbuf_allocation_failures
                    .with_label_values(&[&mac])
                    .set(
                        stats
                            .rx_nombuf
                            .try_into()
                            .expect("convert rx_nombuf to i64"),
                    );

                for queue_id in 0..stats.q_ipackets.len() {
                    let queue = queue_id.to_string();

                    self.metric_received_packets_per_queue
                        .with_label_values(&[&mac, &queue])
                        .set(
                            stats.q_ipackets[queue_id]
                                .try_into()
                                .expect("convert q_ipackets to i64"),
                        );
                    self.metric_transmitted_packets_per_queue
                        .with_label_values(&[&mac, &queue])
                        .set(
                            stats.q_opackets[queue_id]
                                .try_into()
                                .expect("convert q_opackets to i64"),
                        );
                    self.metric_dropped_packets_per_queue
                        .with_label_values(&[&mac, &queue])
                        .set(
                            stats.q_errors[queue_id]
                                .try_into()
                                .expect("convert q_errors to i64"),
                        );
                    self.metric_received_bytes_per_queue
                        .with_label_values(&[&mac, &queue])
                        .set(
                            stats.q_ibytes[queue_id]
                                .try_into()
                                .expect("convert q_ibytes to i64"),
                        );
                    self.metric_transmitted_bytes_per_queue
                        .with_label_values(&[&mac, &queue])
                        .set(
                            stats.q_obytes[queue_id]
                                .try_into()
                                .expect("convert q_obytes to i64"),
                        );
                }
            }
        }
    }
}
