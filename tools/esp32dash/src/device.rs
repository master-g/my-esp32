use std::{
    collections::BTreeSet,
    ffi::CString,
    fs, io,
    os::{
        fd::{AsRawFd, FromRawFd, OwnedFd},
        unix::ffi::OsStrExt,
    },
    path::Path,
    sync::{
        Arc, Mutex,
        atomic::{AtomicU64, Ordering},
        mpsc as std_mpsc,
    },
    thread,
    time::{Duration, Instant},
};

use crate::approvals::ApprovalDecision;
use crate::model::{
    DeviceHello, DeviceListEntry, DeviceScreenshot, RpcRequest, ScreenshotChunkEvent,
    ScreenshotDoneEvent, ScreenshotErrorEvent, ScreenshotStartResponse, SerialConnectionStatus,
    Snapshot, WireFrame,
};
use anyhow::{Context, Result, anyhow, bail};
use base64::{Engine as _, engine::general_purpose::STANDARD as BASE64};
use serde_json::{Value, json};
use tokio::sync::{mpsc, oneshot};
use tracing::{debug, info, warn};

pub const PROTOCOL_PREFIX: &str = "@esp32dash ";

const MAX_LINE_BUFFER: usize = 4096;
const HELLO_TIMEOUT: Duration = Duration::from_secs(2);
const RPC_TIMEOUT: Duration = Duration::from_secs(2);
const RPC_TIMEOUT_SCREENSHOT_START: Duration = Duration::from_secs(10);
const IDLE_POLL_INTERVAL: Duration = Duration::from_millis(200);
const READ_POLL_INTERVAL: Duration = Duration::from_millis(10);
const RECONNECT_DELAY: Duration = Duration::from_secs(2);
const SCREENSHOT_STREAM_TIMEOUT: Duration = Duration::from_secs(60);
const SCREENSHOT_FORMAT_RGB565_LE: &str = "rgb565_le";
const SCREENSHOT_MAX_WIDTH: usize = 640;
const SCREENSHOT_MAX_HEIGHT: usize = 172;
const SCREENSHOT_MAX_BYTES: usize = SCREENSHOT_MAX_WIDTH * SCREENSHOT_MAX_HEIGHT * 2;

static NEXT_REQUEST_ID: AtomicU64 = AtomicU64::new(1);

pub trait DeviceSession: Send {
    fn read_frame(&mut self, timeout: Duration) -> Result<Option<WireFrame>>;
    fn write_frame(&mut self, frame: &WireFrame) -> Result<()>;
}

pub trait SessionFactory: Send + Sync {
    fn connect(&self, port: &str, baud: u32) -> Result<Box<dyn DeviceSession>>;
}

#[derive(Debug, Clone)]
pub struct DeviceConfig {
    pub preferred_port: Option<String>,
    pub baud: u32,
    pub rpc_timeout_approval: Duration,
}

enum WorkerCommand {
    UpdateSnapshot(Snapshot),
    Event {
        method: String,
        payload: Value,
    },
    Rpc {
        request: RpcRequest,
        reply: oneshot::Sender<Result<Value, String>>,
    },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DeviceEvent {
    ApprovalResolved {
        id: String,
        decision: ApprovalDecision,
    },
    PromptResolved {
        id: String,
        selection_index: u8,
    },
}

#[derive(Clone)]
pub struct DeviceManager {
    cmd_tx: std_mpsc::Sender<WorkerCommand>,
    status: Arc<Mutex<SerialConnectionStatus>>,
}

impl DeviceManager {
    pub fn start(
        config: DeviceConfig,
        factory: Arc<dyn SessionFactory>,
        initial_snapshot: Option<Snapshot>,
        event_tx: mpsc::UnboundedSender<DeviceEvent>,
    ) -> Self {
        let status = Arc::new(Mutex::new(SerialConnectionStatus {
            configured_port: config.preferred_port.clone(),
            ..SerialConnectionStatus::default()
        }));
        let (cmd_tx, cmd_rx) = std_mpsc::channel();
        let worker_status = status.clone();

        thread::spawn(move || {
            worker_loop(config, factory, worker_status, cmd_rx, initial_snapshot, event_tx)
        });

        Self {
            cmd_tx,
            status,
        }
    }

    pub fn send_snapshot(&self, snapshot: Snapshot) {
        if let Err(err) = self.cmd_tx.send(WorkerCommand::UpdateSnapshot(snapshot)) {
            tracing::warn!("failed to queue snapshot for device worker: {err}");
        }
    }

    pub fn send_protocol_event(&self, method: impl Into<String>, payload: Value) -> Result<()> {
        self.cmd_tx
            .send(WorkerCommand::Event {
                method: method.into(),
                payload,
            })
            .map_err(|_| anyhow!("device worker is unavailable"))
    }

    pub async fn rpc(&self, request: RpcRequest) -> Result<Value> {
        let (reply_tx, reply_rx) = oneshot::channel();
        self.cmd_tx
            .send(WorkerCommand::Rpc {
                request,
                reply: reply_tx,
            })
            .map_err(|_| anyhow!("device worker is unavailable"))?;

        reply_rx
            .await
            .map_err(|_| anyhow!("device worker dropped rpc response"))?
            .map_err(|message| anyhow!(message))
    }

