//! D-Bus and job errors are logged with what failed, never dropped (C13).

use std::fmt::Display;

pub trait Logged<T> {
    /// The value, or `None` after a warning that names `what` and the error.
    fn logged(self, what: &str) -> Option<T>;
    /// As `logged`, at debug level: for a failure the caller expects, such as releasing a hold that already ended.
    fn expected(self, what: &str) -> Option<T>;
}

impl<T, E: Display> Logged<T> for Result<T, E> {
    fn logged(self, what: &str) -> Option<T> {
        self.map_err(|e| log::warn!("{what}: {e}")).ok()
    }

    fn expected(self, what: &str) -> Option<T> {
        self.map_err(|e| log::debug!("{what}: {e}")).ok()
    }
}
