use std::fmt::Display;

use macaddr::MacAddr6;

use crate::MAX_PORTS;

/// Reverse-engineered, IDK where this constant is defined
const RTE_ETHDEV_QUEUE_STAT_CNTRS: usize = 16;

#[repr(C)]
#[derive(Clone)]
pub struct Statistics {
    port_stats: [PortStats; MAX_PORTS],
}

impl Display for Statistics {
    fn fmt(&self, fmt: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        for port_stat in &self.port_stats {
            // Only print slots that have actual statistics
            if port_stat.mac_addr.is_nil() {
                continue;
            }

            writeln!(fmt, "{port_stat}")?;
        }

        Ok(())
    }
}

// Same memory layout as rte_eth_stats
#[repr(C)]
#[derive(Clone)]
pub struct PortStats {
    mac_addr: MacAddr6,

    /// Total number of successfully received packets.
    ipackets: u64,
    /// Total number of successfully transmitted packets.
    opackets: u64,
    /// Total number of successfully received bytes.
    ibytes: u64,
    /// Total number of successfully transmitted bytes.
    obytes: u64,
    /// Total of Rx packets dropped by the HW, because there are no available buffer (i.e. Rx queues are full).
    imissed: u64,
    /// Total number of erroneous received packets.
    ierrors: u64,
    /// Total number of failed transmitted packets.
    oerrors: u64,
    /// Total number of Rx mbuf allocation failures.
    rx_nombuf: u64,
    /// Total number of queue Rx packets.
    q_ipackets: [u64; RTE_ETHDEV_QUEUE_STAT_CNTRS],
    /// Total number of queue Tx packets.
    q_opackets: [u64; RTE_ETHDEV_QUEUE_STAT_CNTRS],
    /// Total number of successfully received queue bytes.
    q_ibytes: [u64; RTE_ETHDEV_QUEUE_STAT_CNTRS],
    /// Total number of successfully transmitted queue bytes.
    q_obytes: [u64; RTE_ETHDEV_QUEUE_STAT_CNTRS],
    /// Total number of queue packets received that are dropped.
    q_errors: [u64; RTE_ETHDEV_QUEUE_STAT_CNTRS],
}

impl Display for PortStats {
    fn fmt(&self, fmt: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            fmt,
            "MAC {} RX: {} packets, {} dropped, {} bytes",
            self.mac_addr, self.ipackets, self.imissed, self.ibytes
        )?;

        // Only print TX stats in case something has been send
        if self.opackets != 0 {
            write!(
                fmt,
                "\nMAC {:?} TX: {} packets, {} bytes",
                self.mac_addr, self.opackets, self.obytes
            )?;
        }

        Ok(())
    }
}
