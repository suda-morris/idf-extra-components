// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
//! The sans-IO boundary.
//!
//! The protocol engine never touches hardware. Every side effect is expressed
//! as a call on [`Io`], which the embedding layer implements by pushing bytes
//! to a UART, delivering payload to the application, pulling from a data
//! source, or reporting lifecycle events.
//!
//! Keeping the engine free of I/O is what makes the whole state machine unit
//! testable on a host: a test implements `Io` with a few `Vec`s and a fake clock.

/// A side effect on [`Io`] did not happen (UART write failed, storage full,
/// source over-reported its length, ...).
///
/// The engine maps this to [`Failure::Local(LocalReason::Io)`] and aborts
/// immediately: it will not emit a further protocol frame (no ACK, no EOT)
/// except a best-effort CAN, whose own I/O error is ignored to avoid
/// recursion.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct IoError;

/// Everything the protocol engine needs from the outside world.
///
/// An implementation must be cheap and non-blocking: the engine calls these
/// from `on_receive()` / `poll()`, which run in the caller's task context.
/// Implementations must not re-enter the engine from a callback.
pub trait Io {
    /// Push protocol bytes onto the wire (handshake, ACK/NAK/CAN, EOT, packets).
    /// A call may contain one byte or a whole packet.
    ///
    /// Return `Err` to abort the transfer at once. The engine will not send
    /// EOT or ACK after a failed emit.
    fn emit(&mut self, bytes: &[u8]) -> Result<(), IoError>;

    /// Data sink: hand each accepted data block's payload to the application.
    ///
    /// Classic XMODEM (no declared size) cannot distinguish payload from
    /// trailing SUB padding, so the whole block is handed over. When the
    /// peer declared a file size in block 0, the last block is trimmed to
    /// that size and padding is not delivered.
    ///
    /// Return `Err` to abort without ACKing the block, so the peer can retry.
    fn data_sink(&mut self, data: &[u8]) -> Result<(), IoError>;

    /// Data source: pull the next chunk of payload to transmit. Return the
    /// number of bytes written into `buf`; `0` signals end of data. A short
    /// read is allowed and means "this is the last block". Return `Err` to
    /// abort without sending EOT.
    fn data_source(&mut self, buf: &mut [u8]) -> Result<usize, IoError>;

    /// A block 0 (file header) was verified. Classic senders never emit one,
    /// so this is simply never triggered for them.
    ///
    /// Return `true` to accept the file, `false` to refuse it: the engine then
    /// cancels with CAN and reports `Failure::Local(LocalReason::FileRejected)`.
    /// Note this is a deliberate application decision, NOT a broken callback.
    ///
    /// `size` is `None` for legacy headers without a length field, in which
    /// case payload is delivered with classic whole-block semantics. Cheap and
    /// non-blocking.
    fn on_file(&mut self, name: &[u8], size: Option<u64>) -> bool {
        let _ = (name, size);
        true
    }

    /// Lifecycle notification. May be ignored by implementations that poll
    /// `status()` instead.
    fn on_event(&mut self, event: Event);
}

/// Protocol lifecycle events delivered through [`Io::on_event`].
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Event {
    /// The handshake completed and the negotiated check mode is known.
    Handshake,
    /// A data block was accepted (receiver) or acknowledged (sender).
    Block,
    /// A YMODEM file ended (EOT after a block-0 header). The session stays
    /// open for the next header or the empty terminator; [`Event::Complete`]
    /// still marks the whole batch. Classic header-less transfers skip this
    /// and go straight to `Complete`.
    FileComplete,
    /// The transfer finished successfully.
    Complete,
    /// The transfer failed; see `status()` for the [`Failure`].
    Failed(Failure),
}

/// Why a transfer ended, split by **who is responsible**.
///
/// A flat error enum cannot express the one distinction the state machine
/// actually cares about: a wire-level failure (peer behaviour, timeouts,
/// retries) versus a local one (the application or its `Io` implementation).
/// Keeping them separate means, for example, that a local cancel can never be
/// mistaken for a peer cancel -- and therefore can never accidentally suppress
/// the CAN bytes it has to send.
///
/// The C ABI deliberately flattens this back into one `XYMODEM_ERR_*` range:
/// C callers want a single `int` to switch on, not a tagged union.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Failure {
    /// The protocol/peer side ended the transfer.
    Wire(WireReason),
    /// The local application or its [`Io`] implementation ended it.
    Local(LocalReason),
}

impl Failure {
    /// Whether the peer already put two CAN bytes on the wire.
    ///
    /// This is the *only* case where the engine must not echo CAN back (doing
    /// so would start a CAN storm). Every other failure emits CAN. Deriving
    /// the rule from the error type, rather than comparing a variant inside
    /// `abort()`, is what makes the local/peer cancel distinction enforceable.
    pub const fn peer_already_cancelled(self) -> bool {
        matches!(self, Failure::Wire(WireReason::CancelByPeer))
    }
}

/// Failures attributable to the wire / the peer / protocol timing.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum WireReason {
    /// Peer sent two consecutive CAN bytes.
    CancelByPeer,
    /// No valid response before the configured timeout.
    NoResponse,
    /// Retry counter exhausted.
    RetryExceeded,
    /// Block number was neither expected nor previous.
    OutOfSync,
}

/// Failures attributable to this side: the application or its [`Io`].
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LocalReason {
    /// The application called [`crate::modem::Engine::cancel`]. Emits CAN so
    /// the peer stops sending.
    Cancelled,
    /// `Io::on_file` refused a block-0 file header.
    FileRejected,
    /// An `Io` callback failed (emit/sink/source returned an error, or the
    /// source over-reported its length).
    Io,
}
