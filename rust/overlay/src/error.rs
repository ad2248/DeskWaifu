use std::fmt;

/// Everything that can make [`crate::Overlay`] setup or a frame fail.
///
/// Every variant carries a fixed message; the compositor's own error strings are
/// never forwarded, so callers can match on stable text.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// A global required by this crate is not advertised by the compositor.
    MissingGlobal(&'static str),
    /// A global was advertised at a lower version than a requested feature needs.
    Unsupported(&'static str),
    /// The requested output name is not advertised by the compositor.
    UnknownOutput(String),
    /// The Wayland connection failed (transport or protocol error).
    Wayland(String),
    /// EGL setup or swap failed.
    Egl(khronos_egl::Error),
    /// The compositor closed the layer surface.
    Closed,
    /// The layer surface never received a `configure` event.
    NotConfigured,
    /// The configured surface size is zero.
    InvalidSize,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::MissingGlobal(name) => {
                write!(f, "missing required Wayland global: {name}")
            }
            Error::Unsupported(message) => write!(f, "unsupported: {message}"),
            Error::UnknownOutput(name) => {
                write!(
                    f,
                    "no wl_output named {name:?} is advertised by the compositor"
                )
            }
            Error::Wayland(message) => write!(f, "Wayland connection error: {message}"),
            Error::Egl(error) => write!(f, "EGL error: {error}"),
            Error::Closed => write!(f, "layer surface was closed by the compositor"),
            Error::NotConfigured => write!(f, "layer surface received no configure event"),
            Error::InvalidSize => write!(f, "overlay width and height must be greater than zero"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Egl(error) => Some(error),
            _ => None,
        }
    }
}

impl From<khronos_egl::Error> for Error {
    fn from(error: khronos_egl::Error) -> Self {
        Error::Egl(error)
    }
}

impl From<wayland_client::backend::WaylandError> for Error {
    fn from(error: wayland_client::backend::WaylandError) -> Self {
        Error::Wayland(error.to_string())
    }
}
