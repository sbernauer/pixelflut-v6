use std::time::{Duration, Instant};

use anyhow::Context;
use input_handling::handle_event;
use rendering::render;
use state::{Message, Model, RunningState, update};

use crate::statistics::{PortStats, Statistics};

mod input_handling;
mod rendering;
mod state;

pub struct Tui<'a> {
    current_statistics: &'a Statistics,
    prev_statistics: Statistics,
    diff: Statistics,
    last_tick: Instant,
}

impl<'a> Tui<'a> {
    pub fn new(current_statistics: &'a Statistics) -> Self {
        Self {
            current_statistics,
            prev_statistics: current_statistics.clone(),
            diff: Statistics::default(),
            last_tick: Instant::now(),
        }
    }

    pub fn run(&mut self) -> anyhow::Result<()> {
        let result = self.inner_run();

        ratatui::restore();

        result
    }

    fn inner_run(&mut self) -> anyhow::Result<()> {
        let mut terminal = ratatui::init();
        let mut model = Model::default();

        // As we emitted some tracing-subscriber stuff earlier, we want to clean our screen.
        // Long term we could use https://crates.io/crates/tui-logger instead.
        terminal.clear().context("failed to clear terminal")?;

        // Initialize with the current state, so that the TUI starts with data being available
        // instantly. The diff (packets/s) here is not known, so we temporarily render it as zero.
        let null_diff = std::iter::repeat(PortStats::default());
        let initial_stats = self
            .current_statistics
            .port_stats
            .iter()
            .filter(|port| !port.mac_addr.is_nil())
            .cloned()
            .zip(null_diff)
            .collect();
        update(
            &mut model,
            Message::StatsUpdate {
                stats: initial_stats,
            },
        );

        while model.running_state != RunningState::Done {
            if self.last_tick.elapsed() > Duration::from_secs(1) {
                self.last_tick = Instant::now();
                self.diff = self
                    .current_statistics
                    .saturating_sub(&self.prev_statistics);
                self.prev_statistics = self.current_statistics.clone();

                let stats = self
                    .current_statistics
                    .port_stats
                    .iter()
                    .filter(|port| !port.mac_addr.is_nil())
                    .cloned()
                    .zip(
                        self.diff
                            .port_stats
                            .iter()
                            .filter(|port| !port.mac_addr.is_nil())
                            .cloned(),
                    )
                    .collect();
                update(&mut model, Message::StatsUpdate { stats });
            }

            // Render the current view
            terminal
                .draw(|f| render(&mut model, f))
                .context("failed to draw to terminal")?;

            // Handle events and map to a Message
            let mut current_msg = handle_event(&model).context("failed to handle event")?;

            // Process updates as long as they return a non-None message
            while current_msg.is_some() {
                current_msg = update(&mut model, current_msg.unwrap());
            }
        }

        Ok(())
    }
}
