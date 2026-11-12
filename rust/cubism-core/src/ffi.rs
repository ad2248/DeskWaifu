//! Minimal handwritten declarations from the external Live2DCubismCore.h C API.
#![allow(non_snake_case)]
use std::ffi::{c_char, c_int, c_void};

pub type Moc = c_void;
pub type Model = c_void;

#[repr(C)]
#[derive(Default)]
pub struct Vector2 {
    pub x: f32,
    pub y: f32,
}

extern "C" {
    pub fn csmGetVersion() -> u32;
    pub fn csmGetLatestMocVersion() -> u32;
    pub fn csmGetMocVersion(address: *const c_void, size: u32) -> u32;
    pub fn csmHasMocConsistency(address: *mut c_void, size: u32) -> c_int;
    pub fn csmReviveMocInPlace(address: *mut c_void, size: u32) -> *mut Moc;
    pub fn csmGetSizeofModel(moc: *const Moc) -> u32;
    pub fn csmInitializeModelInPlace(
        moc: *const Moc,
        address: *mut c_void,
        size: u32,
    ) -> *mut Model;
    pub fn csmUpdateModel(model: *mut Model);
    pub fn csmReadCanvasInfo(
        model: *const Model,
        size: *mut Vector2,
        origin: *mut Vector2,
        pixels_per_unit: *mut f32,
    );
    pub fn csmGetParameterCount(model: *const Model) -> c_int;
    pub fn csmGetParameterIds(model: *const Model) -> *const *const c_char;
    pub fn csmGetParameterTypes(model: *const Model) -> *const c_int;
    pub fn csmGetParameterMinimumValues(model: *const Model) -> *const f32;
    pub fn csmGetParameterMaximumValues(model: *const Model) -> *const f32;
    pub fn csmGetParameterDefaultValues(model: *const Model) -> *const f32;
    pub fn csmGetParameterValues(model: *mut Model) -> *mut f32;
}
