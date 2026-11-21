use serde::Serialize;
use serde_json::{Value, json};
use std::path::Path;

#[derive(Serialize)]
struct Action {
    label: String,
    args: Vec<String>,
    selected: bool,
}

#[derive(Serialize)]
struct Section {
    title: &'static str,
    actions: Vec<Action>,
    lines: Vec<String>,
    cycle: bool,
}

impl Section {
    fn new(title: &'static str) -> Self {
        Self {
            title,
            actions: Vec::new(),
            lines: Vec::new(),
            cycle: false,
        }
    }

    fn action(&mut self, label: impl Into<String>, args: &[&str], selected: bool) {
        self.actions.push(Action {
            label: label.into(),
            args: std::iter::once("terminal")
                .chain(args.iter().copied())
                .map(str::to_owned)
                .collect(),
            selected,
        });
    }
}

// VTube Studio serializes the chord as individual trigger names.
fn panel_key(keys: &[String]) -> Option<String> {
    let modifiers = ["ctrl", "alt", "shift", "super"];
    let mut selected = [false; 4];
    let mut key = None;
    for trigger in keys {
        let normalized = trigger.to_ascii_lowercase();
        let name = match normalized.as_str() {
            "control" | "leftcontrol" | "rightcontrol" | "leftctrl" | "rightctrl" => "ctrl",
            "leftalt" | "rightalt" => "alt",
            "leftshift" | "rightshift" => "shift",
            "command" | "leftwindows" | "rightwindows" => "super",
            "return" => "enter",
            "escape" => "escape",
            _ if normalized.len() == 6 && normalized.starts_with("alpha") => &normalized[5..],
            _ => &normalized,
        };
        if let Some(index) = modifiers.iter().position(|modifier| *modifier == name) {
            if std::mem::replace(&mut selected[index], true) {
                return None;
            }
        } else if ((name.len() == 1 && name.as_bytes()[0].is_ascii_alphanumeric())
            || name
                .strip_prefix('f')
                .and_then(|n| n.parse::<u8>().ok())
                .is_some_and(|n| (1..=24).contains(&n))
            || matches!(
                name,
                "space"
                    | "enter"
                    | "escape"
                    | "up"
                    | "down"
                    | "left"
                    | "right"
                    | "home"
                    | "end"
                    | "delete"
                    | "backspace"
                    | "insert"
                    | "pageup"
                    | "pagedown"
            ))
            && key.is_none()
        {
            key = Some(name.to_owned());
        } else {
            return None;
        }
    }
    let key = key?;
    let mut parts: Vec<_> = modifiers
        .into_iter()
        .zip(selected)
        .filter_map(|(name, selected)| selected.then_some(name))
        .collect();
    parts.push(&key);
    Some(parts.join("-"))
}

fn shortcut_section(snapshot: &Value) -> (Section, Vec<Value>) {
    let mut section = Section::new("模型快捷键");
    let mut bindings = Vec::new();
    let Some((path, session)) = snapshot["model"].as_str().zip(snapshot["session"].as_u64()) else {
        return (section, bindings);
    };
    let shortcuts = match super::vtube::load(Path::new(path)) {
        Ok(shortcuts) => shortcuts,
        Err(error) => {
            section.lines.push(error);
            return (section, bindings);
        }
    };
    let mut chords = std::collections::HashMap::new();
    for shortcut in &shortcuts {
        if shortcut.enabled && shortcut.unsupported_reason.is_none() {
            if let Some(key) = panel_key(&shortcut.keys) {
                *chords.entry(key).or_insert(0usize) += 1;
            }
        }
    }
    for shortcut in shortcuts {
        let active = shortcut
            .expression
            .as_ref()
            .and_then(|file| file.to_str())
            .is_some_and(|file| {
                strings(&snapshot["active_expression_toggles"]).any(|active| active == file)
            });
        let keys = shortcut.keys.join(" + ");
        let chord = panel_key(&shortcut.keys);
        let detail = if let Some(reason) = &shortcut.unsupported_reason {
            reason.as_str()
        } else if !shortcut.enabled {
            "已禁用"
        } else if !shortcut.keys.is_empty() && chord.is_none() {
            "按键不受支持"
        } else if chord
            .as_ref()
            .is_some_and(|key| chords.get(key).copied().unwrap_or(0) > 1)
        {
            "按键冲突"
        } else if active {
            "已激活"
        } else {
            "可用"
        };
        section.lines.push(format!(
            "{} · {} · {} · {}",
            shortcut.name, keys, shortcut.action, detail
        ));
        if shortcut.enabled && shortcut.unsupported_reason.is_none() {
            section.action(
                shortcut.name,
                &["toggle-expression", &session.to_string(), &shortcut.id],
                active,
            );
            if let Some(key) = chord.filter(|key| chords.get(key) == Some(&1)) {
                bindings.push(json!({"key": key, "args": ["terminal", "toggle-expression", session.to_string(), shortcut.id]}));
            }
        }
    }
    (section, bindings)
}

