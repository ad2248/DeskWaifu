//! Valid shared library intentionally lacking deskwaifu_plugin_create.
#[allow(unsafe_code)] // Export a harmless symbol so this is a real, loadable fixture library.
#[unsafe(no_mangle)]
pub fn unrelated_symbol() -> usize {
    42
}
