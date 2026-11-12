use serde::de::{MapAccess, SeqAccess, Visitor};
use serde::{Deserialize, Deserializer};
use serde_json::{Map, Number, Value};
use std::env;
use std::fmt;
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::time::Duration;

const HELP: &str = "Control DeskWaifu over its newline-delimited JSON Unix socket.\n\nUsage: deskwaifu-ctl <command> [--key value ...]\n\nExamples:\n  deskwaifu-ctl get_status\n  deskwaifu-ctl set_model --name \"MyModel\"\n  deskwaifu-ctl set_model --index 2\n  deskwaifu-ctl do_motion --group TapBody --index 0 --priority 2\n  deskwaifu-ctl set_mouth_y --value 0.8\n  deskwaifu-ctl set_look --x 0.5 --y -0.3\n  deskwaifu-ctl set_look --reset\n  deskwaifu-ctl toggle_voice_input\n  deskwaifu-ctl get_voice_input_status\n  deskwaifu-ctl set_voice_input_settings --max_recording_seconds 60\n\nThe socket is $XDG_RUNTIME_DIR/deskwaifu.sock (fallback: /tmp/deskwaifu.sock).\n--reset is shorthand for --reset true. --stream_source is for long-lived raw sockets, not this one-shot CLI.\n";
const INTS: &[&str] = &[
    "index",
    "priority",
    "crop_x",
    "crop_y",
    "crop_width",
    "crop_height",
    "expected_revision",
    "max_recording_seconds",
];
const NUMBERS: &[&str] = &[
    "value",
    "x",
    "y",
    "zoom",
    "radius",
    "opacity",
    "idle_opacity",
    "fullscreen_opacity",
];
const BOOLS: &[&str] = &[
    "reset",
    "stream_source",
    "screen",
    "overwrite",
    "cropped",
    "enabled",
    "fullscreen_enabled",
];

fn parameter(key: &str, text: &str) -> Result<Value, String> {
    if INTS.contains(&key) {
        if text.is_empty()
            || text == "-"
            || (text.starts_with('0') && text.len() > 1)
            || text.starts_with("-0") && text.len() > 2
            || !text
                .trim_start_matches('-')
                .bytes()
                .all(|b| b.is_ascii_digit())
        {
            return Err(format!("--{key} requires an integer"));
        }
        let n: i64 = text
            .parse()
            .map_err(|_| format!("--{key} is out of range"))?;
        let (min, max) = if key == "expected_revision" {
            (0, u32::MAX as i64 - 1)
        } else {
            (i32::MIN as i64, i32::MAX as i64)
        };
        if n < min || n > max {
            return Err(format!("--{key} is out of range"));
        }
        return Ok(Value::Number(Number::from(n)));
    }
    if NUMBERS.contains(&key) {
        let v: Value =
            serde_json::from_str(text).map_err(|_| format!("--{key} requires a finite number"))?;
        return match v {
            Value::Number(n)
                if n.as_i64().is_some()
                    || n.as_u64().is_some()
                    || n.as_f64().is_some_and(f64::is_finite) =>
            {
                Ok(Value::Number(n))
            }
            _ => Err(format!("--{key} requires a finite number")),
        };
    }
    if BOOLS.contains(&key) {
        return match text {
            "true" | "1" => Ok(Value::Bool(true)),
            "false" | "0" => Ok(Value::Bool(false)),
            _ => Err(format!("--{key} requires true or false")),
        };
    }
    Ok(Value::String(text.to_owned()))
}

struct UniqueValue(Value);
impl<'de> Deserialize<'de> for UniqueValue {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        struct V;
        impl<'de> Visitor<'de> for V {
            type Value = UniqueValue;
            fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
                f.write_str("JSON value without duplicate object keys")
            }
            fn visit_unit<E: serde::de::Error>(self) -> Result<Self::Value, E> {
                Ok(UniqueValue(Value::Null))
            }
            fn visit_bool<E: serde::de::Error>(self, v: bool) -> Result<Self::Value, E> {
                Ok(UniqueValue(Value::Bool(v)))
            }
            fn visit_i64<E: serde::de::Error>(self, v: i64) -> Result<Self::Value, E> {
                Ok(UniqueValue(v.into()))
            }
            fn visit_u64<E: serde::de::Error>(self, v: u64) -> Result<Self::Value, E> {
                Ok(UniqueValue(v.into()))
            }
            fn visit_f64<E: serde::de::Error>(self, v: f64) -> Result<Self::Value, E> {
                Number::from_f64(v)
                    .map(|n| UniqueValue(Value::Number(n)))
                    .ok_or_else(|| E::custom("nonfinite number"))
            }
            fn visit_str<E: serde::de::Error>(self, v: &str) -> Result<Self::Value, E> {
                Ok(UniqueValue(v.into()))
            }
            fn visit_string<E: serde::de::Error>(self, v: String) -> Result<Self::Value, E> {
                Ok(UniqueValue(Value::String(v)))
            }
            fn visit_seq<A: SeqAccess<'de>>(self, mut a: A) -> Result<Self::Value, A::Error> {
                let mut v = vec![];
                while let Some(x) = a.next_element::<UniqueValue>()? {
                    v.push(x.0)
                }
                Ok(UniqueValue(Value::Array(v)))
            }
            fn visit_map<A: MapAccess<'de>>(self, mut a: A) -> Result<Self::Value, A::Error> {
                let mut m = Map::new();
                while let Some((k, v)) = a.next_entry::<String, UniqueValue>()? {
                    if m.insert(k.clone(), v.0).is_some() {
                        return Err(serde::de::Error::custom(format!(
                            "Duplicate response key: {k}"
                        )));
                    }
                }
                Ok(UniqueValue(Value::Object(m)))
            }
        }
        d.deserialize_any(V)
    }
}

