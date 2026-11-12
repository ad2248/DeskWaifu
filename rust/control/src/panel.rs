//! Pure, window-free parts of the control panel: request payloads, numeric
//! input validation, and reply parsing. Every command here is one the Rust
//! runtime in `rust/app` actually serves.

use deskwaifu_client::Response;
use serde_json::{json, Map, Value};

pub const ZOOM_MIN: f32 = 0.1;
pub const ZOOM_MAX: f32 = 10.0;
pub const STEP: f32 = 0.1;

pub fn status_request() -> Value {
    json!({ "command": "get_status" })
}

pub fn models_request() -> Value {
    json!({ "command": "get_available_models" })
}

pub fn current_model_request() -> Value {
    json!({ "command": "get_current_model" })
}

pub fn position_request() -> Value {
    json!({ "command": "get_model_position" })
}

pub fn control_status_request() -> Value {
    json!({ "command": "get_control_status" })
}

pub fn set_model_request(name: &str) -> Value {
    json!({ "command": "set_model", "name": name })
}

/// `next` selects `next_model`, otherwise `prev_model`.
pub fn cycle_model_request(next: bool) -> Value {
    json!({ "command": if next { "next_model" } else { "prev_model" } })
}

pub fn zoom_request(value: f32) -> Value {
    let mut request = Map::new();
    request.insert("command".into(), "set_model_zoom".into());
    request.insert("value".into(), number(value));
    Value::Object(request)
}

/// Only the axes that were provided are included: the runtime keeps the
/// current value for a missing axis.
pub fn position_set_request(x: Option<f32>, y: Option<f32>) -> Value {
    let mut request = Map::new();
    request.insert("command".into(), "set_model_position".into());
    if let Some(x) = x {
        request.insert("x".into(), number(x));
    }
    if let Some(y) = y {
        request.insert("y".into(), number(y));
    }
    Value::Object(request)
}

pub fn toggle_hidden_request() -> Value {
    json!({ "command": "toggle_hidden" })
}

/// One-shot control writes use the persistent source, never a stream source.
pub fn mouth_request(value: f32) -> Value {
    control_request("set_mouth_y", &[("value", value)])
}

pub fn look_request(x: f32, y: f32) -> Value {
    control_request("set_look", &[("x", x), ("y", y)])
}

/// `set_look` with `reset` clears the persistent look source, which returns the
/// model to mouse following; it is not the same as releasing every channel.
pub fn look_reset_request() -> Value {
    json!({ "command": "set_look", "reset": true, "stream_source": false })
}

pub fn release_request() -> Value {
    json!({ "command": "release_source", "stream_source": false })
}

fn control_request(command: &str, fields: &[(&str, f32)]) -> Value {
    let mut request = Map::new();
    request.insert("command".into(), command.into());
    for (key, value) in fields {
        request.insert((*key).into(), number(*value));
    }
    request.insert("stream_source".into(), Value::Bool(false));
    Value::Object(request)
}

/// Two decimals is enough for every step the panel offers and keeps repeated
/// `+ 0.1` increments from accumulating visible float noise.
pub fn format_number(value: f32) -> String {
    format!("{value:.2}")
}

/// Payload numbers go out at the precision the panel shows, so the wire value is
/// `0.7` rather than the widened binary form of `0.7f32`.
fn number(value: f32) -> Value {
    format_number(value)
        .parse::<f64>()
        .ok()
        .and_then(serde_json::Number::from_f64)
        .map(Value::Number)
        .unwrap_or(Value::Null)
}

pub fn parse_zoom(text: &str) -> Result<f32, String> {
    let value = parse_finite(text, "zoom")?;
    if !(ZOOM_MIN..=ZOOM_MAX).contains(&value) {
        return Err(format!("zoom must be {ZOOM_MIN}..{ZOOM_MAX}"));
    }
    Ok(value)
}

pub fn parse_offset(text: &str) -> Result<f32, String> {
    parse_finite(text, "offset")
}

pub fn parse_look(text: &str) -> Result<f32, String> {
    parse_finite(text, "look")
}

