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
}

#[derive(Debug, Clone)]
struct DeviceConfigState {
    timezone_name: Option<String>,
    timezone_tz: String,
    weather_city_label: String,
    weather_latitude: String,
    weather_longitude: String,
}

#[derive(Debug, Default, Clone)]
struct PendingChanges {
    timezone: Option<TimeZoneChoice>,
    weather: Option<WeatherChoice>,
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
struct WiFiProfilesResponse {
    profiles: Vec<WiFiProfileEntry>,
}

#[derive(Debug, Clone, Deserialize, PartialEq, Eq)]
struct WiFiProfileEntry {
    ssid: String,
    #[serde(default)]
    hidden: bool,
    #[serde(default)]
    has_password: bool,
    #[serde(default)]
    active: bool,
}

impl fmt::Display for WiFiProfileEntry {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}{}{}{}",
            self.ssid,
            if self.hidden {
                " [hidden]"
            } else {
                ""
            },
            if self.active {
                " [active]"
            } else {
                ""
            },
            if self.has_password {
                ""
            } else {
                " [open]"
            }
        )
    }
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
            Self::Cancel => f.write_str("Exit without submitting pending time/weather changes"),
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
    let mut wifi_profiles = load_wifi_profiles(port.as_deref()).await?;
    let mut pending = PendingChanges::default();

    loop {
        let choice = prompt_select(
            "Select a configuration area to update",
            build_menu_choices(&current, &wifi_profiles, &pending),
        )?;

        let Some(choice) = choice else {
            println!("Exited configuration editor.");
            return Ok(());
        };

        match choice {
            ConfigMenuChoice::Wifi(_) => {
                if edit_wifi_profiles(&wifi_profiles, port.as_deref()).await? {
                    wifi_profiles = load_wifi_profiles(port.as_deref()).await?;
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
                    println!("No pending timezone or weather changes.");
                    return Ok(());
                }

                print_pending_summary(&current, &pending);
                let Some(confirm) =
                    prompt_confirm("Submit these changes to the device now?", true)?
                else {
                    println!(
                        "Exited configuration editor without submitting pending time/weather changes."
                    );
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
                println!(
                    "Exited configuration editor without submitting pending time/weather changes."
                );
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

async fn load_wifi_profiles(port: Option<&str>) -> Result<Vec<WiFiProfileEntry>> {
    let value = run_request(
        port,
        RpcRequest {
            method: "wifi.profiles.list".into(),
            params: json!({}),
        },
    )
    .await?;
    let response: WiFiProfilesResponse = serde_json::from_value(value)
        .context("device returned an unsupported wifi.profiles.list payload")?;
    Ok(response.profiles)
}

fn parse_config_state(value: &Value) -> Result<DeviceConfigState> {
    let export: ExportResponse = serde_json::from_value(value.clone())
        .context("device returned an unsupported config.export shape")?;
    let mut state = DeviceConfigState {
        timezone_name: None,
        timezone_tz: String::new(),
        weather_city_label: String::new(),
        weather_latitude: String::new(),
        weather_longitude: String::new(),
    };

    for item in export.items {
        match item.key.as_str() {
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
    wifi_profiles: &[WiFiProfileEntry],
    pending: &PendingChanges,
) -> Vec<ConfigMenuChoice> {
    let active_profile = wifi_profiles
        .iter()
        .find(|profile| profile.active)
        .map(|profile| profile.ssid.as_str())
        .unwrap_or("<none>");
    let timezone_name = pending
        .timezone
        .as_ref()
        .map(|zone| zone.name)
        .or(current.timezone_name.as_deref())
        .unwrap_or("Custom TZ");
    let weather =
        pending.weather.as_ref().map(|weather| weather.label.as_str()).unwrap_or_else(|| {
            if current.weather_city_label.is_empty() {
                "<not set>"
            } else {
                current.weather_city_label.as_str()
            }
        });
    let pending_count = pending.pending_count();

    vec![
        ConfigMenuChoice::Wifi(format!(
            "Wi-Fi Profiles: {} stored / active {}",
            wifi_profiles.len(),
            active_profile
        )),
        ConfigMenuChoice::Time(format!("Time zone: {} ({})", timezone_name, current.timezone_tz)),
        ConfigMenuChoice::Weather(format!(
            "Weather city: {} ({}, {})",
            weather, current.weather_latitude, current.weather_longitude
        )),
        ConfigMenuChoice::Submit(format!("Submit {} pending change(s)", pending_count)),
        ConfigMenuChoice::Cancel,
    ]
}

async fn edit_wifi_profiles(profiles: &[WiFiProfileEntry], port: Option<&str>) -> Result<bool> {
    #[derive(Debug, Clone)]
    enum WiFiAction {
        AddVisible,
        AddHidden,
        Remove,
        Back,
    }

    impl fmt::Display for WiFiAction {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            match self {
                Self::AddVisible => f.write_str("Add visible network"),
                Self::AddHidden => f.write_str("Add hidden network"),
                Self::Remove => f.write_str("Remove stored profile"),
                Self::Back => f.write_str("Back"),
            }
        }
    }

    println!("Stored Wi-Fi profiles:");
    if profiles.is_empty() {
        println!("  <none>");
    } else {
        for (index, profile) in profiles.iter().enumerate() {
            println!("  {}. {}", index + 1, profile);
        }
    }

    let Some(action) = prompt_select(
        "Choose how to manage Wi-Fi profiles",
        vec![WiFiAction::AddVisible, WiFiAction::AddHidden, WiFiAction::Remove, WiFiAction::Back],
    )?
    else {
        return Ok(false);
    };

    match action {
        WiFiAction::AddVisible => select_visible_network(profiles, port).await,
        WiFiAction::AddHidden => join_hidden_network(profiles, port).await,
        WiFiAction::Remove => remove_wifi_profile(profiles, port).await,
        WiFiAction::Back => Ok(false),
    }
}

async fn select_visible_network(profiles: &[WiFiProfileEntry], port: Option<&str>) -> Result<bool> {
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
        return Ok(false);
    }

    let mut aps = response.aps;
    aps.sort_by(|left, right| right.rssi.cmp(&left.rssi).then_with(|| left.ssid.cmp(&right.ssid)));
    aps.dedup_by(|left, right| left.ssid == right.ssid);

    let Some(ap) = prompt_select("Select a visible Wi-Fi network", aps)? else {
        return Ok(false);
    };

    if !ap.auth_required {
        send_wifi_profile_add(port, &ap.ssid, Some(""), false).await?;
        return Ok(true);
    }

    if let Some(_existing) = find_profile(profiles, &ap.ssid) {
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
            vec![PasswordAction::Keep, PasswordAction::EnterNew, PasswordAction::Clear],
        )?
        else {
            return Ok(false);
        };

        return match action {
            PasswordAction::Keep => {
                send_wifi_profile_add(port, &ap.ssid, None, false).await?;
                Ok(true)
            }
            PasswordAction::EnterNew => prompt_password_update(&ap.ssid, false, port).await,
            PasswordAction::Clear => {
                send_wifi_profile_add(port, &ap.ssid, Some(""), false).await?;
                Ok(true)
            }
        };
    }

    prompt_password_update(&ap.ssid, false, port).await
}

async fn join_hidden_network(profiles: &[WiFiProfileEntry], port: Option<&str>) -> Result<bool> {
    let Some(ssid) = prompt_text("Enter the hidden SSID", None)? else {
        return Ok(false);
    };
    let ssid = ssid.trim().to_string();
    if ssid.is_empty() {
        println!("Hidden SSID cannot be empty.");
        return Ok(false);
    }

    if let Some(existing) = find_profile(profiles, &ssid) {
        #[derive(Debug, Clone)]
        enum HiddenAction {
            Keep,
            EnterNew,
            Clear,
        }

        impl fmt::Display for HiddenAction {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                match self {
                    Self::Keep => f.write_str("Keep stored password"),
                    Self::EnterNew => f.write_str("Enter new password"),
                    Self::Clear => f.write_str("Use empty password"),
                }
            }
        }

        if existing.has_password {
            let Some(action) = prompt_select(
                &format!("How should hidden profile `{}` use its password?", ssid),
                vec![HiddenAction::Keep, HiddenAction::EnterNew, HiddenAction::Clear],
            )?
            else {
                return Ok(false);
            };

            return match action {
                HiddenAction::Keep => {
                    send_wifi_profile_add(port, &ssid, None, true).await?;
                    Ok(true)
                }
                HiddenAction::EnterNew => prompt_password_update(&ssid, true, port).await,
                HiddenAction::Clear => {
                    send_wifi_profile_add(port, &ssid, Some(""), true).await?;
                    Ok(true)
                }
            };
        }
    }

    prompt_password_update(&ssid, true, port).await
}

async fn prompt_password_update(ssid: &str, hidden: bool, port: Option<&str>) -> Result<bool> {
    let Some(password) = prompt_password(&format!("Enter the password for `{ssid}`"))? else {
        return Ok(false);
    };
    send_wifi_profile_add(port, ssid, Some(&password), hidden).await?;
    Ok(true)
}

async fn remove_wifi_profile(profiles: &[WiFiProfileEntry], port: Option<&str>) -> Result<bool> {
    if profiles.is_empty() {
        println!("No stored Wi-Fi profiles to remove.");
        return Ok(false);
    }

    let Some(profile) =
        prompt_select("Select a stored Wi-Fi profile to remove", profiles.to_vec())?
    else {
        return Ok(false);
    };
    let Some(confirm) =
        prompt_confirm(&format!("Remove `{}` from stored Wi-Fi profiles?", profile.ssid), false)?
    else {
        return Ok(false);
    };
    if !confirm {
        return Ok(false);
    }

    run_request(
        port,
        RpcRequest {
            method: "wifi.profile.remove".into(),
            params: json!({
                "ssid": profile.ssid,
            }),
        },
    )
    .await?;
    Ok(true)
}

async fn send_wifi_profile_add(
    port: Option<&str>,
    ssid: &str,
    password: Option<&str>,
    hidden: bool,
) -> Result<()> {
    let mut params = json!({
        "ssid": ssid,
        "hidden": hidden,
    });

    if let Some(password) = password {
        params["password"] = json!(password);
    }

    run_request(
        port,
        RpcRequest {
            method: "wifi.profile.add".into(),
            params,
        },
    )
    .await?;
    Ok(())
}

fn find_profile<'a>(profiles: &'a [WiFiProfileEntry], ssid: &str) -> Option<&'a WiFiProfileEntry> {
    profiles.iter().find(|profile| profile.ssid == ssid)
}