    pub fn status(&self) -> SerialConnectionStatus {
        self.status.lock().expect("serial status mutex poisoned").clone()
    }
}

pub fn discover_devices(
    factory: Arc<dyn SessionFactory>,
    baud: u32,
) -> Result<Vec<DeviceListEntry>> {
    let candidates = candidate_ports()?;
    debug!(count = candidates.len(), "probing serial port candidates");
    let mut devices = Vec::new();
    for port in candidates {
        debug!(port, "probing...");
        if let Ok((_, hello)) = connect_port(factory.as_ref(), &port, baud) {
            devices.push(DeviceListEntry {
                port,
                hello,
            });
        }
    }
    Ok(devices)
}

pub fn request_direct(
    factory: Arc<dyn SessionFactory>,
    preferred_port: Option<&str>,
    baud: u32,
    request: RpcRequest,
) -> Result<Value> {
    request_direct_with_timeout(factory, preferred_port, baud, request, RPC_TIMEOUT)
}

pub fn request_direct_with_timeout(
    factory: Arc<dyn SessionFactory>,
    preferred_port: Option<&str>,
    baud: u32,
    request: RpcRequest,
    timeout: Duration,
) -> Result<Value> {
    let port = resolve_port(factory.clone(), preferred_port, baud)?;
    let (mut session, _) = connect_port(factory.as_ref(), &port, baud)?;
    perform_rpc(session.as_mut(), request, timeout)
}

pub fn capture_screenshot_direct(
    factory: Arc<dyn SessionFactory>,
    preferred_port: Option<&str>,
    baud: u32,
) -> Result<DeviceScreenshot> {
    let port = resolve_port(factory.clone(), preferred_port, baud)?;
    let (mut session, _) = connect_port(factory.as_ref(), &port, baud)?;
    capture_screenshot(session.as_mut())
}

pub fn send_event_direct(
    factory: Arc<dyn SessionFactory>,
    preferred_port: Option<&str>,
    baud: u32,
    snapshot: &Snapshot,
) -> Result<()> {
    send_protocol_event_direct(
        factory,
        preferred_port,
        baud,
        "claude.update",
        serde_json::to_value(snapshot)?,
    )
}

pub fn send_protocol_event_direct(
    factory: Arc<dyn SessionFactory>,
    preferred_port: Option<&str>,
    baud: u32,
    method: &str,
    payload: Value,
) -> Result<()> {
    let port = resolve_port(factory.clone(), preferred_port, baud)?;
    let (mut session, _) = connect_port(factory.as_ref(), &port, baud)?;
    send_named_event(session.as_mut(), method, payload)?;
    // Verify delivery with a round-trip RPC; the device processes
    // frames in order, so a successful response guarantees the
    // preceding event has been handled.
    let _ = perform_rpc(
        session.as_mut(),
        RpcRequest {
            method: "device.info".into(),
            params: serde_json::json!({}),
        },
        RPC_TIMEOUT,
    )?;
    Ok(())
}

pub fn open_direct_session(
    factory: Arc<dyn SessionFactory>,
    preferred_port: Option<&str>,
    baud: u32,
) -> Result<Box<dyn DeviceSession>> {
    let port = resolve_port(factory.clone(), preferred_port, baud)?;
    let (session, _) = connect_port(factory.as_ref(), &port, baud)?;
    Ok(session)
}

pub fn encode_frame_line(frame: &WireFrame) -> Result<String> {
    Ok(format!("{}{}\n", PROTOCOL_PREFIX, serde_json::to_string(frame)?))
}

pub fn decode_frame_line(line: &str) -> Result<Option<WireFrame>> {
    let trimmed = line.trim_end_matches('\n').trim_end_matches('\r');
    let Some(prefix_pos) = trimmed.rfind(PROTOCOL_PREFIX) else {
        return Ok(None);
    };

    let payload = &trimmed[prefix_pos + PROTOCOL_PREFIX.len()..];
    if payload.trim().is_empty() {
        return Ok(None);
    }
    let frame = serde_json::from_str(payload)
        .with_context(|| format!("invalid esp32dash frame payload: {payload}"))?;
    Ok(Some(frame))
}

fn worker_loop(
    config: DeviceConfig,
    factory: Arc<dyn SessionFactory>,
    status: Arc<Mutex<SerialConnectionStatus>>,
    cmd_rx: std_mpsc::Receiver<WorkerCommand>,
    initial_snapshot: Option<Snapshot>,
    event_tx: mpsc::UnboundedSender<DeviceEvent>,
) {
    let mut pending_snapshot = initial_snapshot.filter(|snapshot| snapshot.seq > 0);

    loop {
        match connect_for_worker(config.clone(), factory.clone()) {
            Ok((port, mut session, hello)) => {
                set_connected(&status, &port, &hello);

                if let Some(snapshot) = pending_snapshot.as_ref() {
                    if let Err(err) = send_snapshot_update(session.as_mut(), snapshot) {
                        set_last_error(
                            &status,
                            format!("failed to send initial snapshot: {err:#}"),
                        );
                        set_disconnected(&status);
                        thread::sleep(RECONNECT_DELAY);
                        continue;
                    }
                }

                loop {
                    match cmd_rx.recv_timeout(IDLE_POLL_INTERVAL) {
                        Ok(WorkerCommand::UpdateSnapshot(snapshot)) => {
                            pending_snapshot = Some(snapshot.clone());
                            if let Err(err) = send_snapshot_update(session.as_mut(), &snapshot) {
                                set_last_error(
                                    &status,
                                    format!("failed to push claude snapshot over serial: {err:#}"),
                                );
                                set_disconnected(&status);
                                break;
                            }
                        }
                        Ok(WorkerCommand::Event {
                            method,
                            payload,
                        }) => {
                            if let Err(err) = send_named_event(session.as_mut(), &method, payload) {
                                set_last_error(
                                    &status,
                                    format!("failed to push event {method} over serial: {err:#}"),
                                );
                                set_disconnected(&status);
                                break;
                            }
                        }
                        Ok(WorkerCommand::Rpc {
                            request,
                            reply,
                        }) => {
                            let timeout = if request.method == "claude.approve" {
                                config.rpc_timeout_approval
                            } else {
                                RPC_TIMEOUT
                            };
                            let result = perform_rpc(session.as_mut(), request, timeout)
                                .map_err(|err| err.to_string());
                            let should_disconnect = result.is_err();
                            let _ = reply.send(result);
                            if should_disconnect {
                                set_last_error(
                                    &status,
                                    "serial rpc failed; reconnecting active session".to_string(),
                                );
                                set_disconnected(&status);
                                break;
                            }
                        }
                        Err(std_mpsc::RecvTimeoutError::Disconnected) => return,
                        Err(std_mpsc::RecvTimeoutError::Timeout) => {}
                    }

                    match session.read_frame(READ_POLL_INTERVAL) {
                        Ok(Some(frame)) => match frame {
                            WireFrame::Hello {
                                protocol_version,
                                device_id,
                                product,
                                capabilities,
                            } => {
                                set_connected(
                                    &status,
                                    &port,
                                    &DeviceHello {
                                        protocol_version,
                                        device_id,
                                        product,
                                        capabilities,
                                    },
                                );
                            }
                            WireFrame::Event {
                                method,
                                payload,
                            } => {
                                if let Some(device_event) = parse_device_event(&method, &payload) {
                                    if let Err(err) = event_tx.send(device_event) {
                                        warn!(
                                            "dropping device event because receiver is gone: {err}"
                                        );
                                    }
                                }
                            }
                            _ => {}
                        },
                        Ok(None) => {}
                        Err(err) => {
                            set_last_error(&status, format!("serial session dropped: {err:#}"));
                            set_disconnected(&status);
                            break;
                        }
                    }
                }
            }
            Err(err) => {
                set_last_error(&status, err.to_string());
                set_disconnected(&status);
                match cmd_rx.recv_timeout(RECONNECT_DELAY) {
                    Ok(WorkerCommand::UpdateSnapshot(snapshot)) => {
                        pending_snapshot = Some(snapshot)
                    }
                    Ok(WorkerCommand::Event {
                        ..
                    }) => {}
                    Ok(WorkerCommand::Rpc {
                        reply,
                        ..
                    }) => {
                        let _ = reply.send(Err("no compatible device connected".to_string()));
                    }
                    Err(std_mpsc::RecvTimeoutError::Timeout) => {}
                    Err(std_mpsc::RecvTimeoutError::Disconnected) => return,
                }
            }
        }
    }
}

fn connect_for_worker(
    config: DeviceConfig,
    factory: Arc<dyn SessionFactory>,
) -> Result<(String, Box<dyn DeviceSession>, DeviceHello)> {
    let port = resolve_port(factory.clone(), config.preferred_port.as_deref(), config.baud)?;
    let (session, hello) = connect_port(factory.as_ref(), &port, config.baud)?;
    Ok((port, session, hello))
}

fn resolve_port(
    factory: Arc<dyn SessionFactory>,
    preferred_port: Option<&str>,
    baud: u32,
) -> Result<String> {
    if let Some(port) = preferred_port {
        info!(port, "using configured serial port");
        return Ok(port.to_string());
    }

    info!("scanning serial ports for compatible devices...");
    let devices = discover_devices(factory, baud)?;
    match devices.len() {
        0 => bail!("no compatible esp32dash device found on serial ports"),
        1 => {
            info!(
                port = devices[0].port,
                device_id = devices[0].hello.device_id,
                "auto-discovered device"
            );
            Ok(devices[0].port.clone())
        }
        _ => bail!("multiple compatible esp32dash devices found; set ESP32DASH_SERIAL_PORT"),
    }
}

fn connect_port(
    factory: &dyn SessionFactory,
    port: &str,
    baud: u32,
) -> Result<(Box<dyn DeviceSession>, DeviceHello)> {
    let mut session = factory
        .connect(port, baud)
        .with_context(|| format!("failed to open serial port {port}"))?;
    let hello = wait_for_hello(session.as_mut(), HELLO_TIMEOUT)
        .with_context(|| format!("device on {port} did not send a valid hello frame"))?;
    Ok((session, hello))
}

fn capture_screenshot(session: &mut dyn DeviceSession) -> Result<DeviceScreenshot> {
    let start_value = perform_rpc(
        session,
        RpcRequest {
            method: "screen.capture.start".into(),
            params: json!({}),
        },
        RPC_TIMEOUT_SCREENSHOT_START,
    )?;
    let meta: ScreenshotStartResponse =
        serde_json::from_value(start_value).context("invalid screenshot start response")?;
    validate_screenshot_meta(&meta)?;

    let mut data = vec![0u8; meta.data_size as usize];
    let mut received = vec![false; meta.chunk_count as usize];
    let mut bytes_received = 0usize;
    let deadline = Instant::now() + SCREENSHOT_STREAM_TIMEOUT;

    loop {
        let remaining = remaining(deadline);
        if remaining.is_zero() {
            bail!("timed out waiting for screenshot stream");
        }

        match session.read_frame(remaining)? {
            Some(WireFrame::Event {
                method,
                payload,
            }) if method == "screen.capture.chunk" => {
                let chunk: ScreenshotChunkEvent =
                    serde_json::from_value(payload).context("invalid screenshot chunk payload")?;
                let index = chunk.index as usize;
                let offset = index * meta.chunk_bytes as usize;
                let decoded = BASE64
                    .decode(chunk.data.as_bytes())
                    .context("invalid base64 screenshot chunk")?;

                if chunk.capture_id != meta.capture_id {
                    continue;
                }
                if index >= received.len() {
                    bail!("screenshot chunk index {index} is out of range");
                }
                if received[index] {
                    bail!("received duplicate screenshot chunk {index}");
                }
                let expected_chunk_len = usize::min(meta.chunk_bytes as usize, data.len() - offset);
                if decoded.len() != expected_chunk_len {
                    bail!(
                        "screenshot chunk {index} has invalid size {} (expected {expected_chunk_len})",
                        decoded.len()
                    );
                }
                if offset + decoded.len() > data.len() {
                    bail!("screenshot chunk {index} exceeds declared image size");
                }

                data[offset..offset + decoded.len()].copy_from_slice(&decoded);
                received[index] = true;
                bytes_received += decoded.len();
            }
            Some(WireFrame::Event {
                method,
                payload,
            }) if method == "screen.capture.done" => {
                let done: ScreenshotDoneEvent =
                    serde_json::from_value(payload).context("invalid screenshot done payload")?;

                if done.capture_id != meta.capture_id {
                    continue;
                }
                if done.bytes_sent != meta.data_size {
                    bail!(
                        "device reported {} screenshot bytes, expected {}",
                        done.bytes_sent,
                        meta.data_size
                    );
                }
                if done.chunks_sent != meta.chunk_count {
                    bail!(
                        "device reported {} screenshot chunks, expected {}",
                        done.chunks_sent,
                        meta.chunk_count
                    );
                }
                break;
            }
            Some(WireFrame::Event {
                method,
                payload,
            }) if method == "screen.capture.error" => {
                let error: ScreenshotErrorEvent =
                    serde_json::from_value(payload).context("invalid screenshot error payload")?;

                if error.capture_id.is_empty() || error.capture_id == meta.capture_id {
                    bail!("device screenshot failed: {}", error.message);
                }
            }
            Some(_) => {}
            None => bail!("timed out waiting for screenshot stream"),
        }
    }

    if received.iter().any(|chunk| !chunk) {
        bail!("screenshot stream ended before all chunks arrived");
    }
    if bytes_received != data.len() {
        bail!("screenshot stream delivered {} bytes, expected {}", bytes_received, data.len());
    }

    Ok(DeviceScreenshot {
        meta,
        data,
    })
}

fn validate_screenshot_meta(meta: &ScreenshotStartResponse) -> Result<()> {
    if meta.capture_id.is_empty() {
        bail!("device returned an empty screenshot capture_id");
    }
    if meta.format != SCREENSHOT_FORMAT_RGB565_LE {
        bail!(
            "unsupported screenshot format {} (expected {SCREENSHOT_FORMAT_RGB565_LE})",
            meta.format
        );
    }
    if meta.width == 0
        || meta.height == 0
        || meta.stride_bytes == 0
        || meta.data_size == 0
        || meta.chunk_bytes == 0
        || meta.chunk_count == 0
    {
        bail!("device returned incomplete screenshot metadata");
    }

    let width = usize::from(meta.width);
    let height = usize::from(meta.height);
    let stride_bytes = usize::from(meta.stride_bytes);
    let data_size = meta.data_size as usize;
    let chunk_bytes = meta.chunk_bytes as usize;
    let chunk_count = meta.chunk_count as usize;

    if width > SCREENSHOT_MAX_WIDTH || height > SCREENSHOT_MAX_HEIGHT {
        bail!("device returned oversized screenshot dimensions {}x{}", meta.width, meta.height);
    }

    let expected_stride =
        width.checked_mul(2).ok_or_else(|| anyhow!("screenshot stride overflow"))?;
    if stride_bytes != expected_stride {
        bail!(
            "device reported screenshot stride {} but expected {}",
            meta.stride_bytes,
            expected_stride
        );
    }

    let expected_data_size =
        stride_bytes.checked_mul(height).ok_or_else(|| anyhow!("screenshot byte size overflow"))?;
    if expected_data_size > SCREENSHOT_MAX_BYTES {
        bail!(
            "device reported screenshot size {} larger than supported maximum {}",
            expected_data_size,
            SCREENSHOT_MAX_BYTES
        );
    }
    if data_size != expected_data_size {
        bail!(
            "device reported screenshot size {} but expected {}",
            meta.data_size,
            expected_data_size
        );
    }

    let expected_chunk_count = data_size.div_ceil(chunk_bytes);
    if chunk_count != expected_chunk_count {
        bail!(
            "device reported screenshot chunk_count {} but expected {}",
            meta.chunk_count,
            expected_chunk_count
        );
    }

    Ok(())
}

fn wait_for_hello(session: &mut dyn DeviceSession, timeout: Duration) -> Result<DeviceHello> {
    let deadline = Instant::now() + timeout;
    loop {
        let remaining = remaining(deadline);
        if remaining.is_zero() {
            bail!("timed out waiting for device hello");
        }

        match session.read_frame(remaining)? {
            Some(frame) => {
                if let Some(hello) = frame.into_hello() {
                    return Ok(hello);
                }
            }
            None => bail!("timed out waiting for device hello"),
        }
    }
}

fn perform_rpc(
    session: &mut dyn DeviceSession,
    request: RpcRequest,
    timeout: Duration,
) -> Result<Value> {
    let request_id = format!("rpc-{}", NEXT_REQUEST_ID.fetch_add(1, Ordering::SeqCst));
    session.write_frame(&WireFrame::request(request_id.clone(), request.method, request.params))?;

    let deadline = Instant::now() + timeout;
    loop {
        let remaining = remaining(deadline);
        if remaining.is_zero() {
            bail!("device rpc timed out");
        }

        match session.read_frame(remaining)? {
            Some(WireFrame::Response {
                id,
                ok,
                result,
                error,
            }) if id == request_id => {
                if ok {
                    return Ok(result.unwrap_or_else(|| json!({})));
                }

                let message = error
                    .map(|error| format!("{}: {}", error.code, error.message))
                    .unwrap_or_else(|| "device returned an unknown rpc error".to_string());
                bail!(message);
            }
            Some(_) => {}
            None => bail!("device rpc timed out"),
        }
    }
}

fn send_snapshot_update(session: &mut dyn DeviceSession, snapshot: &Snapshot) -> Result<()> {
    send_named_event(session, "claude.update", serde_json::to_value(snapshot)?)
}

fn send_named_event(session: &mut dyn DeviceSession, method: &str, payload: Value) -> Result<()> {
    session.write_frame(&WireFrame::event(method, payload))
}

fn parse_device_event(method: &str, payload: &Value) -> Option<DeviceEvent> {
    match method {
        "claude.approval.resolved" => {
            let id = payload.get("id")?.as_str()?.to_string();
            let decision = match payload.get("decision")?.as_str()? {
                "allow" => ApprovalDecision::Allow,
                "deny" => ApprovalDecision::Deny,
                "allow_always" | "yolo" => ApprovalDecision::AllowAlways,
                other => {
                    warn!(decision = other, "ignoring unknown approval decision from device");
                    return None;
                }
            };

            Some(DeviceEvent::ApprovalResolved { id, decision })
        }
        "claude.prompt.response" => {
            let id = payload.get("id")?.as_str()?.to_string();
            let selection_index = payload
                .get("selection_index")
                .and_then(|v| v.as_u64())
                .map(|v| v as u8)?;

            Some(DeviceEvent::PromptResolved {
                id,
                selection_index,
            })
        }
        _ => None,
    }
}

fn set_connected(status: &Arc<Mutex<SerialConnectionStatus>>, port: &str, hello: &DeviceHello) {
    info!(
        port,
        device_id = hello.device_id,
        product = hello.product,
        protocol = hello.protocol_version,
        "connected to device"
    );
    let mut guard = status.lock().expect("serial status mutex poisoned");
    guard.active_port = Some(port.to_string());
    guard.connected = true;
    guard.protocol_version = Some(hello.protocol_version);
    guard.device_id = Some(hello.device_id.clone());
    guard.product = Some(hello.product.clone());
    guard.capabilities = hello.capabilities.clone();
    guard.last_error = None;
}

fn set_disconnected(status: &Arc<Mutex<SerialConnectionStatus>>) {
    debug!("device disconnected, will attempt reconnection");
    let mut guard = status.lock().expect("serial status mutex poisoned");
    guard.connected = false;
    guard.active_port = None;
    guard.protocol_version = None;
    guard.device_id = None;
    guard.product = None;
    guard.capabilities.clear();
}

fn set_last_error(status: &Arc<Mutex<SerialConnectionStatus>>, message: String) {
    warn!(error = message, "device error");
    let mut guard = status.lock().expect("serial status mutex poisoned");
    guard.last_error = Some(message);
}

fn remaining(deadline: Instant) -> Duration {
    deadline.saturating_duration_since(Instant::now())
}

fn candidate_ports() -> Result<Vec<String>> {
    let mut names = Vec::new();
    let mut seen = BTreeSet::new();
    for entry in fs::read_dir("/dev").context("failed to read /dev for serial discovery")? {
        let entry = entry?;
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if looks_like_serial_port(&name) {
            names.push(name.to_string());
        }
    }

    names.sort_by_key(|name| {
        if name.starts_with("tty.") {
            0
        } else {
            1
        }
    });

    let mut ports = Vec::new();
    for name in names {
        let canonical = canonical_port_name(&name);
        if seen.insert(canonical) {
            ports.push(format!("/dev/{name}"));
        }
    }

    Ok(ports)
}

fn looks_like_serial_port(name: &str) -> bool {
    name.starts_with("cu.usb")
        || name.starts_with("tty.usb")
        || name.starts_with("ttyUSB")
        || name.starts_with("ttyACM")
        || name.contains("usbmodem")
        || name.contains("usbserial")
        || name.contains("wchusbserial")
}

fn canonical_port_name(name: &str) -> String {
    if let Some(stripped) = name.strip_prefix("tty.").or_else(|| name.strip_prefix("cu.")) {
        stripped.to_string()
    } else {
        name.to_string()
    }
}

#[derive(Default)]
pub struct UnixSerialFactory;

impl SessionFactory for UnixSerialFactory {
    fn connect(&self, port: &str, baud: u32) -> Result<Box<dyn DeviceSession>> {
        Ok(Box::new(UnixSerialSession::open(port, baud)?))
    }
}

struct UnixSerialSession {
    port: UnixSerialPort,
}

impl UnixSerialSession {
    fn open(path: &str, baud: u32) -> Result<Self> {
        Ok(Self {
            port: UnixSerialPort::open(path, baud)?,
        })
    }
}

impl DeviceSession for UnixSerialSession {
    fn read_frame(&mut self, timeout: Duration) -> Result<Option<WireFrame>> {
        let deadline = Instant::now() + timeout;
        loop {
            let remaining = remaining(deadline);
            if remaining.is_zero() {
                return Ok(None);
            }

            match self.port.read_line(remaining)? {
                Some(line) => match decode_frame_line(&line)? {
                    Some(frame) => return Ok(Some(frame)),
                    None => continue,
                },
                None => return Ok(None),
            }
        }
    }

