use std::env;

pub const DEFAULT_ADMIN_ADDR: &str = "127.0.0.1:37125";
pub const DEFAULT_SERIAL_BAUD: u32 = 115_200;

pub fn env_value(name: &str) -> Option<String> {
    env::var(name).ok()
}

pub fn env_value_or(name: &str, default: &str) -> String {
    env_value(name).unwrap_or_else(|| default.to_string())
}

pub fn admin_addr() -> String {
    env_value_or("ESP32DASH_ADMIN_ADDR", DEFAULT_ADMIN_ADDR)
}

pub fn state_dir() -> Option<String> {
    env_value("ESP32DASH_STATE_DIR")
}

pub fn serial_port() -> Option<String> {
    env_value("ESP32DASH_SERIAL_PORT")
}

pub fn serial_baud() -> u32 {
    env_value_or("ESP32DASH_SERIAL_BAUD", &DEFAULT_SERIAL_BAUD.to_string())
        .parse::<u32>()
        .unwrap_or(DEFAULT_SERIAL_BAUD)
}
