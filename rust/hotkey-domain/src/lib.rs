//! Pure model-hotkey and expression state, ported from
//! `plugins/desktop/private/processor/model/ModelHotkeys.cpp`.
//!
//! The crate only reads the paths it is asked to read through [`Reader`]; it does
//! not walk directories, load Cubism, or render. [`Hotkeys::apply`] therefore
//! returns the blend operations a renderer must perform instead of mutating a
//! model.
//!
//! Model tokens come from a process-wide atomic counter and have the form
//! `model-<n>`. Every [`Hotkeys::load`] and [`Hotkeys::reset`] issues a new,
//! never-repeating token, so a request carrying an older token is recognizably
//! stale even when the ID still exists.
//!
//! ```
//! use deskwaifu_hotkey_domain::{Hotkeys, Reader};
//!
//! struct Memory;
//! impl Reader for Memory {
//!     fn read(&self, path: &str) -> Option<String> {
//!         match path {
//!             "test.vtube.json" => Some(r#"{"Hotkeys":[{"HotkeyID":"wave",
//!                 "Action":"ToggleExpression","File":"wave.exp3.json"}]}"#.into()),
//!             "exp/wave.exp3.json" => Some(r#"{"Parameters":[{"Id":"ParamAngleX","Value":1}]}"#.into()),
//!             _ => None,
//!         }
//!     }
//! }
//!
//! let mut hotkeys = Hotkeys::new();
//! hotkeys.load(&Memory, "test.model3.json");
//! hotkeys.trigger("wave").unwrap();
//! assert!(hotkeys.is_active("wave"));
//! ```

use std::fmt;
use std::path::Path;
use std::sync::atomic::{AtomicU64, Ordering};

/// Invalid VTube Studio hotkey configuration file.
pub const INVALID_CONFIGURATION: &str = "Invalid VTube Studio hotkey configuration";
/// The binding ID is absent from the currently loaded model.
pub const NOT_FOUND: &str = "Hotkey not found (the model may have changed)";
/// The binding is disabled in the model configuration.
pub const DISABLED: &str = "Hotkey disabled";
/// The binding declares something this crate does not execute.
pub const UNSUPPORTED_ACTION: &str = "Unsupported action";
/// The binding releases the expression on key-up, which is not supported.
pub const RELEASE_TO_DEACTIVATE: &str = "Release-to-deactivate is not supported";
/// The binding has no usable expression file.
pub const EXPRESSION_MISSING: &str = "Expression file missing or invalid";
/// The binding has an empty or already used hotkey ID.
pub const MISSING_OR_DUPLICATE_ID: &str = "Missing or duplicate hotkey ID";

const MODEL_SUFFIX: &str = ".model3.json";
const EXPRESSION_DIRECTORIES: [&str; 3] = ["", "Expressions/", "exp/"];
const TRIGGER_KEYS: [&str; 3] = ["Trigger1", "Trigger2", "Trigger3"];

/// Directories searched for expression files, after the configured folder.
///
/// Mirrors `ModelHotkeys::ExpressionDirectories`; paths are relative to the model
/// directory and are only used for reads through [`Reader`].
pub fn expression_directories() -> &'static [&'static str] {
    &EXPRESSION_DIRECTORIES
}

/// Names the operation a caller performs for one parameter.
///
/// This only labels the operation; the caller owns the arithmetic and applies
/// [`ApplyOp::weight`] itself.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Blend {
    /// Add the expression value.
    Add,
    /// Multiply the current value.
    Multiply,
    /// Overwrite the current value.
    Overwrite,
}

impl Blend {
    /// Blank or absent blends default to [`Blend::Add`]; anything else is invalid.
    fn parse(text: &str) -> Option<Self> {
        match text {
            "" | "Add" => Some(Self::Add),
            "Multiply" => Some(Self::Multiply),
            "Overwrite" => Some(Self::Overwrite),
            _ => None,
        }
    }
}

/// One expression parameter and its blend mode.
#[derive(Clone, Debug, PartialEq)]
pub struct Parameter {
    pub id: String,
    pub blend: Blend,
    pub value: f32,
}