    fn write_frame(&mut self, frame: &WireFrame) -> Result<()> {
        self.port.write_line(&encode_frame_line(frame)?)
    }
}

struct UnixSerialPort {
    fd: OwnedFd,
    read_buf: Vec<u8>,
}

impl Drop for UnixSerialPort {
    fn drop(&mut self) {
        unsafe { libc::tcdrain(self.fd.as_raw_fd()) };
    }
}

impl UnixSerialPort {
    fn open(path: &str, baud: u32) -> Result<Self> {
        let c_path = CString::new(Path::new(path).as_os_str().as_bytes())
            .with_context(|| format!("serial path contains interior NUL: {path}"))?;
        let fd = unsafe {
            libc::open(c_path.as_ptr(), libc::O_RDWR | libc::O_NOCTTY | libc::O_NONBLOCK)
        };
        if fd < 0 {
            return Err(io::Error::last_os_error())
                .with_context(|| format!("failed to open serial port {path}"));
        }

        let fd = unsafe { OwnedFd::from_raw_fd(fd) };
        configure_serial(fd.as_raw_fd(), baud)?;

        Ok(Self {
            fd,
            read_buf: Vec::with_capacity(1024),
        })
    }

    fn write_line(&mut self, line: &str) -> Result<()> {
        let mut written = 0;
        let bytes = line.as_bytes();
        while written < bytes.len() {
            wait_fd(self.fd.as_raw_fd(), libc::POLLOUT, Duration::from_secs(1))?;
            let rc = unsafe {
                libc::write(
                    self.fd.as_raw_fd(),
                    bytes[written..].as_ptr().cast(),
                    bytes.len() - written,
                )
            };
            if rc < 0 {
                let err = io::Error::last_os_error();
                if err.kind() == io::ErrorKind::WouldBlock {
                    continue;
                }
                return Err(err).context("failed to write serial frame");
            }

            written += rc as usize;
        }

        Ok(())
    }

