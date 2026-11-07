// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
//! The XMODEM protocol state machine (sans-IO).
//!
//! # Model
//!
//! The caller owns an [`Engine`] that holds nothing but state. Bytes arrive
//! **passively** via [`Engine::on_receive`] (the driver calls it from its RX
//! event / FIFO drain), the caller drives the machine with [`Engine::poll`]
//! from its own task, and all output leaves through an [`Io`] implementation.
//! The engine never blocks, never reads a clock and never calls a transport
//! `recv()` -- time is injected as `now_ms`.
//!
//! Two consequences worth calling out:
//!
//! * Tests can advance time and feed arbitrary byte-stream slices deterministically.
//! * The same engine works behind a UART task, a USB CDC task, or a test bench.
//!
//! # One optional block 0 per send session
//!
//! XMODEM and YMODEM are not two protocols. A send session is classic XMODEM
//! plus one optional block 0 carrying file name/size: pass
//! [`SessionKind::File`] with a [`FileMeta`] to [`Engine::start_send`] to send
//! it, and a receiver simply recognises it when block 0 arrives. That is a
//! per-session *data* choice, not a mode baked into [`Config`] -- so one handle
//! alternates header-less and header-ful sessions freely, with no "protocol"
//! switches anywhere in the state machine.
//!
//! A batch needs no batch state machine:
//!
//! * **Sender**: several `start_send(SessionKind::File(..))` calls (one file
//!   each) followed by one `SessionKind::BatchEnd` (the standard empty
//!   block 0 terminator). Each call waits for the receiver's fresh 'C', which
//!   is exactly the YMODEM batch wire timing.
//! * **Receiver**: one session handles the whole batch. EOT is followed by a
//!   fresh 'C' asking for the next block 0; a non-empty header simply starts
//!   the next file, and an empty one ends the session.
//!
//! # State model
//!
//! [`Session`] is the single source of truth. It is a tagged union: the
//! receiver-only and sender-only fields live in [`RecvSession`] /
//! [`SendSession`], so code on one side physically cannot read the other
//! side's data. Terminal outcomes carry their [`Failure`] in the enum itself.
//! [`Status`] is a coarse public view **derived** from `Session`, never
//! stored -- there is no second state variable that could drift out of sync.

use core::num::NonZeroU32;

use crate::frame::{
    self, packet_len, verify_packet, Check, Frame, Framer, ACK, BLOCK_1024, BLOCK_128, CAN, CRC,
    EOT, NAK, SOH, STX, SUB,
};
use crate::io::{Event, Failure, Io, LocalReason, WireReason};

/// A file name that cannot be packed into a 128-byte YMODEM header block
/// (empty, contains NUL, or name + decimal size overflow the payload).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct InvalidName;

/// File metadata for [`SessionKind::File`]. Construct with [`FileMeta::new`];
/// an empty name is not a file, it is [`SessionKind::BatchEnd`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FileMeta<'a> {
    name: &'a [u8],
    size: u64,
}

impl<'a> FileMeta<'a> {
    /// Reject names that cannot appear on the wire. `name` is raw bytes
    /// (YMODEM names are not required to be UTF-8).
    pub fn new(name: &'a [u8], size: u64) -> Result<Self, InvalidName> {
        if name.is_empty() || name.contains(&0) {
            return Err(InvalidName);
        }
        let digits = u64_digit_count(size);
        if name.len() + digits + 2 > BLOCK_128 {
            return Err(InvalidName);
        }
        Ok(Self { name, size })
    }

    pub fn name(self) -> &'a [u8] {
        self.name
    }

    pub fn size(self) -> u64 {
        self.size
    }
}

/// What a send session puts on the wire before payload. Classic, a named
/// file header, and the empty block-0 batch terminator are different
/// things; they must not share `Option<FileMeta>` / an empty name.
#[derive(Clone, Copy, Debug)]
pub enum SessionKind<'a> {
    /// Classic XMODEM: no block 0, wait for NAK/'C', then payload.
    Classic,
    /// YMODEM file: block 0 carries [`FileMeta::name`] / [`FileMeta::size`].
    File(FileMeta<'a>),
    /// Empty block 0 that ends a YMODEM batch (no payload).
    BatchEnd,
}

/// Decimal digit count of `value` (0 → 1). Used to size a header name.
const fn u64_digit_count(mut value: u64) -> usize {
    if value == 0 {
        return 1;
    }
    let mut n = 0;
    while value > 0 {
        n += 1;
        value /= 10;
    }
    n
}

/// Bytes of `payload` that belong to the file when `remaining` bytes are still
/// expected. Take the minimum in the `u64` domain *before* narrowing: casting
/// `remaining` to `usize` first would truncate sizes >= 4 GiB on a 32-bit MCU
/// (all our targets) to 0 and silently swallow the whole file. `payload.len()`
/// itself is always a small block size, so the narrowing is lossless.
const fn deliverable_len(remaining: u64, payload_len: usize) -> usize {
    // `u64::min` is not const-stable yet (Ord as a const trait), so write the
    // clamp out. u64 -> usize is only safe once the value is known to fit a
    // block, which this comparison guarantees.
    let cap = payload_len as u64;
    (if remaining < cap { remaining } else { cap }) as usize
}

// Compile-time proof that a >4 GiB remaining size is not truncated. On a
// 64-bit host this is trivially true; on the 32-bit firmware targets it is the
// regression guard that `cargo test` on the host cannot provide.
const _: () = assert!(deliverable_len(0x1_0000_0000u64, 128) == 128);
const _: () = assert!(deliverable_len(128, 128) == 128);
const _: () = assert!(deliverable_len(10, 128) == 10);

/// Why a session failed to start. Name checking lives on [`FileMeta::new`],
/// so the only start-time failure is "already in flight".
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum StartError {
    /// A session is still in flight. Wait for it to finish instead of silently
    /// abandoning it (which used to happen on release builds).
    Busy,
}

/// How the receiver picks its check mode.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum CheckMode {
    /// Try CRC-16 first; if the peer stays silent, fall back to SUM8.
    Auto,
    /// Force 8-bit checksum.
    Sum8,
    /// Force CRC-16.
    Crc16,
}

impl CheckMode {
    fn initial(self) -> Check {
        match self {
            CheckMode::Sum8 => Check::Sum8,
            _ => Check::Crc16,
        }
    }
    fn allows_fallback(self) -> bool {
        matches!(self, Self::Auto)
    }
}

/// Tunables. Timeouts are in milliseconds and interpreted against the `now_ms`
/// supplied to `poll()` / `on_receive()`. Zero is unrepresentable: a timeout
/// of 0 ms is not a timeout.
#[derive(Clone, Copy, Debug)]
pub struct Config {
    pub check_mode: CheckMode,
    /// Sender: emit 1K (STX) blocks instead of 128-byte (SOH) blocks. A last
    /// block of at most 128 bytes is sent as SOH (XMODEM-1K mixed sizing).
    /// The receiver takes no part in this choice: XMODEM has no wire token for
    /// "send me 1K", so a receiver simply accepts SOH and STX packets alike
    /// (the start byte says which).
    pub block_1k: bool,
    /// Max retries before `Failure::Wire(WireReason::RetryExceeded)`.
    pub max_retries: u16,
    /// Receiver: line silence before re-requesting a packet / falling back.
    pub packet_timeout_ms: NonZeroU32,
    /// Sender: silence before `Failure::Wire(WireReason::NoResponse)`.
    pub response_timeout_ms: NonZeroU32,
}

impl Config {
    fn packet_ms(self) -> u32 {
        self.packet_timeout_ms.get()
    }

    fn response_ms(self) -> u32 {
        self.response_timeout_ms.get()
    }
}

impl Default for Config {
    fn default() -> Self {
        Self {
            check_mode: CheckMode::Auto,
            block_1k: false,
            max_retries: 10,
            packet_timeout_ms: NonZeroU32::new(3000).unwrap(),
            response_timeout_ms: NonZeroU32::new(30000).unwrap(),
        }
    }
}

/// Which side of the wire this engine is currently playing. `None` when idle
/// or terminal: after `Complete` there is no role, the application chooses
/// the next `start_*`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Role {
    None,
    Recv,
    Send,
}

/// CRC `'C'` handshake attempts (including the first emission) before
/// [`CheckMode::Auto`] falls back to SUM8. Matches common XMODEM-CRC
/// practice, and gives a YMODEM sender time to start before we switch to
/// NAK -- which a header-ful sender ignores.
const CRC_ATTEMPTS_BEFORE_FALLBACK: u16 = 3;

/// High-level status, safe to inspect from the caller's task at any time.
///
/// Derived on demand from [`Session`]; the engine never stores this value.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Status {
    /// Created; no session started yet.
    Idle,
    /// Handshake in progress (receiver is sending its request / sender waiting).
    Handshake,
    /// Transfer in progress.
    Transfer,
    /// Finished successfully.
    Complete,
    /// Finished with an error.
    Failed(Failure),
}

/// Receiver phases. Explicit on purpose: no hidden "blocking receive" loops.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum RecvPhase {
    /// Waiting for the first block. A data block means a classic (header-less)
    /// sender; block 0 means a file header.
    Handshake,
    /// Waiting for the next data packet (or EOT).
    Data,
    /// A header-ful file ended with EOT; standard senders follow it with an
    /// empty block 0. Silence is a graceful end (single-file tools often skip
    /// the terminator).
    AfterEot,
}

impl RecvPhase {
    const fn is_handshake(self) -> bool {
        matches!(self, Self::Handshake)
    }
}

/// Sender phases.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum SendPhase {
    /// Waiting for the receiver's NAK/'C' handshake byte.
    Handshake,
    /// Header-ful session, waiting for the receiver's 'C' to send block 0.
    HeaderHandshake,
    /// Block 0 sent, waiting for its ACK.
    HeaderWaitAck,
    /// Waiting for ACK/NAK of the block just sent.
    WaitAck,
    /// Sent EOT, waiting for its ACK.
    WaitEotAck,
}

impl SendPhase {
    const fn is_handshake(self) -> bool {
        matches!(
            self,
            Self::Handshake | Self::HeaderHandshake | Self::HeaderWaitAck
        )
    }
}

/// Receiver-only session state. Nothing here is reachable from a send path.
struct RecvSession {
    phase: RecvPhase,
    /// Next block number expected, wraps mod 256.
    blk: u8,
    /// Last accepted block number, for duplicate detection.
    last_blk: u8,
    /// Bytes of the current file still expected. `None` = unknown (classic
    /// semantics: deliver whole blocks, SUB padding included).
    file_remaining: Option<u64>,
    /// This file arrived with a block 0, so EOT is followed by the standard
    /// empty-header terminator instead of immediate completion.
    saw_header: bool,
}

impl RecvSession {
    const fn new() -> Self {
        Self {
            phase: RecvPhase::Handshake,
            blk: 1,
            last_blk: 0,
            file_remaining: None,
            saw_header: false,
        }
    }

    /// Start the next file of a YMODEM batch: reset per-file counters. This is
    /// why the receiver needs no batch state machine of its own.
    fn begin_file(&mut self, size: Option<u64>) {
        self.phase = RecvPhase::Data;
        self.blk = 1;
        self.last_blk = 0;
        self.file_remaining = size;
        self.saw_header = true;
    }
}

