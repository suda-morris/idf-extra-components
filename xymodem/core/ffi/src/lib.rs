// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
//! C ABI export of the `xymodem-proto` sans-IO engine.
//!
//! The whole state machine lives in a caller-owned, fixed-size opaque buffer
//! ([`XymodemCore`]). There is no allocation and no hidden global state: the C
//! side owns the memory, feeds bytes with `xymodem_core_on_receive`, drives the
//! machine with `xymodem_core_poll`, and supplies every side effect through the
//! [`XymodemCoreIo`] callback table. `xymodem_core.h` is generated from this
//! file by cbindgen -- never edit it by hand.

#![cfg_attr(not(test), no_std)]
// Variant identifiers are the C names (XYMODEM_OK, XYMODEM_ST_*, ...) so the
// generated header matches what C callers switch on.
#![allow(non_camel_case_types)]

use core::ffi::{c_char, c_int, c_void};
use core::num::NonZeroU32;
use xymodem_proto::io::{Event, Failure, Io, IoError, LocalReason, WireReason};
use xymodem_proto::modem::{
    CheckMode, Config, Engine, FileMeta, Role, SessionKind, StartError, Status,
};

// A staticlib is the final artifact on bare metal: it must provide the panic
// handler (the `proto` rlib deliberately does not). `loop {}` here would hang
// the chip silently on ANY panic (a bad slice, a debug_assert, ...). Route it
// to newlib's abort() instead: that reaches ESP-IDF's panic handler and
// reboots with a diagnostic instead of freezing forever.
// Gated off under `cfg(test)` so the std test harness can own panics on the host.
#[cfg(not(test))]
extern "C" {
    fn abort() -> !;
}

#[cfg(not(test))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    // SAFETY: abort() is provided by newlib (linked into every ESP-IDF app)
    // and never returns.
    unsafe { abort() }
}

// ---------------------------------------------------------------------------
// ABI enumerations (cbindgen exports these as C enums so a switch on the C
// side can warn when a variant is missing. Numeric values are stable.)
// ---------------------------------------------------------------------------

/// Return / failure codes of every `xymodem_core_*` function that reports an
/// error. Status and role use their own enums.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum XymodemErr {
    /// Success.
    XYMODEM_OK = 0,
    /// A NULL pointer or an invalid config / name was passed.
    XYMODEM_ERR_INVALID_ARG = -1,
    /// Peer sent two consecutive CAN bytes.
    XYMODEM_ERR_CANCEL_BY_PEER = -2,
    /// No valid response before the configured timeout.
    XYMODEM_ERR_NO_RESPONSE = -3,
    /// Retry counter exhausted.
    XYMODEM_ERR_RETRY_EXCEEDED = -4,
    /// Block number was neither expected nor previous.
    XYMODEM_ERR_OUT_OF_SYNC = -5,
    /// An I/O callback failed (e.g. the source over-reported its length).
    XYMODEM_ERR_IO = -6,
    /// The application rejected a block-0 file header via on_file.
    XYMODEM_ERR_FILE_REJECTED = -7,
    /// The local application cancelled the transfer (xymodem_core_cancel).
    XYMODEM_ERR_CANCEL_BY_LOCAL = -8,
    /// A session is still in flight; wait for it to finish before starting again.
    /// Returned by start_recv/start_send, not by xymodem_core_error().
    XYMODEM_ERR_BUSY = -9,
}

/// How the receiver picks its check mode. Stored as `uint8_t` in
/// [`XymodemCoreConfig`] so a garbage value can still be rejected at init.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum XymodemCheckMode {
    /// Try CRC-16 first, fall back to SUM8 if the peer is silent.
    XYMODEM_CHECK_AUTO = 0,
    /// Force 8-bit checksum.
    XYMODEM_CHECK_SUM8 = 1,
    /// Force CRC-16.
    XYMODEM_CHECK_CRC16 = 2,
}

/// High-level transfer status. Contiguous 0..5 so a NULL handle can return
/// XYMODEM_ST_INVALID instead of mixing an error code into this enum.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum XymodemStatus {
    /// Initialized, no session started yet.
    XYMODEM_ST_IDLE = 0,
    /// Waiting for / negotiating the handshake.
    XYMODEM_ST_HANDSHAKE = 1,
    /// A transfer is in progress.
    XYMODEM_ST_TRANSFER = 2,
    /// The transfer completed successfully.
    XYMODEM_ST_COMPLETE = 3,
    /// The transfer failed; call `xymodem_core_error()` for the reason.
    XYMODEM_ST_FAILED = 4,
    /// Not a live engine (NULL pointer, or the IDF handle's magic was cleared).
    XYMODEM_ST_INVALID = 5,
}