/// The runtime clamps in range but rejects non-finite values, so the panel
/// validates first and clamps to the documented `0.0..=1.0` range.
pub fn parse_mouth(text: &str) -> Result<f32, String> {
    Ok(parse_finite(text, "mouth")?.clamp(0.0, 1.0))
}

fn parse_finite(text: &str, field: &str) -> Result<f32, String> {
    let value: f32 = text
        .trim()
        .parse()
        .map_err(|_| format!("{field} must be a number"))?;
    if !value.is_finite() {
        return Err(format!("{field} must be finite"));
    }
    Ok(value)
}

pub fn zoom_step(current: f32, delta: f32) -> Result<f32, String> {
    parse_zoom(&format_number(current + delta))
}

pub fn offset_step(current: f32, delta: f32) -> Result<f32, String> {
    parse_offset(&format_number(current + delta))
}

pub fn look_step(current: f32, delta: f32) -> Result<f32, String> {
    parse_look(&format_number(current + delta))
}

/// The server's own diagnostic for a rejected command, or a fallback when the
/// reply carries no readable `error` string.
pub fn response_error(response: &Response) -> Option<String> {
    if response.ok {
        return None;
    }
    Some(
        response
            .value
            .get("error")
            .and_then(Value::as_str)
            .unwrap_or("command failed without an error message")
            .to_owned(),
    )
}

fn checked(response: &Response) -> Result<&Value, String> {
    match response_error(response) {
        Some(error) => Err(error),
        None => Ok(&response.value),
    }
}

fn string_field(value: &Value, key: &str) -> Result<String, String> {
    value
        .get(key)
        .and_then(Value::as_str)
        .map(str::to_owned)
        .ok_or_else(|| format!("reply lacks a string '{key}'"))
}

fn u64_field(value: &Value, key: &str) -> Result<u64, String> {
    value
        .get(key)
        .and_then(Value::as_u64)
        .ok_or_else(|| format!("reply lacks an unsigned '{key}'"))
}

fn f32_field(value: &Value, key: &str) -> Result<f32, String> {
    let number = value
        .get(key)
        .and_then(Value::as_f64)
        .ok_or_else(|| format!("reply lacks a numeric '{key}'"))?;
    if !number.is_finite() || !(number as f32).is_finite() {
        return Err(format!("'{key}' must be finite"));
    }
    Ok(number as f32)
}

fn bool_field(value: &Value, key: &str) -> Result<bool, String> {
    value
        .get(key)
        .and_then(Value::as_bool)
        .ok_or_else(|| format!("reply lacks a boolean '{key}'"))
}

#[derive(Clone, Debug, PartialEq)]
pub struct Status {
    pub model: String,
    pub model_index: u64,
    pub model_count: u64,
    pub hidden: bool,
    pub zoom: f32,
    pub x: f32,
    pub y: f32,
    pub renderer_available: bool,
}

pub fn parse_status(response: &Response) -> Result<Status, String> {
    let value = checked(response)?;
    Ok(Status {
        model: string_field(value, "model")?,
        model_index: u64_field(value, "model_index")?,
        model_count: u64_field(value, "model_count")?,
        hidden: bool_field(value, "hidden")?,
        zoom: f32_field(value, "zoom")?,
        x: f32_field(value, "x")?,
        y: f32_field(value, "y")?,
        renderer_available: bool_field(value, "renderer_available")?,
    })
}

#[derive(Clone, Debug, PartialEq)]
pub struct Transform {
    pub zoom: f32,
    pub x: f32,
    pub y: f32,
}

impl Default for Transform {
    fn default() -> Self {
        Self {
            zoom: 1.0,
            x: 0.0,
            y: 0.0,
        }
    }
}

pub fn parse_transform(status: &Status) -> Transform {
    Transform {
        zoom: status.zoom,
        x: status.x,
        y: status.y,
    }
}

