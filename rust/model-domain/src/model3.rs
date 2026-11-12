//! Pure parser for Live2D `.model3.json` model descriptors.
//!
//! This mirrors the metadata the desktop runtime reads in
//! `plugins/desktop/private/processor/model/` (`LAppModel::SetupModel`,
//! `SetupTextures`, `GetExpressionIds`, `GetMotionList`,
//! `LAppLive2DManager::HitSnapshot`) and in the Cubism SDK's
//! `CubismModelSettingJson`. It performs no filesystem access, no Cubism work
//! and no rendering; callers resolve paths against the model directory.
//!
//! Behaviour deliberately matching the C++ loader:
//!
//! * Motion groups and their motions keep declaration order. `csmMap` returns
//!   keys in insertion order and `GetMotionList()` walks
//!   `GetMotionGroupName(g)` for every group.
//! * Textures keep declaration order and empty entries. `SetupTextures()`
//!   skips empty names but still binds by texture index, so [`Model3`] keeps
//!   them and the caller must skip `""` when loading while preserving indices.
//! * A missing `FadeInTime`/`FadeOutTime` is [`None`]. The C++ settings loader
//!   returns `-1` there, which the motion manager reads as "use the motion's
//!   own fade"; a declared value is kept as written.
//! * `Groups[].Target` is ignored; groups are found by name, which is how
//!   `CubismModelSettingJson` matches `EyeBlink`/`LipSync`.
//! * Unknown keys are ignored everywhere; Cubism looks keys up by name and real
//!   model files carry extra keys such as `FileReferences.UserData`.
//! * `Version` is optional (the C++ loader never requires it) but validated
//!   when present.
//!
//! Validation is stricter than the C++ loader, which tolerated missing or
//! mistyped fields and returned empty strings: required fields, JSON types, a
//! blank `Moc`, string-array entries, fade times, `Layout` values and duplicate
//! group names are rejected through [`Model3Error`].
//!
//! Deliberately not copied from C++:
//!
//! * The automatic expression/motion discovery in `LAppModel::SetupModel` that
//!   scans the model directory for `.exp3.json`/`.motion3.json` files. That is
//!   filesystem work and outside this crate's contract; the parser reports only
//!   what `FileReferences` declares, so an IPC `get_expressions` that should
//!   include discovered files must add them in the caller.
//! * `CubismModelSettingJson`'s lenient `ToFloat` fallbacks and its `-1` sentinel
//!   for absent fade times. Absent stays [`None`] rather than a magic number.
//! * Its first-match-wins handling of duplicate `Groups` names. Duplicates are
//!   ambiguous metadata, so both duplicate `Groups` names and duplicate `Motions`
//!   keys are rejected; the SDK samples declare no duplicates.
//! * `Groups[].Target` and `FileReferences.UserData` as fields: `Target` is not
//!   exposed because the runtime never branches on it, and `UserData` because
//!   the crate has no user-data parser yet.
//! * Cubism's first-value-wins handling of a duplicated JSON key. This parser
//!   keeps the last value, as `serde_json` does; duplicated motion-group keys
//!   are rejected outright instead of silently picking one.

use std::collections::{BTreeMap, HashSet};
use std::fmt;

use serde::de::{Deserialize, Deserializer, MapAccess, SeqAccess, Visitor};

/// Name of the parameter group used for automatic eye blinking.
pub const EYE_BLINK_GROUP: &str = "EyeBlink";
/// Name of the parameter group used for lip sync.
pub const LIP_SYNC_GROUP: &str = "LipSync";

/// A parsed model3 document.
#[derive(Clone, Debug, PartialEq)]
pub struct Model3 {
    /// Declared `Version`, which real files set to `3`. Absent is allowed.
    pub version: Option<u32>,
    pub file_references: FileReferences,
    /// Parameter groups in declaration order.
    pub groups: Vec<Group>,
    pub hit_areas: Vec<HitArea>,
    /// `Layout` values keyed by name; empty when the document has no `Layout`.
    /// Names are looked up, never ordered, so a sorted map is used.
    pub layout: BTreeMap<String, f32>,
}

