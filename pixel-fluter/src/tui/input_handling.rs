use std::time::Duration;

use ratatui::crossterm::event::{self, Event, KeyCode, KeyModifiers};

use super::state::{Message, Model};

/// Convert Event to Message
///
/// We don't need to pass in a `model` to this function in this example
/// but you might need it as your project evolves
pub fn handle_event(_: &Model) -> anyhow::Result<Option<Message>> {
    if event::poll(Duration::from_millis(250))? {
        if let Event::Key(key) = event::read()? {
            if key.kind == event::KeyEventKind::Press {
                return Ok(handle_key(key));
            }
        }
    }
    Ok(None)
}

fn handle_key(key: event::KeyEvent) -> Option<Message> {
    let ctrl_pressed = key.modifiers.contains(KeyModifiers::CONTROL);

    match key.code {
        KeyCode::Char('c') if ctrl_pressed => Some(Message::Quit),
        KeyCode::Char('k') | KeyCode::Up => Some(Message::PortListUp { steps: 1 }),
        KeyCode::Char('j') | KeyCode::Down => Some(Message::PortListDown { steps: 1 }),
        _ => None,
    }
}