/// Sender-only session state. Nothing here is reachable from a receive path.
struct SendSession {
    phase: SendPhase,
    /// Next block number to send, wraps mod 256.
    blk: u8,
    /// True once a peer byte (NAK/'C'/ACK) has been seen. Distinguishes
    /// "peer never answered" (`NoResponse`) from "transfer broke midway".
    started: bool,
    /// This session sent a block 0.
    has_header: bool,
    /// The data source is exhausted -> send EOT next.
    source_exhausted: bool,
    /// NUL-free file name for block 0 (copied at start_send).
    file_name: [u8; 128],
    file_name_len: usize,
    /// Declared file size for block 0.
    file_size: u64,
    /// The exact bytes of the block awaiting ACK (for retransmit).
    pending: [u8; 3 + BLOCK_1024 + 2],
    pending_len: usize,
}

impl SendSession {
    fn new(has_header: bool) -> Self {
        Self {
            phase: SendPhase::Handshake,
            blk: 1,
            started: false,
            has_header,
            source_exhausted: false,
            file_name: [0; 128],
            file_name_len: 0,
            file_size: 0,
            pending: [0; 3 + BLOCK_1024 + 2],
            pending_len: 0,
        }
    }

    /// Build block 0 into `pending`: SOH 00 FF <name> 00 <decimal size> 00
    /// ... CRC16. An empty name yields the all-zero batch terminator.
    fn build_header_block(&mut self) {
        let mut digits = [0u8; 20];
        let nd = u64_ascii(self.file_size, &mut digits);
        self.pending[0] = SOH;
        self.pending[1] = 0;
        self.pending[2] = 0xFF;
        self.pending[3..3 + self.file_name_len]
            .copy_from_slice(&self.file_name[..self.file_name_len]);
        let mut p = 3 + self.file_name_len;
        self.pending[p] = 0; // name terminator
        p += 1;
        if self.file_name_len > 0 {
            self.pending[p..p + nd].copy_from_slice(&digits[20 - nd..]);
            p += nd;
            self.pending[p] = 0; // size terminator
            p += 1;
        }
        // Everything after the name/size NULs stays zero (the batch terminator
        // is exactly this with an empty name).
        for b in self.pending[p..3 + BLOCK_128].iter_mut() {
            *b = 0;
        }
        let crc = frame::crc16(&self.pending[3..3 + BLOCK_128]);
        self.pending[3 + BLOCK_128] = (crc >> 8) as u8;
        self.pending[4 + BLOCK_128] = crc as u8;
        self.pending_len = 3 + BLOCK_128 + 2;
    }
}

/// The whole session state, and the engine's single source of truth.
///
/// Exactly one arm is live. Role-specific fields are unreachable from the other
/// role's code paths, and a terminal outcome carries its own [`Failure`].
///
/// The sender arm is much larger than the receiver arm because it owns the
/// 1K packet scratch buffer. Boxing it is not an option: this is a `no_std`
/// crate with no allocator, and the FFI already reserves a fixed opaque
/// buffer sized for the largest variant. Stack/struct size is a deliberate
/// trade for zero allocation and deterministic memory use on an MCU.
#[allow(clippy::large_enum_variant)]
enum Session {
    Idle,
    Recv(RecvSession),
    Send(SendSession),
    Complete,
    Failed(Failure),
}

impl Session {
    fn is_active(&self) -> bool {
        matches!(self, Self::Recv(_) | Self::Send(_))
    }

    /// A new session may only start while idle or after one finished.
    fn can_start(&self) -> bool {
        matches!(self, Self::Idle | Self::Complete | Self::Failed(_))
    }

    /// Derive the coarse public view; nothing is stored, so it cannot drift.
    fn status(&self) -> Status {
        match self {
            Self::Idle => Status::Idle,
            Self::Recv(r) => {
                if r.phase.is_handshake() {
                    Status::Handshake
                } else {
                    Status::Transfer
                }
            }
            Self::Send(s) => {
                if s.phase.is_handshake() {
                    Status::Handshake
                } else {
                    Status::Transfer
                }
            }
            Self::Complete => Status::Complete,
            Self::Failed(f) => Status::Failed(*f),
        }
    }
}

/// State shared by both roles: negotiated check mode, retry budget, timer and
/// the framer-command mailbox (see `drive_bytes`).
struct Common {
    check: Check,
    retries: u16,
    deadline_ms: u32,
    timer_armed: bool,
    /// Last session role. Survives `Complete` so a late NAK can re-emit EOT
    /// (sender) and a late empty block 0 can be ACKed (receiver).
    was_send: bool,
    /// Framer changes requested while `framer` is temporarily moved out during
    /// `drive_bytes`; applied once the real framer is put back.
    pending_check: Option<Check>,
    pending_reset: bool,
}

impl Common {
    fn new(check: Check) -> Self {
        Self {
            check,
            retries: 0,
            deadline_ms: 0,
            timer_armed: false,
            was_send: false,
            pending_check: None,
            pending_reset: false,
        }
    }

    fn reset(&mut self, check: Check) {
        *self = Self::new(check);
    }

    fn arm(&mut self, timeout: u32, now_ms: u32) {
        self.deadline_ms = now_ms.wrapping_add(timeout);
        self.timer_armed = true;
    }

    fn disarm(&mut self) {
        self.timer_armed = false;
    }

    /// Wrap-safe "now >= deadline": stays correct across the u32 millisecond
    /// rollover at ~49.7 days of uptime (timeouts must stay below 2^31 ms).
    fn deadline_reached(&self, now_ms: u32) -> bool {
        self.timer_armed && (now_ms.wrapping_sub(self.deadline_ms) as i32) >= 0
    }
}

/// The protocol engine. Generic over `Io` so tests can supply an in-memory
/// implementation; the FFI layer wraps it in a fixed-size, opaque buffer.
pub struct Engine<T: Io> {
    io: T,
    cfg: Config,
    /// Temporarily `take()`n out while streaming bytes through `push`, so
    /// the frame callback can drive `&mut self` without an aliasing conflict.
    framer: Option<Framer>,
    common: Common,
    session: Session,
}

impl<T: Io> Engine<T> {
    /// Create an idle engine. Nothing runs yet: no byte is emitted, no timer
    /// is armed, received bytes are ignored. Call `start_recv` or `start_send`
    /// to open a session.
    pub fn new(io: T, cfg: Config) -> Self {
        let check = cfg.check_mode.initial();
        Self {
            io,
            cfg,
            framer: Some(Framer::new(check)),
            common: Common::new(check),
            session: Session::Idle,
        }
    }

    /// Whether a new session may be started now (idle or terminal).
    /// `start_recv`/`start_send` check this themselves and return
    /// [`StartError::Busy`], so this is only needed by embedders that want to
    /// probe before committing.
    pub fn can_start(&self) -> bool {
        self.session.can_start()
    }

    /// Start a receive session. Emits the handshake request right away.
    ///
    /// Everything protocol-related is reset: session state, block counters,
    /// retries, the negotiated check mode and the partially assembled frame.
    /// Nothing in `Io` is touched (no callback, no application buffer).
    /// `now_ms` is the caller's monotonic clock; timeouts are armed from this
    /// instant.
    pub fn start_recv(&mut self, now_ms: u32) -> Result<(), StartError> {
        if !self.session.can_start() {
            return Err(StartError::Busy);
        }
        let check = self.cfg.check_mode.initial();
        self.begin_session(check, Session::Recv(RecvSession::new()));
        self.emit_handshake(now_ms);
        self.flush_framer();
        Ok(())
    }

    /// Start a send session.
    ///
    /// [`SessionKind::Classic`] waits for the peer's NAK/'C', then pulls
    /// payload. [`SessionKind::File`] forces CRC-16 and sends block 0 first.
    /// [`SessionKind::BatchEnd`] sends the empty block 0 that terminates a
    /// YMODEM batch (no payload). A name that cannot fit a header is rejected
    /// by [`FileMeta::new`], not here.
    ///
    /// Returns `Err(Busy)` if a session is still in flight.
    pub fn start_send(&mut self, kind: SessionKind<'_>, now_ms: u32) -> Result<(), StartError> {
        if !self.session.can_start() {
            return Err(StartError::Busy);
        }
        let (has_header, check) = match kind {
            SessionKind::Classic => (false, self.cfg.check_mode.initial()),
            SessionKind::File(_) | SessionKind::BatchEnd => (true, Check::Crc16),
        };
        let mut send = SendSession::new(has_header);
        send.phase = match kind {
            SessionKind::Classic => SendPhase::Handshake,
            SessionKind::File(f) => {
                send.file_name[..f.name().len()].copy_from_slice(f.name());
                send.file_name_len = f.name().len();
                send.file_size = f.size();
                send.build_header_block();
                SendPhase::HeaderHandshake
            }
            SessionKind::BatchEnd => {
                send.build_header_block();
                SendPhase::HeaderHandshake
            }
        };
        self.begin_session(check, Session::Send(send));
        self.arm(self.cfg.response_ms(), now_ms);
        self.flush_framer();
        Ok(())
    }

    /// Reset everything protocol-related. The caller picks timers and the
    /// first emission afterwards.
    fn begin_session(&mut self, check: Check, session: Session) {
        let was_send = matches!(session, Session::Send(_));
        self.framer = Some(Framer::new(check));
        self.common.reset(check);
        self.common.was_send = was_send;
        self.session = session;
    }

    /// Read back a reference to the I/O implementation (e.g. to drain a test's
    /// recorded output, or to feed a source).
    pub fn io(&mut self) -> &mut T {
        &mut self.io
    }

    pub fn status(&self) -> Status {
        self.session.status()
    }

    /// Current role. `None` when idle or the session has finished.
    pub fn role(&self) -> Role {
        match self.session {
            Session::Recv(_) => Role::Recv,
            Session::Send(_) => Role::Send,
            Session::Idle | Session::Complete | Session::Failed(_) => Role::None,
        }
    }

    /// Feed bytes received from the hardware. Any slice is valid: partial
    /// packets, several packets, or control bytes mixed together. `now_ms` is
    /// the caller's monotonic millisecond clock.
    pub fn on_receive(&mut self, data: &[u8], now_ms: u32) {
        // `Complete` still accepts a late EOT so a peer whose ACK was lost
        // can finish; `Failed` / `Idle` ignore the wire.
        if matches!(self.session, Session::Idle | Session::Failed(_)) {
            return;
        }
        self.drive_bytes(data, now_ms);
    }

    /// Drive the state machine: handle timeouts and (re)transmit. Call
    /// regularly from a task, e.g. every 1--10 ms, with a monotonic `now_ms`.
    pub fn poll(&mut self, now_ms: u32) {
        if !self.session.is_active() {
            return;
        }
        if self.common.deadline_reached(now_ms) {
            self.on_timeout(now_ms);
        }
    }

    /// Abort the transfer from the application side, emitting CAN bytes.
    ///
    /// Uses `Failure::Local(LocalReason::Cancelled)`, not a peer cancel: a
    /// local cancel has to inform the peer with CAN, whereas echoing a peer's
    /// CAN back would start a CAN storm.
    pub fn cancel(&mut self) {
        if !self.session.is_active() {
            return;
        }
        self.abort(Failure::Local(LocalReason::Cancelled));
    }

    /// Report that an [`Io`] callback failed (emit/sink/source returned an
    /// error). Aborts with `Failure::Local(LocalReason::Io)`, which emits
    /// CAN and is reported as the same error code through both `status()` and
    /// `on_event()`.
    ///
    /// Safe to call at any time. A late failure -- e.g. the final ACK could
    /// not be sent -- must not be reported as a successful transfer, so even
    /// an already-`Complete` session is turned into a failure here.
    pub fn fail_io(&mut self) {
        if matches!(&self.session, Session::Idle | Session::Failed(_)) {
            return;
        }
        self.abort(Failure::Local(LocalReason::Io));
    }