    fn read_line(&mut self, timeout: Duration) -> Result<Option<String>> {
        let deadline = Instant::now() + timeout;
        loop {
            if let Some(pos) = self.read_buf.iter().position(|byte| *byte == b'\n') {
                let line = self.read_buf.drain(..=pos).collect::<Vec<_>>();
                return Ok(Some(String::from_utf8_lossy(&line).into_owned()));
            }

            let remaining = remaining(deadline);
            if remaining.is_zero() {
                return Ok(None);
            }

            if !wait_fd(self.fd.as_raw_fd(), libc::POLLIN, remaining)? {
                return Ok(None);
            }

            let mut buf = [0u8; 512];
            let rc = unsafe { libc::read(self.fd.as_raw_fd(), buf.as_mut_ptr().cast(), buf.len()) };
            if rc == 0 {
                bail!("serial device closed the connection");
            }
            if rc < 0 {
                let err = io::Error::last_os_error();
                if err.kind() == io::ErrorKind::WouldBlock {
                    continue;
                }
                return Err(err).context("failed to read serial line");
            }

            self.read_buf.extend_from_slice(&buf[..rc as usize]);

            if self.read_buf.len() > MAX_LINE_BUFFER {
                tracing::warn!(
                    "serial read buffer exceeded {MAX_LINE_BUFFER} bytes without newline, discarding"
                );
                self.read_buf.clear();
            }
        }
    }
}

fn configure_serial(fd: i32, baud: u32) -> Result<()> {
    let speed = baud_constant(baud)?;
    let mut termios = unsafe { std::mem::zeroed::<libc::termios>() };

    if unsafe { libc::tcgetattr(fd, &mut termios) } != 0 {
        return Err(io::Error::last_os_error()).context("tcgetattr failed");
    }

    unsafe {
        libc::cfmakeraw(&mut termios);
        libc::cfsetispeed(&mut termios, speed);
        libc::cfsetospeed(&mut termios, speed);
    }

    termios.c_cflag |= libc::CLOCAL | libc::CREAD;
    termios.c_cc[libc::VMIN] = 0;
    termios.c_cc[libc::VTIME] = 0;

    if unsafe { libc::tcsetattr(fd, libc::TCSANOW, &termios) } != 0 {
        return Err(io::Error::last_os_error()).context("tcsetattr failed");
    }

    Ok(())
}

fn baud_constant(baud: u32) -> Result<libc::speed_t> {
    match baud {
        9_600 => Ok(libc::B9600),
        19_200 => Ok(libc::B19200),
        38_400 => Ok(libc::B38400),
        57_600 => Ok(libc::B57600),
        115_200 => Ok(libc::B115200),
        230_400 => Ok(libc::B230400),
        _ => bail!("unsupported serial baud rate: {baud}"),
    }
}

fn wait_fd(fd: i32, events: i16, timeout: Duration) -> Result<bool> {
    let timeout_ms_u128 = timeout.as_millis().min(i32::MAX as u128);
    let timeout_ms = i32::try_from(timeout_ms_u128).unwrap_or(i32::MAX);
    let mut poll_fd = libc::pollfd {
        fd,
        events,
        revents: 0,
    };

    let rc = unsafe { libc::poll(&mut poll_fd, 1, timeout_ms) };
    if rc < 0 {
        return Err(io::Error::last_os_error()).context("poll on serial fd failed");
    }

    Ok(rc > 0)
}

#[cfg(test)]
mod tests {
    use std::collections::VecDeque;
    use std::sync::Mutex;