/// The `FileReferences` section, whose `Moc` entry is required.
#[derive(Clone, Debug, PartialEq)]
pub struct FileReferences {
    /// The `.moc3` file name; never empty or blank.
    pub moc: String,
    /// Texture file names in declaration order, including empty entries.
    pub textures: Vec<String>,
    /// `Physics` file name when declared. An empty string is kept as declared;
    /// the C++ loader treats `""` the same as absent.
    pub physics: Option<String>,
    /// `Pose` file name when declared.
    pub pose: Option<String>,
    /// `DisplayInfo` file name when declared.
    pub display_info: Option<String>,
    /// Expression entries in declaration order.
    pub expressions: Vec<Expression>,
    /// Motion groups in declaration order.
    pub motions: Vec<MotionGroup>,
}

/// One `Expressions` entry.
#[derive(Clone, Debug, PartialEq)]
pub struct Expression {
    pub name: String,
    pub file: String,
}

/// One `Motions` group, keeping the order the document declares.
#[derive(Clone, Debug, PartialEq)]
pub struct MotionGroup {
    pub name: String,
    pub motions: Vec<Motion>,
}

/// One motion inside a group.
#[derive(Clone, Debug, PartialEq)]
pub struct Motion {
    pub file: String,
    /// Declared `FadeInTime`, if any; C++ passes `-1` for an absent value.
    pub fade_in_time: Option<f32>,
    /// Declared `FadeOutTime`, if any; C++ passes `-1` for an absent value.
    pub fade_out_time: Option<f32>,
    /// Declared `Sound` file, if any.
    pub sound: Option<String>,
}

/// One `Groups` entry. `Target` is deliberately not represented.
#[derive(Clone, Debug, PartialEq)]
pub struct Group {
    pub name: String,
    pub ids: Vec<String>,
}

/// One `HitAreas` entry.
#[derive(Clone, Debug, PartialEq)]
pub struct HitArea {
    pub name: String,
    pub id: String,
}

/// One flattened motion, shaped like the C++ `LAppModel::MotionInfo`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MotionRef<'a> {
    pub group: &'a str,
    pub index: usize,
    pub file: &'a str,
}

/// A rejected model3 document.
#[derive(Clone, Debug, PartialEq)]
pub enum Model3Error {
    /// The document is not valid JSON (including trailing data).
    InvalidJson(String),
    /// The top-level value is not a JSON object.
    NotAnObject,
    /// A required field is missing.
    MissingField { path: String },
    /// A field exists but has the wrong JSON type.
    WrongType {
        path: String,
        expected: &'static str,
    },
    /// A required string is empty or only whitespace.
    BlankField { path: String },
    /// A number is negative, non-integral where an integer is required, or not
    /// finite once converted to `f32`.
    InvalidNumber { path: String, value: f64 },
    /// The `Groups` array names the same parameter group twice.
    DuplicateGroup { name: String },
    /// The `Motions` object declares the same group name twice.
    DuplicateMotionGroup { name: String },
}

impl fmt::Display for Model3Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidJson(message) => write!(f, "invalid model3 JSON: {message}"),
            Self::NotAnObject => write!(f, "model3 document must be a JSON object"),
            Self::MissingField { path } => write!(f, "missing required field `{path}`"),
            Self::WrongType { path, expected } => {
                write!(f, "field `{path}` must be a JSON {expected}")
            }
            Self::BlankField { path } => write!(f, "field `{path}` must not be blank"),
            Self::InvalidNumber { path, value } => {
                write!(f, "field `{path}` has an invalid number: {value}")
            }
            Self::DuplicateGroup { name } => write!(f, "duplicate parameter group `{name}`"),
            Self::DuplicateMotionGroup { name } => write!(f, "duplicate motion group `{name}`"),
        }
    }
}

impl std::error::Error for Model3Error {}

impl Model3 {
    /// Parses a `.model3.json` document.
    pub fn parse(json: &str) -> Result<Self, Model3Error> {
        let raw: Raw = serde_json::from_str(json)
            .map_err(|error| Model3Error::InvalidJson(error.to_string()))?;
        Self::from_raw(raw)
    }

