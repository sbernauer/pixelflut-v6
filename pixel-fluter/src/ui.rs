use std::{
    io::{self, Stdout},
    time::{Duration, Instant},
};

use anyhow::{Context, Result};
use macaddr::MacAddr6;
use number_prefix::NumberPrefix;
use ratatui::{
    crossterm::{
        event::{self, Event, KeyCode},
        execute,
        terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
    },
    prelude::*,
    widgets::*,
};

use crate::statistics::{PortStats, Statistics};

pub struct Ui<'a> {
    current_statistics: &'a Statistics,
    prev_statistics: Statistics,
    diff: Statistics,
    last_tick: Instant,
}

impl<'a> Ui<'a> {
    pub fn new(current_statistics: &'a Statistics) -> Self {
        Self {
            current_statistics,
            prev_statistics: current_statistics.clone(),
            diff: Statistics::default(),
            last_tick: Instant::now(),
        }
    }

    /// This is a bare minimum example. There are many approaches to running an application loop, so
    /// this is not meant to be prescriptive. It is only meant to demonstrate the basic setup and
    /// teardown of a terminal application.
    ///
    /// A more robust application would probably want to handle errors and ensure that the terminal is
    /// restored to a sane state before exiting. This example does not do that. It also does not handle
    /// events or update the application state. It just draws a greeting and exits when the user
    /// presses 'q'.
    pub fn start(&mut self) -> Result<()> {
        let mut terminal = Self::setup_terminal().context("setup failed")?;

        self.run(&mut terminal).context("main app loop failed")?;

        Self::restore_terminal(&mut terminal).context("restore of terminal failed")?;

        Ok(())
    }

    /// Setup the terminal. This is where you would enable raw mode, enter the alternate screen, and
    /// hide the cursor. This example does not handle errors. A more robust application would probably
    /// want to handle errors and ensure that the terminal is restored to a sane state before exiting.
    fn setup_terminal() -> Result<Terminal<CrosstermBackend<Stdout>>> {
        let mut stdout = io::stdout();
        enable_raw_mode().context("failed to enable raw mode")?;
        execute!(stdout, EnterAlternateScreen).context("unable to enter alternate screen")?;
        Terminal::new(CrosstermBackend::new(stdout)).context("creating terminal failed")
    }

    /// Restore the terminal. This is where you disable raw mode, leave the alternate screen, and show
    /// the cursor.
    fn restore_terminal(terminal: &mut Terminal<CrosstermBackend<Stdout>>) -> Result<()> {
        disable_raw_mode().context("failed to disable raw mode")?;
        execute!(terminal.backend_mut(), LeaveAlternateScreen)
            .context("unable to switch to main screen")?;
        terminal.show_cursor().context("unable to show cursor")
    }

    /// Run the application loop. This is where you would handle events and update the application
    /// state. This example exits when the user presses 'q'. Other styles of application loops are
    /// possible, for example, you could have multiple application states and switch between them based
    /// on events, or you could have a single application state and update it based on events.
    fn run(&mut self, terminal: &mut Terminal<CrosstermBackend<Stdout>>) -> Result<()> {
        loop {
            terminal.draw(|f| self.render_app(f))?;
            if Self::should_quit()? {
                break;
            }
        }
        Ok(())
    }

    /// Render the application. This is where you would draw the application UI. This example just
    /// draws a greeting.
    fn render_app(&mut self, frame: &mut Frame) {
        let main_layout = Layout::new(
            Direction::Vertical,
            [Constraint::Percentage(50), Constraint::Percentage(50)],
        )
        .split(frame.size());

        let rows = self.get_rows();
        let widths = [
            Constraint::Length(18),
            Constraint::Length(12),
            Constraint::Length(12),
            Constraint::Length(12),
            Constraint::Length(12),
            Constraint::Length(12),
            Constraint::Length(12),
            Constraint::Length(12),
            Constraint::Length(12),
        ];
        let table = Table::new(rows, widths)
            // ...and they can be separated by a fixed spacing.
            .column_spacing(1)
            // You can set the style of the entire Table.
            .style(Style::new())
            // It has an optional header, which is simply a Row always visible at the top.
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
                // To add space between the header and the rest of the rows, specify the margin
                .bottom_margin(1),
            )
            // As any other widget, a Table can be wrapped in a Block.
            .block(Block::new().title("Port statistics").borders(Borders::TOP));

        frame.render_widget(table, main_layout[0]);

        // let greeting =
        //     Paragraph::new("TODO").block(Block::new().title("Histogram").borders(Borders::TOP));
        // frame.render_widget(greeting, main_layout[1]);
    }

    /// Check if the user has pressed 'q'. This is where you would handle events. This example just
    /// checks if the user has pressed 'q' and returns true if they have. It does not handle any other
    /// events. There is a 100ms timeout on the event poll so that the application can exit in a timely
    /// manner, and to ensure that the terminal is rendered at least once every 100ms.
    fn should_quit() -> Result<bool> {
        if event::poll(Duration::from_millis(100)).context("event poll failed")? {
            if let Event::Key(key) = event::read().context("event read failed")? {
                match key.code {
                    // Poor mans way of handling CTRL + C :)
                    KeyCode::Char('c') => return Ok(true),
                    _ => return Ok(false),
                }
            }
        }
        Ok(false)
    }

    fn get_rows(&mut self) -> Vec<Row> {
        if self.last_tick.elapsed() > Duration::from_secs(1) {
            self.last_tick = Instant::now();
            self.diff = self
                .current_statistics
                .saturating_sub(&self.prev_statistics);
            self.prev_statistics = self.current_statistics.clone();
        }

        let mut rows = Vec::new();

        for (current_port_stat, diff) in self
            .current_statistics
            .port_stats
            .iter()
            .zip(self.diff.port_stats.iter())
            .into_iter()
        {
            // Only print slots that have actual statistics
            if current_port_stat.mac_addr.is_nil() {
                continue;
            }

            rows.push(Row::new(vec![
                current_port_stat.mac_addr.to_string(),
                format_packets(diff.ipackets as f64),
                format_bytes_per_s(diff.ibytes as f64),
                format_packets(diff.imissed as f64),
                format_packets(diff.ierrors as f64),
                format_packets(current_port_stat.ipackets as f64),
                format_bytes(current_port_stat.ibytes as f64),
                format_packets(current_port_stat.imissed as f64),
                format_packets(current_port_stat.ierrors as f64),
            ]));
        }

        let mut current_sum: PortStats = self.current_statistics.port_stats.iter().sum();
        current_sum.mac_addr = MacAddr6::broadcast();
        let mut diff_sum: PortStats = self.diff.port_stats.iter().sum();
        diff_sum.mac_addr = MacAddr6::broadcast();

        rows.push(
            Row::new(vec![
                "Total".to_owned(),
                format_packets(diff_sum.ipackets as f64),
                format_bytes_per_s(diff_sum.ibytes as f64),
                format_packets(diff_sum.imissed as f64),
                format_packets(diff_sum.ierrors as f64),
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
            format!("{packets}")
        }
        NumberPrefix::Prefixed(prefix, value) => {
            format!("{value:.2}{prefix}")
        }
    }
}