/// Lifecycle events delivered through `on_event`. 1-based so 0 can serve as a
/// "no event" sentinel in C.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum XymodemEvent {
    /// Handshake negotiated. `arg` is unused (XYMODEM_OK).
    XYMODEM_EV_HANDSHAKE = 1,
    /// A block was accepted (receiver) or acknowledged (sender). `arg` = XYMODEM_OK.
    XYMODEM_EV_BLOCK = 2,
    /// Transfer completed. `arg` = XYMODEM_OK.
    XYMODEM_EV_COMPLETE = 3,
    /// Transfer failed. `arg` carries a negative XYMODEM_ERR_* code.
    XYMODEM_EV_FAILED = 4,
    /// A YMODEM file ended (EOT after a block-0 header). The session stays
    /// open for the next header; XYMODEM_EV_COMPLETE still marks the whole batch.
    XYMODEM_EV_FILE_COMPLETE = 5,
}

/// Which side of the wire the engine is currently playing. XYMODEM_ROLE_NONE
/// when idle or terminal: after Complete the application chooses the next
/// `start_*`.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum XymodemRole {
    /// Idle, complete, failed, or a NULL pointer.
    XYMODEM_ROLE_NONE = 0,
    /// A receive session is in flight.
    XYMODEM_ROLE_RECV = 1,
    /// A send session is in flight.
    XYMODEM_ROLE_SEND = 2,
}

/// Number of `uint64_t` words in `xymodem_core_t::_storage`. 320 words = 2560
/// bytes. Measured `Engine<CIo>` is 2312 B on a 64-bit host and 2272 B on
/// riscv32imc, so this leaves ~250 B of headroom and saves 1.5 KiB of RAM per
/// handle versus the original 512 words. The static assertions below turn any
/// future growth into a build error, so the headroom is convenience, not a
/// safety net. Exported as a C #define so the struct's array size resolves.
pub const XYMODEM_CORE_STATE_WORDS: usize = 320;

// ---------------------------------------------------------------------------
// C-facing data types
// ---------------------------------------------------------------------------

/// Opaque protocol state, owned and allocated by the caller.
///
/// Declare one as `xymodem_core_t core;` (stack or static) or allocate
/// `sizeof(xymodem_core_t)` bytes, then pass `&core` to `xymodem_core_init`.
/// The `uint64_t` backing array guarantees 8-byte alignment on every target
/// because [`Engine`] holds `u64` fields (file size); do not reinterpret or
/// copy the buffer while a transfer is in progress.
#[repr(C)]
pub struct XymodemCore {
    pub _storage: [u64; XYMODEM_CORE_STATE_WORDS],
}

/// Callback table supplied by the C caller.
///
/// Every callback is nullable so a zero-initialised struct is safe to pass:
/// reading a NULL C function pointer into a non-optional Rust `fn` would be
/// undefined behaviour *before any call happens*. `emit` is required and
/// `xymodem_core_init` rejects a table without it; the others are optional.
/// A missing `data_sink` discards received payload, a missing `data_source`
/// behaves as an exhausted source, and a missing `on_file` accepts every file.
/// The `user` pointer is passed back verbatim to every callback.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct XymodemCoreIo {
    /// Required. Push protocol bytes onto the wire (SOH/STX/ACK/NAK/CAN/EOT,
    /// packets). A call may contain one byte or a whole packet. Return 0 on
    /// success; any other value aborts with XYMODEM_ERR_IO (no EOT/ACK after).
    pub emit: Option<extern "C" fn(user: *mut c_void, data: *const u8, len: usize) -> c_int>,
    /// Data sink: hand one accepted data block's payload to the application.
    /// Classic XMODEM (no declared size) hands the whole block, padding
    /// included; a declared size trims the last block. Return 0 on success;
    /// any other value aborts without ACKing the block.
    pub data_sink: Option<extern "C" fn(user: *mut c_void, data: *const u8, len: usize) -> c_int>,
    /// Data source: pull the next payload chunk to transmit into `buf`.
    /// Write the byte count (0 = end of data, must be <= `len`) to `written`.
    /// Return 0 on success; any other value aborts without sending EOT.
    /// If NULL, the source is treated as exhausted.
    pub data_source: Option<
        extern "C" fn(user: *mut c_void, buf: *mut u8, len: usize, written: *mut usize) -> c_int,
    >,
    /// A block 0 (file header) was verified. Classic senders never emit one,
    /// so this is simply never triggered for them.
    ///
    /// Return `true` to accept the file, `false` to refuse it: the transfer is
    /// cancelled with CAN and reported as XYMODEM_ERR_FILE_REJECTED. `name`
    /// is NUL-terminated and valid only during the call; `has_size` is false
    /// for legacy headers without a length field (payload is then delivered
    /// with classic whole-block semantics). If NULL, every file is accepted.
    pub on_file: Option<
        extern "C" fn(user: *mut c_void, name: *const c_char, size: u64, has_size: bool) -> bool,
    >,
    /// Optional lifecycle notification. `event` is an XYMODEM_EV_* value; on
    /// XYMODEM_EV_FAILED, `arg` is a negative XYMODEM_ERR_* code.
    pub on_event: Option<extern "C" fn(user: *mut c_void, event: XymodemEvent, arg: XymodemErr)>,
    /// Opaque user pointer passed back to every callback.
    pub user: *mut c_void,
}

