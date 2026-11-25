use crate::ipc::PanelView;
use gpui_kit::{KeyDownEvent, Keystroke, OperatingSystem};
use serde::{Deserialize, Deserializer};

#[derive(Deserialize)]
pub struct PanelBinding {
    key: String,
    args: Vec<String>,
}

pub fn deserialize<'de, D: Deserializer<'de>>(d: D) -> Result<Vec<PanelBinding>, D::Error> {
    // An optional shortcut must never prevent a page's buttons from rendering.
    let value = serde_json::Value::deserialize(d)?;
    let serde_json::Value::Array(items) = value else {
        return Ok(Vec::new());
    };
    Ok(items
        .into_iter()
        .filter_map(|item| serde_json::from_value(item).ok())
        .collect())
}

#[derive(Default)]
pub struct PageBindings {
    shortcuts: Vec<(Keystroke, Vec<String>)>,
    pub completion: Option<Vec<String>>,
}

fn valid_command(args: &[String], plugin: &str) -> bool {
    args.first()
        .is_some_and(|name| name == plugin && !name.is_empty())
        && args
            .get(1)
            .is_some_and(|command| !command.trim().is_empty())
}

impl PageBindings {
    pub fn from_view(view: &PanelView, plugin: &str) -> Self {
        let shortcuts = view
            .bindings
            .iter()
            .filter(|binding| valid_command(&binding.args, plugin))
            .filter_map(|binding| Some((parse_key(&binding.key)?, binding.args.clone())))
            .collect();
        Self {
            shortcuts,
            completion: view
                .completion_query
                .as_ref()
                .filter(|args| valid_command(args, plugin))
                .cloned(),
        }
    }
}

fn parse_key(value: &str) -> Option<Keystroke> {
    let key = Keystroke::parse_with_os(value, OperatingSystem::Linux).ok()?;
    let name = key.key.as_str();
    let letter_or_digit = name.len() == 1
        && name
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit());
    let function = name.strip_prefix('f').is_some_and(|number| {
        number
            .parse::<u8>()
            .is_ok_and(|n| (1..=24).contains(&n) && number == n.to_string())
    });
    let named = matches!(
        name,
        "space"
            | "enter"
            | "escape"
            | "backspace"
            | "delete"
            | "insert"
            | "home"
            | "end"
            | "pageup"
            | "pagedown"
            | "left"
            | "right"
            | "up"
            | "down"
    );
    if !(letter_or_digit || function || named) || key.key_char.is_some() || key.modifiers.function {
        return None;
    }
    let mut canonical = String::new();
    for (set, prefix) in [
        (key.modifiers.control, "ctrl-"),
        (key.modifiers.alt, "alt-"),
        (key.modifiers.shift, "shift-"),
        (key.modifiers.platform, "super-"),
    ] {
        if set {
            canonical.push_str(prefix);
        }
    }
    canonical.push_str(name);
    (value == canonical).then_some(key)
}

pub fn resolve<'a>(
    pages: &'a [Option<PageBindings>],
    preferred: usize,
    event: &KeyDownEvent,
) -> Option<(usize, &'a [String])> {
    let mut matched: Option<(usize, &[String])> = None;
    for (index, page) in pages
        .iter()
        .enumerate()
        .filter_map(|(index, page)| Some((index, page.as_ref()?)))
    {
        for (key, args) in &page.shortcuts {
            if key.key != event.keystroke.key || key.modifiers != event.keystroke.modifiers {
                continue;
            }
            if let Some((_, previous)) = matched {
                if previous != args {
                    return None;
                }
                if index != preferred {
                    continue;
                }
            }
            matched = Some((index, args));
        }
    }
    matched
}