fn edit_timezone(
    current: &DeviceConfigState,
    pending: &PendingChanges,
) -> Result<Option<TimeZoneChoice>> {
    let initial =
        pending.timezone.as_ref().map(|zone| zone.name).or(current.timezone_name.as_deref());

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
                [("name", query), ("count", "10"), ("language", "en"), ("format", "json")],
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
    if let Some(zone) = pending.timezone.as_ref() {
        println!(
            "  time.timezone_name: {} -> {}",
            current.timezone_name.as_deref().unwrap_or("Custom TZ"),
            zone.name
        );
        println!("  time.timezone_tz: {} -> {}", current.timezone_tz, zone.posix_tz);
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
    if value.is_empty() {
        "<empty>"
    } else {
        value
    }
}

fn truncate_city_label(name: &str) -> String {
    name.chars().take(WEATHER_CITY_LABEL_MAX_CHARS).collect()
}

fn format_geocoding_label(result: &GeocodingResult) -> String {
    let mut parts = vec![result.name.clone()];
    if let Some(admin1) = result.admin1.as_ref().filter(|admin1| *admin1 != &result.name) {
        parts.push(admin1.clone());
    }
    if let Some(country) = result.country.as_ref() {
        parts.push(country.clone());
    }
    if let Some(timezone) = result.timezone.as_ref() {
        parts.push(timezone.clone());
    }
    format!("{} ({:.4}, {:.4})", parts.join(", "), result.latitude, result.longitude)
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
    use super::{WiFiProfilesResponse, find_timezones, parse_config_state};
    use serde_json::json;

    #[test]
    fn parse_config_state_reads_expected_keys() {
        let state = parse_config_state(&json!({
            "items": [
                {"key": "time.timezone_name", "value": "Asia/Shanghai"},
                {"key": "time.timezone_tz", "value": "CST-8"},
                {"key": "weather.city_label", "value": "Shanghai"},
                {"key": "weather.latitude", "value": "31.2304"},
                {"key": "weather.longitude", "value": "121.4737"}
            ]
        }))
        .unwrap();

        assert_eq!(state.timezone_name.as_deref(), Some("Asia/Shanghai"));
        assert_eq!(state.weather_city_label, "Shanghai");
    }

    #[test]
    fn parse_wifi_profiles_response_reads_active_profile() {
        let response: WiFiProfilesResponse = serde_json::from_value(json!({
            "profiles": [
                {"ssid": "Home-5G", "hidden": false, "has_password": true, "active": true},
                {"ssid": "Office", "hidden": true, "has_password": true, "active": false}
            ]
        }))
        .unwrap();

        assert_eq!(response.profiles.len(), 2);
        assert!(response.profiles[0].active);
        assert!(response.profiles[1].hidden);
    }

    #[test]
    fn timezone_search_matches_by_substring() {
        let matches = find_timezones("shanghai");
        assert!(!matches.is_empty());
        assert_eq!(matches[0].name, "Asia/Shanghai");
    }
}