/// Static protocol tuning. Copy it, fill it, pass a pointer to
/// `xymodem_core_init`. All timeouts are in milliseconds, measured against the
/// `now_ms` passed to `on_receive`/`poll`. Zero is rejected: a timeout of 0 ms
/// is not a timeout.
///
/// There is no "protocol" field: a session is classic XMODEM plus one
/// optional block-0 file header, chosen per `start_send` call by whether
/// `file` is NULL, a named file, or an empty name (batch terminator).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct XymodemCoreConfig {
    /// XYMODEM_CHECK_AUTO / XYMODEM_CHECK_SUM8 / XYMODEM_CHECK_CRC16.
    pub check_mode: u8,
    /// true: sender emits 1K (STX) blocks instead of 128-byte (SOH) ones.
    /// A last block of at most 128 bytes is sent as SOH. The receiver takes
    /// no part in this choice: XMODEM has no wire token for "send me 1K", so
    /// a receiver accepts SOH and STX packets alike.
    pub block_1k: bool,
    /// Max retries before XYMODEM_ERR_RETRY_EXCEEDED.
    pub max_retries: u16,
    /// Receiver: line silence before re-requesting a packet (ms, > 0).
    pub packet_timeout_ms: u32,
    /// Sender: silence before XYMODEM_ERR_NO_RESPONSE (ms, > 0).
    pub response_timeout_ms: u32,
}

/// File metadata for `xymodem_core_start_send`. `name` must be NUL-terminated.
/// A non-empty name is a YMODEM file header (name + decimal size must fit a
/// 128-byte block). An empty name (`""`) is the empty block 0 that terminates
/// a YMODEM batch and carries no payload. `file == NULL` is classic XMODEM.
#[repr(C)]
pub struct XymodemFileInfo {
    pub name: *const c_char,
    pub size: u64,
}

// ---------------------------------------------------------------------------
// The concrete Io implementation: turns the callback table into `proto::Io`.
// ---------------------------------------------------------------------------

struct CIo {
    io: XymodemCoreIo,
}

impl Io for CIo {
    fn emit(&mut self, bytes: &[u8]) -> Result<(), IoError> {
        match self.io.emit {
            Some(cb) if cb(self.io.user, bytes.as_ptr(), bytes.len()) != 0 => Err(IoError),
            _ => Ok(()),
        }
    }
    fn data_sink(&mut self, data: &[u8]) -> Result<(), IoError> {
        match self.io.data_sink {
            Some(cb) if cb(self.io.user, data.as_ptr(), data.len()) != 0 => Err(IoError),
            _ => Ok(()),
        }
    }
    fn data_source(&mut self, buf: &mut [u8]) -> Result<usize, IoError> {
        let Some(cb) = self.io.data_source else {
            return Ok(0); // no source -> end of data
        };
        let mut n = 0usize;
        if cb(self.io.user, buf.as_mut_ptr(), buf.len(), &mut n) != 0 {
            return Err(IoError);
        }
        if n > buf.len() {
            return Err(IoError);
        }
        Ok(n)
    }
    fn on_file(&mut self, name: &[u8], size: Option<u64>) -> bool {
        let Some(cb) = self.io.on_file else {
            return true;
        };
        // The C callback expects a NUL-terminated name; copy into stack
        // scratch. `name` is at most one 128-byte header payload, but clamp
        // anyway: a panic here is now an abort/reboot, and the caller must
        // never be able to trigger one by handing us a longer slice.
        let mut buf = [0u8; 129];
        let n = name.len().min(128);
        buf[..n].copy_from_slice(&name[..n]);
        cb(
            self.io.user,
            buf.as_ptr() as *const c_char,
            size.unwrap_or(0),
            size.is_some(),
        )
    }
    fn on_event(&mut self, event: Event) {
        if let Some(cb) = self.io.on_event {
            let (code, arg) = match event {
                Event::Handshake => (XymodemEvent::XYMODEM_EV_HANDSHAKE, XymodemErr::XYMODEM_OK),
                Event::Block => (XymodemEvent::XYMODEM_EV_BLOCK, XymodemErr::XYMODEM_OK),
                Event::FileComplete => (
                    XymodemEvent::XYMODEM_EV_FILE_COMPLETE,
                    XymodemErr::XYMODEM_OK,
                ),
                Event::Complete => (XymodemEvent::XYMODEM_EV_COMPLETE, XymodemErr::XYMODEM_OK),
                Event::Failed(f) => (XymodemEvent::XYMODEM_EV_FAILED, failure_code(f)),
            };
            cb(self.io.user, code, arg);
        }
    }
}