fn run(args: &[String]) -> Result<i32, String> {
    if args.is_empty() {
        return Err(HELP.to_owned());
    }
    if args == ["--help"] {
        print!("{HELP}");
        return Ok(0);
    }
    let mut req = Map::new();
    req.insert("command".into(), Value::String(args[0].clone()));
    let mut i = 1;
    while i < args.len() {
        let option = &args[i];
        if !option.starts_with("--") || option.len() == 2 {
            return Err(format!("Expected --key, got {option:?}"));
        }
        let key = &option[2..];
        if req.contains_key(key) {
            return Err(format!("Duplicate or reserved option: {option}"));
        }
        i += 1;
        if key == "reset" && (i == args.len() || args[i].starts_with("--")) {
            req.insert(key.into(), Value::Bool(true));
            continue;
        }
        if i == args.len() {
            return Err(format!("Missing value for {option}"));
        }
        req.insert(key.into(), parameter(key, &args[i])?);
        i += 1;
    }
    let path = env::var_os("XDG_RUNTIME_DIR")
        .filter(|p| !p.is_empty())
        .map(|p| std::path::PathBuf::from(p).join("deskwaifu.sock"))
        .unwrap_or_else(|| "/tmp/deskwaifu.sock".into());
    let mut stream = UnixStream::connect(path).map_err(|e| e.to_string())?;
    stream
        .set_read_timeout(Some(Duration::from_secs(5)))
        .map_err(|e| e.to_string())?;
    stream
        .set_write_timeout(Some(Duration::from_secs(5)))
        .map_err(|e| e.to_string())?;
    serde_json::to_writer(&mut stream, &Value::Object(req)).map_err(|e| e.to_string())?;
    stream.write_all(b"\n").map_err(|e| e.to_string())?;
    let mut line = Vec::new();
    BufReader::new(stream)
        .read_until(b'\n', &mut line)
        .map_err(|e| e.to_string())?;
    if !line.ends_with(b"\n") {
        return Err("Empty or truncated response (missing newline)".into());
    }
    let response: UniqueValue =
        serde_json::from_slice(&line[..line.len() - 1]).map_err(|e| e.to_string())?;
    let obj = response
        .0
        .as_object()
        .ok_or("Response must be an object with a boolean 'ok' field")?;
    let ok = obj
        .get("ok")
        .and_then(Value::as_bool)
        .ok_or("Response must be an object with a boolean 'ok' field")?;
    println!(
        "{}",
        serde_json::to_string_pretty(&response.0).map_err(|e| e.to_string())?
    );
    Ok(if ok { 0 } else { 1 })
}
fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    match run(&args) {
        Ok(code) => std::process::exit(code),
        Err(e) => {
            eprintln!("Error: {e}");
            std::process::exit(1)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn types_and_ranges() {
        assert_eq!(parameter("index", "-2").unwrap(), json!(-2));
        assert!(parameter("index", "2147483648").is_err());
        assert!(parameter("expected_revision", "4294967295").is_err());
        assert_eq!(
            parameter("expected_revision", "4294967294").unwrap(),
            json!(4294967294u64)
        );
        assert!(parameter("value", "NaN").is_err());
        assert_eq!(parameter("enabled", "1").unwrap(), json!(true));
        assert_eq!(parameter("name", "001").unwrap(), json!("001"));
    }
    #[test]
    fn duplicate_and_reserved() {
        assert!(run(&["cmd".into(), "--command".into(), "x".into()])
            .unwrap_err()
            .contains("reserved"));
        assert!(run(&[
            "cmd".into(),
            "--x".into(),
            "1".into(),
            "--x".into(),
            "2".into()
        ])
        .unwrap_err()
        .contains("Duplicate"));
    }
    #[test]
    fn duplicate_response() {
        assert!(serde_json::from_str::<UniqueValue>(r#"{"ok":true,"ok":false}"#).is_err());
    }
    #[test]
    fn response_semantics() {
        for s in [r#"{"ok":true}"#, r#"{"ok":false}"#] {
            let v: UniqueValue = serde_json::from_str(s).unwrap();
            assert!(v.0["ok"].is_boolean());
        }
        assert!(serde_json::from_str::<UniqueValue>(r#"{"ok":true}"#)
            .unwrap()
            .0["ok"]
            .as_bool()
            .unwrap());
        assert!(serde_json::from_str::<UniqueValue>(r#"{"ok":1}"#)
            .unwrap()
            .0["ok"]
            .as_bool()
            .is_none());
        assert!(serde_json::from_slice::<UniqueValue>(b"{\"ok\":true").is_err());
    }
}