    // ------------------------------------------------------------------
    // Streaming path. `framer` is moved out of `self` for the duration of the
    // push, so the callback can call `&mut self` methods directly. This avoids
    // a fixed-size frame queue (which could silently drop frames on a large
    // FIFO read) and needs no unsafe aliasing tricks.
    // ------------------------------------------------------------------
    fn drive_bytes(&mut self, data: &[u8], now_ms: u32) {
        let mut framer = self.framer.take().unwrap();
        let me = &mut *self;

        framer.push(data, |frame| {
            if matches!(me.session, Session::Failed(_)) {
                return;
            }
            match frame {
                // `packet` borrows the local `framer`, not `me`, so it can be
                // handed over directly; `handle_packet` finishes with it
                // before `push` reuses the buffer for the next byte.
                Frame::Packet(packet) => me.handle_packet(packet, now_ms),
                Frame::Control(b) => me.handle_control(b, now_ms),
            }
        });
        self.framer = Some(framer);
        self.flush_framer();
    }

    /// Request a framer check-mode change. Safe to call while the framer is
    /// temporarily moved out (during `drive_bytes`); the change is applied by
    /// `flush_framer()` as soon as it is back in place.
    fn request_check(&mut self, check: Check) {
        self.common.pending_check = Some(check);
    }

    fn request_reset(&mut self) {
        self.common.pending_reset = true;
    }

    fn flush_framer(&mut self) {
        if let Some(framer) = self.framer.as_mut() {
            if self.common.pending_reset {
                framer.reset_packet();
                self.common.pending_reset = false;
            }
            if let Some(check) = self.common.pending_check.take() {
                framer.set_check(check);
            }
        }
    }

    fn arm(&mut self, timeout: u32, now_ms: u32) {
        self.common.arm(timeout, now_ms)
    }

    fn disarm(&mut self) {
        self.common.disarm()
    }

    /// Push bytes onto the wire. On I/O failure the session is aborted with
    /// `LocalReason::Io` and this returns `false`; callers must stop.
    fn emit(&mut self, bytes: &[u8]) -> bool {
        if matches!(self.session, Session::Failed(_)) {
            return false;
        }
        if self.io.emit(bytes).is_err() {
            self.abort(Failure::Local(LocalReason::Io));
            return false;
        }
        true
    }

    fn emit_handshake(&mut self, now_ms: u32) -> bool {
        if !self.emit(&[self.common.check.request_byte()]) {
            return false;
        }
        self.arm(self.cfg.packet_ms(), now_ms);
        true
    }

    // ---- packet / control handling --------------------------------------

    fn handle_packet(&mut self, packet: &[u8], now_ms: u32) {
        if matches!(self.session, Session::Complete) {
            self.handle_late_packet(packet);
            return;
        }
        if !matches!(&self.session, Session::Recv(_)) {
            // A data packet arriving at a sender is line noise; the
            // timeout/NAK loop is the recovery path.
            return;
        }
        if !verify_packet(packet, self.common.check) {
            self.handle_bad_packet(now_ms);
            return;
        }
        self.accept_packet(packet, now_ms);
    }

    /// After we have completed, keep answering the one frame the peer may
    /// still be retrying: an empty block 0 (YMODEM batch terminator) whose
    /// ACK was lost. Anything else is dropped.
    fn handle_late_packet(&mut self, packet: &[u8]) {
        if self.common.was_send {
            return;
        }
        if !verify_packet(packet, self.common.check) || packet[1] != 0 {
            return;
        }
        let payload_end = 3 + frame::block_len(packet[0]);
        let payload = &packet[3..payload_end];
        if payload.first() == Some(&0) {
            let _ = self.emit(&[ACK]);
        }
    }

    fn accept_packet(&mut self, packet: &[u8], now_ms: u32) {
        let start = packet[0];
        let blk = packet[1];
        let payload_end = 3 + frame::block_len(start);
        let payload = &packet[3..payload_end];

        enum Action {
            Header,
            FirstData,
            Data,
            Duplicate,
            Nak,
        }
        let action = {
            let Session::Recv(r) = &mut self.session else {
                return;
            };
            match r.phase {
                // Both states expect block 0: the first header opens a file, a
                // later one is the next file of a batch, and an empty name ends
                // it. handle_header covers all three with no batch state.
                RecvPhase::Handshake | RecvPhase::AfterEot if blk == 0 => Action::Header,
                RecvPhase::Handshake => {
                    // A data block before any header: a classic (header-less)
                    // sender. Deliver whole blocks; EOT ends the session.
                    r.phase = RecvPhase::Data;
                    Action::FirstData
                }
                RecvPhase::Data => Action::Data,
                // Last-block ACK was lost and the sender is still retrying it
                // after we already took EOT: ACK the duplicate, do not NAK-
                // starve them into RetryExceeded.
                RecvPhase::AfterEot if blk == r.last_blk => Action::Duplicate,
                RecvPhase::AfterEot => Action::Nak,
            }
        };
        match action {
            Action::Header => self.handle_header(payload, now_ms),
            Action::FirstData => {
                self.io.on_event(Event::Handshake);
                self.accept_data_packet(payload, blk, now_ms);
            }
            Action::Data => self.accept_data_packet(payload, blk, now_ms),
            Action::Duplicate => {
                if self.emit(&[ACK]) {
                    self.arm(self.cfg.packet_ms(), now_ms);
                }
            }
            Action::Nak => {
                if self.emit(&[NAK]) {
                    self.arm(self.cfg.packet_ms(), now_ms);
                }
            }
        }
    }

    /// Deliver one accepted data block to `Io::data_sink`, honouring the
    /// optional file size: bytes beyond the declared size are ACKed but
    /// dropped, and the last block is trimmed to the exact file length.
    fn accept_data_packet(&mut self, payload: &[u8], blk: u8, now_ms: u32) {
        enum Action {
            Accept(usize),
            Duplicate,
            Desync,
        }
        let action = {
            let Self {
                common, session, ..
            } = self;
            let Session::Recv(r) = session else {
                return;
            };
            if blk == r.blk {
                let n = match r.file_remaining {
                    Some(remaining) => deliverable_len(remaining, payload.len()),
                    None => payload.len(),
                };
                if let Some(rem) = r.file_remaining.as_mut() {
                    *rem -= n as u64;
                }
                r.last_blk = blk;
                r.blk = r.blk.wrapping_add(1);
                common.retries = 0;
                Action::Accept(n)
            } else if blk == r.last_blk {
                // Duplicate of the last block (our ACK was lost): ACK again,
                // do not deliver the payload a second time.
                Action::Duplicate
            } else {
                // Neither expected nor previous: unrecoverable desync.
                Action::Desync
            }
        };
        match action {
            Action::Accept(n) => {
                if n > 0 && self.io.data_sink(&payload[..n]).is_err() {
                    self.abort(Failure::Local(LocalReason::Io));
                    return;
                }
                if !self.emit(&[ACK]) {
                    return;
                }
                self.io.on_event(Event::Block);
                self.arm(self.cfg.packet_ms(), now_ms);
            }
            Action::Duplicate => {
                if !self.emit(&[ACK]) {
                    return;
                }
                self.arm(self.cfg.packet_ms(), now_ms);
            }
            Action::Desync => self.abort(Failure::Wire(WireReason::OutOfSync)),
        }
    }

    /// Block 0 arrived while waiting for a (first or next) file. An empty
    /// name ends the batch; otherwise the application accepts/rejects the
    /// file before any payload is requested. A receiver therefore handles a
    /// whole multi-file batch in one session without any batch state of its
    /// own -- each file just resets the block counter and file size.
    fn handle_header(&mut self, payload: &[u8], now_ms: u32) {
        // A valid block-0 payload is exactly BLOCK_128 bytes, so the name
        // cannot exceed it and needs no extra cap.
        let name_end = payload
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(payload.len());
        if name_end == 0 {
            if self.emit(&[ACK]) {
                self.finish_complete();
            }
            return;
        }
        let size = parse_header_size(payload, name_end);
        if !self.io.on_file(&payload[..name_end], size) {
            self.abort(Failure::Local(LocalReason::FileRejected));
            return;
        }
        if !self.emit(&[ACK]) {
            return;
        }
        // Ask for block 1 with a fresh 'C'.
        if !self.emit_handshake(now_ms) {
            return;
        }
        let Self {
            common, session, ..
        } = self;
        let Session::Recv(r) = session else {
            return;
        };
        r.begin_file(size);
        common.retries = 0;
    }

    fn handle_control(&mut self, b: u8, now_ms: u32) {
        if matches!(self.session, Session::Complete) {
            // A peer whose ACK of EOT was lost will resend EOT; ACK it again
            // so they can finish. A sender whose EOT itself was lost (we
            // already completed on retries) must still answer a late NAK/'C'
            // with another EOT -- otherwise both sides burn the retry budget.
            if b == EOT {
                let _ = self.emit(&[ACK]);
            } else if matches!(b, NAK | CRC) && self.common.was_send {
                let _ = self.emit(&[EOT]);
            }
            return;
        }
        if let Session::Send(s) = &mut self.session {
            // A peer byte arrived: this is no longer "the other side never
            // answered". Handshake silence after this fails as RetryExceeded
            // rather than hanging on WaitAgain forever.
            if matches!(b, NAK | CRC | ACK) {
                s.started = true;
            }
        }
        match b {
            CAN => self.abort(Failure::Wire(WireReason::CancelByPeer)),
            EOT => self.handle_eot(now_ms),
            NAK | CRC => self.handle_peer_request(b, now_ms),
            ACK => self.handle_ack(now_ms),
            _ => {}
        }
    }

    fn handle_eot(&mut self, now_ms: u32) {
        enum Action {
            EmptyOrDone,
            BatchNext,
            AckEot,
            Ignore,
        }
        let action = {
            let Session::Recv(r) = &self.session else {
                return;
            };
            match r.phase {
                // Empty transfer (no header, no data), or a classic header-less
                // transfer that ended: ACK and complete.
                RecvPhase::Handshake | RecvPhase::Data if !r.saw_header => Action::EmptyOrDone,
                RecvPhase::Data => Action::BatchNext,
                // Sender's EOT ACK was lost; ACK again. Do not re-send 'C'
                // here: a WaitEotAck sender would treat 'C' as NAK-of-EOT.
                RecvPhase::AfterEot => Action::AckEot,
                _ => Action::Ignore,
            }
        };
        match action {
            Action::EmptyOrDone => {
                if self.emit(&[ACK]) {
                    self.finish_complete();
                }
            }
            Action::AckEot => {
                let _ = self.emit(&[ACK]);
            }
            Action::BatchNext => {
                // A YMODEM batch carries on with the next block 0: ACK the EOT
                // and request it with a fresh 'C', exactly like the initial
                // handshake. A single-file sender that omits the empty
                // terminator stays silent, and AfterEot then times out into a
                // graceful complete.
                self.io.on_event(Event::FileComplete);
                let req = self.common.check.request_byte();
                if !self.emit(&[ACK, req]) {
                    return;
                }
                {
                    let Self {
                        common, session, ..
                    } = self;
                    if let Session::Recv(r) = session {
                        r.phase = RecvPhase::AfterEot;
                    }
                    common.retries = 0;
                }
                self.arm(self.cfg.response_ms(), now_ms);
            }
            Action::Ignore => {}
        }
    }