/// Flatten the core's layered [`Failure`] into the single C error range.
/// The exhaustive match is deliberate: adding a Rust failure variant forces
/// a new code here at compile time.
fn failure_code(f: Failure) -> XymodemErr {
    match f {
        Failure::Wire(WireReason::CancelByPeer) => XymodemErr::XYMODEM_ERR_CANCEL_BY_PEER,
        Failure::Wire(WireReason::NoResponse) => XymodemErr::XYMODEM_ERR_NO_RESPONSE,
        Failure::Wire(WireReason::RetryExceeded) => XymodemErr::XYMODEM_ERR_RETRY_EXCEEDED,
        Failure::Wire(WireReason::OutOfSync) => XymodemErr::XYMODEM_ERR_OUT_OF_SYNC,
        Failure::Local(LocalReason::Cancelled) => XymodemErr::XYMODEM_ERR_CANCEL_BY_LOCAL,
        Failure::Local(LocalReason::FileRejected) => XymodemErr::XYMODEM_ERR_FILE_REJECTED,
        Failure::Local(LocalReason::Io) => XymodemErr::XYMODEM_ERR_IO,
    }
}

fn start_error_code(e: StartError) -> XymodemErr {
    match e {
        StartError::Busy => XymodemErr::XYMODEM_ERR_BUSY,
    }
}

fn status_code(s: Status) -> XymodemStatus {
    match s {
        Status::Idle => XymodemStatus::XYMODEM_ST_IDLE,
        Status::Handshake => XymodemStatus::XYMODEM_ST_HANDSHAKE,
        Status::Transfer => XymodemStatus::XYMODEM_ST_TRANSFER,
        Status::Complete => XymodemStatus::XYMODEM_ST_COMPLETE,
        Status::Failed(_) => XymodemStatus::XYMODEM_ST_FAILED,
    }
}

fn error_code(s: Status) -> XymodemErr {
    match s {
        Status::Failed(f) => failure_code(f),
        _ => XymodemErr::XYMODEM_OK,
    }
}

fn role_code(r: Role) -> XymodemRole {
    match r {
        Role::None => XymodemRole::XYMODEM_ROLE_NONE,
        Role::Recv => XymodemRole::XYMODEM_ROLE_RECV,
        Role::Send => XymodemRole::XYMODEM_ROLE_SEND,
    }
}

fn to_config(c: &XymodemCoreConfig) -> Option<Config> {
    // Destructure so adding a field to XymodemCoreConfig becomes a COMPILE
    // ERROR until this mapping is updated -- no silently dropped field.
    let XymodemCoreConfig {
        check_mode,
        block_1k,
        max_retries,
        packet_timeout_ms,
        response_timeout_ms,
    } = *c;
    let check_mode = match check_mode {
        x if x == XymodemCheckMode::XYMODEM_CHECK_AUTO as u8 => CheckMode::Auto,
        x if x == XymodemCheckMode::XYMODEM_CHECK_SUM8 as u8 => CheckMode::Sum8,
        x if x == XymodemCheckMode::XYMODEM_CHECK_CRC16 as u8 => CheckMode::Crc16,
        _ => return None,
    };
    Some(Config {
        check_mode,
        block_1k,
        max_retries,
        packet_timeout_ms: NonZeroU32::new(packet_timeout_ms)?,
        response_timeout_ms: NonZeroU32::new(response_timeout_ms)?,
    })
}

// Compile-time guards, evaluated for the *target* being built: the opaque
// buffer must fit the engine (it is largest on 64-bit hosts, where function
// pointers are 8 bytes) and satisfy its alignment. The backing array is
// `u64` because Engine holds `u64` file-size fields; asserting `align == 4`
// would be wrong. Catch both here rather than discovering a too-small buffer
// as a runtime error on the device.
const _: () = assert!(core::mem::align_of::<Engine<CIo>>() <= core::mem::align_of::<u64>());
const _: () = assert!(core::mem::size_of::<Engine<CIo>>() <= core::mem::size_of::<XymodemCore>());

/// Maximum bytes scanned for a block-0 file name: one 128-byte header payload
/// plus its NUL terminator.
const MAX_NAME_BYTES: usize = 129;

/// Read a NUL-terminated C name into `out`. Returns `Some(len)` (excluding the
/// NUL), or `None` if no NUL appears within [`MAX_NAME_BYTES`].
///
/// Unlike `CStr::from_ptr`, this reads only up to the NUL for a well-formed
/// short string and is bounded even when the NUL is missing: a malformed
/// pointer is rejected after 129 bytes instead of being scanned arbitrarily
/// far (which is exactly the UB that `CStr::from_ptr` cannot prevent).
///
/// # Safety
/// `ptr` must be non-null and point to a readable NUL-terminated C string
/// whose terminator lies within `MAX_NAME_BYTES` bytes.
unsafe fn read_name(ptr: *const c_char, out: &mut [u8; MAX_NAME_BYTES]) -> Option<usize> {
    let p = ptr.cast::<u8>();
    let mut n = 0;
    while n < MAX_NAME_BYTES {
        // SAFETY: the caller guarantees bytes up to the NUL are readable and
        // `n` is capped by MAX_NAME_BYTES, so a missing NUL cannot run away.
        let b = unsafe { *p.add(n) };
        if b == 0 {
            return Some(n);
        }
        out[n] = b;
        n += 1;
    }
    None
}

