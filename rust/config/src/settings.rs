//! Pure JSON document parsing and the desktop LAppConfig value contract.

use std::collections::BTreeMap;

/// The original document and canonical JSON fragments, retaining unknown values
/// without changing number spellings, string escapes, or nested member order.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConfigDocument {
    pub document: String,
    pub values: BTreeMap<String, String>,
}

impl ConfigDocument {
    /// Parse an object, rejecting decoded duplicate keys at every depth.
    pub fn parse(text: &str, allow_comments: bool) -> Result<Self, String> {
        let mut parser = Parser {
            text,
            pos: 0,
            allow_comments,
        };
        let values = parser.object(0)?;
        parser.skip();
        if parser.pos != text.len() {
            return Err("Trailing JSON content".into());
        }
        Ok(Self {
            document: text.into(),
            values,
        })
    }
}

struct Parser<'a> {
    text: &'a str,
    pos: usize,
    allow_comments: bool,
}
impl Parser<'_> {
    fn peek(&self) -> Option<u8> {
        self.text.as_bytes().get(self.pos).copied()
    }
    fn skip(&mut self) {
        loop {
            while matches!(self.peek(), Some(b' ' | b'\t' | b'\r' | b'\n')) {
                self.pos += 1;
            }
            if !self.allow_comments || !self.text[self.pos..].starts_with("//") {
                break;
            }
            while self.peek().is_some_and(|b| b != b'\n') {
                self.pos += 1;
            }
        }
    }
    fn take(&mut self, byte: u8) -> bool {
        self.skip();
        if self.peek() == Some(byte) {
            self.pos += 1;
            true
        } else {
            false
        }
    }
    fn string(&mut self) -> Result<String, String> {
        self.skip();
        let start = self.pos;
        if !self.take(b'"') {
            return Err("Expected JSON string".into());
        }
        while let Some(b) = self.peek() {
            self.pos += 1;
            if b == b'\\' {
                if self.peek().is_none() {
                    break;
                }
                self.pos += 1;
            } else if b == b'"' {
                return serde_json::from_str(&self.text[start..self.pos])
                    .map_err(|e| e.to_string());
            }
        }
        Err("Unterminated JSON string".into())
    }
    fn object(&mut self, depth: usize) -> Result<BTreeMap<String, String>, String> {
        if depth > 64 || !self.take(b'{') {
            return Err("Expected JSON object (maximum depth 64)".into());
        }
        let mut fields = BTreeMap::new();
        if self.take(b'}') {
            return Ok(fields);
        }
        loop {
            let key = self.string()?;
            if !self.take(b':') {
                return Err("Expected colon".into());
            }
            self.skip();
            let start = self.pos;
            self.value(depth + 1)?;
            let raw = canonical(&self.text[start..self.pos]);
            if fields.insert(key, raw).is_some() {
                return Err("Duplicate decoded JSON key".into());
            }
            if self.take(b'}') {
                return Ok(fields);
            }
            if !self.take(b',') {
                return Err("Expected comma or closing brace".into());
            }
        }
    }
    fn value(&mut self, depth: usize) -> Result<(), String> {
        self.skip();
        if depth > 64 {
            return Err("Maximum JSON depth 64".into());
        }
        match self.peek() {
            Some(b'{') => {
                self.object(depth)?;
            }
            Some(b'"') => {
                self.string()?;
            }
            Some(b'[') => {
                self.pos += 1;
                if self.take(b']') {
                    return Ok(());
                }
                loop {
                    self.value(depth + 1)?;
                    if self.take(b']') {
                        break;
                    }
                    if !self.take(b',') {
                        return Err("Expected comma or closing bracket".into());
                    }
                }
            }
            Some(b't' | b'f' | b'n') => {
                let literal = match self.peek() {
                    Some(b't') => "true",
                    Some(b'f') => "false",
                    _ => "null",
                };
                if !self.text[self.pos..].starts_with(literal) {
                    return Err("Invalid JSON literal".into());
                }
                self.pos += literal.len();
            }
            _ => self.number()?,
        }
        Ok(())
    }
    // Validate syntax without converting: unknown JSON numbers may exceed f64.
    fn number(&mut self) -> Result<(), String> {
        if self.peek() == Some(b'-') {
            self.pos += 1;
        }
        match self.peek() {
            Some(b'0') => self.pos += 1,
            Some(b'1'..=b'9') => self.digits(),
            _ => return Err("Invalid JSON number".into()),
        }
        if self.peek() == Some(b'.') {
            self.pos += 1;
            let start = self.pos;
            self.digits();
            if start == self.pos {
                return Err("Invalid JSON fraction".into());
            }
        }
        if matches!(self.peek(), Some(b'e' | b'E')) {
            self.pos += 1;
            if matches!(self.peek(), Some(b'+' | b'-')) {
                self.pos += 1;
            }
            let start = self.pos;
            self.digits();
            if start == self.pos {
                return Err("Invalid JSON exponent".into());
            }
        }
        Ok(())
    }
    fn digits(&mut self) {
        while self.peek().is_some_and(|b| b.is_ascii_digit()) {
            self.pos += 1;
        }
    }
}

