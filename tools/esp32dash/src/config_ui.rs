use std::fmt;

use anyhow::{Context, Result, bail};
use inquire::{Confirm, InquireError, Password, Select, Text};
use reqwest::{Client, Url};
use serde::Deserialize;
use serde_json::{Value, json};

use crate::{model::RpcRequest, run_request};

const GEOCODING_ENDPOINT: &str = "https://geocoding-api.open-meteo.com/v1/search";
const WEATHER_CITY_LABEL_MAX_CHARS: usize = 23;

#[derive(Debug, Clone, Deserialize)]
struct ExportResponse {
    items: Vec<ExportItem>,
}

#[derive(Debug, Clone, Deserialize)]
struct ExportItem {
    key: String,
    #[serde(default)]
    value: Option<Value>,
    #[serde(default)]
    has_value: Option<bool>,
}

#[derive(Debug, Clone)]
struct DeviceConfigState {
    wifi_ssid: String,
    wifi_password_has_value: bool,
    timezone_name: Option<String>,
    timezone_tz: String,
    weather_city_label: String,
    weather_latitude: String,
    weather_longitude: String,
}

#[derive(Debug, Default, Clone)]
struct PendingChanges {
    wifi_ssid: Option<String>,
    wifi_password: PasswordChange,
    timezone: Option<TimeZoneChoice>,
    weather: Option<WeatherChoice>,
}

#[derive(Debug, Default, Clone)]
enum PasswordChange {
    #[default]
    Unchanged,
    Set(String),
}

#[derive(Debug, Clone)]
struct TimeZoneChoice {
    name: &'static str,
    posix_tz: &'static str,
    label: &'static str,
}

impl fmt::Display for TimeZoneChoice {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.label)
    }
}

#[derive(Debug, Clone)]
struct WeatherChoice {
    city_label: String,
    latitude: String,
    longitude: String,
    label: String,
}

impl fmt::Display for WeatherChoice {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.label)
    }
}

#[derive(Debug, Clone, Deserialize)]
struct WiFiScanResponse {
    aps: Vec<WiFiScanEntry>,
}

#[derive(Debug, Clone, Deserialize)]
struct WiFiScanEntry {
    ssid: String,
    rssi: i32,
    auth_mode: String,
    auth_required: bool,
}

impl fmt::Display for WiFiScanEntry {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}  {} dBm  {}",
            self.ssid,
            self.rssi,
            if self.auth_required {
                self.auth_mode.as_str()
            } else {
                "open"
            }
        )
    }
}

#[derive(Debug, Clone)]
enum ConfigMenuChoice {
    Wifi(String),
    Time(String),
    Weather(String),
    Submit(String),
    Cancel,
}