pub fn parse_position(response: &Response) -> Result<Transform, String> {
    let value = checked(response)?;
    Ok(Transform {
        zoom: f32_field(value, "zoom")?,
        x: f32_field(value, "x")?,
        y: f32_field(value, "y")?,
    })
}

pub fn parse_models(response: &Response) -> Result<Vec<String>, String> {
    let value = checked(response)?;
    let models = value
        .get("models")
        .and_then(Value::as_array)
        .ok_or("reply lacks a 'models' array")?;
    models
        .iter()
        .enumerate()
        .map(|(index, model)| {
            model
                .as_str()
                .map(str::to_owned)
                .ok_or_else(|| format!("model #{index} is not a string"))
        })
        .collect()
}

pub fn parse_current_model(response: &Response) -> Result<String, String> {
    string_field(checked(response)?, "model")
}

pub fn parse_hidden(response: &Response) -> Result<bool, String> {
    bool_field(checked(response)?, "hidden")
}

#[derive(Clone, Debug, PartialEq)]
pub struct ControlStatus {
    pub has_mouth: bool,
    pub mouth: f32,
    pub has_look: bool,
    pub look_x: f32,
    pub look_y: f32,
}

pub fn parse_control_status(response: &Response) -> Result<ControlStatus, String> {
    let value = checked(response)?;
    Ok(ControlStatus {
        has_mouth: bool_field(value, "has_mouth")?,
        mouth: f32_field(value, "mouth")?,
        has_look: bool_field(value, "has_look")?,
        look_x: f32_field(value, "look_x")?,
        look_y: f32_field(value, "look_y")?,
    })
}