    fn from_raw(raw: Raw) -> Result<Self, Model3Error> {
        let mut fields = match raw {
            Raw::Object(fields) => fields,
            _ => return Err(Model3Error::NotAnObject),
        };
        let version = match take(&mut fields, "Version") {
            Some(raw) => Some(version(raw)?),
            None => None,
        };
        let file_references = match take(&mut fields, "FileReferences") {
            Some(raw) => FileReferences::from_raw(raw)?,
            None => {
                return Err(Model3Error::MissingField {
                    path: "FileReferences".into(),
                })
            }
        };
        let groups = match take(&mut fields, "Groups") {
            Some(raw) => parameter_groups(raw)?,
            None => Vec::new(),
        };
        let hit_areas = match take(&mut fields, "HitAreas") {
            Some(raw) => object_array(raw, "HitAreas", hit_area)?,
            None => Vec::new(),
        };
        let layout = match take(&mut fields, "Layout") {
            Some(raw) => layout(raw)?,
            None => BTreeMap::new(),
        };
        Ok(Self {
            version,
            file_references,
            groups,
            hit_areas,
            layout,
        })
    }

    /// The referenced `.moc3` file name (`FileReferences.Moc`).
    pub fn model_file_name(&self) -> &str {
        &self.file_references.moc
    }

    /// Texture file names in declaration order, including empty entries.
    pub fn texture_file_names(&self) -> &[String] {
        &self.file_references.textures
    }

    /// All motions flattened as `(group, index, file)` in declaration order,
    /// matching the C++ `GetMotionList()`.
    pub fn motion_list(&self) -> Vec<MotionRef<'_>> {
        let mut result = Vec::new();
        for group in &self.file_references.motions {
            for (index, motion) in group.motions.iter().enumerate() {
                result.push(MotionRef {
                    group: &group.name,
                    index,
                    file: &motion.file,
                });
            }
        }
        result
    }

    /// Expression `Name` values in declaration order.
    pub fn expression_names(&self) -> Vec<&str> {
        self.file_references
            .expressions
            .iter()
            .map(|expression| expression.name.as_str())
            .collect()
    }

    /// Parameter ids of the group with `name`, as `Groups` declares them.
    pub fn parameter_group(&self, name: &str) -> Option<&[String]> {
        self.groups
            .iter()
            .find(|group| group.name == name)
            .map(|group| group.ids.as_slice())
    }

    /// Eye-blink parameter ids, or an empty slice without an `EyeBlink` group.
    pub fn eye_blink_parameter_ids(&self) -> &[String] {
        self.parameter_group(EYE_BLINK_GROUP).unwrap_or(&[])
    }

    /// Lip-sync parameter ids, or an empty slice without a `LipSync` group.
    pub fn lip_sync_parameter_ids(&self) -> &[String] {
        self.parameter_group(LIP_SYNC_GROUP).unwrap_or(&[])
    }

    /// Hit-area `(name, id)` pairs in declaration order.
    pub fn hit_area_pairs(&self) -> Vec<(&str, &str)> {
        self.hit_areas
            .iter()
            .map(|area| (area.name.as_str(), area.id.as_str()))
            .collect()
    }
}

impl FileReferences {
    fn from_raw(raw: Raw) -> Result<Self, Model3Error> {
        let mut fields = object(raw, "FileReferences")?;
        let moc = required_string(&mut fields, "Moc", "FileReferences")?;
        if moc.trim().is_empty() {
            return Err(Model3Error::BlankField {
                path: "FileReferences.Moc".into(),
            });
        }
        let textures = match take(&mut fields, "Textures") {
            Some(raw) => string_array(raw, "FileReferences.Textures")?,
            None => Vec::new(),
        };
        let physics = optional_string(&mut fields, "Physics", "FileReferences")?;
        let pose = optional_string(&mut fields, "Pose", "FileReferences")?;
        let display_info = optional_string(&mut fields, "DisplayInfo", "FileReferences")?;
        let expressions = match take(&mut fields, "Expressions") {
            Some(raw) => object_array(raw, "FileReferences.Expressions", expression)?,
            None => Vec::new(),
        };
        let motions = match take(&mut fields, "Motions") {
            Some(raw) => motion_groups(raw)?,
            None => Vec::new(),
        };
        Ok(Self {
            moc,
            textures,
            physics,
            pose,
            display_info,
            expressions,
            motions,
        })
    }
}

fn expression(raw: Raw, path: &str) -> Result<Expression, Model3Error> {
    let mut fields = object(raw, path)?;
    Ok(Expression {
        name: required_string(&mut fields, "Name", path)?,
        file: required_string(&mut fields, "File", path)?,
    })
}