/// A persistent expression and its fade/timeout state.
#[derive(Clone, Debug, PartialEq)]
pub struct Expression {
    /// Path relative to the model directory where the expression was found.
    pub file: String,
    pub parameters: Vec<Parameter>,
    pub active: bool,
    pub weight: f32,
    pub fade: f32,
    /// Seconds left before automatic deactivation; negative means no timeout.
    pub remaining: f32,
}

impl Default for Expression {
    fn default() -> Self {
        Self {
            file: String::new(),
            parameters: Vec::new(),
            active: false,
            weight: 0.0,
            fade: 0.5,
            remaining: -1.0,
        }
    }
}

/// A VTube Studio hotkey entry and the reason it cannot be triggered, if any.
#[derive(Clone, Debug, PartialEq)]
pub struct Binding {
    pub id: String,
    pub name: String,
    pub action: String,
    pub file: String,
    /// Empty when the binding is triggerable.
    pub reason: String,
    pub keys: Vec<String>,
    pub enabled: bool,
    /// Fade duration in seconds.
    pub fade: f32,
    /// Automatic deactivation delay in seconds; negative means none.
    pub duration: f32,
    /// Resolved expression index, absent when resolution failed.
    pub expression: Option<usize>,
}

impl Default for Binding {
    fn default() -> Self {
        Self {
            id: String::new(),
            name: String::new(),
            action: String::new(),
            file: String::new(),
            reason: String::new(),
            keys: Vec::new(),
            enabled: true,
            fade: 0.5,
            duration: -1.0,
            expression: None,
        }
    }
}

/// One blend operation for a renderer to perform, in expression order.
#[derive(Clone, Debug, PartialEq)]
pub struct ApplyOp {
    pub parameter_id: String,
    pub blend: Blend,
    pub value: f32,
    /// Expression weight the caller scales the operation by; never zero.
    pub weight: f32,
}

/// Reads a file relative to the model directory.
pub trait Reader {
    /// Returns the file contents, or `None` when the file does not exist or cannot be read.
    fn read(&self, path: &str) -> Option<String>;
}

/// [`Reader`] backed by the real filesystem.
pub struct FsReader<'a> {
    directory: &'a Path,
}

impl<'a> FsReader<'a> {
    pub fn new(directory: &'a Path) -> Self {
        Self { directory }
    }
}

impl Reader for FsReader<'_> {
    fn read(&self, path: &str) -> Option<String> {
        std::fs::read_to_string(self.directory.join(path)).ok()
    }
}

/// Why a trigger did not toggle an expression.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TriggerError {
    /// No currently loaded binding has this ID.
    NotFound,
    /// The binding is disabled in the model configuration.
    Disabled,
    /// The binding is not triggerable for the carried reason.
    Invalid(String),
}

impl TriggerError {
    /// The message the C++ implementation reports to callers.
    pub fn reason(&self) -> &str {
        match self {
            Self::NotFound => NOT_FOUND,
            Self::Disabled => DISABLED,
            Self::Invalid(reason) => reason,
        }
    }
}

impl fmt::Display for TriggerError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.reason())
    }
}

impl std::error::Error for TriggerError {}

/// Process-wide token counter; tokens stay unique and increasing across loads.
static TOKENS: AtomicU64 = AtomicU64::new(0);

fn next_token() -> String {
    format!("model-{}", TOKENS.fetch_add(1, Ordering::Relaxed) + 1)
}

/// Model hotkeys, resolved expressions, and their persistent runtime state.
#[derive(Clone, Debug, PartialEq)]
pub struct Hotkeys {
    bindings: Vec<Binding>,
    expressions: Vec<Expression>,
    error: String,
    token: String,
}

impl Default for Hotkeys {
    fn default() -> Self {
        Self::new()
    }
}

impl Hotkeys {
    /// Creates an empty, unloaded instance with an empty model token.
    ///
    /// Like the C++ class, the first [`Hotkeys::load`] or [`Hotkeys::reset`] issues
    /// the token, so callers must not treat the empty value as a valid model token.
    pub fn new() -> Self {
        Self {
            bindings: Vec::new(),
            expressions: Vec::new(),
            error: String::new(),
            token: String::new(),
        }
    }

