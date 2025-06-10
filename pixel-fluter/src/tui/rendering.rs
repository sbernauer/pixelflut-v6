use macaddr::MacAddr6;
use number_prefix::NumberPrefix;
use ratatui::{
    Frame,
    buffer::Buffer,
    layout::{Constraint, Layout, Rect},
    style::{Style, Stylize},
    widgets::{Block, Borders, Row, StatefulWidget, Table, Widget},
};

use crate::statistics::PortStats;

use super::state::Model;

pub fn render(model: &mut Model, frame: &mut Frame) {
    let [ports_area, queues_area] =
        Layout::vertical([Constraint::Fill(1), Constraint::Fill(2)]).areas(frame.area());

    render_ports(model, ports_area, frame.buffer_mut());
    render_queues(model, queues_area, frame.buffer_mut());
}

pub fn render_ports(model: &mut Model, area: Rect, buffer: &mut Buffer) {
    let rows = get_port_rows(model);
    let widths = [
        Constraint::Length(18),
        Constraint::Length(13),
        Constraint::Length(13),
        Constraint::Length(13),
        Constraint::Length(13),
        Constraint::Length(13),
        Constraint::Length(13),
        Constraint::Length(13),
        Constraint::Length(13),
    ];
    let table = Table::new(rows, widths)
        .column_spacing(1)
        .style(Style::new())
        .row_highlight_style(Style::new().reversed())
        .header(
            Row::new(vec![
                "MAC address",
                "Pkt/s",
                "Bit/s",
                "Missed pkt/s",
                "Error pkt/s",
                "Packets",
                "Bytes",
                "Missed pkts",
                "Error pkts",
            ])
            .style(Style::new().bold())
            .bottom_margin(1),
        )
        .block(Block::new().title("Port statistics").borders(Borders::TOP));

    if model.ports_table_state.selected().is_none() {
        model.ports_table_state.select(Some(0));
    }

    StatefulWidget::render(table, area, buffer, &mut model.ports_table_state);
}

fn get_port_rows<'a>(model: &Model) -> Vec<Row<'a>> {
    let mut rows = Vec::new();

    for (current_port_stats, diff) in &model.stats {
        rows.push(Row::new(vec![
            current_port_stats.mac_addr.to_string(),
            format_packets_per_s(diff.ipackets as f64),
            format_bytes_per_s(diff.ibytes as f64),
            format_packets_per_s(diff.imissed as f64),
            format_packets_per_s(diff.ierrors as f64),
            format_packets(current_port_stats.ipackets as f64),
            format_bytes(current_port_stats.ibytes as f64),
            format_packets(current_port_stats.imissed as f64),
            format_packets(current_port_stats.ierrors as f64),
        ]));
    }

    let mut current_sum: PortStats = model
        .stats
        .iter()
        .map(|(current_stats, _)| current_stats)
        .sum();
    let mut diff_sum: PortStats = model
        .stats
        .iter()
        .map(|(_, current_diff)| current_diff)
        .sum();
    current_sum.mac_addr = MacAddr6::broadcast();
    diff_sum.mac_addr = MacAddr6::broadcast();

    rows.push(
        Row::new(vec![
            "Total".to_owned(),
            format_packets_per_s(diff_sum.ipackets as f64),
            format_bytes_per_s(diff_sum.ibytes as f64),
            format_packets_per_s(diff_sum.imissed as f64),
            format_packets_per_s(diff_sum.ierrors as f64),
            format_packets(current_sum.ipackets as f64),
            format_bytes(current_sum.ibytes as f64),
            format_packets(current_sum.imissed as f64),
            format_packets(current_sum.ierrors as f64),
        ])
        .top_margin(1)
        .yellow(),
    );

    rows
}

pub fn render_queues(model: &Model, area: Rect, buffer: &mut Buffer) {
    let selected_port = model
        .ports_table_state
        .selected()
        .expect("The ports table must have something selected at this point");

    let (current_port_stats, diff) = model.stats.get(selected_port).unwrap_or_else(|| {
        panic!("The selected port {selected_port} must be present in the statistics!")
    });

    let rows = get_queue_rows(current_port_stats, diff);
    let widths = [
        Constraint::Length(8),
        Constraint::Length(13),
        Constraint::Length(13),
        Constraint::Length(13),
        Constraint::Length(13),
        Constraint::Length(13),
        Constraint::Length(13),
    ];
    let table = Table::new(rows, widths)
        .column_spacing(1)
        .style(Style::new())
        .header(
            Row::new(vec![
                "Queue",
                "Pkt/s",
                "Bit/s",
                "Error pkt/s",
                "Packets",
                "Bytes",
                "Error pkts",
            ])
            .style(Style::new().bold())
            .bottom_margin(1),
        )
        .block(
            Block::new()
                .title(format!(
                    "Port {mac} queue statistics",
                    mac = current_port_stats.mac_addr
                ))
                .borders(Borders::TOP),
        );

    Widget::render(table, area, buffer);
}

fn get_queue_rows<'a>(current_port_stats: &PortStats, diff: &PortStats) -> Vec<Row<'a>> {
    let mut rows = Vec::new();

    for queue in 0..current_port_stats.q_ipackets.len() {
        rows.push(Row::new(vec![
            queue.to_string(),
            format_packets_per_s(diff.q_ipackets[queue] as f64),
            format_bytes_per_s(diff.q_ibytes[queue] as f64),
            format_packets_per_s(diff.q_errors[queue] as f64),
            format_packets(diff.q_ipackets[queue] as f64),
            format_bytes(diff.q_ibytes[queue] as f64),
            format_packets(diff.q_errors[queue] as f64),
        ]));
    }

    rows
}

fn format_bytes(bytes: f64) -> String {
    match NumberPrefix::decimal(bytes) {
        NumberPrefix::Standalone(bytes) => {
            format!("{bytes} bytes")
        }
        NumberPrefix::Prefixed(prefix, value) => {
            format!("{value:.2} {prefix}B")
        }
    }
}

fn format_bytes_per_s(bytes_per_s: f64) -> String {
    match NumberPrefix::decimal(bytes_per_s * 8.0) {
        NumberPrefix::Standalone(bits_per_s) => {
            format!("{bits_per_s} bit/s")
        }
        NumberPrefix::Prefixed(prefix, value) => {
            format!("{value:.2} {prefix}b/s")
        }
    }
}

fn format_packets(packets: f64) -> String {
    match NumberPrefix::decimal(packets) {
        NumberPrefix::Standalone(packets) => {
            format!("{packets} p")
        }
        NumberPrefix::Prefixed(prefix, value) => {
            format!("{value:.2} {prefix}p")
        }
    }
}

fn format_packets_per_s(packets: f64) -> String {
    match NumberPrefix::decimal(packets) {
        NumberPrefix::Standalone(packets) => {
            format!("{packets} p/s")
        }
        NumberPrefix::Prefixed(prefix, value) => {
            format!("{value:.2} {prefix}p/s")
        }
    }
}