fn motion_groups(raw: Raw) -> Result<Vec<MotionGroup>, Model3Error> {
    let fields = object(raw, "FileReferences.Motions")?;
    let mut seen = HashSet::new();
    let mut groups = Vec::with_capacity(fields.len());
    for (name, raw) in fields {
        if !seen.insert(name.clone()) {
            return Err(Model3Error::DuplicateMotionGroup { name });
        }
        let path = format!("FileReferences.Motions.{name}");
        let motions = object_array(raw, &path, motion)?;
        groups.push(MotionGroup { name, motions });
    }
    Ok(groups)
}

fn motion(raw: Raw, path: &str) -> Result<Motion, Model3Error> {
    let mut fields = object(raw, path)?;
    Ok(Motion {
        file: required_string(&mut fields, "File", path)?,
        fade_in_time: optional_fade(&mut fields, "FadeInTime", path)?,
        fade_out_time: optional_fade(&mut fields, "FadeOutTime", path)?,
        sound: optional_string(&mut fields, "Sound", path)?,
    })
}

fn parameter_groups(raw: Raw) -> Result<Vec<Group>, Model3Error> {
    let mut seen = HashSet::new();
    let mut groups = Vec::new();
    for group in object_array(raw, "Groups", group)? {
        if !seen.insert(group.name.clone()) {
            return Err(Model3Error::DuplicateGroup { name: group.name });
        }
        groups.push(group);
    }
    Ok(groups)
}

fn group(raw: Raw, path: &str) -> Result<Group, Model3Error> {
    let mut fields = object(raw, path)?;
    let name = required_string(&mut fields, "Name", path)?;
    let ids = match take(&mut fields, "Ids") {
        Some(raw) => string_array(raw, &format!("{path}.Ids"))?,
        None => {
            return Err(Model3Error::MissingField {
                path: format!("{path}.Ids"),
            })
        }
    };
    Ok(Group { name, ids })
}

fn hit_area(raw: Raw, path: &str) -> Result<HitArea, Model3Error> {
    let mut fields = object(raw, path)?;
    Ok(HitArea {
        name: required_string(&mut fields, "Name", path)?,
        id: required_string(&mut fields, "Id", path)?,
    })
}

fn layout(raw: Raw) -> Result<BTreeMap<String, f32>, Model3Error> {
    let mut result = BTreeMap::new();
    for (key, raw) in object(raw, "Layout")? {
        let path = format!("Layout.{key}");
        let value = match raw {
            Raw::Number(value) => value as f32,
            _ => {
                return Err(Model3Error::WrongType {
                    path,
                    expected: "number",
                })
            }
        };
        if !value.is_finite() {
            return Err(Model3Error::InvalidNumber {
                path,
                value: f64::from(value),
            });
        }
        result.insert(key, value);
    }
    Ok(result)
}

fn version(raw: Raw) -> Result<u32, Model3Error> {
    match raw {
        Raw::Number(value)
            if value.fract() == 0.0 && (0.0..=f64::from(u32::MAX)).contains(&value) =>
        {
            Ok(value as u32)
        }
        Raw::Number(value) => Err(Model3Error::InvalidNumber {
            path: "Version".into(),
            value,
        }),
        _ => Err(Model3Error::WrongType {
            path: "Version".into(),
            expected: "integer",
        }),
    }
}

fn required_string(
    fields: &mut Vec<(String, Raw)>,
    key: &str,
    base: &str,
) -> Result<String, Model3Error> {
    let path = format!("{base}.{key}");
    match take(fields, key) {
        Some(raw) => string(raw, &path),
        None => Err(Model3Error::MissingField { path }),
    }
}

fn optional_string(
    fields: &mut Vec<(String, Raw)>,
    key: &str,
    base: &str,
) -> Result<Option<String>, Model3Error> {
    let path = format!("{base}.{key}");
    match take(fields, key) {
        Some(raw) => string(raw, &path).map(Some),
        None => Ok(None),
    }
}