/// Applies a parsed reply only when it succeeded. A failure leaves the prior
/// value intact so a transient error never erases what the panel already knows.
pub fn merge<T>(slot: &mut T, result: Result<T, String>) -> Result<(), String> {
    match result {
        Ok(value) => {
            *slot = value;
            Ok(())
        }
        Err(error) => Err(error),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn reply(value: Value) -> Response {
        Response {
            ok: value
                .get("ok")
                .and_then(Value::as_bool)
                .expect("test replies carry 'ok'"),
            value,
        }
    }

    #[test]
    fn zoom_bounds_are_inclusive() {
        assert_eq!(parse_zoom("0.1"), Ok(0.1));
        assert_eq!(parse_zoom("10.0"), Ok(10.0));
        assert_eq!(parse_zoom("1"), Ok(1.0));
        assert!(parse_zoom("0.099").is_err());
        assert!(parse_zoom("10.01").is_err());
    }

    #[test]
    fn zoom_rejects_non_finite_and_garbage() {
        for text in ["NaN", "inf", "-inf", "abc", ""] {
            assert!(parse_zoom(text).is_err(), "{text} must be rejected");
        }
        assert!(parse_offset("NaN").is_err());
        assert!(parse_offset("inf").is_err());
        assert!(parse_mouth("NaN").is_err());
        assert!(parse_look("inf").is_err());
    }

    #[test]
    fn zoom_steps_stay_inside_the_range() {
        assert_eq!(zoom_step(1.0, STEP), Ok(1.1));
        assert_eq!(zoom_step(9.9, STEP), Ok(10.0));
        assert!(zoom_step(10.0, STEP).is_err());
        assert!(zoom_step(0.1, -STEP).is_err());
    }

    #[test]
    fn mouth_is_validated_then_clamped() {
        assert_eq!(parse_mouth("0.4"), Ok(0.4));
        assert_eq!(parse_mouth("-0.2"), Ok(0.0));
        assert_eq!(parse_mouth("1.7"), Ok(1.0));
        assert!(parse_mouth("loud").is_err());
    }

    #[test]
    fn partial_position_updates_omit_missing_axes() {
        assert_eq!(
            position_set_request(Some(0.5), None),
            json!({ "command": "set_model_position", "x": 0.5 })
        );
        assert_eq!(
            position_set_request(None, Some(-0.5)),
            json!({ "command": "set_model_position", "y": -0.5 })
        );
        assert_eq!(
            position_set_request(Some(0.5), Some(-0.5)),
            json!({ "command": "set_model_position", "x": 0.5, "y": -0.5 })
        );
    }

    #[test]
    fn command_payloads_match_the_runtime_handlers() {
        assert_eq!(status_request(), json!({ "command": "get_status" }));
        assert_eq!(
            models_request(),
            json!({ "command": "get_available_models" })
        );
        assert_eq!(
            current_model_request(),
            json!({ "command": "get_current_model" })
        );
        assert_eq!(
            position_request(),
            json!({ "command": "get_model_position" })
        );
        assert_eq!(
            control_status_request(),
            json!({ "command": "get_control_status" })
        );
        assert_eq!(
            set_model_request("Yuki"),
            json!({ "command": "set_model", "name": "Yuki" })
        );
        assert_eq!(
            cycle_model_request(true),
            json!({ "command": "next_model" })
        );
        assert_eq!(
            cycle_model_request(false),
            json!({ "command": "prev_model" })
        );
        assert_eq!(
            zoom_request(2.5),
            json!({ "command": "set_model_zoom", "value": 2.5 })
        );
        assert_eq!(
            toggle_hidden_request(),
            json!({ "command": "toggle_hidden" })
        );
        assert_eq!(
            mouth_request(0.7),
            json!({
                "command": "set_mouth_y",
                "value": 0.7,
                "stream_source": false
            })
        );
        assert_eq!(
            look_request(0.2, -0.3),
            json!({
                "command": "set_look",
                "x": 0.2,
                "y": -0.3,
                "stream_source": false
            })
        );
        assert_eq!(
            release_request(),
            json!({ "command": "release_source", "stream_source": false })
        );
        assert_eq!(
            look_reset_request(),
            json!({
                "command": "set_look",
                "reset": true,
                "stream_source": false
            })
        );
    }

    #[test]
    fn status_parsing_reads_every_field() {
        let response = reply(json!({
            "ok": true,
            "renderer_available": false,
            "model_core_ready": true,
            "model": "Yuki",
            "model_index": 1,
            "model_count": 3,
            "hidden": false,
            "zoom": 1.5,
            "x": -12.0,
            "y": 34.0,
        }));
        let status = parse_status(&response).expect("well-formed status");
        assert_eq!(
            status,
            Status {
                model: "Yuki".into(),
                model_index: 1,
                model_count: 3,
                hidden: false,
                zoom: 1.5,
                x: -12.0,
                y: 34.0,
                renderer_available: false,
            }
        );
        assert_eq!(
            parse_transform(&status),
            Transform {
                zoom: 1.5,
                x: -12.0,
                y: 34.0
            }
        );
    }

    #[test]
    fn status_parsing_rejects_missing_or_wrongly_typed_fields() {
        let missing = reply(json!({ "ok": true, "model": "Yuki" }));
        assert!(parse_status(&missing).is_err());
        let wrong_type = reply(json!({
            "ok": true,
            "renderer_available": false,
            "model": "Yuki",
            "model_index": "1",
            "model_count": 3,
            "hidden": false,
            "zoom": 1.0,
            "x": 0.0,
            "y": 0.0,
        }));
        assert!(parse_status(&wrong_type).is_err());
    }

    #[test]
    fn model_list_parsing_handles_well_formed_and_malformed_replies() {
        let good = reply(json!({ "ok": true, "models": ["Yuki", "Mao"] }));
        assert_eq!(
            parse_models(&good),
            Ok(vec!["Yuki".to_owned(), "Mao".to_owned()])
        );

        let empty = reply(json!({ "ok": true, "models": [] }));
        assert_eq!(parse_models(&empty), Ok(Vec::new()));

        let not_array = reply(json!({ "ok": true, "models": "Yuki" }));
        assert!(parse_models(&not_array).is_err());

        let not_strings = reply(json!({ "ok": true, "models": ["Yuki", 7] }));
        assert!(parse_models(&not_strings).is_err());

        let absent = reply(json!({ "ok": true }));
        assert!(parse_models(&absent).is_err());

        let rejected = reply(json!({ "ok": false, "error": "no models loaded" }));
        assert_eq!(parse_models(&rejected), Err("no models loaded".to_owned()));
    }

    #[test]
    fn current_model_and_hidden_parse() {
        assert_eq!(
            parse_current_model(&reply(json!({ "ok": true, "model": "Mao" }))),
            Ok("Mao".to_owned())
        );
        assert!(parse_current_model(&reply(json!({ "ok": true }))).is_err());
        assert_eq!(
            parse_hidden(&reply(json!({ "ok": true, "hidden": true }))),
            Ok(true)
        );
        assert!(parse_hidden(&reply(json!({ "ok": true, "hidden": 1 }))).is_err());
    }

    #[test]
    fn control_status_parsing_covers_present_and_missing_fields() {
        let full = reply(json!({
            "ok": true,
            "has_mouth": true,
            "mouth": 0.25,
            "has_look": true,
            "look_x": -0.5,
            "look_y": 0.75,
        }));
        assert_eq!(
            parse_control_status(&full),
            Ok(ControlStatus {
                has_mouth: true,
                mouth: 0.25,
                has_look: true,
                look_x: -0.5,
                look_y: 0.75,
            })
        );

        let partial = reply(json!({ "ok": true, "has_mouth": false }));
        assert!(parse_control_status(&partial).is_err());

        let missing_look = reply(json!({
            "ok": true,
            "has_mouth": true,
            "mouth": 0.25,
            "has_look": true,
            "look_x": 0.0,
        }));
        assert!(parse_control_status(&missing_look).is_err());
    }

    #[test]
    fn position_parsing_requires_all_three_values() {
        assert_eq!(
            parse_position(&reply(
                json!({ "ok": true, "x": 1.0, "y": 2.0, "zoom": 3.0 })
            )),
            Ok(Transform {
                zoom: 3.0,
                x: 1.0,
                y: 2.0
            })
        );
        assert!(parse_position(&reply(json!({ "ok": true, "x": 1.0, "y": 2.0 }))).is_err());
    }

    #[test]
    fn rejected_replies_expose_the_server_error() {
        let rejected = reply(json!({ "ok": false, "error": "value must be 0.1..10.0" }));
        assert_eq!(
            response_error(&rejected),
            Some("value must be 0.1..10.0".to_owned())
        );
        assert_eq!(
            response_error(&reply(json!({ "ok": false }))),
            Some("command failed without an error message".to_owned())
        );
        assert_eq!(
            response_error(&reply(json!({ "ok": true, "x": 1.0 }))),
            None
        );
        assert_eq!(
            parse_status(&rejected),
            Err("value must be 0.1..10.0".to_owned())
        );
    }

    #[test]
    fn merge_keeps_prior_state_when_a_reply_fails() {
        let mut known = Status {
            model: "Yuki".into(),
            model_index: 0,
            model_count: 2,
            hidden: false,
            zoom: 1.0,
            x: 0.0,
            y: 0.0,
            renderer_available: false,
        };
        let before = known.clone();

        let failure = merge(
            &mut known,
            parse_status(&reply(json!({ "ok": false, "error": "busy" }))),
        );
        assert_eq!(failure, Err("busy".to_owned()));
        assert_eq!(known, before);

        let mut models = vec!["Yuki".to_owned()];
        let failure = merge(
            &mut models,
            parse_models(&reply(json!({ "ok": false, "error": "busy" }))),
        );
        assert_eq!(failure, Err("busy".to_owned()));
        assert_eq!(models, vec!["Yuki".to_owned()]);

        let ok = merge(
            &mut known,
            parse_status(&reply(json!({
                "ok": true,
                "renderer_available": false,
                "model": "Mao",
                "model_index": 1,
                "model_count": 2,
                "hidden": true,
                "zoom": 2.0,
                "x": 5.0,
                "y": -5.0,
            }))),
        );
        assert_eq!(ok, Ok(()));
        assert_eq!(known.model, "Mao");
        assert!(known.hidden);
    }
}