impl fmt::Display for ConfigMenuChoice {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Wifi(label) | Self::Time(label) | Self::Weather(label) | Self::Submit(label) => {
                f.write_str(label)
            }
            Self::Cancel => f.write_str("Cancel without submitting changes"),
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
struct GeocodingResponse {
    #[serde(default)]
    results: Vec<GeocodingResult>,
}

#[derive(Debug, Clone, Deserialize)]
struct GeocodingResult {
    name: String,
    latitude: f64,
    longitude: f64,
    #[serde(default)]
    country: Option<String>,
    #[serde(default)]
    admin1: Option<String>,
    #[serde(default)]
    timezone: Option<String>,
}

pub async fn run_config_editor(port: Option<String>) -> Result<()> {
    let current = load_current_config(port.as_deref()).await?;
    let mut pending = PendingChanges::default();

    loop {
        let choice = prompt_select(
            "Select a configuration area to update",
            build_menu_choices(&current, &pending),
        )?;

        let Some(choice) = choice else {
            println!("Canceled. No configuration changes were submitted.");
            return Ok(());
        };

        match choice {
            ConfigMenuChoice::Wifi(_) => {
                if let Some((ssid, password)) =
                    edit_wifi(&current, &pending, port.as_deref()).await?
                {
                    pending.wifi_ssid = Some(ssid);
                    pending.wifi_password = password;
                }
            }
            ConfigMenuChoice::Time(_) => {
                if let Some(timezone) = edit_timezone(&current, &pending)? {
                    pending.timezone = Some(timezone);
                }
            }
            ConfigMenuChoice::Weather(_) => {
                if let Some(weather) = edit_weather().await? {
                    pending.weather = Some(weather);
                }
            }
            ConfigMenuChoice::Submit(_) => {
                if !pending.has_changes() {
                    println!("No pending changes. Nothing to submit.");
                    return Ok(());
                }

                print_pending_summary(&current, &pending);
                let Some(confirm) =
                    prompt_confirm("Submit these changes to the device now?", true)?
                else {
                    println!("Canceled. No configuration changes were submitted.");
                    return Ok(());
                };
                if !confirm {
                    continue;
                }

                let payload = pending.to_set_many_payload();
                let result = run_request(
                    port.as_deref(),
                    RpcRequest {
                        method: "config.set_many".into(),
                        params: payload,
                    },
                )
                .await?;

                println!("{}", serde_json::to_string_pretty(&result)?);
                return Ok(());
            }
            ConfigMenuChoice::Cancel => {
                println!("Canceled. No configuration changes were submitted.");
                return Ok(());
            }
        }
    }
}

async fn load_current_config(port: Option<&str>) -> Result<DeviceConfigState> {
    let value = run_request(
        port,
        RpcRequest {
            method: "config.export".into(),
            params: json!({}),
        },
    )
    .await?;
    parse_config_state(&value)
}

fn parse_config_state(value: &Value) -> Result<DeviceConfigState> {
    let export: ExportResponse = serde_json::from_value(value.clone())
        .context("device returned an unsupported config.export shape")?;
    let mut state = DeviceConfigState {
        wifi_ssid: String::new(),
        wifi_password_has_value: false,
        timezone_name: None,
        timezone_tz: String::new(),
        weather_city_label: String::new(),
        weather_latitude: String::new(),
        weather_longitude: String::new(),
    };

    for item in export.items {
        match item.key.as_str() {
            "wifi.ssid" => {
                state.wifi_ssid = item.value.and_then(value_to_string).unwrap_or_default()
            }
            "wifi.password" => state.wifi_password_has_value = item.has_value.unwrap_or(false),
            "time.timezone_name" => state.timezone_name = item.value.and_then(value_to_string),
            "time.timezone_tz" => {
                state.timezone_tz = item.value.and_then(value_to_string).unwrap_or_default()
            }
            "weather.city_label" => {
                state.weather_city_label = item.value.and_then(value_to_string).unwrap_or_default()
            }
            "weather.latitude" => {
                state.weather_latitude = item.value.and_then(value_to_string).unwrap_or_default()
            }
            "weather.longitude" => {
                state.weather_longitude = item.value.and_then(value_to_string).unwrap_or_default()
            }
            _ => {}
        }
    }

    if state.timezone_tz.is_empty() {
        bail!("config.export did not include time.timezone_tz");
    }

    Ok(state)
}

fn build_menu_choices(
    current: &DeviceConfigState,
    pending: &PendingChanges,
) -> Vec<ConfigMenuChoice> {
    let wifi_ssid = pending.wifi_ssid.as_deref().unwrap_or_else(|| {
        if current.wifi_ssid.is_empty() {
            "<not set>"
        } else {
            &current.wifi_ssid
        }
    });
    let wifi_password = match &pending.wifi_password {
        PasswordChange::Unchanged => {
            if current.wifi_password_has_value {
                "<stored>"
            } else {
                "<empty>"
            }
        }
        PasswordChange::Set(value) if value.is_empty() => "<cleared>",
        PasswordChange::Set(_) => "<updated>",
    };
    let timezone_name = pending
        .timezone
        .as_ref()
        .map(|zone| zone.name)
        .or(current.timezone_name.as_deref())
        .unwrap_or("Custom TZ");
    let weather = pending
        .weather
        .as_ref()
        .map(|weather| weather.label.as_str())
        .unwrap_or_else(|| {
            if current.weather_city_label.is_empty() {
                "<not set>"
            } else {
                current.weather_city_label.as_str()
            }
        });
    let pending_count = pending.pending_count();

    vec![
        ConfigMenuChoice::Wifi(format!("Wi-Fi: {} / password {}", wifi_ssid, wifi_password)),
        ConfigMenuChoice::Time(format!(
            "Time zone: {} ({})",
            timezone_name, current.timezone_tz
        )),
        ConfigMenuChoice::Weather(format!(
            "Weather city: {} ({}, {})",
            weather, current.weather_latitude, current.weather_longitude
        )),
        ConfigMenuChoice::Submit(format!("Submit {} pending change(s)", pending_count)),
        ConfigMenuChoice::Cancel,
    ]
}

async fn edit_wifi(
    current: &DeviceConfigState,
    pending: &PendingChanges,
    port: Option<&str>,
) -> Result<Option<(String, PasswordChange)>> {
    #[derive(Debug, Clone)]
    enum WiFiAction {
        Scan,
        Hidden,
        Clear,
        Back,
    }

    impl fmt::Display for WiFiAction {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            match self {
                Self::Scan => f.write_str("Scan visible networks"),
                Self::Hidden => f.write_str("Join hidden network"),
                Self::Clear => f.write_str("Clear stored Wi-Fi credentials"),
                Self::Back => f.write_str("Back"),
            }
        }
    }