    fn handle_peer_request(&mut self, b: u8, now_ms: u32) {
        enum Action {
            Ignore,
            /// Classic handshake: negotiate the check mode and send block 1.
            StartBlock(Check),
            /// Header already fixed CRC-16; a NAK here can only mean "send
            /// block 1" (the receiver's 'C' was lost), never a mode change.
            StartBlockCrc16,
            /// Send block 0 and wait for its ACK.
            StartHeader,
            /// Re-send block 0 at a phase boundary.
            ResendHeader,
            RetryBlock,
            RetryEot,
        }
        let action = {
            let Session::Send(s) = &self.session else {
                // Receiver-side: a stray NAK/'C' is not for us. Our own
                // requests are emitted by the timeout path.
                return;
            };
            match s.phase {
                SendPhase::Handshake if s.has_header && b == NAK => Action::StartBlockCrc16,
                SendPhase::Handshake if s.has_header && b != CRC => Action::Ignore,
                SendPhase::Handshake => {
                    Action::StartBlock(if b == CRC { Check::Crc16 } else { Check::Sum8 })
                }
                // Only 'C' requests block 0 (CRC-16 only).
                SendPhase::HeaderHandshake if b == CRC => Action::StartHeader,
                // NAK or 'C': the header (or its ACK) was lost. Some CRC
                // receivers re-request with 'C' rather than NAK.
                SendPhase::HeaderWaitAck => Action::ResendHeader,
                SendPhase::WaitAck => Action::RetryBlock,
                SendPhase::WaitEotAck => Action::RetryEot,
                _ => Action::Ignore,
            }
        };
        match action {
            Action::Ignore => {}
            Action::StartBlock(check) => {
                self.common.check = check;
                self.request_check(check);
                self.begin_transfer(now_ms);
            }
            Action::StartBlockCrc16 => self.begin_transfer(now_ms),
            Action::StartHeader => {
                self.resend_pending(now_ms);
                let Self { session, .. } = self;
                if let Session::Send(s) = session {
                    s.phase = SendPhase::HeaderWaitAck;
                }
            }
            Action::ResendHeader => {
                // The receiver asking again means our header (or its ACK) was
                // lost at a phase boundary -- answer with the same header.
                self.resend_pending(now_ms);
            }
            Action::RetryBlock => {
                // Peer NAKs the block we just sent: retransmit it.
                self.retry_or_abort(now_ms, |e, n| e.resend_pending(n));
            }
            Action::RetryEot => {
                // Receivers commonly NAK the first EOT to make sure no data
                // block is still in flight; resend the EOT.
                self.retry_or_abort(now_ms, |e, n| {
                    if e.emit(&[EOT]) {
                        e.arm(e.cfg.response_ms(), n);
                    }
                });
            }
        }
    }

    /// Common "handshake done, start streaming" transition for a sender.
    fn begin_transfer(&mut self, now_ms: u32) {
        {
            let Session::Send(s) = &mut self.session else {
                return;
            };
            s.phase = SendPhase::WaitAck;
        }
        self.io.on_event(Event::Handshake);
        self.send_next_block_or_eot(now_ms);
    }

    fn handle_ack(&mut self, now_ms: u32) {
        enum Action {
            NextBlock,
            EmptyHeaderDone,
            WaitSecondC,
            Complete,
            Ignore,
        }
        let action = {
            let Session::Send(s) = &self.session else {
                return;
            };
            match s.phase {
                SendPhase::WaitAck => Action::NextBlock,
                SendPhase::HeaderWaitAck if s.file_name_len == 0 => Action::EmptyHeaderDone,
                SendPhase::HeaderWaitAck => Action::WaitSecondC,
                SendPhase::WaitEotAck => Action::Complete,
                _ => Action::Ignore,
            }
        };
        match action {
            Action::NextBlock => {
                self.common.retries = 0;
                {
                    let Session::Send(s) = &mut self.session else {
                        return;
                    };
                    // The peer accepted this block; the next one gets the next
                    // sequence number. A NAK goes through `resend_pending`
                    // and deliberately does NOT bump it.
                    s.blk = s.blk.wrapping_add(1);
                }
                self.io.on_event(Event::Block);
                self.send_next_block_or_eot(now_ms);
            }
            Action::EmptyHeaderDone | Action::Complete => self.finish_complete(),
            Action::WaitSecondC => {
                // Header accepted. The receiver now asks for block 1 with a
                // fresh 'C'; wait for it like a classic handshake.
                self.common.retries = 0;
                {
                    let Session::Send(s) = &mut self.session else {
                        return;
                    };
                    s.phase = SendPhase::Handshake;
                }
                self.arm(self.cfg.response_ms(), now_ms);
            }
            Action::Ignore => {}
        }
    }

    fn handle_bad_packet(&mut self, now_ms: u32) {
        // Corrupt / incomplete / checksum-mismatched packet. Purge the framer
        // and ask for a retransmission with NAK.
        self.request_reset();
        let ask_nak = matches!(
            &self.session,
            Session::Recv(r) if matches!(r.phase, RecvPhase::Handshake | RecvPhase::Data)
        );
        if ask_nak {
            self.retry_or_abort(now_ms, |e, _| {
                let _ = e.emit(&[NAK]);
            });
        }
    }

    fn on_timeout(&mut self, now_ms: u32) {
        self.disarm();
        enum Action {
            FallbackSum8,
            RetryCrcHandshake,
            RetryHandshake,
            EndRequest,
            Complete,
            RetryNak,
            WaitAgain,
            AbortNoResponse,
            ResendBlock,
            ResendEot,
            Ignore,
        }
        let action = match &self.session {
            Session::Recv(r) => match r.phase {
                RecvPhase::Handshake => {
                    if self.cfg.check_mode.allows_fallback() && self.common.check == Check::Crc16 {
                        // `retries` counts CRC attempts after the initial 'C'.
                        // After CRC_ATTEMPTS_BEFORE_FALLBACK emissions, fall
                        // back to SUM8 without consuming the transfer budget.
                        if self.common.retries + 1 < CRC_ATTEMPTS_BEFORE_FALLBACK {
                            Action::RetryCrcHandshake
                        } else {
                            Action::FallbackSum8
                        }
                    } else {
                        Action::RetryHandshake
                    }
                }
                RecvPhase::AfterEot => {
                    // EOT was already ACKed. Keep asking for the next block 0
                    // for the full retry budget: a short magic count completed
                    // while a YMODEM peer was still opening the next file.
                    // Silence after that is a graceful single-file end.
                    if self.common.retries >= self.cfg.max_retries {
                        Action::Complete
                    } else {
                        Action::EndRequest
                    }
                }
                RecvPhase::Data => Action::RetryNak,
            },
            Session::Send(s) => match s.phase {
                SendPhase::HeaderHandshake | SendPhase::Handshake => {
                    if s.started {
                        // Just re-arm: the receiver re-requests on its own
                        // timeout; sending anything unsolicited would race it.
                        Action::WaitAgain
                    } else {
                        Action::AbortNoResponse
                    }
                }
                // The header/block (or its ACK) may have been lost -- notably
                // the ACK of the final block. Retransmit; the receiver ACKs
                // duplicates without delivering the payload twice, and does
                // not invoke on_file again.
                SendPhase::HeaderWaitAck | SendPhase::WaitAck => Action::ResendBlock,
                SendPhase::WaitEotAck => Action::ResendEot,
            },
            _ => Action::Ignore,
        };
        match action {
            Action::FallbackSum8 => {
                self.common.check = Check::Sum8;
                self.request_check(Check::Sum8);
                self.flush_framer();
                self.common.retries = 0;
                let _ = self.emit_handshake(now_ms);
            }
            Action::RetryCrcHandshake => {
                self.common.retries += 1;
                let _ = self.emit_handshake(now_ms);
            }
            Action::RetryHandshake => self.retry_or_abort(now_ms, |e, n| {
                let _ = e.emit_handshake(n);
            }),
            Action::EndRequest => {
                self.common.retries += 1;
                if self.emit(&[self.common.check.request_byte()]) {
                    self.arm(self.cfg.response_ms(), now_ms);
                }
            }
            Action::Complete => self.finish_complete(),
            Action::RetryNak => self.retry_or_abort(now_ms, |e, n| {
                if e.emit(&[NAK]) {
                    e.arm(e.cfg.packet_ms(), n);
                }
            }),
            Action::WaitAgain => self.retry_or_abort(now_ms, |e, n| e.arm(e.cfg.response_ms(), n)),
            Action::AbortNoResponse => self.abort(Failure::Wire(WireReason::NoResponse)),
            Action::ResendBlock => self.retry_or_abort(now_ms, |e, n| e.resend_pending(n)),
            Action::ResendEot => {
                if self.common.retries >= self.cfg.max_retries {
                    // All data blocks were ACKed; the receiver has likely
                    // already completed and is ignoring further EOT.
                    self.finish_complete();
                } else {
                    self.retry_or_abort(now_ms, |e, n| {
                        if e.emit(&[EOT]) {
                            e.arm(e.cfg.response_ms(), n);
                        }
                    });
                }
            }
            Action::Ignore => {}
        }
    }

    fn retry_or_abort<F>(&mut self, now_ms: u32, action: F)
    where
        F: FnOnce(&mut Self, u32),
    {
        if self.common.retries >= self.cfg.max_retries {
            self.abort(Failure::Wire(WireReason::RetryExceeded));
            return;
        }
        self.common.retries += 1;
        action(self, now_ms);
    }

    // ---- sender block production ----------------------------------------

    fn send_next_block_or_eot(&mut self, now_ms: u32) {
        enum Produced {
            Eot,
            IoFail,
            Block(usize, usize),
        }
        let produced = {
            let Self {
                io, cfg, session, ..
            } = self;
            let Session::Send(s) = session else {
                return;
            };
            if s.source_exhausted {
                Produced::Eot
            } else {
                let cap = if cfg.block_1k { BLOCK_1024 } else { BLOCK_128 };
                // Pull straight into the packet's payload slot: no intermediate
                // buffer, so a 1K send needs no transient 1K stack arrays.
                match io.data_source(&mut s.pending[3..3 + cap]) {
                    Err(_) => Produced::IoFail,
                    Ok(0) => {
                        s.source_exhausted = true;
                        Produced::Eot
                    }
                    Ok(n) if n > cap => Produced::IoFail,
                    Ok(n) => {
                        // XMODEM-1K: a short last block (<= 128) goes out as
                        // SOH, matching sx/lrzsz and the 0.1.x driver.
                        let block_len = if cfg.block_1k && n > BLOCK_128 {
                            BLOCK_1024
                        } else {
                            BLOCK_128
                        };
                        Produced::Block(n, block_len)
                    }
                }
            }
        };
        match produced {
            Produced::IoFail => {
                self.abort(Failure::Local(LocalReason::Io));
            }
            Produced::Eot => {
                let response_timeout = self.cfg.response_ms();
                let emit_err = {
                    let Self {
                        io,
                        common,
                        session,
                        ..
                    } = self;
                    let Session::Send(s) = session else {
                        return;
                    };
                    s.phase = SendPhase::WaitEotAck;
                    common.arm(response_timeout, now_ms);
                    io.emit(&[EOT]).is_err()
                };
                if emit_err {
                    self.abort(Failure::Local(LocalReason::Io));
                }
            }
            Produced::Block(n, block_len) => {
                let response_timeout = self.cfg.response_ms();
                let emit_err = {
                    let Self {
                        io,
                        common,
                        session,
                        ..
                    } = self;
                    let Session::Send(s) = session else {
                        return;
                    };
                    let start = if block_len == BLOCK_1024 { STX } else { SOH };
                    for b in s.pending[3 + n..3 + block_len].iter_mut() {
                        *b = SUB
                    }
                    let total = packet_len(start, common.check);
                    s.pending[0] = start;
                    s.pending[1] = s.blk;
                    s.pending[2] = !s.blk;
                    match common.check {
                        Check::Sum8 => {
                            let sum = frame::sum8(&s.pending[3..3 + block_len]);
                            s.pending[3 + block_len] = sum;
                        }
                        Check::Crc16 => {
                            let crc = frame::crc16(&s.pending[3..3 + block_len]);
                            s.pending[3 + block_len] = (crc >> 8) as u8;
                            s.pending[4 + block_len] = crc as u8;
                        }
                    }
                    s.pending_len = total;
                    s.phase = SendPhase::WaitAck;
                    common.arm(response_timeout, now_ms);
                    io.emit(&s.pending[..total]).is_err()
                };
                if emit_err {
                    self.abort(Failure::Local(LocalReason::Io));
                }
            }
        }
    }