/// Cast the opaque caller buffer to the live engine. The caller guarantees
/// `core` came from `xymodem_core_init` and stays valid until teardown.
///
/// These helpers return RAW POINTERS on purpose. Forming a reference here
/// would give it an unconstrained `'a` and hide the unsafe deref behind a
/// safe fn; each exported function instead forms its own short-lived
/// reference with an explicit SAFETY comment.
#[inline]
fn engine_ptr(core: *mut XymodemCore) -> Option<*mut Engine<CIo>> {
    if core.is_null() {
        None
    } else {
        Some(core.cast::<Engine<CIo>>())
    }
}

#[inline]
fn engine_ptr_const(core: *const XymodemCore) -> Option<*const Engine<CIo>> {
    if core.is_null() {
        None
    } else {
        Some(core.cast::<Engine<CIo>>())
    }
}

// ---------------------------------------------------------------------------
// Exported C API
// ---------------------------------------------------------------------------

/// Return `sizeof(xymodem_core_t)` in bytes. Useful when allocating the state
/// dynamically; a stack/static `xymodem_core_t` needs none of this.
#[no_mangle]
pub extern "C" fn xymodem_core_state_size() -> usize {
    core::mem::size_of::<XymodemCore>()
}

/// Initialize an idle engine. No session runs yet: nothing is emitted, no
/// timer is armed, received bytes are ignored. Call
/// `xymodem_core_start_recv()` or `xymodem_core_start_send()` to open one.
///
/// Returns XYMODEM_OK, or XYMODEM_ERR_INVALID_ARG on a NULL pointer, an
/// invalid config, or a callback table without `emit`. The buffer must be at
/// least `xymodem_core_state_size()` bytes and 8-byte aligned; it is left
/// untouched on error.
///
/// # Safety
/// `core` must point to writable, 8-aligned memory of `sizeof(xymodem_core_t)`
/// bytes. `config` and `io` must point to valid structs. All pointers are used
/// only for the duration of this call except `io`, whose contents are copied.
#[no_mangle]
pub unsafe extern "C" fn xymodem_core_init(
    core: *mut XymodemCore,
    config: *const XymodemCoreConfig,
    io: *const XymodemCoreIo,
) -> XymodemErr {
    if core.is_null() || config.is_null() || io.is_null() {
        return XymodemErr::XYMODEM_ERR_INVALID_ARG;
    }
    // SAFETY: caller guarantees valid, aligned pointers.
    let (cfg, c_io) = unsafe { (&*config, &*io) };
    let Some(proto_cfg) = to_config(cfg) else {
        return XymodemErr::XYMODEM_ERR_INVALID_ARG;
    };
    // `emit` is the one callback the engine cannot work without; every other
    // callback has a defined no-op/EOF behaviour when NULL. Validating here
    // turns a zero-initialised table into a clean error instead of silently
    // swallowing all protocol output.
    if c_io.emit.is_none() {
        return XymodemErr::XYMODEM_ERR_INVALID_ARG;
    }
    // A 32-bit `size_t` data_source returning a value > len is caught by the
    // engine as an I/O error; no extra validation is needed here. The opaque
    // buffer's size/alignment are guaranteed by the static assertions above.

    let state = Engine::new(CIo { io: *c_io }, proto_cfg);
    // SAFETY: `core` is a valid, aligned, writable slot of at least
    // sizeof(Engine<CIo>) bytes (guaranteed by the static assertions).
    // XymodemCore has no Drop, so overwriting it leaks nothing.
    unsafe { core::ptr::write(core.cast::<Engine<CIo>>(), state) };
    XymodemErr::XYMODEM_OK
}

/// Begin a receive session on an initialized core. Resets the state machine,
/// the block counter and the negotiated check mode, then emits the handshake
/// request right away. A sender with a block 0 is recognised automatically;
/// a classic header-less sender is detected from its first data block.
///
/// `now_ms` is the caller's monotonic millisecond clock; timeouts are armed
/// from this instant, not from zero.
///
/// Returns XYMODEM_OK, XYMODEM_ERR_INVALID_ARG for a NULL pointer, or
/// XYMODEM_ERR_BUSY if a session is still in flight (which is no longer
/// silently abandoned).
///
/// # Safety
/// `core` must have been initialized by `xymodem_core_init`.
#[no_mangle]
pub unsafe extern "C" fn xymodem_core_start_recv(
    core: *mut XymodemCore,
    now_ms: u32,
) -> XymodemErr {
    let Some(p) = engine_ptr(core) else {
        return XymodemErr::XYMODEM_ERR_INVALID_ARG;
    };
    // SAFETY: `core` was initialized by xymodem_core_init and is non-null;
    // the reference lives only for this call.
    let e = unsafe { &mut *p };
    match e.start_recv(now_ms) {
        Ok(()) => XymodemErr::XYMODEM_OK,
        Err(err) => start_error_code(err),
    }
}