    let Some(action) = prompt_select(
        "Choose how to update Wi-Fi",
        vec![
            WiFiAction::Scan,
            WiFiAction::Hidden,
            WiFiAction::Clear,
            WiFiAction::Back,
        ],
    )?
    else {
        return Ok(None);
    };

    match action {
        WiFiAction::Scan => select_visible_network(current, pending, port).await,
        WiFiAction::Hidden => join_hidden_network(),
        WiFiAction::Clear => {
            let Some(confirm) = prompt_confirm("Clear the stored Wi-Fi SSID and password?", false)?
            else {
                return Ok(None);
            };
            if confirm {
                Ok(Some((String::new(), PasswordChange::Set(String::new()))))
            } else {
                Ok(None)
            }
        }
        WiFiAction::Back => Ok(None),
    }
}

async fn select_visible_network(
    current: &DeviceConfigState,
    pending: &PendingChanges,
    port: Option<&str>,
) -> Result<Option<(String, PasswordChange)>> {
    let value = run_request(
        port,
        RpcRequest {
            method: "wifi.scan".into(),
            params: json!({}),
        },
    )
    .await?;
    let response: WiFiScanResponse = serde_json::from_value(value)
        .context("device returned an unsupported wifi.scan payload")?;

    if response.aps.is_empty() {
        println!("No visible Wi-Fi networks were found.");
        return Ok(None);
    }

    let mut aps = response.aps;
    aps.sort_by(|left, right| {
        right
            .rssi
            .cmp(&left.rssi)
            .then_with(|| left.ssid.cmp(&right.ssid))
    });
    aps.dedup_by(|left, right| left.ssid == right.ssid);

    let Some(ap) = prompt_select("Select a visible Wi-Fi network", aps)? else {
        return Ok(None);
    };

    if !ap.auth_required {
        return Ok(Some((ap.ssid, PasswordChange::Set(String::new()))));
    }

    if effective_has_password(current, pending) && effective_ssid(current, pending) == ap.ssid {
        #[derive(Debug, Clone)]
        enum PasswordAction {
            Keep,
            EnterNew,
            Clear,
        }

        impl fmt::Display for PasswordAction {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                match self {
                    Self::Keep => f.write_str("Keep stored password"),
                    Self::EnterNew => f.write_str("Enter new password"),
                    Self::Clear => f.write_str("Clear stored password"),
                }
            }
        }

        let Some(action) = prompt_select(
            &format!("How should `{}` use its password?", ap.ssid),
            vec![
                PasswordAction::Keep,
                PasswordAction::EnterNew,
                PasswordAction::Clear,
            ],
        )?
        else {
            return Ok(None);
        };

        return match action {
            PasswordAction::Keep => {
                println!("Keeping the stored password for `{}`.", ap.ssid);
                Ok(None)
            }
            PasswordAction::EnterNew => prompt_password_update(ap.ssid),
            PasswordAction::Clear => Ok(Some((ap.ssid, PasswordChange::Set(String::new())))),
        };
    }

    prompt_password_update(ap.ssid)
}