    fn resend_pending(&mut self, now_ms: u32) {
        let response_timeout = self.cfg.response_ms();
        let emit_err = {
            let Self {
                io,
                common,
                session,
                ..
            } = self;
            let Session::Send(s) = session else {
                return;
            };
            if s.pending_len == 0 {
                return;
            }
            let err = io.emit(&s.pending[..s.pending_len]).is_err();
            if !err {
                common.arm(response_timeout, now_ms);
            }
            err
        };
        if emit_err {
            self.abort(Failure::Local(LocalReason::Io));
        }
    }

    // ---- terminal transitions -------------------------------------------

    fn finish_complete(&mut self) {
        self.session = Session::Complete;
        self.disarm();
        self.io.on_event(Event::Complete);
    }

    fn abort(&mut self, failure: Failure) {
        if matches!(self.session, Session::Failed(_)) {
            return;
        }
        // Only a peer-initiated cancel suppresses CAN: the peer already sent
        // them and echoing back would start a CAN storm. Every other failure,
        // including a local cancel, must inform the peer. Use the raw `Io`
        // emit so an I/O failure here cannot recurse through `self.emit`.
        if !failure.peer_already_cancelled() {
            let _ = self.io.emit(&[CAN, CAN]);
        }
        self.session = Session::Failed(failure);
        self.disarm();
        self.io.on_event(Event::Failed(failure));
    }
}

/// Write `value` as decimal ASCII into `buf` (no_std: no alloc).
/// Returns the digit count `nd`; the digits occupy `buf[buf.len() - nd..]`.
fn u64_ascii(value: u64, buf: &mut [u8; 20]) -> usize {
    if value == 0 {
        buf[19] = b'0';
        return 1;
    }
    let mut v = value;
    let mut i = buf.len();
    while v > 0 {
        i -= 1;
        buf[i] = b'0' + (v % 10) as u8;
        v /= 10;
    }
    buf.len() - i
}

/// Parse the optional decimal size field of a header block: the digits
/// after the name's NUL, up to the next NUL **or space** -- the YMODEM
/// header format is `name NUL size [SP date [SP mode]] NUL`, so later
/// fields are space-separated. `None` when absent or not a valid decimal
/// number (legacy headers without a length).
fn parse_header_size(payload: &[u8], name_end: usize) -> Option<u64> {
    let rest = payload.get(name_end + 1..)?;
    let end = rest
        .iter()
        .position(|&b| b == 0 || b == b' ')
        .unwrap_or(rest.len());
    let digits = &rest[..end];
    if digits.is_empty() || digits.len() > 20 {
        return None;
    }
    let mut size: u64 = 0;
    for b in digits {
        if !b.is_ascii_digit() {
            return None;
        }
        size = size.saturating_mul(10).saturating_add((b - b'0') as u64);
    }
    Some(size)
}