    use super::*;
    use crate::model::Attention;

    static REQUEST_ID_LOCK: Mutex<()> = Mutex::new(());

    struct FakeSession {
        reads: VecDeque<Result<Option<WireFrame>>>,
        writes: Vec<WireFrame>,
    }

    impl FakeSession {
        fn new(reads: Vec<Result<Option<WireFrame>>>) -> Self {
            Self {
                reads: VecDeque::from(reads),
                writes: Vec::new(),
            }
        }
    }

    impl DeviceSession for FakeSession {
        fn read_frame(&mut self, _timeout: Duration) -> Result<Option<WireFrame>> {
            self.reads.pop_front().unwrap_or_else(|| Ok(None))
        }

        fn write_frame(&mut self, frame: &WireFrame) -> Result<()> {
            self.writes.push(frame.clone());
            Ok(())
        }
    }

    #[test]
    fn codec_ignores_plain_log_lines() {
        assert!(decode_frame_line("I (42) app: hello").unwrap().is_none());
    }

    #[test]
    fn codec_round_trips_protocol_frame() {
        let frame = WireFrame::request("rpc-1", "device.info", json!({"verbose": true}));
        let encoded = encode_frame_line(&frame).unwrap();
        let decoded = decode_frame_line(&encoded).unwrap();
        assert_eq!(decoded, Some(frame));
    }