fn join_hidden_network() -> Result<Option<(String, PasswordChange)>> {
    let Some(ssid) = prompt_text("Enter the hidden SSID", None)? else {
        return Ok(None);
    };
    let ssid = ssid.trim().to_string();
    if ssid.is_empty() {
        println!("Hidden SSID cannot be empty.");
        return Ok(None);
    }

    let Some(password) =
        prompt_password("Enter the Wi-Fi password (leave empty for open network)")?
    else {
        return Ok(None);
    };
    Ok(Some((ssid, PasswordChange::Set(password))))
}

fn prompt_password_update(ssid: String) -> Result<Option<(String, PasswordChange)>> {
    let Some(password) = prompt_password(&format!("Enter the password for `{ssid}`"))? else {
        return Ok(None);
    };
    Ok(Some((ssid, PasswordChange::Set(password))))
}

fn edit_timezone(
    current: &DeviceConfigState,
    pending: &PendingChanges,
) -> Result<Option<TimeZoneChoice>> {
    let initial = pending
        .timezone
        .as_ref()
        .map(|zone| zone.name)
        .or(current.timezone_name.as_deref());

    loop {
        let Some(query) = prompt_text("Search for a timezone name", initial)? else {
            return Ok(None);
        };
        let query = query.trim();
        let matches = find_timezones(query);
        if matches.is_empty() {
            println!("No matching timezones for `{query}`.");
            continue;
        }

        return prompt_select("Select a timezone", matches);
    }
}

async fn edit_weather() -> Result<Option<WeatherChoice>> {
    let Some(query) = prompt_text("Search for a weather city", None)? else {
        return Ok(None);
    };
    let query = query.trim().to_string();
    if query.is_empty() {
        return Ok(None);
    }

    let results = search_weather_locations(&query).await?;
    if results.is_empty() {
        println!("No city matches were found for `{query}`.");
        return Ok(None);
    }

    prompt_select("Select a weather location", results)
}

async fn search_weather_locations(query: &str) -> Result<Vec<WeatherChoice>> {
    let response = Client::new()
        .get(
            Url::parse_with_params(
                GEOCODING_ENDPOINT,
                [
                    ("name", query),
                    ("count", "10"),
                    ("language", "en"),
                    ("format", "json"),
                ],
            )
            .context("failed to build the geocoding request URL")?,
        )
        .timeout(std::time::Duration::from_secs(10))
        .send()
        .await
        .context("failed to query the weather geocoding service")?
        .error_for_status()
        .context("weather geocoding service returned a non-success status")?;

    let payload = response
        .json::<GeocodingResponse>()
        .await
        .context("failed to decode geocoding search results")?;

    Ok(payload
        .results
        .into_iter()
        .map(|result| WeatherChoice {
            city_label: truncate_city_label(&result.name),
            latitude: format!("{:.4}", result.latitude),
            longitude: format!("{:.4}", result.longitude),
            label: format_geocoding_label(&result),
        })
        .collect())
}