// ---------------------------------------------------------------------------
// Tests. The state machine is driven only through `on_receive`/`poll` and the
// `Io` trait, so every case below is a deterministic table of "feed these
// bytes at this timestamp, then check what came out".
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    extern crate std;
    use std::vec;
    use std::vec::Vec;

    use super::*;
    use crate::frame::{packet_len, verify_packet};
    use crate::test_support::{make_header, make_packet, unpadded, MemIo};
    use core::num::NonZeroU32;

    fn nz(ms: u32) -> NonZeroU32 {
        NonZeroU32::new(ms).unwrap()
    }

    fn file(name: &'static [u8], size: u64) -> SessionKind<'static> {
        SessionKind::File(FileMeta::new(name, size).unwrap())
    }

    fn recv_cfg(check: CheckMode) -> Config {
        Config {
            check_mode: check,
            ..Default::default()
        }
    }

    fn send_cfg() -> Config {
        Config {
            check_mode: CheckMode::Crc16,
            ..Default::default()
        }
    }

    /// An engine already running a receive session (request byte emitted).
    fn recv_engine(check: CheckMode) -> Engine<MemIo> {
        let mut e = Engine::new(MemIo::default(), recv_cfg(check));
        e.start_recv(0).unwrap();
        e
    }

    /// An engine already running a classic (header-less) send session.
    fn send_engine(data: &[u8]) -> Engine<MemIo> {
        let mut e = Engine::new(MemIo::new(data), send_cfg());
        e.start_send(SessionKind::Classic, 0).unwrap();
        e
    }

    /// An engine already running a header-ful (block 0) send session.
    fn header_engine(data: &[u8], name: &str, size: u64) -> Engine<MemIo> {
        let mut e = Engine::new(MemIo::new(data), send_cfg());
        e.start_send(
            SessionKind::File(FileMeta::new(name.as_bytes(), size).unwrap()),
            0,
        )
        .unwrap();
        e
    }

    // ---- session lifecycle ----

    #[test]
    fn idle_engine_emits_nothing_until_started() {
        let mut e = Engine::new(MemIo::default(), recv_cfg(CheckMode::Crc16));
        assert_eq!(e.status(), Status::Idle);
        // Nothing happens on its own: no handshake byte, no timer, and bytes
        // that arrive before the first session are ignored.
        e.poll(60_000);
        e.on_receive(&[SOH], 60_000);
        assert_eq!(e.io().take(), vec![]);
        assert_eq!(e.status(), Status::Idle);

        e.start_recv(0).unwrap();
        assert_eq!(e.status(), Status::Handshake);
        assert_eq!(e.io().take(), vec![CRC]);
    }

    #[test]
    fn one_handle_alternates_send_and_receive() {
        let mut e = send_engine(b"abc");
        e.on_receive(&[CRC], 0);
        e.on_receive(&[ACK], 1);
        e.on_receive(&[ACK], 2);
        assert_eq!(e.status(), Status::Complete);
        e.io().take();

        // Same engine, same callbacks, opposite direction.
        e.start_recv(0).unwrap();
        assert_eq!(e.status(), Status::Handshake);
        assert_eq!(e.io().take(), vec![CRC]);

        let p1 = make_packet(1, b"fresh", Check::Crc16, false);
        e.on_receive(&p1, 3);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(unpadded(e.io().sinked.clone()), b"fresh");
    }

    #[test]
    fn start_rewinds_the_block_counter_and_check_mode() {
        let mut e = recv_engine(CheckMode::Auto);
        e.io().take();
        e.poll(3000);
        e.poll(6000);
        e.poll(9000); // fall back to SUM8 after three 'C's
        assert_eq!(e.io().take(), vec![CRC, CRC, NAK]);
        let p1 = make_packet(1, b"x", Check::Sum8, false);
        e.on_receive(&p1, 3000);
        e.on_receive(&[EOT], 3001);
        assert_eq!(e.status(), Status::Complete);
        e.io().take();

        e.start_recv(0).unwrap();
        // Back to the configured CRC-first handshake and to block 1.
        assert_eq!(e.io().take(), vec![CRC]);
        // The delivered payload buffer is the application's, not ours.
        e.io().sinked.clear();
        let p1 = make_packet(1, b"y", Check::Crc16, false);
        e.on_receive(&p1, 4000);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(unpadded(e.io().sinked.clone()), b"y");
    }

    #[test]
    fn start_restarts_a_session_after_cancel() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        e.cancel();
        // A local cancel is its own reason AND must put CAN on the wire.
        assert_eq!(
            e.status(),
            Status::Failed(Failure::Local(LocalReason::Cancelled))
        );
        assert_eq!(e.io().take(), vec![CAN, CAN]);
        e.start_recv(0).unwrap();
        assert_eq!(e.status(), Status::Handshake);
        assert_eq!(e.io().take(), vec![CRC]);
    }

    #[test]
    fn fail_io_reports_io_error_and_emits_can() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        e.fail_io();
        assert_eq!(e.status(), Status::Failed(Failure::Local(LocalReason::Io)));
        assert_eq!(e.io().take(), vec![CAN, CAN]);
        assert!(e
            .io()
            .events
            .contains(&Event::Failed(Failure::Local(LocalReason::Io))));
        // Idempotent: a second report must not re-emit CAN / re-fire the event.
        e.fail_io();
        assert!(e.io().take().is_empty());
        assert_eq!(e.io().events.len(), 1);
    }

    #[test]
    fn fail_io_overrides_a_late_complete() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        e.on_receive(&[EOT], 0); // empty transfer completes
        assert_eq!(e.status(), Status::Complete);
        e.io().take();
        // The final ACK could not be sent: do not report success.
        e.fail_io();
        assert_eq!(e.status(), Status::Failed(Failure::Local(LocalReason::Io)));
        assert_eq!(e.io().take(), vec![CAN, CAN]);
    }

    #[test]
    fn start_deadlines_are_relative_to_now_ms() {
        // Arming with a wall clock of 0 made the first poll after a few
        // seconds of uptime look like an instant timeout (NO_RESPONSE).
        let cfg = Config {
            response_timeout_ms: nz(2000),
            ..send_cfg()
        };
        let mut e = Engine::new(MemIo::new(b"x"), cfg);
        e.start_send(SessionKind::Classic, 5_000).unwrap();
        e.poll(5_000);
        assert_eq!(e.status(), Status::Handshake);
        e.poll(7_000);
        assert_eq!(
            e.status(),
            Status::Failed(Failure::Wire(WireReason::NoResponse))
        );
    }

    // ---- receiver ----

    #[test]
    fn receiver_handshake_emits_crc_request_then_falls_back_to_sum8() {
        let mut e = recv_engine(CheckMode::Auto);
        assert_eq!(e.io().take(), vec![CRC]);
        // Three 'C' attempts (initial + two retries) then SUM8, matching
        // common XMODEM-CRC practice and giving a YMODEM sender time to start.
        e.poll(3000);
        assert_eq!(e.io().take(), vec![CRC]);
        e.poll(6000);
        assert_eq!(e.io().take(), vec![CRC]);
        e.poll(9000);
        assert_eq!(e.io().take(), vec![NAK]);
        assert_eq!(e.status(), Status::Handshake);
    }

    #[test]
    fn receiver_forced_crc_never_falls_back() {
        let mut e = recv_engine(CheckMode::Crc16);
        assert_eq!(e.io().take(), vec![CRC]);
        e.poll(3000);
        // It re-sends the CRC request on retry, not a NAK.
        assert_eq!(e.io().take(), vec![CRC]);
    }

    #[test]
    fn receiver_accepts_blocks_and_completes_on_eot() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        let p1 = make_packet(1, b"hello", Check::Crc16, false);
        let p2 = make_packet(2, b"world", Check::Crc16, false);

        e.on_receive(&p1, 10);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(e.status(), Status::Transfer);
        assert_eq!(unpadded(e.io().sinked.clone()), b"hello");

        e.on_receive(&p2, 20);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(e.io().sinked.len(), 2 * BLOCK_128);
        assert_eq!(&e.io().sinked[..5], b"hello");
        assert_eq!(&e.io().sinked[BLOCK_128..BLOCK_128 + 5], b"world");

        e.on_receive(&[EOT], 30);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(e.status(), Status::Complete);
        assert!(e.io().events.contains(&Event::Complete));
    }

    #[test]
    fn receiver_acks_duplicate_without_double_delivery() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        let p1 = make_packet(1, b"data", Check::Crc16, false);
        e.on_receive(&p1, 0);
        assert_eq!(e.io().take(), vec![ACK]);
        // Same block number again (our ACK was lost): ACK, no second delivery.
        e.on_receive(&p1, 1);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(unpadded(e.io().sinked.clone()), b"data");
    }

    #[test]
    fn receiver_out_of_sync_aborts() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        let p1 = make_packet(1, b"a", Check::Crc16, false);
        e.on_receive(&p1, 0); // accepted, expects blk 2
        let p9 = make_packet(9, b"b", Check::Crc16, false);
        e.on_receive(&p9, 1); // neither expected (2) nor previous (1)
        assert_eq!(
            e.status(),
            Status::Failed(Failure::Wire(WireReason::OutOfSync))
        );
        assert!(e
            .io()
            .events
            .contains(&Event::Failed(Failure::Wire(WireReason::OutOfSync))));
    }

    #[test]
    fn receiver_naks_a_corrupt_packet_and_recovers() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        let mut bad = make_packet(1, b"x", Check::Crc16, false);
        bad[5] ^= 0xFF;
        e.on_receive(&bad, 0);
        assert_eq!(e.io().take(), vec![NAK]);
        assert_eq!(e.status(), Status::Handshake); // still waiting for block 1
        let good = make_packet(1, b"y", Check::Crc16, false);
        e.on_receive(&good, 1);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(unpadded(e.io().sinked.clone()), b"y");
    }

    #[test]
    fn receiver_double_can_is_cancel_by_peer() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        e.on_receive(&[CAN, CAN], 0);
        assert_eq!(
            e.status(),
            Status::Failed(Failure::Wire(WireReason::CancelByPeer))
        );
        // A peer cancel must NOT emit CAN back (avoids a CAN storm).
        assert!(e.io().take().is_empty());
    }

    #[test]
    fn receiver_timeout_retries_then_gives_up() {
        let cfg = Config {
            max_retries: 2,
            packet_timeout_ms: nz(100),
            ..recv_cfg(CheckMode::Crc16)
        };
        let mut e = Engine::new(MemIo::default(), cfg);
        e.start_recv(0).unwrap();
        e.io().take();
        // Three consecutive timeouts (initial + 2 retries) -> RetryExceeded.
        e.poll(100);
        e.poll(200);
        e.poll(300);
        assert_eq!(
            e.status(),
            Status::Failed(Failure::Wire(WireReason::RetryExceeded))
        );
    }

    #[test]
    fn receiver_handles_more_than_sixteen_frames_in_one_chunk() {
        // An earlier revision used a fixed 16-frame queue and silently dropped
        // frames beyond it. One large FIFO drain must deliver every block.
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        let mut stream = Vec::new();
        const N: u8 = 24;
        for blk in 1..=N {
            stream.extend_from_slice(&make_packet(blk, &[blk], Check::Crc16, false));
        }
        e.on_receive(&stream, 0);
        assert_eq!(e.status(), Status::Transfer);
        assert_eq!(e.io().sinked.len(), (N as usize) * BLOCK_128);
        for (i, blk) in (1..=N).enumerate() {
            assert_eq!(e.io().sinked[i * BLOCK_128], blk);
        }
        assert_eq!(e.io().take(), vec![ACK; N as usize]);
        e.on_receive(&[EOT], 1);
        assert_eq!(e.status(), Status::Complete);
    }

    // ---- sender ----

    #[test]
    fn sender_full_transfer_then_eot_then_ack() {
        let mut e = send_engine(b"abc");
        assert_eq!(e.io().take(), vec![]); // nothing until the peer handshakes

        e.on_receive(&[CRC], 0);
        let out = e.io().take();
        assert_eq!(out[0], SOH);
        assert_eq!(out[1], 1);
        assert_eq!(out[2], !1u8);
        assert_eq!(out.len(), packet_len(SOH, Check::Crc16));
        assert!(verify_packet(&out, Check::Crc16));
        assert_eq!(&out[3..6], b"abc");
        assert_eq!(out[6], SUB); // padded

        e.on_receive(&[ACK], 1);
        assert_eq!(e.io().take(), vec![EOT]);
        e.on_receive(&[ACK], 2);
        assert_eq!(e.status(), Status::Complete);
    }

    #[test]
    fn sender_sum8_when_peer_naks() {
        let mut e = send_engine(b"z");
        e.on_receive(&[NAK], 0); // classic checksum handshake
        let out = e.io().take();
        assert_eq!(out.len(), packet_len(SOH, Check::Sum8));
        assert!(verify_packet(&out, Check::Sum8));
    }

    #[test]
    fn sender_retransmits_same_block_on_nak() {
        let mut e = send_engine(b"payload");
        e.on_receive(&[CRC], 0);
        let first = e.io().take();
        e.on_receive(&[NAK], 1);
        let resend = e.io().take();
        assert_eq!(resend, first); // byte-identical retransmission

        e.on_receive(&[ACK], 2);
        assert_eq!(e.io().take(), vec![EOT]);
    }

    #[test]
    fn sender_nak_retries_exceeded() {
        let cfg = Config {
            max_retries: 2,
            ..send_cfg()
        };
        let mut e = Engine::new(MemIo::new(b"x"), cfg);
        e.start_send(SessionKind::Classic, 0).unwrap();
        e.on_receive(&[CRC], 0);
        e.io().take();
        e.on_receive(&[NAK], 1);
        e.on_receive(&[NAK], 2);
        e.on_receive(&[NAK], 3); // third NAK exhausts retries
        assert_eq!(
            e.status(),
            Status::Failed(Failure::Wire(WireReason::RetryExceeded))
        );
    }

    #[test]
    fn sender_no_response_timeout() {
        let cfg = Config {
            response_timeout_ms: nz(1000),
            ..send_cfg()
        };
        let mut e = Engine::new(MemIo::new(b"x"), cfg);
        e.start_send(SessionKind::Classic, 0).unwrap();
        e.poll(1000); // never got a handshake byte at all
        assert_eq!(
            e.status(),
            Status::Failed(Failure::Wire(WireReason::NoResponse))
        );
    }

    #[test]
    fn sender_1k_blocks_when_requested() {
        let src: Vec<u8> = (0..1500).map(|i| (i & 0xFF) as u8).collect();
        let cfg = Config {
            block_1k: true,
            ..send_cfg()
        };
        let mut e = Engine::new(MemIo::new(&src), cfg);
        e.start_send(SessionKind::Classic, 0).unwrap();
        e.on_receive(&[CRC], 0);
        let out = e.io().take();
        assert_eq!(out[0], STX);
        assert_eq!(out.len(), packet_len(STX, Check::Crc16));
        assert!(verify_packet(&out, Check::Crc16));
        assert_eq!(out[3], 0);
    }

    #[test]
    fn sender_rejects_broken_source() {
        let mut io = MemIo::new(b"x");
        io.broken_source = true;
        let mut e = Engine::new(io, send_cfg());
        e.start_send(SessionKind::Classic, 0).unwrap();
        e.on_receive(&[CRC], 0);
        assert_eq!(e.status(), Status::Failed(Failure::Local(LocalReason::Io)));
        // Must not send EOT then CAN: the source failed, there is no file.
        let out = e.io().take();
        assert!(!out.contains(&EOT));
        assert_eq!(&out[out.len() - 2..], &[CAN, CAN]);
    }

    // ---- end to end: a sender wired to a receiver through their Io bytes ----

    /// Pump bytes between two engines until both finish (or the loop budget
    /// runs out). `step_ms` also lets receiver-side timeouts fire.
    fn pump(tx: &mut Engine<MemIo>, rx: &mut Engine<MemIo>, step_ms: u32, rounds: u32) {
        // The receiver's initial handshake request.
        let hs = rx.io().take();
        tx.on_receive(&hs, 0);
        let mut now = 0u32;
        for _ in 0..rounds {
            now += step_ms;
            tx.poll(now);
            rx.poll(now);
            let to_rx = tx.io().take();
            if !to_rx.is_empty() {
                rx.on_receive(&to_rx, now);
            }
            let to_tx = rx.io().take();
            if !to_tx.is_empty() {
                tx.on_receive(&to_tx, now);
            }
            if tx.status() == Status::Complete && rx.status() == Status::Complete {
                break;
            }
        }
    }

    #[test]
    fn loopback_sender_to_receiver() {
        let payload: Vec<u8> = (0..300).map(|i| (i & 0xFF) as u8).collect();
        let mut tx = send_engine(&payload);
        let mut rx = recv_engine(CheckMode::Crc16);
        pump(&mut tx, &mut rx, 1, 64);

        assert_eq!(tx.status(), Status::Complete);
        assert_eq!(rx.status(), Status::Complete);
        // XMODEM pads the last block with SUB; strip it before comparing.
        assert_eq!(unpadded(rx.io().sinked.clone()), payload);
    }

    #[test]
    fn loopback_with_1k_blocks_and_sum8() {
        let payload: Vec<u8> = (0..2100).map(|i| (i & 0xFF) as u8).collect();
        let cfg_tx = Config {
            check_mode: CheckMode::Sum8,
            block_1k: true,
            ..Default::default()
        };
        let cfg_rx = Config {
            check_mode: CheckMode::Sum8,
            block_1k: true,
            ..Default::default()
        };
        let mut tx = Engine::new(MemIo::new(&payload), cfg_tx);
        tx.start_send(SessionKind::Classic, 0).unwrap();
        let mut rx = Engine::new(MemIo::default(), cfg_rx);
        rx.start_recv(0).unwrap();
        let hs = rx.io().take();
        assert_eq!(hs, vec![NAK]);
        tx.on_receive(&hs, 0);

        let mut now = 0u32;
        for _ in 0..64 {
            now += 1;
            tx.poll(now);
            rx.poll(now);
            let a = tx.io().take();
            if !a.is_empty() {
                rx.on_receive(&a, now)
            }
            let b = rx.io().take();
            if !b.is_empty() {
                tx.on_receive(&b, now)
            }
            if tx.status() == Status::Complete && rx.status() == Status::Complete {
                break;
            }
        }
        assert_eq!(rx.status(), Status::Complete);
        assert_eq!(unpadded(rx.io().sinked.clone()), payload);
    }

    // ---- optional block 0 (file header) ----

    #[test]
    fn large_declared_size_delivers_full_blocks() {
        // A size >= 4 GiB used to be cast to `usize` before clamping, which on
        // the 32-bit firmware targets truncated to 0: the first data block was
        // delivered as an empty slice, ACKed, and the file swallowed silently.
        // `deliverable_len` clamps in the u64 domain first, so a full block
        // must still be delivered regardless of the pointer width.
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take(); // the 'C' request

        let huge = 0x1_0000_0000u64 + 64; // 4 GiB + 64 B
        e.on_receive(&make_header(b"big.bin", Some(huge)), 0);
        assert_eq!(e.io().take(), vec![ACK, CRC]);
        assert_eq!(e.io().files, vec![(b"big.bin".to_vec(), Some(huge))]);

        let mut payload = [b'Z'; BLOCK_128];
        payload[0..4].copy_from_slice(b"data");
        e.on_receive(&make_packet(1, &payload, Check::Crc16, false), 1);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(e.io().sinked.len(), BLOCK_128);
        assert_eq!(&e.io().sinked[..4], b"data");

        // The remaining count stayed in u64 land: still ~4 GiB to go, so the
        // engine is nowhere near finishing.
        assert_eq!(e.status(), Status::Transfer);
    }

    #[test]
    fn receive_trims_padding_to_declared_size() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take(); // the 'C' request

        e.on_receive(&make_header(b"fw.bin", Some(5)), 0);
        let out = e.io().take();
        assert_eq!(out, vec![ACK, CRC]); // ACK the header, 'C' for block 1
        assert_eq!(e.io().files, vec![(b"fw.bin".to_vec(), Some(5))]);

        e.on_receive(&make_packet(1, b"hello!", Check::Crc16, false), 1);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(e.io().sinked, b"hello"); // trimmed, no SUB, no '!'

        e.on_receive(&[EOT], 2);
        // ACK the EOT, then request the next block 0 with a fresh 'C'.
        assert_eq!(e.io().take(), vec![ACK, CRC]);
        // Standard senders follow with the empty block 0 terminator.
        e.on_receive(&make_header(b"", None), 3);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(e.status(), Status::Complete);
        assert!(e.io().events.contains(&Event::Complete));
    }

    #[test]
    fn receiver_accepts_classic_sender_without_header() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take(); // 'C'
        e.on_receive(&make_packet(1, b"raw!", Check::Crc16, false), 0);
        assert_eq!(e.io().take(), vec![ACK]);
        assert!(e.io().files.is_empty()); // no header, no metadata callback

        e.on_receive(&[EOT], 1);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(e.status(), Status::Complete);
        assert_eq!(e.io().sinked.len(), BLOCK_128);
        assert_eq!(unpadded(e.io().sinked.clone()), b"raw!");
    }

    /// The heart of the redesign: one receiver, no protocol mode, detects a
    /// header-ful sender in one session and a classic one in the next.
    #[test]
    fn recv_session_detects_header_then_classic() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        e.on_receive(&make_header(b"a", Some(1)), 0);
        e.on_receive(&make_packet(1, b"A", Check::Crc16, false), 1);
        e.on_receive(&[EOT], 2);
        e.on_receive(&make_header(b"", None), 3);
        assert_eq!(e.status(), Status::Complete);
        assert_eq!(e.io().sinked, b"A");
        let n = e.io().sinked.len();
        e.io().take(); // drain the header-ful session's wire output

        e.start_recv(4).unwrap();
        assert_eq!(e.io().take(), vec![CRC]);
        e.on_receive(&make_packet(1, b"raw", Check::Crc16, false), 5);
        e.on_receive(&[EOT], 6);
        assert_eq!(e.status(), Status::Complete);
        assert_eq!(unpadded(e.io().sinked[n..].to_vec()), b"raw");
    }

    #[test]
    fn receiver_graceful_end_when_terminator_omitted() {
        let cfg = Config {
            max_retries: 2,
            packet_timeout_ms: nz(100),
            response_timeout_ms: nz(100),
            ..recv_cfg(CheckMode::Crc16)
        };
        let mut e = Engine::new(MemIo::default(), cfg);
        e.start_recv(0).unwrap();
        e.io().take();
        e.on_receive(&make_header(b"one", Some(1)), 0);
        e.io().take();
        e.on_receive(&make_packet(1, b"Z", Check::Crc16, false), 1);
        e.io().take(); // data ACK
        e.on_receive(&[EOT], 2);
        // EOT is ACKed and the next block 0 is requested. A single-file host
        // tool omits the terminator, so the line stays quiet...
        assert_eq!(e.io().take(), vec![ACK, CRC]);
        // ...so after re-requesting the terminator for the retry budget, a
        // quiet line is a successful end, not a timeout failure.
        for t in 1..=50 {
            e.poll(t * 100);
        }
        assert_eq!(e.status(), Status::Complete);
    }

    #[test]
    fn reject_file_cancels_with_can() {
        let io = MemIo {
            reject_files: true,
            ..Default::default()
        };
        let mut e = Engine::new(io, recv_cfg(CheckMode::Crc16));
        e.start_recv(0).unwrap();
        e.io().take();

        e.on_receive(&make_header(b"big.bin", Some(9999)), 0);
        let out = e.io().take();
        // A rejected file is a deliberate decision by the application, not a
        // broken callback: it has its own Reason so callers can tell them apart.
        assert_eq!(
            e.status(),
            Status::Failed(Failure::Local(LocalReason::FileRejected))
        );
        assert_eq!(&out[out.len() - 2..], &[CAN, CAN]);
        assert_eq!(e.io().files[0].0.as_slice(), b"big.bin");
    }

    #[test]
    fn sizeless_header_keeps_whole_blocks() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();

        e.on_receive(&make_header(b"legacy", None), 0);
        assert_eq!(e.io().files, vec![(b"legacy".to_vec(), None)]);
        e.io().take(); // ACK + 'C'

        e.on_receive(&make_packet(1, b"data", Check::Crc16, false), 1);
        e.io().take();
        e.on_receive(&[EOT], 2);
        e.io().take();
        e.on_receive(&make_header(b"", None), 3);
        assert_eq!(e.status(), Status::Complete);
        // No size declared: whole-block delivery, SUB padding included.
        assert_eq!(e.io().sinked.len(), BLOCK_128);
        assert_eq!(unpadded(e.io().sinked.clone()), b"data");
    }

    #[test]
    fn send_one_file_with_header() {
        let mut e = header_engine(b"hello", "abc.bin", 5);
        assert_eq!(e.io().take(), vec![]); // nothing until the receiver asks

        e.on_receive(&[CRC], 0); // receiver requests the header
        let header = e.io().take();
        assert_eq!(header[0], SOH);
        assert_eq!(header[1], 0);
        assert_eq!(header[2], 0xFF);
        assert_eq!(&header[3..10], b"abc.bin");
        assert_eq!(header[10], 0);
        assert_eq!(&header[11..12], b"5");
        assert!(verify_packet(&header, Check::Crc16));

        e.on_receive(&[ACK], 1);
        assert_eq!(e.io().take(), vec![]); // now waiting for the second 'C'
        e.on_receive(&[CRC], 2); // receiver requests block 1
        let out = e.io().take();
        assert_eq!(out[0], SOH);
        assert_eq!(out[1], 1);
        assert_eq!(&out[3..8], b"hello");

        e.on_receive(&[ACK], 3);
        assert_eq!(e.io().take(), vec![EOT]);
        e.on_receive(&[ACK], 4);
        assert_eq!(e.status(), Status::Complete);
    }

    /// Regression: after block 0 is ACKed the sender sits in SendHandshake and
    /// the receiver re-requests block 1 with NAK (its 'C' was lost and its
    /// packet timeout fired). The header already fixed CRC-16, so that NAK can
    /// only mean "send block 1" -- it must NOT be ignored (which used to make
    /// both sides burn their retry budgets) or downgrade the check to SUM8.
    #[test]
    fn header_ack_then_nak_sends_block_1_as_crc16() {
        let mut e = header_engine(b"hello", "abc.bin", 5);
        e.on_receive(&[CRC], 0); // receiver requests block 0
        assert_eq!(e.io().take()[1], 0); // header (block 0) went out
        e.on_receive(&[ACK], 1); // header ACKed -> SendHandshake
        assert!(e.io().take().is_empty());

        // The receiver's 'C' was lost; its timeout re-asks with NAK.
        e.on_receive(&[NAK], 2);
        let out = e.io().take();
        assert_eq!(out[0], SOH);
        assert_eq!(out[1], 1); // block 1, not a retransmitted header
        assert_eq!(out.len(), packet_len(SOH, Check::Crc16));
        assert!(verify_packet(&out, Check::Crc16)); // still CRC-16
        assert_eq!(&out[3..8], b"hello");
        assert_eq!(e.status(), Status::Transfer);
    }

    /// A YMODEM batch is just several `start_send` calls on one handle: each
    /// waits for the receiver's fresh 'C', which is exactly the batch wire
    /// timing. The library keeps no batch bookkeeping of its own.
    #[test]
    fn repeated_send_calls_form_a_ymodem_batch() {
        let mut e = Engine::new(MemIo::new(b"hi"), send_cfg());

        // File 1.
        e.start_send(file(b"a.bin", 2), 0).unwrap();
        e.on_receive(&[CRC], 0);
        assert_eq!(e.io().take()[0], SOH); // header
        e.on_receive(&[ACK], 1);
        e.on_receive(&[CRC], 2);
        assert_eq!(e.io().take()[1], 1); // block 1
        e.on_receive(&[ACK], 3);
        assert_eq!(e.io().take(), vec![EOT]);
        e.on_receive(&[ACK], 4);
        assert_eq!(e.status(), Status::Complete);

        // Terminator: an empty name on the SAME handle, a fresh session.
        e.start_send(SessionKind::BatchEnd, 5).unwrap();
        e.on_receive(&[CRC], 6);
        let header = e.io().take();
        assert_eq!(&header[3..5], &[0, 0]); // empty name
        e.on_receive(&[ACK], 7);
        assert_eq!(e.status(), Status::Complete);

        // The very same handle also speaks classic XMODEM: no mode to switch.
        // start_send never touches the application's buffers, so the app
        // rewinds its own source for the fresh session.
        e.io().source_pos = 0;
        e.start_send(SessionKind::Classic, 8).unwrap();
        e.on_receive(&[CRC], 9);
        let out = e.io().take();
        assert_eq!(out[1], 1); // straight to data block 1, no header
        assert_eq!(&out[3..5], b"hi");
    }

    /// A receiver needs no batch state: block 0 after EOT is simply the next
    /// file, and an empty block 0 ends the session. This is the standard
    /// YMODEM batch wire, handled by one receive session.
    #[test]
    fn recv_one_session_handles_a_multi_file_batch() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();

        // File 1 (header a, one byte).
        e.on_receive(&make_header(b"a", Some(1)), 0);
        e.io().take(); // header ACK + 'C' for block 1
        e.on_receive(&make_packet(1, b"A", Check::Crc16, false), 1);
        e.on_receive(&[EOT], 2);
        assert_eq!(e.io().take(), vec![ACK, ACK, CRC]); // data ACK, EOT ACK, 'C'

        // File 2 arrives in the SAME session: a fresh block 0.
        e.on_receive(&make_header(b"b", Some(2)), 3);
        e.io().take(); // header ACK + 'C' for block 1
        e.on_receive(&make_packet(1, b"BC", Check::Crc16, false), 4);
        e.on_receive(&[EOT], 5);
        e.io().take();

        // Empty block 0 closes the batch.
        e.on_receive(&make_header(b"", None), 6);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(e.status(), Status::Complete);
        assert_eq!(e.io().sinked, b"ABC");
        assert_eq!(
            e.io().files,
            vec![(b"a".to_vec(), Some(1)), (b"b".to_vec(), Some(2)),]
        );
        let files_done = e
            .io()
            .events
            .iter()
            .filter(|ev| **ev == Event::FileComplete)
            .count();
        assert_eq!(files_done, 2);
        assert!(e.io().events.contains(&Event::Complete));
    }

    #[test]
    fn start_send_validates_the_name() {
        assert_eq!(FileMeta::new(b"a\0b", 1), Err(InvalidName));
        let long = [b'a'; 200];
        assert_eq!(FileMeta::new(&long, 1), Err(InvalidName));
        assert_eq!(FileMeta::new(b"", 0), Err(InvalidName));
        assert!(FileMeta::new(b"ok.bin", 1).is_ok());

        let mut e2 = Engine::new(MemIo::new(b"x"), send_cfg());
        assert_eq!(e2.start_send(SessionKind::BatchEnd, 0), Ok(()));
        let mut e3 = Engine::new(MemIo::new(b"x"), send_cfg());
        assert_eq!(e3.start_send(SessionKind::Classic, 0), Ok(()));
    }

    /// Starting on top of an in-flight session used to silently abandon it.
    /// It is now a machine-checked error, discoverable via `can_start()`.
    #[test]
    fn start_while_active_is_busy_not_a_silent_reset() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        assert!(!e.can_start());
        assert_eq!(e.start_recv(1), Err(StartError::Busy));
        assert_eq!(e.start_send(SessionKind::Classic, 1), Err(StartError::Busy));
        // The in-flight session is untouched: still waiting for block 1.
        assert_eq!(e.status(), Status::Handshake);
        assert!(e.io().take().is_empty());

        // Once it finishes, a new session is allowed again.
        e.on_receive(&[EOT], 2); // empty transfer: ACK + Complete
        assert_eq!(e.status(), Status::Complete);
        assert!(e.can_start());
        e.io().take(); // drain the empty transfer's ACK
        assert_eq!(e.start_recv(3), Ok(()));
        assert_eq!(e.io().take(), vec![CRC]);
    }

    /// The whole point of the Wire/Local split: a peer cancel and a local
    /// cancel are different failures with opposite CAN behaviour.
    #[test]
    fn failure_layers_distinguish_peer_and_local_cancels() {
        let peer = Failure::Wire(WireReason::CancelByPeer);
        let local = Failure::Local(LocalReason::Cancelled);
        assert!(peer.peer_already_cancelled());
        assert!(!local.peer_already_cancelled());

        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        e.on_receive(&[CAN, CAN], 0);
        assert_eq!(e.status(), Status::Failed(peer));
        assert!(e.io().take().is_empty()); // no CAN echo -> no storm
    }

    #[test]
    fn loopback_with_file_header() {
        let payload: Vec<u8> = (0..300).map(|i| (i & 0xFF) as u8).collect();
        let cfg = Config {
            max_retries: 2,
            packet_timeout_ms: nz(10),
            response_timeout_ms: nz(500),
            ..send_cfg()
        };
        let mut tx = Engine::new(MemIo::new(&payload), cfg);
        tx.start_send(
            SessionKind::File(FileMeta::new(b"fw.bin", payload.len() as u64).unwrap()),
            0,
        )
        .unwrap();
        let rx_cfg = Config {
            max_retries: 2,
            packet_timeout_ms: nz(10),
            response_timeout_ms: nz(10),
            ..recv_cfg(CheckMode::Crc16)
        };
        let mut rx = Engine::new(MemIo::default(), rx_cfg);
        rx.start_recv(0).unwrap();
        // The sender omits the empty terminator; the receiver re-requests it,
        // then completes gracefully. Enough rounds for that timeout chain.
        pump(&mut tx, &mut rx, 10, 256);

        assert_eq!(tx.status(), Status::Complete);
        assert_eq!(rx.status(), Status::Complete);
        assert_eq!(rx.io().sinked, payload); // trimmed to the declared size
        assert_eq!(
            rx.io().files,
            vec![(b"fw.bin".to_vec(), Some(payload.len() as u64))]
        );
        assert!(rx.io().events.contains(&Event::FileComplete));
        assert!(rx.io().events.contains(&Event::Complete));
    }

    #[test]
    fn wait_ack_c_resends_the_block() {
        let mut e = send_engine(b"abc");
        e.on_receive(&[CRC], 0);
        let block = e.io().take();
        e.on_receive(&[CRC], 1); // CRC-mode retry, not NAK
        assert_eq!(e.io().take(), block);
    }

    #[test]
    fn wait_eot_ack_c_resends_eot() {
        let mut e = send_engine(b"abc");
        e.on_receive(&[CRC], 0);
        e.io().take();
        e.on_receive(&[ACK], 1);
        assert_eq!(e.io().take(), vec![EOT]);
        e.on_receive(&[CRC], 2);
        assert_eq!(e.io().take(), vec![EOT]);
    }

    #[test]
    fn header_wait_ack_nak_resends_header() {
        let mut e = header_engine(b"hello", "abc.bin", 5);
        e.on_receive(&[CRC], 0);
        let header = e.io().take();
        assert_eq!(header[1], 0);
        e.on_receive(&[NAK], 1);
        let again = e.io().take();
        assert_eq!(again[1], 0);
        assert_eq!(again, header);
    }

    #[test]
    fn sender_late_nak_after_eot_complete_resends_eot() {
        let cfg = Config {
            max_retries: 2,
            response_timeout_ms: nz(100),
            ..send_cfg()
        };
        let mut e = Engine::new(MemIo::new(b"x"), cfg);
        e.start_send(SessionKind::Classic, 0).unwrap();
        e.on_receive(&[CRC], 0);
        e.io().take();
        e.on_receive(&[ACK], 1);
        assert_eq!(e.io().take(), vec![EOT]);
        e.poll(101);
        e.poll(201);
        e.poll(301);
        assert_eq!(e.status(), Status::Complete);
        e.io().take(); // drain EOT retries from the timeout path
        e.on_receive(&[NAK], 400);
        assert_eq!(e.io().take(), vec![EOT]);
        assert_eq!(e.status(), Status::Complete);
    }

    #[test]
    fn receiver_acks_late_empty_header_after_complete() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        e.on_receive(&make_header(b"a", Some(1)), 0);
        e.io().take();
        e.on_receive(&make_packet(1, b"A", Check::Crc16, false), 1);
        e.io().take();
        e.on_receive(&[EOT], 2);
        e.io().take();
        e.on_receive(&make_header(b"", None), 3);
        assert_eq!(e.status(), Status::Complete);
        e.io().take();
        e.on_receive(&make_header(b"", None), 4);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(e.status(), Status::Complete);
    }

    #[test]
    fn after_eot_duplicate_last_block_is_acked() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        e.on_receive(&make_header(b"a", Some(1)), 0);
        e.io().take();
        let block = make_packet(1, b"A", Check::Crc16, false);
        e.on_receive(&block, 1);
        e.io().take();
        e.on_receive(&[EOT], 2);
        e.io().take();
        e.on_receive(&block, 3);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(e.status(), Status::Transfer);
    }

    #[test]
    fn sender_second_c_timeout_fails() {
        let cfg = Config {
            max_retries: 2,
            response_timeout_ms: nz(100),
            ..send_cfg()
        };
        let mut e = Engine::new(MemIo::new(b"x"), cfg);
        e.start_send(file(b"a.bin", 1), 0).unwrap();
        e.on_receive(&[CRC], 0);
        e.io().take();
        e.on_receive(&[ACK], 1); // header ACKed; waiting for the second 'C'
        assert!(e.io().take().is_empty());
        e.poll(101);
        e.poll(201);
        e.poll(301);
        assert_eq!(
            e.status(),
            Status::Failed(Failure::Wire(WireReason::RetryExceeded))
        );
    }

    #[test]
    fn sender_naks_of_eot_are_resent_then_acked() {
        let mut e = send_engine(b"abc");
        e.on_receive(&[CRC], 0);
        e.io().take();
        e.on_receive(&[ACK], 1);
        assert_eq!(e.io().take(), vec![EOT]);
        e.on_receive(&[NAK], 2);
        assert_eq!(e.io().take(), vec![EOT]);
        e.on_receive(&[ACK], 3);
        assert_eq!(e.status(), Status::Complete);
    }

    #[test]
    fn receiver_acks_late_eot_after_complete() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        e.on_receive(&[EOT], 0);
        assert_eq!(e.status(), Status::Complete);
        e.io().take();
        e.on_receive(&[EOT], 1);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(e.status(), Status::Complete);
    }

    #[test]
    fn after_eot_late_eot_is_acked_without_complete() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        e.on_receive(&make_header(b"a", Some(1)), 0);
        e.io().take();
        e.on_receive(&make_packet(1, b"A", Check::Crc16, false), 1);
        e.io().take();
        e.on_receive(&[EOT], 2);
        assert_eq!(e.io().take(), vec![ACK, CRC]);
        assert!(e.io().events.contains(&Event::FileComplete));
        e.on_receive(&[EOT], 3);
        assert_eq!(e.io().take(), vec![ACK]);
        assert_eq!(e.status(), Status::Transfer);
    }

    #[test]
    fn sender_eot_timeout_after_data_ack_is_success() {
        let cfg = Config {
            max_retries: 2,
            response_timeout_ms: nz(100),
            ..send_cfg()
        };
        let mut e = Engine::new(MemIo::new(b"x"), cfg);
        e.start_send(SessionKind::Classic, 0).unwrap();
        e.on_receive(&[CRC], 0);
        e.io().take();
        e.on_receive(&[ACK], 1); // EOT armed at 1 + 100 ms
        assert_eq!(e.io().take(), vec![EOT]);
        e.poll(101);
        e.poll(201);
        e.poll(301);
        assert_eq!(e.status(), Status::Complete);
    }

    #[test]
    fn sender_1k_last_block_drops_to_soh() {
        let cfg = Config {
            block_1k: true,
            ..send_cfg()
        };
        let mut e = Engine::new(MemIo::new(b"short"), cfg);
        e.start_send(SessionKind::Classic, 0).unwrap();
        e.on_receive(&[CRC], 0);
        let out = e.io().take();
        assert_eq!(out[0], SOH);
        assert_eq!(out.len(), packet_len(SOH, Check::Crc16));
        assert!(verify_packet(&out, Check::Crc16));
        assert_eq!(&out[3..8], b"short");
    }

    #[test]
    fn sender_1k_then_short_remainder_is_soh() {
        let src: Vec<u8> = (0..1100).map(|i| (i & 0xFF) as u8).collect();
        let cfg = Config {
            block_1k: true,
            ..send_cfg()
        };
        let mut e = Engine::new(MemIo::new(&src), cfg);
        e.start_send(SessionKind::Classic, 0).unwrap();
        e.on_receive(&[CRC], 0);
        let first = e.io().take();
        assert_eq!(first[0], STX);
        e.on_receive(&[ACK], 1);
        let second = e.io().take();
        assert_eq!(second[0], SOH);
        assert_eq!(second.len(), packet_len(SOH, Check::Crc16));
        assert_eq!(&second[3..3 + 76], &src[1024..]);
    }

    #[test]
    fn sink_failure_aborts_without_ack() {
        let io = MemIo {
            fail_sink: true,
            ..Default::default()
        };
        let mut e = Engine::new(io, recv_cfg(CheckMode::Crc16));
        e.start_recv(0).unwrap();
        e.io().take();
        e.on_receive(&make_packet(1, b"x", Check::Crc16, false), 0);
        assert_eq!(e.status(), Status::Failed(Failure::Local(LocalReason::Io)));
        let out = e.io().take();
        assert!(!out.contains(&ACK));
        assert_eq!(&out[out.len() - 2..], &[CAN, CAN]);
    }

    #[test]
    fn emit_failure_on_first_block_does_not_send_eot() {
        let mut io = MemIo::new(b"x");
        io.fail_emit = true;
        let mut e = Engine::new(io, send_cfg());
        e.start_send(SessionKind::Classic, 0).unwrap();
        e.on_receive(&[CRC], 0);
        assert_eq!(e.status(), Status::Failed(Failure::Local(LocalReason::Io)));
        assert!(!e.io().emitted.contains(&EOT));
    }

    #[test]
    fn receiver_block_number_wraps_from_255_to_0() {
        let mut e = recv_engine(CheckMode::Crc16);
        e.io().take();
        let mut stream = Vec::new();
        for blk in 1..=255u8 {
            stream.extend_from_slice(&make_packet(blk, &[blk], Check::Crc16, false));
        }
        stream.extend_from_slice(&make_packet(0, &[0], Check::Crc16, false));
        e.on_receive(&stream, 0);
        assert_eq!(e.status(), Status::Transfer);
        assert_eq!(e.io().sinked.len(), 256 * BLOCK_128);
        assert_eq!(e.io().sinked[254 * BLOCK_128], 255);
        assert_eq!(e.io().sinked[255 * BLOCK_128], 0);
        e.on_receive(&[EOT], 1);
        assert_eq!(e.status(), Status::Complete);
    }
}