    /// Reloads bindings and expressions for `model_file` and issues a new token.
    ///
    /// Only `<model>.model3.json` names are accepted. A missing configuration file
    /// yields no bindings and no error; an unparsable one sets [`Hotkeys::error`].
    pub fn load<R: Reader>(&mut self, reader: &R, model_file: &str) {
        self.bindings.clear();
        self.expressions.clear();
        self.error.clear();
        self.reset();
        let Some(stem) = model_file.strip_suffix(MODEL_SUFFIX) else {
            return;
        };
        let Some(contents) = reader.read(&format!("{stem}.vtube.json")) else {
            return;
        };
        let Ok(configuration) = serde_json::from_str::<serde_json::Value>(&contents) else {
            self.error = INVALID_CONFIGURATION.to_string();
            return;
        };
        let Some(hotkeys) = configuration.get("Hotkeys").and_then(|v| v.as_array()) else {
            self.error = INVALID_CONFIGURATION.to_string();
            return;
        };

        let mut ids = std::collections::HashSet::new();
        for source in hotkeys {
            let mut binding = Binding {
                id: text(source.get("HotkeyID")),
                file: text(source.get("File")),
                name: text(source.get("Name")),
                action: text(source.get("Action")),
                ..Binding::default()
            };
            if binding.name.is_empty() {
                binding.name = binding.file.clone();
            }
            binding.enabled = flag(source.get("IsActive"), true);
            binding.fade = nonnegative(source.get("FadeSecondsAmount"), 0.5);
            if flag(source.get("DeactivateAfterSeconds"), false) {
                binding.duration = nonnegative(source.get("DeactivateAfterSecondsAmount"), 10.0);
            }
            for key in TRIGGER_KEYS {
                let key = text(
                    source
                        .get("Triggers")
                        .and_then(|triggers| triggers.get(key)),
                );
                if !key.is_empty() {
                    binding.keys.push(key);
                }
            }

            if binding.id.is_empty() || !ids.insert(binding.id.clone()) {
                binding.reason = MISSING_OR_DUPLICATE_ID.to_string();
            } else if binding.action != "ToggleExpression" {
                binding.reason = UNSUPPORTED_ACTION.to_string();
            } else if flag(source.get("DeactivateAfterKeyUp"), false) {
                binding.reason = RELEASE_TO_DEACTIVATE.to_string();
            } else {
                binding.expression =
                    self.resolve(reader, &binding.file, &text(source.get("Folder")));
                if binding.expression.is_none() {
                    binding.reason = EXPRESSION_MISSING.to_string();
                }
            }
            self.bindings.push(binding);
        }
    }

    /// Resolves the first usable expression candidate and records it in the table.
    fn resolve<R: Reader>(&mut self, reader: &R, file: &str, folder: &str) -> Option<usize> {
        let mut candidates = Vec::new();
        if !folder.is_empty() && relative_path(folder) {
            candidates.push(format!("{folder}/{file}"));
        }
        for prefix in expression_directories() {
            candidates.push(format!("{prefix}{file}"));
        }
        for candidate in candidates {
            if !relative_path(&candidate) {
                continue;
            }
            let Some(contents) = reader.read(&candidate) else {
                continue;
            };
            let Ok(expression) = serde_json::from_str::<serde_json::Value>(&contents) else {
                continue;
            };
            let Some(parameters) = expression.get("Parameters").and_then(|v| v.as_array()) else {
                continue;
            };
            let index = match self.expressions.iter().position(|e| e.file == candidate) {
                Some(index) => index,
                None => {
                    self.expressions.push(Expression {
                        file: candidate,
                        parameters: parameters.iter().filter_map(parameter).collect(),
                        ..Expression::default()
                    });
                    self.expressions.len() - 1
                }
            };
            return (!self.expressions[index].parameters.is_empty()).then_some(index);
        }
        None
    }

    /// Clears runtime state and issues a new model token.
    pub fn reset(&mut self) {
        self.token = next_token();
        for expression in &mut self.expressions {
            expression.active = false;
            expression.weight = 0.0;
            expression.remaining = -1.0;
        }
    }