fn print_pending_summary(current: &DeviceConfigState, pending: &PendingChanges) {
    println!("Pending configuration changes:");

    if let Some(ssid) = pending.wifi_ssid.as_deref() {
        println!(
            "  wifi.ssid: {} -> {}",
            display_text(&current.wifi_ssid),
            display_text(ssid)
        );
    }
    match &pending.wifi_password {
        PasswordChange::Unchanged => {}
        PasswordChange::Set(value) if value.is_empty() => {
            println!(
                "  wifi.password: {} -> <cleared>",
                if current.wifi_password_has_value {
                    "<stored>"
                } else {
                    "<empty>"
                }
            );
        }
        PasswordChange::Set(_) => {
            println!(
                "  wifi.password: {} -> <updated>",
                if current.wifi_password_has_value {
                    "<stored>"
                } else {
                    "<empty>"
                }
            );
        }
    }
    if let Some(zone) = pending.timezone.as_ref() {
        println!(
            "  time.timezone_name: {} -> {}",
            current.timezone_name.as_deref().unwrap_or("Custom TZ"),
            zone.name
        );
        println!(
            "  time.timezone_tz: {} -> {}",
            current.timezone_tz, zone.posix_tz
        );
    }
    if let Some(weather) = pending.weather.as_ref() {
        println!(
            "  weather.city_label: {} -> {}",
            display_text(&current.weather_city_label),
            display_text(&weather.city_label)
        );
        println!(
            "  weather.latitude: {} -> {}",
            display_text(&current.weather_latitude),
            weather.latitude
        );
        println!(
            "  weather.longitude: {} -> {}",
            display_text(&current.weather_longitude),
            weather.longitude
        );
    }
}

fn effective_ssid(current: &DeviceConfigState, pending: &PendingChanges) -> String {
    pending
        .wifi_ssid
        .clone()
        .unwrap_or_else(|| current.wifi_ssid.clone())
}

fn effective_has_password(current: &DeviceConfigState, pending: &PendingChanges) -> bool {
    match &pending.wifi_password {
        PasswordChange::Unchanged => current.wifi_password_has_value,
        PasswordChange::Set(value) => !value.is_empty(),
    }
}

fn prompt_select<T>(message: &str, options: Vec<T>) -> Result<Option<T>>
where
    T: Clone + fmt::Display,
{
    match Select::new(message, options).prompt() {
        Ok(choice) => Ok(Some(choice)),
        Err(InquireError::OperationCanceled | InquireError::OperationInterrupted) => Ok(None),
        Err(err) => Err(err.into()),
    }
}

fn prompt_text(message: &str, initial: Option<&str>) -> Result<Option<String>> {
    let prompt = if let Some(initial) = initial {
        Text::new(message).with_initial_value(initial)
    } else {
        Text::new(message)
    };

    match prompt.prompt() {
        Ok(value) => Ok(Some(value)),
        Err(InquireError::OperationCanceled | InquireError::OperationInterrupted) => Ok(None),
        Err(err) => Err(err.into()),
    }
}

fn prompt_password(message: &str) -> Result<Option<String>> {
    match Password::new(message).without_confirmation().prompt() {
        Ok(value) => Ok(Some(value)),
        Err(InquireError::OperationCanceled | InquireError::OperationInterrupted) => Ok(None),
        Err(err) => Err(err.into()),
    }
}

fn prompt_confirm(message: &str, default: bool) -> Result<Option<bool>> {
    match Confirm::new(message).with_default(default).prompt() {
        Ok(value) => Ok(Some(value)),
        Err(InquireError::OperationCanceled | InquireError::OperationInterrupted) => Ok(None),
        Err(err) => Err(err.into()),
    }
}

fn value_to_string(value: Value) -> Option<String> {
    match value {
        Value::Null => None,
        Value::String(value) => Some(value),
        Value::Number(value) => Some(value.to_string()),
        Value::Bool(value) => Some(value.to_string()),
        _ => None,
    }
}

fn display_text(value: &str) -> &str {
    if value.is_empty() { "<empty>" } else { value }
}

fn truncate_city_label(name: &str) -> String {
    name.chars().take(WEATHER_CITY_LABEL_MAX_CHARS).collect()
}

fn format_geocoding_label(result: &GeocodingResult) -> String {
    let mut parts = vec![result.name.clone()];
    if let Some(admin1) = result
        .admin1
        .as_ref()
        .filter(|admin1| *admin1 != &result.name)
    {
        parts.push(admin1.clone());
    }
    if let Some(country) = result.country.as_ref() {
        parts.push(country.clone());
    }
    if let Some(timezone) = result.timezone.as_ref() {
        parts.push(timezone.clone());
    }
    format!(
        "{} ({:.4}, {:.4})",
        parts.join(", "),
        result.latitude,
        result.longitude
    )
}