/// Begin a send session. `file` selects what goes on the wire before payload:
///
/// - `NULL`: classic XMODEM, header-less.
/// - non-NULL with a non-empty name: block 0 carries `file->name` / `file->size`
///   (YMODEM). The name must be NUL-terminated and fit the 128-byte header
///   together with the decimal size.
/// - non-NULL with an empty name (`""`): the empty block 0 that terminates a
///   YMODEM batch, with no payload. That empty pathname is the YMODEM wire
///   token, not a third protocol mode.
///
/// A batch is simply several named `start_send` calls followed by one
/// empty-name call. The name is bounded-scanned and copied during this call.
///
/// Returns XYMODEM_OK, XYMODEM_ERR_INVALID_ARG for a NULL core / a name
/// without a NUL inside the bound / a name that does not fit, or
/// XYMODEM_ERR_BUSY if a session is still in flight.
///
/// # Safety
/// `core` must have been initialized by `xymodem_core_init`; when `file` is
/// non-NULL, `file->name` must point to a readable NUL-terminated string valid
/// for the duration of this call (the engine copies it).
#[no_mangle]
pub unsafe extern "C" fn xymodem_core_start_send(
    core: *mut XymodemCore,
    file: *const XymodemFileInfo,
    now_ms: u32,
) -> XymodemErr {
    let Some(p) = engine_ptr(core) else {
        return XymodemErr::XYMODEM_ERR_INVALID_ARG;
    };
    // SAFETY: `core` was initialized by xymodem_core_init and is non-null.
    let e = unsafe { &mut *p };
    let mut name_buf = [0u8; MAX_NAME_BYTES];
    let session = if file.is_null() {
        SessionKind::Classic
    } else {
        // SAFETY: caller guarantees a valid XymodemFileInfo when non-NULL.
        let f = unsafe { &*file };
        if f.name.is_null() {
            return XymodemErr::XYMODEM_ERR_INVALID_ARG;
        }
        // SAFETY: f.name is non-null and points to a readable NUL-terminated
        // C string; read_name stops at the NUL and is capped at MAX_NAME_BYTES.
        let Some(n) = (unsafe { read_name(f.name, &mut name_buf) }) else {
            return XymodemErr::XYMODEM_ERR_INVALID_ARG;
        };
        if n == 0 {
            SessionKind::BatchEnd
        } else {
            let Ok(meta) = FileMeta::new(&name_buf[..n], f.size) else {
                return XymodemErr::XYMODEM_ERR_INVALID_ARG;
            };
            SessionKind::File(meta)
        }
    };
    match e.start_send(session, now_ms) {
        Ok(()) => XymodemErr::XYMODEM_OK,
        Err(err) => start_error_code(err),
    }
}

/// Feed bytes received from the hardware into the state machine. Any slice is
/// valid: partial packets, several packets, or control bytes mixed together.
/// Call this from the driver's RX event / FIFO drain, not from a blocking read.
/// `now_ms` is the caller's monotonic millisecond clock.
///
/// Returns XYMODEM_OK, or XYMODEM_ERR_INVALID_ARG for a NULL pointer. Transfer
/// outcomes are reported asynchronously via status/error/on_event, not here.
///
/// # Safety
/// `core` must be initialized. `data` must point to `len` readable bytes (a
/// NULL `data` with `len == 0` is allowed).
#[no_mangle]
pub unsafe extern "C" fn xymodem_core_on_receive(
    core: *mut XymodemCore,
    data: *const u8,
    len: usize,
    now_ms: u32,
) -> XymodemErr {
    let Some(p) = engine_ptr(core) else {
        return XymodemErr::XYMODEM_ERR_INVALID_ARG;
    };
    // An empty chunk is a no-op; time advances only through xymodem_core_poll.
    if len == 0 {
        return XymodemErr::XYMODEM_OK;
    }
    if data.is_null() {
        return XymodemErr::XYMODEM_ERR_INVALID_ARG;
    }
    // SAFETY: `core` was initialized and is non-null; `data` points to `len`
    // readable bytes. The reference lives only for this call.
    let e = unsafe { &mut *p };
    // SAFETY: caller guarantees `len` readable bytes at `data`.
    let bytes = unsafe { core::slice::from_raw_parts(data, len) };
    e.on_receive(bytes, now_ms);
    XymodemErr::XYMODEM_OK
}