fn canonical(text: &str) -> String {
    let mut out = String::new();
    let mut chars = text.chars().peekable();
    let mut quoted = false;
    while let Some(c) = chars.next() {
        if quoted {
            out.push(c);
            if c == '\\' {
                if let Some(c) = chars.next() {
                    out.push(c);
                }
            } else if c == '"' {
                quoted = false;
            }
        } else if c == '"' {
            quoted = true;
            out.push(c);
        } else if c == '/' && chars.peek() == Some(&'/') {
            for c in chars.by_ref() {
                if c == '\n' {
                    break;
                }
            }
        } else if !matches!(c, ' ' | '\t' | '\r' | '\n') {
            out.push(c);
        }
    }
    out
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ModelRegion {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Settings {
    pub additional_model_dirs: Vec<String>,
    pub default_model: String,
    pub model_output: String,
    pub render_device: String,
    pub plugin_dir: String,
    pub emotion_timeout: f32,
    pub model_scale: f32,
    pub model_x: f32,
    pub model_y: f32,
    pub window_width: i32,
    pub window_height: i32,
    pub hover_opacity_enabled: bool,
    pub hover_opacity: f32,
    pub idle_opacity: f32,
    pub fullscreen_opacity_enabled: bool,
    pub fullscreen_opacity: f32,
    pub mouse_look_radius: f32,
    pub model_region: Option<ModelRegion>,
    pub unknown_fields: BTreeMap<String, String>,
}
impl Default for Settings {
    fn default() -> Self {
        Self {
            additional_model_dirs: vec![],
            default_model: String::new(),
            model_output: String::new(),
            render_device: String::new(),
            plugin_dir: "/usr/lib/deskwaifu/plugins".into(),
            emotion_timeout: 5.0,
            model_scale: 1.0,
            model_x: 0.0,
            model_y: 0.0,
            window_width: 1900,
            window_height: 1000,
            hover_opacity_enabled: false,
            hover_opacity: 0.5,
            idle_opacity: 1.0,
            fullscreen_opacity_enabled: false,
            fullscreen_opacity: 0.5,
            mouse_look_radius: 0.5,
            model_region: None,
            unknown_fields: BTreeMap::new(),
        }
    }
}
impl Settings {
    /// Decode into fresh defaults, replacing the target only after full validation.
    pub fn decode(doc: &ConfigDocument, target: &mut Self) -> Result<(), String> {
        let mut next = Self::default();
        let mut fields = doc.values.clone();
        macro_rules! typed {
            ($field:ident) => {
                if let Some(raw) = fields.remove(stringify!($field)) {
                    next.$field = serde_json::from_str(&raw)
                        .map_err(|_| format!("Invalid {}", stringify!($field)))?;
                }
            };
        }
        typed!(additional_model_dirs);
        typed!(default_model);
        typed!(model_output);
        typed!(render_device);
        typed!(plugin_dir);
        if !valid_device(&next.render_device) {
            return Err("Invalid render_device: expected a PCI address string or auto".into());
        }
        if next.plugin_dir.contains('\0') {
            return Err("Invalid plugin_dir: expected a directory path string".into());
        }
        // C++ stream conversion requires the entire token and finite float results.
        macro_rules! legacy_float {
            ($field:ident) => {
                if let Some(raw) = fields.remove(stringify!($field)) {
                    if let Ok(value) = raw.parse::<f32>() {
                        if value.is_finite() {
                            next.$field = value;
                        }
                    }
                }
            };
        }
        legacy_float!(emotion_timeout);
        legacy_float!(model_scale);
        legacy_float!(model_x);
        legacy_float!(model_y);
        for (key, destination) in [
            ("window_width", &mut next.window_width),
            ("window_height", &mut next.window_height),
        ] {
            if let Some(raw) = fields.remove(key) {
                if let Ok(value) = raw.parse::<i32>() {
                    *destination = value;
                }
            }
        }
        typed!(hover_opacity_enabled);
        typed!(fullscreen_opacity_enabled);
        for (key, destination) in [
            ("hover_opacity", &mut next.hover_opacity),
            ("idle_opacity", &mut next.idle_opacity),
            ("fullscreen_opacity", &mut next.fullscreen_opacity),
        ] {
            if let Some(raw) = fields.remove(key) {
                let value = raw
                    .parse::<f64>()
                    .ok()
                    .filter(|v| v.is_finite() && (0.0..=1.0).contains(v))
                    .ok_or_else(|| {
                        format!("Invalid {key}: expected a finite number within 0..1")
                    })?;
                *destination = value as f32;
            }
        }
        if let Some(raw) = fields.remove("mouse_look_radius") {
            let value = raw
                .parse::<f64>()
                .ok()
                .filter(|v| v.is_finite() && *v >= 0.0 && *v <= f32::MAX as f64)
                .ok_or("Invalid mouse_look_radius: expected a finite non-negative float")?;
            next.mouse_look_radius = value as f32;
        }
        if let Some(raw) = fields.remove("model_region") {
            let rectangle = ConfigDocument::parse(&raw, true)?;
            let mut region = ModelRegion {
                x: 0,
                y: 0,
                width: 0,
                height: 0,
            };
            for (key, raw) in &rectangle.values {
                let destination = match key.as_str() {
                    "x" => &mut region.x,
                    "y" => &mut region.y,
                    "width" => &mut region.width,
                    "height" => &mut region.height,
                    _ => return Err("Invalid integer model_region".into()),
                };
                *destination = raw
                    .parse::<i32>()
                    .map_err(|_| "Invalid integer model_region")?;
            }
            if region.width <= 0 || region.height <= 0 {
                return Err("Crop dimensions must be positive integers".into());
            }
            next.model_region = Some(region);
        }
        next.unknown_fields = fields;
        *target = next;
        Ok(())
    }

    /// Encode and parse/decode the complete result before returning it.
    pub fn encode(&self) -> Result<String, String> {
        let mut fields = BTreeMap::new();
        macro_rules! put {
            ($field:ident) => {{
                fields.insert(
                    stringify!($field).to_owned(),
                    serde_json::to_string(&self.$field).map_err(|e| e.to_string())?,
                );
            }};
        }
        // serde_json represents non-finite floats as null; C++ serialization rejects
        // them even for legacy fields whose decode would otherwise fall back.
        if [
            self.emotion_timeout,
            self.model_scale,
            self.model_x,
            self.model_y,
            self.hover_opacity,
            self.idle_opacity,
            self.fullscreen_opacity,
            self.mouse_look_radius,
        ]
        .iter()
        .any(|v| !v.is_finite())
        {
            return Err("Cannot serialize non-finite settings".into());
        }
        put!(additional_model_dirs);
        put!(default_model);
        put!(plugin_dir);
        put!(emotion_timeout);
        put!(model_scale);
        put!(model_x);
        put!(model_y);
        put!(window_width);
        put!(window_height);
        put!(hover_opacity_enabled);
        put!(hover_opacity);
        put!(idle_opacity);
        put!(fullscreen_opacity_enabled);
        put!(fullscreen_opacity);
        // Widen before formatting so f32::MAX's shortest decimal does not round
        // above the strict f64 range check when the document is decoded again.
        fields.insert(
            "mouse_look_radius".into(),
            serde_json::to_string(&(self.mouse_look_radius as f64)).map_err(|e| e.to_string())?,
        );
        if !self.model_output.is_empty() {
            put!(model_output);
        }
        if !self.render_device.is_empty() {
            put!(render_device);
        }
        if let Some(r) = &self.model_region {
            fields.insert(
                "model_region".into(),
                format!(
                    "{{\"x\":{},\"y\":{},\"width\":{},\"height\":{}}}",
                    r.x, r.y, r.width, r.height
                ),
            );
        }
        // Append rather than overwrite: collisions must fail duplicate-key validation.
        let mut members = Vec::new();
        for (key, raw) in fields.iter().chain(self.unknown_fields.iter()) {
            members.push(format!(
                "  {}: {}",
                serde_json::to_string(key).map_err(|e| e.to_string())?,
                raw
            ));
        }
        let text = format!("{{\n{}\n}}\n", members.join(",\n"));
        let document = ConfigDocument::parse(&text, true)?;
        Self::decode(&document, &mut Self::default())?;
        Ok(text)
    }
}

fn valid_device(s: &str) -> bool {
    if s.is_empty() || s == "auto" {
        return true;
    }
    let b = s.as_bytes();
    b.len() == 12
        && b[4] == b':'
        && b[7] == b':'
        && b[10] == b'.'
        && b.iter().enumerate().all(|(i, c)| {
            matches!(i, 4 | 7 | 10) || c.is_ascii_digit() || (b'a'..=b'f').contains(c)
        })
        && b[8] <= b'1'
        && b[11] <= b'7'
}