fn find_timezones(query: &str) -> Vec<TimeZoneChoice> {
    let query = query.to_ascii_lowercase();
    TIMEZONES
        .iter()
        .filter(|zone| {
            query.is_empty()
                || zone.name.to_ascii_lowercase().contains(&query)
                || zone.label.to_ascii_lowercase().contains(&query)
        })
        .cloned()
        .collect()
}

impl PendingChanges {
    fn has_changes(&self) -> bool {
        self.pending_count() > 0
    }

    fn pending_count(&self) -> usize {
        let mut count = 0;
        if self.wifi_ssid.is_some() {
            count += 1;
        }
        if !matches!(self.wifi_password, PasswordChange::Unchanged) {
            count += 1;
        }
        if self.timezone.is_some() {
            count += 2;
        }
        if self.weather.is_some() {
            count += 3;
        }
        count
    }

    fn to_set_many_payload(&self) -> Value {
        let mut items = Vec::new();

        if let Some(ssid) = self.wifi_ssid.as_ref() {
            items.push(json!({ "key": "wifi.ssid", "value": ssid }));
        }
        if let PasswordChange::Set(password) = &self.wifi_password {
            items.push(json!({ "key": "wifi.password", "value": password }));
        }
        if let Some(zone) = self.timezone.as_ref() {
            items.push(json!({ "key": "time.timezone_name", "value": zone.name }));
            items.push(json!({ "key": "time.timezone_tz", "value": zone.posix_tz }));
        }
        if let Some(weather) = self.weather.as_ref() {
            items.push(json!({ "key": "weather.city_label", "value": weather.city_label }));
            items.push(json!({ "key": "weather.latitude", "value": weather.latitude }));
            items.push(json!({ "key": "weather.longitude", "value": weather.longitude }));
        }

        json!({ "items": items })
    }
}