    #[test]
    fn wait_for_hello_skips_non_hello_frames() {
        let mut session = FakeSession::new(vec![
            Ok(Some(WireFrame::Event {
                method: "claude.update".into(),
                payload: json!({}),
            })),
            Ok(Some(WireFrame::Hello {
                protocol_version: 1,
                device_id: "esp32-dashboard".into(),
                product: "waveshare".into(),
                capabilities: vec!["config.get".into()],
            })),
        ]);

        let hello = wait_for_hello(&mut session, Duration::from_secs(1)).unwrap();
        assert_eq!(hello.device_id, "esp32-dashboard");
    }

    #[test]
    fn send_snapshot_update_uses_claude_update_event() {
        let mut session = FakeSession::new(Vec::new());
        send_snapshot_update(
            &mut session,
            &Snapshot {
                seq: 7,
                source: "claude_code".into(),
                session_id: "sess".into(),
                event: "Stop".into(),
                status: "waiting_for_input".into(),
                emotion: "neutral".into(),
                title: "Ready".into(),
                workspace: "esp32".into(),
                detail: "done".into(),
                permission_mode: "default".into(),
                ts: 1,
                unread: true,
                attention: Attention::Medium,
                has_pending_prompt: false,
            },
        )
        .unwrap();

        assert_eq!(session.writes.len(), 1);
        match &session.writes[0] {
            WireFrame::Event {
                method,
                payload,
            } => {
                assert_eq!(method, "claude.update");
                assert_eq!(payload["seq"], 7);
            }
            other => panic!("unexpected frame: {other:?}"),
        }
    }