pub(super) fn describe(catalog: Result<String, String>, status: &str) -> Result<String, String> {
    let status: Value = serde_json::from_str(status).map_err(|error| error.to_string())?;
    let snapshot = &status["snapshot"];
    let model = snapshot["model"].as_str();
    let ready = snapshot["state"] == "ready" || snapshot["state"] == "starting";
    let mut summary = Section::new("运行状态");
    let state = match snapshot["state"].as_str() {
        Some("ready") => "已就绪",
        Some("starting") => "正在启动",
        Some("failed") => "启动失败",
        _ => "不可用",
    };
    let visible = if snapshot["visible"] == true {
        "可见"
    } else {
        "隐藏"
    };
    let output = snapshot["output"].as_str().unwrap_or("未选择");
    summary.lines.push(format!(
        "终端 · {state}    显示 · {visible}    输出 · {output}"
    ));
    summary
        .lines
        .push(format!("当前模型 · {}", model.unwrap_or("尚未加载")));
    if let Some(error) = snapshot["error"].as_str() {
        summary.lines.push(error.to_owned());
    }

    if let Some(error) = snapshot["transform_save_error"].as_str() {
        summary.lines.push(format!("缩放未保存 · {error}"));
        summary.action("重试保存缩放", &["save-transform"], false);
    }

    let mut models = Section::new("模型目录");
    models.cycle = true;
    match catalog {
        Ok(catalog) => {
            let catalog: Value =
                serde_json::from_str(&catalog).map_err(|error| error.to_string())?;
            for path in strings(&catalog["models"]).take(256) {
                if ready {
                    let name = Path::new(path)
                        .file_name()
                        .and_then(|name| name.to_str())
                        .unwrap_or(path);
                    models.action(name, &["load", path], model == Some(path));
                }
            }
            if models.actions.is_empty() {
                models.lines.push(
                    if ready {
                        "没有可用模型"
                    } else {
                        "渲染后端不可用"
                    }
                    .into(),
                );
            }
        }
        Err(error) => models.lines.push(error),
    }

    let mut motions = Section::new("动作");
    let mut expressions = Section::new("表情");
    let mut outputs = Section::new("输出设置");
    if ready {
        for group in snapshot["motions"].as_array().into_iter().flatten() {
            if let (Some(name), Some(count)) = (group["name"].as_str(), group["count"].as_u64()) {
                for index in 0..count.min(256 - motions.actions.len() as u64) {
                    motions.action(
                        format!("{name} · {}", index + 1),
                        &["motion", name, &index.to_string()],
                        false,
                    );
                }
            }
        }
        for name in strings(&snapshot["expressions"]) {
            expressions.action(name, &["expression", name], false);
        }
        let selected_output = snapshot["selected_output"].as_str();
        outputs.action("自动选择", &["output"], selected_output.is_none());
        outputs.action("移到焦点屏", &["focused-output"], false);
        for name in strings(&snapshot["outputs"]).take(128) {
            outputs.action(name, &["output", name], selected_output == Some(name));
        }
    }
    if motions.actions.is_empty() {
        motions.lines.push("当前模型没有可用动作".into());
    }
    if expressions.actions.is_empty() {
        expressions.lines.push("当前模型没有可用表情".into());
    }
    let edit = if ready && model.is_some() {
        let transform = &snapshot["transform"];
        let crop = &transform["crop"];
        let field = |id: &str, label: &str, value: &Value| json!({"id": id, "label": label, "value": if value.is_number() { value.to_string() } else { String::new() }});
        Some(json!({
            "begin": ["terminal", "edit", "begin"],
            "update": ["terminal", "edit", "update"],
            "apply": ["terminal", "edit", "apply"],
            "cancel": ["terminal", "edit", "cancel"],
            "fields": [
                field("x", "位置 X", &transform["x"]),
                field("y", "位置 Y", &transform["y"]),
                field("zoom", "缩放", &transform["zoom"]),
                field("crop-x", "裁剪 X", &crop["x"]),
                field("crop-y", "裁剪 Y", &crop["y"]),
                field("crop-width", "裁剪宽度", &crop["width"]),
                field("crop-height", "裁剪高度", &crop["height"]),
            ],
            "optional_start": 3,
        }))
    } else {
        None
    };
    let mut gpu = Section::new("渲染设备");
    if let Some(configured) = snapshot["configured_gpu"].as_str() {
        let active = &snapshot["active_gpu"];
        let active_label = active["pci"]
            .as_str()
            .or_else(|| active["render_node"].as_str());
        if let Some(active) = active_label {
            gpu.lines.push(format!("当前设备 · {active}"));
        }
        gpu.lines.push(format!("下次启动 · {configured}"));
        if snapshot["restart_required"] == true {
            gpu.lines.push("重启宿主后应用设备选择".into());
        }
        gpu.action("自动选择", &["gpu", "auto"], configured == "auto");
        for device in snapshot["gpu_devices"]
            .as_array()
            .into_iter()
            .flatten()
            .take(32)
        {
            if let Some(id) = device["pci"]
                .as_str()
                .or_else(|| device["render_node"].as_str())
            {
                gpu.action(id, &["gpu", id], configured == id);
            }
        }
    }
    let (shortcuts, bindings) = shortcut_section(snapshot);
    let result = json!({"sections": [summary, models, motions, expressions, outputs, gpu, shortcuts], "bindings": bindings, "completion_query": ["terminal", "status"], "edit": edit}).to_string();
    let escapes = result
        .bytes()
        .filter(|byte| matches!(byte, b'"' | b'\\'))
        .count();
    if result.len() + escapes + r#"{"status":"ok","output":""}"#.len() > 64 * 1024 {
        return Err("面板响应过大，请改用较小的模型目录".into());
    }
    Ok(result)
}

fn strings(value: &Value) -> impl Iterator<Item = &str> {
    value
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn page_preserves_action_arguments_and_current_selection() {
        // 载入带空格与中文的路径，检查通用控件拿到原路径、选中状态和完成查询。
        let path = "/模型/Sample Model.model3.json";
        let catalog = json!({"models": [path]}).to_string();
        let status = json!({"snapshot": {
            "state": "ready", "model": path,
            "motions": [{"name":"Idle", "count":2}], "expressions": ["F04"]
        }})
        .to_string();
        let page: Value = serde_json::from_str(&describe(Ok(catalog), &status).unwrap()).unwrap();
        assert_eq!(page["completion_query"], json!(["terminal", "status"]));
        let models = &page["sections"][1];
        assert_eq!(models["cycle"], true);
        assert_eq!(models["actions"][0]["selected"], true);
        assert_eq!(
            models["actions"][0]["args"],
            json!(["terminal", "load", path])
        );
        assert_eq!(
            page["sections"][2]["actions"][1]["args"],
            json!(["terminal", "motion", "Idle", "1"])
        );
    }
}
