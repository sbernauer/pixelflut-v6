use std::{
    io::{self, Stdout},
    time::{Duration, Instant},
};

use anyhow::{Context, Result};
use ratatui::{
    crossterm::{
        event::{self, Event, KeyCode},
        execute,
        terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
    },
    prelude::*,
    widgets::*,
};

use crate::statistics::Statistics;

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
        // let greeting = Paragraph::new("Hello World! (press 'q' to quit)");
        // frame.render_widget(greeting, frame.size());

        let rows = self.get_rows();
        let widths = [
            Constraint::Length(17),
            Constraint::Length(10),
            Constraint::Length(10),
            Constraint::Length(10),
        ];
        let table = Table::new(rows, widths)
            // ...and they can be separated by a fixed spacing.
            .column_spacing(1)
            // You can set the style of the entire Table.
            .style(Style::new().blue())
            // It has an optional header, which is simply a Row always visible at the top.
            .header(
                Row::new(vec!["MAC address", "Packets", "Bytes", "Pkt/s"])
                    .style(Style::new().bold())
                    // To add space between the header and the rest of the rows, specify the margin
                    .bottom_margin(1),
            )
            // It has an optional footer, which is simply a Row always visible at the bottom.
            // .footer(Row::new(vec!["Updated on Dec 28"]))
            // As any other widget, a Table can be wrapped in a Block.
            // .block(Block::new().title("Table"))
            // The selected row and its content can also be styled.
            .highlight_style(Style::new().reversed())
            // ...and potentially show a symbol in front of the selection.
            .highlight_symbol(">>");

        frame.render_widget(table, frame.size());
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
            self.diff = self.current_statistics - &self.prev_statistics;
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

            // let rows = [Row::new(vec!["Cell1", "Cell2", "Cell3"])];
            rows.push(Row::new(vec![
                current_port_stat.mac_addr.to_string(),
                current_port_stat.ipackets.to_string(),
                current_port_stat.ibytes.to_string(),
                diff.ipackets.to_string(),
            ]));
        }

        rows
    }
}