/// Drive the state machine (timeouts, retransmits). Call regularly from a task,
/// e.g. every 1--10 ms, with a monotonically increasing `now_ms`.
///
/// Returns XYMODEM_OK or XYMODEM_ERR_INVALID_ARG.
///
/// # Safety
/// `core` must be initialized.
#[no_mangle]
pub unsafe extern "C" fn xymodem_core_poll(core: *mut XymodemCore, now_ms: u32) -> XymodemErr {
    let Some(p) = engine_ptr(core) else {
        return XymodemErr::XYMODEM_ERR_INVALID_ARG;
    };
    // SAFETY: `core` was initialized and is non-null.
    let e = unsafe { &mut *p };
    e.poll(now_ms);
    XymodemErr::XYMODEM_OK
}

/// Current transfer status: one of the XYMODEM_ST_* values. Returns
/// XYMODEM_ST_INVALID for a NULL pointer (a status, not an error code).
///
/// # Safety
/// `core` must be initialized.
#[no_mangle]
pub unsafe extern "C" fn xymodem_core_status(core: *const XymodemCore) -> XymodemStatus {
    let Some(p) = engine_ptr_const(core) else {
        return XymodemStatus::XYMODEM_ST_INVALID;
    };
    // SAFETY: `core` was initialized and is non-null; shared, read-only use.
    let e = unsafe { &*p };
    status_code(e.status())
}

/// Current role: XYMODEM_ROLE_RECV, XYMODEM_ROLE_SEND, or XYMODEM_ROLE_NONE
/// when idle / terminal / NULL.
///
/// # Safety
/// `core` must be initialized.
#[no_mangle]
pub unsafe extern "C" fn xymodem_core_role(core: *const XymodemCore) -> XymodemRole {
    let Some(p) = engine_ptr_const(core) else {
        return XymodemRole::XYMODEM_ROLE_NONE;
    };
    // SAFETY: `core` was initialized and is non-null; shared, read-only use.
    let e = unsafe { &*p };
    role_code(e.role())
}

/// The failure reason (a negative XYMODEM_ERR_* code) when status is
/// XYMODEM_ST_FAILED; XYMODEM_OK otherwise. Returns XYMODEM_ERR_INVALID_ARG
/// for a NULL pointer.
///
/// # Safety
/// `core` must be initialized.
#[no_mangle]
pub unsafe extern "C" fn xymodem_core_error(core: *const XymodemCore) -> XymodemErr {
    let Some(p) = engine_ptr_const(core) else {
        return XymodemErr::XYMODEM_ERR_INVALID_ARG;
    };
    // SAFETY: `core` was initialized and is non-null; shared, read-only use.
    let e = unsafe { &*p };
    error_code(e.status())
}

/// Abort the transfer from the application side. Emits CAN bytes and reports
/// XYMODEM_ERR_CANCEL_BY_LOCAL through on_event / xymodem_core_error. Safe to
/// call once the transfer is already complete/failed: it is a no-op then.
///
/// Returns XYMODEM_OK, or XYMODEM_ERR_INVALID_ARG for a NULL pointer (same
/// convention as every other entry point).
///
/// # Safety
/// `core` must be initialized.
#[no_mangle]
pub unsafe extern "C" fn xymodem_core_cancel(core: *mut XymodemCore) -> XymodemErr {
    let Some(p) = engine_ptr(core) else {
        return XymodemErr::XYMODEM_ERR_INVALID_ARG;
    };
    // SAFETY: `core` was initialized and is non-null.
    let e = unsafe { &mut *p };
    e.cancel();
    XymodemErr::XYMODEM_OK
}

