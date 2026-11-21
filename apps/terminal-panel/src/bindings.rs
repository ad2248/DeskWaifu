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

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn event(key: &str) -> KeyDownEvent {
        let mut keystroke = Keystroke::parse_with_os(key, OperatingSystem::Linux).unwrap();
        // 匹配物理组合键而不是输入法或布局生成的字符。
        keystroke.key_char = Some("别".into());
        KeyDownEvent {
            keystroke,
            is_held: false,
            prefer_character_input: false,
        }
    }

    #[test]
    fn inactive_page_provides_shortcut_and_its_completion_query() {
        // 模拟当前语音页和未选中的终端页，核对命令参数以及终端页的结果查询。
        let voice: PanelView = serde_json::from_value(json!({"sections": []})).unwrap();
        let terminal: PanelView = serde_json::from_value(json!({
            "sections": [],
            "bindings": [{"key": "ctrl-alt-k", "args": ["terminal", "hotkey", "模型 01"]}],
            "completion_query": ["terminal", "status"]
        }))
        .unwrap();
        let pages = [
            Some(PageBindings::from_view(&voice, "voice")),
            Some(PageBindings::from_view(&terminal, "terminal")),
        ];
        let (index, args) = resolve(&pages, 0, &event("ctrl-alt-k")).unwrap();
        assert_eq!(index, 1);
        assert_eq!(args, ["terminal", "hotkey", "模型 01"]);
        assert_eq!(
            pages[index]
                .as_ref()
                .unwrap()
                .completion
                .as_deref()
                .unwrap(),
            ["terminal", "status"]
        );
        assert!(resolve(&pages, 0, &event("ctrl-alt-shift-k")).is_none());
    }

    #[test]
    fn invalid_bindings_and_conflicts_leave_button_actions_available() {
        // 同页混合正常/非法绑定，并在另一页添加完全重复或彼此冲突的声明。
        let first: PanelView = serde_json::from_value(json!({
            "sections": [{"title": "工具", "actions": [{"label": "执行", "args": ["p", "run"]}]}],
            "bindings": [
                {"key": "ctrl-alt-k", "args": ["p", "run"]},
                {"key": "tab", "args": ["p", "run"]},
                {"key": "ctrl-a", "args": ["other", "run"]},
                {"key": "f24", "args": ["p"]},
                {"key": "f1", "args": null},
                {"key": 32, "args": ["p", "run"]},
                {"key": "F2", "args": ["p", "run"]}
            ]
        }))
        .unwrap();
        assert_eq!(first.sections[0].actions[0].args, ["p", "run"]);
        let first = PageBindings::from_view(&first, "p");
        assert_eq!(first.shortcuts.len(), 1);
        let view = |plugin: &str| -> PanelView {
            serde_json::from_value(json!({
                "sections": [],
                "bindings": [{"key": "ctrl-alt-k", "args": [plugin, "run"]}]
            }))
            .unwrap()
        };
        let mut pages = [Some(first), Some(PageBindings::from_view(&view("p"), "p"))];
        let key = event("ctrl-alt-k");
        assert_eq!(resolve(&pages, 1, &key).unwrap().0, 1);
        pages[1] = Some(PageBindings::from_view(&view("other"), "other"));
        assert!(resolve(&pages, 1, &key).is_none());
    }

    #[test]
    fn accepts_only_canonical_supported_single_keys() {
        for key in ["ctrl-alt-shift-super-k", "f24", "pagedown", "0"] {
            assert!(parse_key(key).is_some(), "{key}");
        }
        for key in [
            "alt-ctrl-k",
            "ctrl-ctrl-k",
            "shift-tab",
            "cmd-k",
            "fn-f1",
            "f25",
            "ctrl-k x",
            "ctrl-k->x",
            "ctrl-a-release",
            "unknown",
            "",
        ] {
            assert!(parse_key(key).is_none(), "{key}");
        }
    }
}
