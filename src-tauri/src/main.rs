// Desktop entry point: the app itself lives in the library (lib.rs), so the same code can also be
// built for iOS, where the platform provides the entry point instead.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    image_vault_lib::run()
}
