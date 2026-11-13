// Deleting a file to the system's "trash": the Recycle Bin on Windows/macOS/Linux (the `trash`
// crate), nothing on iOS — where the app has no system trash, so callers fall back to the vault's
// own `_trash` folder. Keeping the two behind one function means the call sites read the same on
// every platform; on iOS `delete` always fails, so the `_trash` branch always runs.
use std::path::Path;

#[cfg(not(target_os = "ios"))]
pub fn delete(path: &Path) -> Result<(), trash::Error> {
    trash::delete(path)
}

#[cfg(target_os = "ios")]
pub fn delete(_path: &Path) -> Result<(), String> {
    Err("no system trash on iOS".into())
}