fn optional_fade(
    fields: &mut Vec<(String, Raw)>,
    key: &str,
    base: &str,
) -> Result<Option<f32>, Model3Error> {
    let path = format!("{base}.{key}");
    match take(fields, key) {
        Some(Raw::Number(value)) => {
            let fade = value as f32;
            if !fade.is_finite() || fade < 0.0 {
                Err(Model3Error::InvalidNumber { path, value })
            } else {
                Ok(Some(fade))
            }
        }
        Some(_) => Err(Model3Error::WrongType {
            path,
            expected: "number",
        }),
        None => Ok(None),
    }
}

fn object(raw: Raw, path: &str) -> Result<Vec<(String, Raw)>, Model3Error> {
    match raw {
        Raw::Object(fields) => Ok(fields),
        _ => Err(Model3Error::WrongType {
            path: path.into(),
            expected: "object",
        }),
    }
}

fn array(raw: Raw, path: &str) -> Result<Vec<Raw>, Model3Error> {
    match raw {
        Raw::Array(items) => Ok(items),
        _ => Err(Model3Error::WrongType {
            path: path.into(),
            expected: "array",
        }),
    }
}

fn string(raw: Raw, path: &str) -> Result<String, Model3Error> {
    match raw {
        Raw::String(value) => Ok(value),
        _ => Err(Model3Error::WrongType {
            path: path.into(),
            expected: "string",
        }),
    }
}

fn string_array(raw: Raw, path: &str) -> Result<Vec<String>, Model3Error> {
    array(raw, path)?
        .into_iter()
        .enumerate()
        .map(|(index, item)| string(item, &format!("{path}[{index}]")))
        .collect()
}

fn object_array<T>(
    raw: Raw,
    path: &str,
    parse: impl Fn(Raw, &str) -> Result<T, Model3Error>,
) -> Result<Vec<T>, Model3Error> {
    array(raw, path)?
        .into_iter()
        .enumerate()
        .map(|(index, item)| parse(item, &format!("{path}[{index}]")))
        .collect()
}

/// Returns the last value for `key`, matching `serde_json::Value` duplicate-key handling.
fn take(fields: &mut Vec<(String, Raw)>, key: &str) -> Option<Raw> {
    let index = fields.iter().rposition(|(name, _)| name == key)?;
    Some(fields.remove(index).1)
}

/// Order-preserving JSON tree used while parsing, so `Motions` keys and array
/// entries keep declaration order without enabling serde_json's `preserve_order`.
enum Raw {
    Null,
    /// Booleans carry no value: every field this parser reads is a string,
    /// number, array or object, so a boolean is always a type error.
    Bool,
    Number(f64),
    String(String),
    Array(Vec<Raw>),
    Object(Vec<(String, Raw)>),
}

impl<'de> Deserialize<'de> for Raw {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        deserializer.deserialize_any(RawVisitor)
    }
}

struct RawVisitor;

impl<'de> Visitor<'de> for RawVisitor {
    type Value = Raw;

    fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("any JSON value")
    }

    fn visit_bool<E>(self, _value: bool) -> Result<Raw, E> {
        Ok(Raw::Bool)
    }

    fn visit_i64<E>(self, value: i64) -> Result<Raw, E> {
        Ok(Raw::Number(value as f64))
    }

    fn visit_u64<E>(self, value: u64) -> Result<Raw, E> {
        Ok(Raw::Number(value as f64))
    }

    fn visit_f64<E>(self, value: f64) -> Result<Raw, E> {
        Ok(Raw::Number(value))
    }

    fn visit_str<E>(self, value: &str) -> Result<Raw, E> {
        Ok(Raw::String(value.to_owned()))
    }

    fn visit_string<E>(self, value: String) -> Result<Raw, E> {
        Ok(Raw::String(value))
    }

    fn visit_unit<E>(self) -> Result<Raw, E> {
        Ok(Raw::Null)
    }

    fn visit_none<E>(self) -> Result<Raw, E> {
        Ok(Raw::Null)
    }

    fn visit_seq<A: SeqAccess<'de>>(self, mut sequence: A) -> Result<Raw, A::Error> {
        let mut items = Vec::new();
        while let Some(item) = sequence.next_element::<Raw>()? {
            items.push(item);
        }
        Ok(Raw::Array(items))
    }

    fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Raw, A::Error> {
        let mut fields = Vec::new();
        while let Some((key, value)) = map.next_entry::<String, Raw>()? {
            fields.push((key, value));
        }
        Ok(Raw::Object(fields))
    }
}