/// Report that an I/O callback (emit/data_sink/data_source) failed. Aborts the
/// transfer with XYMODEM_ERR_IO, emitting CAN and reporting the SAME code
/// through on_event and xymodem_core_error. A no-op when idle or already
/// failed.
///
/// For C embedders of this ABI that detect a late failure *outside* a
/// callback. The IDF wrapper does not call this: fallible `Io` already
/// aborts inside emit / data_sink / data_source.
///
/// Returns XYMODEM_OK, or XYMODEM_ERR_INVALID_ARG for a NULL pointer.
///
/// # Safety
/// `core` must be initialized.
#[no_mangle]
pub unsafe extern "C" fn xymodem_core_fail_io(core: *mut XymodemCore) -> XymodemErr {
    let Some(p) = engine_ptr(core) else {
        return XymodemErr::XYMODEM_ERR_INVALID_ARG;
    };
    // SAFETY: `core` was initialized and is non-null.
    let e = unsafe { &mut *p };
    e.fail_io();
    XymodemErr::XYMODEM_OK
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::ffi::c_void;
    use core::ptr;

    extern "C" fn emit_ok(_user: *mut c_void, _data: *const u8, _len: usize) -> c_int {
        0
    }

    fn default_cfg() -> XymodemCoreConfig {
        XymodemCoreConfig {
            check_mode: XymodemCheckMode::XYMODEM_CHECK_AUTO as u8,
            block_1k: false,
            max_retries: 10,
            packet_timeout_ms: 3000,
            response_timeout_ms: 30000,
        }
    }

    fn io_with(user: *mut c_void) -> XymodemCoreIo {
        XymodemCoreIo {
            emit: Some(emit_ok),
            data_sink: None,
            data_source: None,
            on_file: None,
            on_event: None,
            user,
        }
    }

    fn init_ok(core: &mut XymodemCore, io: &XymodemCoreIo) {
        let cfg = default_cfg();
        // SAFETY: stack buffers live for the call; io is copied.
        let rc = unsafe { xymodem_core_init(core, &cfg, io) };
        assert_eq!(rc, XymodemErr::XYMODEM_OK);
    }

    #[test]
    fn null_pointers_are_invalid() {
        // SAFETY: every pointer is null; the FFI must reject them without deref.
        unsafe {
            assert_eq!(
                xymodem_core_init(ptr::null_mut(), ptr::null(), ptr::null()),
                XymodemErr::XYMODEM_ERR_INVALID_ARG
            );
            assert_eq!(
                xymodem_core_start_recv(ptr::null_mut(), 0),
                XymodemErr::XYMODEM_ERR_INVALID_ARG
            );
            assert_eq!(
                xymodem_core_start_send(ptr::null_mut(), ptr::null(), 0),
                XymodemErr::XYMODEM_ERR_INVALID_ARG
            );
            assert_eq!(
                xymodem_core_status(ptr::null()),
                XymodemStatus::XYMODEM_ST_INVALID
            );
            assert_eq!(
                xymodem_core_role(ptr::null()),
                XymodemRole::XYMODEM_ROLE_NONE
            );
            assert_eq!(
                xymodem_core_error(ptr::null()),
                XymodemErr::XYMODEM_ERR_INVALID_ARG
            );
        }
    }

    #[test]
    fn zero_timeout_is_rejected() {
        let mut core = XymodemCore {
            _storage: [0; XYMODEM_CORE_STATE_WORDS],
        };
        let io = io_with(ptr::null_mut());
        let mut cfg = default_cfg();
        cfg.packet_timeout_ms = 0;
        // SAFETY: `core` is a stack buffer; cfg/io live for the call.
        unsafe {
            assert_eq!(
                xymodem_core_init(&mut core, &cfg, &io),
                XymodemErr::XYMODEM_ERR_INVALID_ARG
            );
            cfg.packet_timeout_ms = 3000;
            cfg.response_timeout_ms = 0;
            assert_eq!(
                xymodem_core_init(&mut core, &cfg, &io),
                XymodemErr::XYMODEM_ERR_INVALID_ARG
            );
        }
    }

    #[test]
    fn start_send_infers_header_from_file() {
        let mut core = XymodemCore {
            _storage: [0; XYMODEM_CORE_STATE_WORDS],
        };
        let io = io_with(ptr::null_mut());
        init_ok(&mut core, &io);

        // SAFETY: `core` was initialized; file names are C string literals.
        unsafe {
            assert_eq!(xymodem_core_status(&core), XymodemStatus::XYMODEM_ST_IDLE);
            assert_eq!(xymodem_core_role(&core), XymodemRole::XYMODEM_ROLE_NONE);

            assert_eq!(
                xymodem_core_start_send(&mut core, ptr::null(), 0),
                XymodemErr::XYMODEM_OK
            );
            assert_eq!(xymodem_core_role(&core), XymodemRole::XYMODEM_ROLE_SEND);
            assert_eq!(
                xymodem_core_status(&core),
                XymodemStatus::XYMODEM_ST_HANDSHAKE
            );
            assert_eq!(
                xymodem_core_start_send(&mut core, ptr::null(), 0),
                XymodemErr::XYMODEM_ERR_BUSY
            );

            xymodem_core_cancel(&mut core);
            assert_eq!(xymodem_core_role(&core), XymodemRole::XYMODEM_ROLE_NONE);

            let name = c"a.bin";
            let file = XymodemFileInfo {
                name: name.as_ptr(),
                size: 1,
            };
            assert_eq!(
                xymodem_core_start_send(&mut core, &file, 0),
                XymodemErr::XYMODEM_OK
            );
            xymodem_core_cancel(&mut core);

            let empty = XymodemFileInfo {
                name: c"".as_ptr(),
                size: 0,
            };
            assert_eq!(
                xymodem_core_start_send(&mut core, &empty, 0),
                XymodemErr::XYMODEM_OK
            );
        }
    }

    #[test]
    fn start_recv_sets_role() {
        let mut core = XymodemCore {
            _storage: [0; XYMODEM_CORE_STATE_WORDS],
        };
        let io = io_with(ptr::null_mut());
        init_ok(&mut core, &io);

        // SAFETY: `core` was initialized.
        unsafe {
            assert_eq!(
                xymodem_core_start_recv(&mut core, 0),
                XymodemErr::XYMODEM_OK
            );
            assert_eq!(xymodem_core_role(&core), XymodemRole::XYMODEM_ROLE_RECV);
        }
    }
}