const TIMEZONES: &[TimeZoneChoice] = &[
    TimeZoneChoice {
        name: "UTC",
        posix_tz: "UTC0",
        label: "UTC (Coordinated Universal Time)",
    },
    TimeZoneChoice {
        name: "Africa/Johannesburg",
        posix_tz: "SAST-2",
        label: "Africa/Johannesburg (UTC+02:00)",
    },
    TimeZoneChoice {
        name: "America/Anchorage",
        posix_tz: "AKST9AKDT,M3.2.0,M11.1.0",
        label: "America/Anchorage (Alaska Time)",
    },
    TimeZoneChoice {
        name: "America/Chicago",
        posix_tz: "CST6CDT,M3.2.0,M11.1.0",
        label: "America/Chicago (US Central Time)",
    },
    TimeZoneChoice {
        name: "America/Denver",
        posix_tz: "MST7MDT,M3.2.0,M11.1.0",
        label: "America/Denver (US Mountain Time)",
    },
    TimeZoneChoice {
        name: "America/Los_Angeles",
        posix_tz: "PST8PDT,M3.2.0,M11.1.0",
        label: "America/Los_Angeles (US Pacific Time)",
    },
    TimeZoneChoice {
        name: "America/New_York",
        posix_tz: "EST5EDT,M3.2.0,M11.1.0",
        label: "America/New_York (US Eastern Time)",
    },
    TimeZoneChoice {
        name: "America/Phoenix",
        posix_tz: "MST7",
        label: "America/Phoenix (UTC-07:00, no DST)",
    },
    TimeZoneChoice {
        name: "America/Sao_Paulo",
        posix_tz: "BRT3",
        label: "America/Sao_Paulo (UTC-03:00)",
    },
    TimeZoneChoice {
        name: "Asia/Bangkok",
        posix_tz: "ICT-7",
        label: "Asia/Bangkok (UTC+07:00)",
    },
    TimeZoneChoice {
        name: "Asia/Dubai",
        posix_tz: "GST-4",
        label: "Asia/Dubai (UTC+04:00)",
    },
    TimeZoneChoice {
        name: "Asia/Hong_Kong",
        posix_tz: "HKT-8",
        label: "Asia/Hong_Kong (UTC+08:00)",
    },
    TimeZoneChoice {
        name: "Asia/Kolkata",
        posix_tz: "IST-5:30",
        label: "Asia/Kolkata (UTC+05:30)",
    },
    TimeZoneChoice {
        name: "Asia/Seoul",
        posix_tz: "KST-9",
        label: "Asia/Seoul (UTC+09:00)",
    },
    TimeZoneChoice {
        name: "Asia/Shanghai",
        posix_tz: "CST-8",
        label: "Asia/Shanghai (China Standard Time)",
    },
    TimeZoneChoice {
        name: "Asia/Singapore",
        posix_tz: "SGT-8",
        label: "Asia/Singapore (UTC+08:00)",
    },
    TimeZoneChoice {
        name: "Asia/Tokyo",
        posix_tz: "JST-9",
        label: "Asia/Tokyo (UTC+09:00)",
    },
    TimeZoneChoice {
        name: "Australia/Adelaide",
        posix_tz: "ACST-9:30ACDT,M10.1.0,M4.1.0/3",
        label: "Australia/Adelaide (UTC+09:30 / DST)",
    },
    TimeZoneChoice {
        name: "Australia/Perth",
        posix_tz: "AWST-8",
        label: "Australia/Perth (UTC+08:00)",
    },
    TimeZoneChoice {
        name: "Australia/Sydney",
        posix_tz: "AEST-10AEDT,M10.1.0,M4.1.0/3",
        label: "Australia/Sydney (UTC+10:00 / DST)",
    },
    TimeZoneChoice {
        name: "Europe/Berlin",
        posix_tz: "CET-1CEST,M3.5.0/2,M10.5.0/3",
        label: "Europe/Berlin (Central European Time)",
    },
    TimeZoneChoice {
        name: "Europe/London",
        posix_tz: "GMT0BST,M3.5.0/1,M10.5.0",
        label: "Europe/London (UK Time)",
    },
    TimeZoneChoice {
        name: "Europe/Madrid",
        posix_tz: "CET-1CEST,M3.5.0/2,M10.5.0/3",
        label: "Europe/Madrid (Central European Time)",
    },
    TimeZoneChoice {
        name: "Europe/Moscow",
        posix_tz: "MSK-3",
        label: "Europe/Moscow (UTC+03:00)",
    },
    TimeZoneChoice {
        name: "Europe/Paris",
        posix_tz: "CET-1CEST,M3.5.0/2,M10.5.0/3",
        label: "Europe/Paris (Central European Time)",
    },
    TimeZoneChoice {
        name: "Pacific/Auckland",
        posix_tz: "NZST-12NZDT,M9.5.0,M4.1.0/3",
        label: "Pacific/Auckland (UTC+12:00 / DST)",
    },
    TimeZoneChoice {
        name: "Pacific/Honolulu",
        posix_tz: "HST10",
        label: "Pacific/Honolulu (UTC-10:00)",
    },
];

#[cfg(test)]
mod tests {
    use super::{find_timezones, parse_config_state};
    use serde_json::json;

    #[test]
    fn parse_config_state_reads_expected_keys() {
        let state = parse_config_state(&json!({
            "items": [
                {"key": "wifi.ssid", "value": "MyWiFi"},
                {"key": "wifi.password", "value": null, "has_value": true},
                {"key": "time.timezone_name", "value": "Asia/Shanghai"},
                {"key": "time.timezone_tz", "value": "CST-8"},
                {"key": "weather.city_label", "value": "Shanghai"},
                {"key": "weather.latitude", "value": "31.2304"},
                {"key": "weather.longitude", "value": "121.4737"}
            ]
        }))
        .unwrap();

        assert_eq!(state.wifi_ssid, "MyWiFi");
        assert!(state.wifi_password_has_value);
        assert_eq!(state.timezone_name.as_deref(), Some("Asia/Shanghai"));
        assert_eq!(state.weather_city_label, "Shanghai");
    }

    #[test]
    fn timezone_search_matches_by_substring() {
        let matches = find_timezones("shanghai");
        assert!(!matches.is_empty());
        assert_eq!(matches[0].name, "Asia/Shanghai");
    }
}