    /// Toggles the expression bound to `id`.
    pub fn trigger(&mut self, id: &str) -> Result<(), TriggerError> {
        let Some(index) = self.bindings.iter().position(|binding| binding.id == id) else {
            return Err(TriggerError::NotFound);
        };
        let binding = &self.bindings[index];
        if !binding.enabled {
            return Err(TriggerError::Disabled);
        }
        if !binding.reason.is_empty() {
            return Err(TriggerError::Invalid(binding.reason.clone()));
        }
        // `load` only leaves the reason empty for a binding with a resolved expression.
        let Some(expression) = binding.expression else {
            return Err(TriggerError::Invalid(EXPRESSION_MISSING.to_string()));
        };
        let (fade, duration) = (binding.fade, binding.duration);
        let expression = &mut self.expressions[expression];
        expression.active = !expression.active;
        expression.fade = fade;
        expression.remaining = if expression.active { duration } else { -1.0 };
        Ok(())
    }

    /// Returns whether the expression bound to `id` is active in this model.
    pub fn is_active(&self, id: &str) -> bool {
        self.bindings
            .iter()
            .find(|binding| binding.id == id)
            .and_then(|binding| binding.expression)
            .and_then(|index| self.expressions.get(index))
            .is_some_and(|expression| expression.active)
    }

    /// Advances fades and automatic deactivation.
    pub fn update(&mut self, seconds: f32) {
        for expression in &mut self.expressions {
            if expression.active && expression.remaining >= 0.0 {
                expression.remaining -= seconds;
                if expression.remaining <= 0.0 {
                    expression.active = false;
                }
            }
            let step = if expression.fade > 0.0 {
                seconds / expression.fade
            } else {
                1.0
            };
            expression.weight = if expression.active {
                (expression.weight + step).min(1.0)
            } else {
                (expression.weight - step).max(0.0)
            };
        }
    }

    /// Lists the blend operations for every expression with a positive weight.
    ///
    /// Expressions keep load order and parameters keep file order. The caller
    /// restores its saved base values and then performs each named operation with
    /// [`ApplyOp::weight`]; this crate does not implement any blending itself.
    pub fn apply(&self) -> Vec<ApplyOp> {
        let mut operations = Vec::new();
        for expression in &self.expressions {
            if expression.weight <= 0.0 {
                continue;
            }
            operations.extend(expression.parameters.iter().map(|parameter| ApplyOp {
                parameter_id: parameter.id.clone(),
                blend: parameter.blend,
                value: parameter.value,
                weight: expression.weight,
            }));
        }
        operations
    }

    pub fn bindings(&self) -> &[Binding] {
        &self.bindings
    }

    pub fn expressions(&self) -> &[Expression] {
        &self.expressions
    }

    /// Configuration error from the last load; empty when it succeeded.
    pub fn error(&self) -> &str {
        &self.error
    }

    pub fn token(&self) -> &str {
        &self.token
    }
}

fn text(value: Option<&serde_json::Value>) -> String {
    value
        .and_then(serde_json::Value::as_str)
        .unwrap_or_default()
        .to_string()
}

/// Boolean fields only honor JSON booleans; every other type uses `fallback`.
fn flag(value: Option<&serde_json::Value>, fallback: bool) -> bool {
    value
        .and_then(serde_json::Value::as_bool)
        .unwrap_or(fallback)
}

/// Numeric fields use `fallback` when absent or not a number, then clamp to zero.
fn nonnegative(value: Option<&serde_json::Value>, fallback: f32) -> f32 {
    let Some(number) = value.and_then(serde_json::Value::as_f64) else {
        return fallback;
    };
    let number = number as f32;
    if number.is_finite() {
        number.max(0.0)
    } else {
        fallback
    }
}

fn relative_path(path: &str) -> bool {
    !path.is_empty() && !path.starts_with('/') && !path.contains("..") && !path.contains('\\')
}

fn parameter(source: &serde_json::Value) -> Option<Parameter> {
    let id = text(source.get("Id"));
    if id.is_empty() {
        return None;
    }
    let value = source.get("Value")?.as_f64()?;
    let blend = Blend::parse(&text(source.get("Blend")))?;
    Some(Parameter {
        id,
        blend,
        value: value as f32,
    })
}