    #[test]
    fn parses_device_approval_event() {
        let event = parse_device_event(
            "claude.approval.resolved",
            &json!({
                "id": "approval-1",
                "decision": "allow",
            }),
        );

        assert_eq!(
            event,
            Some(DeviceEvent::ApprovalResolved {
                id: "approval-1".into(),
                decision: ApprovalDecision::Allow,
            })
        );
    }

    #[test]
    fn capture_screenshot_collects_chunk_stream() {
        let _guard = REQUEST_ID_LOCK.lock().unwrap();
        let chunk = BASE64.encode([0x34u8, 0x12, 0x78, 0x56]);
        let mut session = FakeSession::new(vec![
            Ok(Some(WireFrame::Response {
                id: "rpc-1".into(),
                ok: true,
                result: Some(json!({
                    "capture_id": "capture-1",
                    "app": "home",
                    "source": "lvgl",
                    "format": "rgb565_le",
                    "width": 2,
                    "height": 1,
                    "stride_bytes": 4,
                    "data_size": 4,
                    "chunk_bytes": 4,
                    "chunk_count": 1
                })),
                error: None,
            })),
            Ok(Some(WireFrame::Event {
                method: "screen.capture.chunk".into(),
                payload: json!({
                    "capture_id": "capture-1",
                    "index": 0,
                    "data": chunk
                }),
            })),
            Ok(Some(WireFrame::Event {
                method: "screen.capture.done".into(),
                payload: json!({
                    "capture_id": "capture-1",
                    "chunks_sent": 1,
                    "bytes_sent": 4
                }),
            })),
        ]);

        NEXT_REQUEST_ID.store(1, Ordering::SeqCst);
        let screenshot = capture_screenshot(&mut session).unwrap();

        assert_eq!(screenshot.meta.capture_id, "capture-1");
        assert_eq!(screenshot.meta.width, 2);
        assert_eq!(screenshot.meta.height, 1);
        assert_eq!(screenshot.data, vec![0x34, 0x12, 0x78, 0x56]);
        assert_eq!(session.writes.len(), 1);
        match &session.writes[0] {
            WireFrame::Request {
                method,
                ..
            } => assert_eq!(method, "screen.capture.start"),
            other => panic!("unexpected frame: {other:?}"),
        }
    }

