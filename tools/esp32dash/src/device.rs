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
        mpsc,
    },
    thread,
    time::{Duration, Instant},
};

use crate::model::{
    DeviceHello, DeviceListEntry, RpcRequest, SerialConnectionStatus, Snapshot, WireFrame,
};
use anyhow::{Context, Result, anyhow, bail};
use serde_json::{Value, json};
use tokio::sync::oneshot;

pub const PROTOCOL_PREFIX: &str = "@esp32dash ";

const MAX_LINE_BUFFER: usize = 4096;
const HELLO_TIMEOUT: Duration = Duration::from_secs(2);
const RPC_TIMEOUT: Duration = Duration::from_secs(2);
const IDLE_POLL_INTERVAL: Duration = Duration::from_millis(200);
const READ_POLL_INTERVAL: Duration = Duration::from_millis(10);
const RECONNECT_DELAY: Duration = Duration::from_secs(2);

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
}

enum WorkerCommand {
    UpdateSnapshot(Snapshot),
    Rpc {
        request: RpcRequest,
        reply: oneshot::Sender<Result<Value, String>>,
    },
}

#[derive(Clone)]
pub struct DeviceManager {
    cmd_tx: mpsc::Sender<WorkerCommand>,
    status: Arc<Mutex<SerialConnectionStatus>>,
}

impl DeviceManager {
    pub fn start(
        config: DeviceConfig,
        factory: Arc<dyn SessionFactory>,
        initial_snapshot: Option<Snapshot>,
    ) -> Self {
        let status = Arc::new(Mutex::new(SerialConnectionStatus {
            configured_port: config.preferred_port.clone(),
            ..SerialConnectionStatus::default()
        }));
        let (cmd_tx, cmd_rx) = mpsc::channel();
        let worker_status = status.clone();

        thread::spawn(move || {
            worker_loop(config, factory, worker_status, cmd_rx, initial_snapshot)
        });

        Self { cmd_tx, status }
    }

    pub fn send_snapshot(&self, snapshot: Snapshot) {
        if let Err(err) = self.cmd_tx.send(WorkerCommand::UpdateSnapshot(snapshot)) {
            tracing::warn!("failed to queue snapshot for device worker: {err}");
        }
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
        self.status
            .lock()
            .expect("serial status mutex poisoned")
            .clone()
    }
}

pub fn discover_devices(
    factory: Arc<dyn SessionFactory>,
    baud: u32,
) -> Result<Vec<DeviceListEntry>> {
    let mut devices = Vec::new();
    for port in candidate_ports()? {
        if let Ok((_, hello)) = connect_port(factory.as_ref(), &port, baud) {
            devices.push(DeviceListEntry { port, hello });
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
    let port = resolve_port(factory.clone(), preferred_port, baud)?;
    let (mut session, _) = connect_port(factory.as_ref(), &port, baud)?;
    perform_rpc(session.as_mut(), request, RPC_TIMEOUT)
}

pub fn send_event_direct(
    factory: Arc<dyn SessionFactory>,
    preferred_port: Option<&str>,
    baud: u32,
    snapshot: &Snapshot,
) -> Result<()> {
    let port = resolve_port(factory.clone(), preferred_port, baud)?;
    let (mut session, _) = connect_port(factory.as_ref(), &port, baud)?;
    send_snapshot_update(session.as_mut(), snapshot)?;
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
    Ok(format!(
        "{}{}\n",
        PROTOCOL_PREFIX,
        serde_json::to_string(frame)?
    ))
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
    cmd_rx: mpsc::Receiver<WorkerCommand>,
    initial_snapshot: Option<Snapshot>,
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
                        Ok(WorkerCommand::Rpc { request, reply }) => {
                            let result = perform_rpc(session.as_mut(), request, RPC_TIMEOUT)
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
                        Err(mpsc::RecvTimeoutError::Disconnected) => return,
                        Err(mpsc::RecvTimeoutError::Timeout) => {}
                    }

                    match session.read_frame(READ_POLL_INTERVAL) {
                        Ok(Some(frame)) => {
                            if let Some(hello) = frame.into_hello() {
                                set_connected(&status, &port, &hello);
                            }
                        }
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
                    Ok(WorkerCommand::Rpc { reply, .. }) => {
                        let _ = reply.send(Err("no compatible device connected".to_string()));
                    }
                    Err(mpsc::RecvTimeoutError::Timeout) => {}
                    Err(mpsc::RecvTimeoutError::Disconnected) => return,
                }
            }
        }
    }
}

fn connect_for_worker(
    config: DeviceConfig,
    factory: Arc<dyn SessionFactory>,
) -> Result<(String, Box<dyn DeviceSession>, DeviceHello)> {
    let port = resolve_port(
        factory.clone(),
        config.preferred_port.as_deref(),
        config.baud,
    )?;
    let (session, hello) = connect_port(factory.as_ref(), &port, config.baud)?;
    Ok((port, session, hello))
}

fn resolve_port(
    factory: Arc<dyn SessionFactory>,
    preferred_port: Option<&str>,
    baud: u32,
) -> Result<String> {
    if let Some(port) = preferred_port {
        return Ok(port.to_string());
    }

    let devices = discover_devices(factory, baud)?;
    match devices.len() {
        0 => bail!("no compatible esp32dash device found on serial ports"),
        1 => Ok(devices[0].port.clone()),
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
    session.write_frame(&WireFrame::request(
        request_id.clone(),
        request.method,
        request.params,
    ))?;

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
    session.write_frame(&WireFrame::event(
        "claude.update",
        serde_json::to_value(snapshot)?,
    ))
}

fn set_connected(status: &Arc<Mutex<SerialConnectionStatus>>, port: &str, hello: &DeviceHello) {
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
    let mut guard = status.lock().expect("serial status mutex poisoned");
    guard.connected = false;
    guard.active_port = None;
    guard.protocol_version = None;
    guard.device_id = None;
    guard.product = None;
    guard.capabilities.clear();
}

fn set_last_error(status: &Arc<Mutex<SerialConnectionStatus>>, message: String) {
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

    names.sort_by_key(|name| if name.starts_with("tty.") { 0 } else { 1 });

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
    if let Some(stripped) = name
        .strip_prefix("tty.")
        .or_else(|| name.strip_prefix("cu."))
    {
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
            libc::open(
                c_path.as_ptr(),
                libc::O_RDWR | libc::O_NOCTTY | libc::O_NONBLOCK,
            )
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

    use super::*;
    use crate::model::Attention;

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
                title: "Ready".into(),
                workspace: "esp32".into(),
                detail: "done".into(),
                permission_mode: "default".into(),
                ts: 1,
                unread: true,
                attention: Attention::Medium,
            },
        )
        .unwrap();

        assert_eq!(session.writes.len(), 1);
        match &session.writes[0] {
            WireFrame::Event { method, payload } => {
                assert_eq!(method, "claude.update");
                assert_eq!(payload["seq"], 7);
            }
            other => panic!("unexpected frame: {other:?}"),
        }
    }
}
