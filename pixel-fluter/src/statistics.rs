use std::{fmt::Display, iter::Sum, ops::Add};

use macaddr::MacAddr6;

use crate::MAX_PORTS;

/// Reverse-engineered, IDK where this constant is defined
const RTE_ETHDEV_QUEUE_STAT_CNTRS: usize = 16;

#[repr(C)]
#[derive(Clone, Default)]
pub struct Statistics {
    pub port_stats: [PortStats; MAX_PORTS],
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

/// I could not find a SaturatingSub trait in std
impl Statistics {
    pub fn saturating_sub(&self, rhs: &Self) -> Self {
        Self {
            port_stats: self
                .port_stats
                .iter()
                .zip(rhs.port_stats.iter())
                .map(|(l, r)| l.saturating_sub(r))
                .collect::<Vec<_>>()
                .try_into()
                .unwrap(),
        }
    }
}

// Same memory layout as rte_eth_stats
#[repr(C)]
#[derive(Clone, Default, Debug)]
pub struct PortStats {
    pub mac_addr: MacAddr6,

    /// Total number of successfully received packets.
    pub ipackets: u64,
    /// Total number of successfully transmitted packets.
    pub opackets: u64,
    /// Total number of successfully received bytes.
    pub ibytes: u64,
    /// Total number of successfully transmitted bytes.
    pub obytes: u64,
    /// Total of Rx packets dropped by the HW, because there are no available buffer (i.e. Rx queues are full).
    pub imissed: u64,
    /// Total number of erroneous received packets.
    pub ierrors: u64,
    /// Total number of failed transmitted packets.
    pub oerrors: u64,
    /// Total number of Rx mbuf allocation failures.
    pub rx_nombuf: u64,
    /// Total number of queue Rx packets.
    pub q_ipackets: [u64; RTE_ETHDEV_QUEUE_STAT_CNTRS],
    /// Total number of queue Tx packets.
    pub q_opackets: [u64; RTE_ETHDEV_QUEUE_STAT_CNTRS],
    /// Total number of successfully received queue bytes.
    pub q_ibytes: [u64; RTE_ETHDEV_QUEUE_STAT_CNTRS],
    /// Total number of successfully transmitted queue bytes.
    pub q_obytes: [u64; RTE_ETHDEV_QUEUE_STAT_CNTRS],
    /// Total number of queue packets received that are dropped.
    pub q_errors: [u64; RTE_ETHDEV_QUEUE_STAT_CNTRS],
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

impl Add<&PortStats> for &PortStats {
    type Output = PortStats;

    fn add(self, rhs: &PortStats) -> Self::Output {
        PortStats {
            mac_addr: self.mac_addr,
            ipackets: self.ipackets + rhs.ipackets,
            opackets: self.opackets + rhs.opackets,
            ibytes: self.ibytes + rhs.ibytes,
            obytes: self.obytes + rhs.obytes,
            imissed: self.imissed + rhs.imissed,
            ierrors: self.ierrors + rhs.ierrors,
            oerrors: self.oerrors + rhs.oerrors,
            rx_nombuf: self.rx_nombuf + rhs.rx_nombuf,
            q_ipackets: self
                .q_ipackets
                .iter()
                .zip(rhs.q_ipackets)
                .map(|(l, r)| l + r)
                .collect::<Vec<_>>()
                .try_into()
                .unwrap(),
            q_opackets: self
                .q_opackets
                .iter()
                .zip(rhs.q_opackets)
                .map(|(l, r)| l + r)
                .collect::<Vec<_>>()
                .try_into()
                .unwrap(),
            q_ibytes: self
                .q_ibytes
                .iter()
                .zip(rhs.q_ibytes)
                .map(|(l, r)| l + r)
                .collect::<Vec<_>>()
                .try_into()
                .unwrap(),
            q_obytes: self
                .q_obytes
                .iter()
                .zip(rhs.q_obytes)
                .map(|(l, r)| l + r)
                .collect::<Vec<_>>()
                .try_into()
                .unwrap(),
            q_errors: self
                .q_errors
                .iter()
                .zip(rhs.q_errors)
                .map(|(l, r)| l + r)
                .collect::<Vec<_>>()
                .try_into()
                .unwrap(),
        }
    }
}

impl<'a> Sum<&'a PortStats> for PortStats {
    fn sum<I: Iterator<Item = &'a PortStats>>(iter: I) -> Self {
        iter.fold(PortStats::default(), |l, r| &l + r)
    }
}

/// I could not find a SaturatingSub trait in std
impl PortStats {
    fn saturating_sub(&self, rhs: &Self) -> PortStats {
        PortStats {
            mac_addr: self.mac_addr,
            ipackets: self.ipackets.saturating_sub(rhs.ipackets),
            opackets: self.opackets.saturating_sub(rhs.opackets),
            ibytes: self.ibytes.saturating_sub(rhs.ibytes),
            obytes: self.obytes.saturating_sub(rhs.obytes),
            imissed: self.imissed.saturating_sub(rhs.imissed),
            ierrors: self.ierrors.saturating_sub(rhs.ierrors),
            oerrors: self.oerrors.saturating_sub(rhs.oerrors),
            rx_nombuf: self.rx_nombuf.saturating_sub(rhs.rx_nombuf),
            q_ipackets: self
                .q_ipackets
                .iter()
                .zip(rhs.q_ipackets)
                .map(|(l, r)| l.saturating_sub(r))
                .collect::<Vec<_>>()
                .try_into()
                .unwrap(),
            q_opackets: self
                .q_opackets
                .iter()
                .zip(rhs.q_opackets)
                .map(|(l, r)| l.saturating_sub(r))
                .collect::<Vec<_>>()
                .try_into()
                .unwrap(),
            q_ibytes: self
                .q_ibytes
                .iter()
                .zip(rhs.q_ibytes)
                .map(|(l, r)| l.saturating_sub(r))
                .collect::<Vec<_>>()
                .try_into()
                .unwrap(),
            q_obytes: self
                .q_obytes
                .iter()
                .zip(rhs.q_obytes)
                .map(|(l, r)| l.saturating_sub(r))
                .collect::<Vec<_>>()
                .try_into()
                .unwrap(),
            q_errors: self
                .q_errors
                .iter()
                .zip(rhs.q_errors)
                .map(|(l, r)| l.saturating_sub(r))
                .collect::<Vec<_>>()
                .try_into()
                .unwrap(),
        }
    }
}