    #[test]
    fn capture_screenshot_rejects_duplicate_chunks() {
        let _guard = REQUEST_ID_LOCK.lock().unwrap();
        let chunk = BASE64.encode([0x34u8, 0x12]);
        let mut session = FakeSession::new(vec![
            Ok(Some(WireFrame::Response {
                id: "rpc-1".into(),
                ok: true,
                result: Some(json!({
                    "capture_id": "capture-2",
                    "app": "home",
                    "source": "lvgl",
                    "format": "rgb565_le",
                    "width": 1,
                    "height": 1,
                    "stride_bytes": 2,
                    "data_size": 2,
                    "chunk_bytes": 2,
                    "chunk_count": 1
                })),
                error: None,
            })),
            Ok(Some(WireFrame::Event {
                method: "screen.capture.chunk".into(),
                payload: json!({
                    "capture_id": "capture-2",
                    "index": 0,
                    "data": chunk
                }),
            })),
            Ok(Some(WireFrame::Event {
                method: "screen.capture.chunk".into(),
                payload: json!({
                    "capture_id": "capture-2",
                    "index": 0,
                    "data": BASE64.encode([0x78u8, 0x56])
                }),
            })),
        ]);

        NEXT_REQUEST_ID.store(1, Ordering::SeqCst);
        let err = capture_screenshot(&mut session).unwrap_err();

        assert!(err.to_string().contains("duplicate screenshot chunk 0"));
    }

    #[test]
    fn capture_screenshot_rejects_oversized_metadata() {
        let _guard = REQUEST_ID_LOCK.lock().unwrap();
        let mut session = FakeSession::new(vec![Ok(Some(WireFrame::Response {
            id: "rpc-1".into(),
            ok: true,
            result: Some(json!({
                "capture_id": "capture-3",
                "app": "home",
                "source": "lvgl",
                "format": "rgb565_le",
                "width": 641,
                "height": 172,
                "stride_bytes": 1282,
                "data_size": 220504,
                "chunk_bytes": 1024,
                "chunk_count": 216
            })),
            error: None,
        }))]);

        NEXT_REQUEST_ID.store(1, Ordering::SeqCst);
        let err = capture_screenshot(&mut session).unwrap_err();

        assert!(err.to_string().contains("oversized screenshot dimensions"));
    }

    #[test]
    fn capture_screenshot_rejects_short_chunk() {
        let _guard = REQUEST_ID_LOCK.lock().unwrap();
        let mut session = FakeSession::new(vec![
            Ok(Some(WireFrame::Response {
                id: "rpc-1".into(),
                ok: true,
                result: Some(json!({
                    "capture_id": "capture-4",
                    "app": "home",
                    "source": "lvgl",
                    "format": "rgb565_le",
                    "width": 1,
                    "height": 1,
                    "stride_bytes": 2,
                    "data_size": 2,
                    "chunk_bytes": 2,
                    "chunk_count": 1
                })),
                error: None,
            })),
            Ok(Some(WireFrame::Event {
                method: "screen.capture.chunk".into(),
                payload: json!({
                    "capture_id": "capture-4",
                    "index": 0,
                    "data": BASE64.encode([0x34u8])
                }),
            })),
        ]);

        NEXT_REQUEST_ID.store(1, Ordering::SeqCst);
        let err = capture_screenshot(&mut session).unwrap_err();

        assert!(err.to_string().contains("invalid size 1 (expected 2)"));
    }
}
