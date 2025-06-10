use std::cmp::min;

use ratatui::widgets::TableState;

use crate::statistics::PortStats;

#[derive(Default)]
pub struct Model {
    pub running_state: RunningState,
    pub ports_table_state: TableState,

    /// First tuple element is total number of packets, the second element is the ones received in
    /// the last second
    pub stats: Vec<(PortStats, PortStats)>,
}

#[derive(Debug, Default, PartialEq)]
pub enum RunningState {
    #[default]
    Running,
    Done,
}

#[derive(Debug)]
pub enum Message {
    Quit,
    PortListUp { steps: usize },
    PortListDown { steps: usize },
    StatsUpdate { stats: Vec<(PortStats, PortStats)> },
}

pub fn update(model: &mut Model, message: Message) -> Option<Message> {
    match message {
        Message::Quit => model.running_state = RunningState::Done,
        Message::PortListUp { steps } => {
            let new_selected = match model.ports_table_state.selected() {
                Some(index) => {
                    if index == 0 {
                        model.stats.len().saturating_sub(1)
                    } else {
                        index.saturating_sub(steps)
                    }
                }
                None => 0,
            };
            model.ports_table_state.select(Some(new_selected));
        }
        Message::PortListDown { steps } => {
            let new_selected = match model.ports_table_state.selected() {
                Some(index) => {
                    if index >= model.stats.len().saturating_sub(1) {
                        0
                    } else {
                        min(index + steps, model.stats.len().saturating_sub(1))
                    }
                }
                None => 0,
            };
            model.ports_table_state.select(Some(new_selected));
        }
        Message::StatsUpdate { stats } => model.stats = stats,
    }

    None
}
