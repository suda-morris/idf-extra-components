// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
//! Shared scaffolding for the crate's unit tests.
//!
//! Only compiled under `cfg(test)`: the library itself needs std-free code.

extern crate std;
use std::string::ToString;
use std::vec;
use std::vec::Vec;

use crate::frame::{self, packet_len, Check, Frame, Framer, BLOCK_1024, BLOCK_128, SOH, STX, SUB};
use crate::io::{Event, Io, IoError};

/// Mirror of an owned frame, so a test can inspect frames after the framer's
/// internal buffer has been reused.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum OwnedFrame {
    Packet(Vec<u8>),
    Control(u8),
}

/// Push a whole byte stream through a framer and copy every frame out.
pub fn own_frames(framer: &mut Framer, stream: &[u8]) -> Vec<OwnedFrame> {
    let mut out = Vec::new();
    framer.push(stream, |frame| match frame {
        Frame::Packet(p) => out.push(OwnedFrame::Packet(p.to_vec())),
        Frame::Control(c) => out.push(OwnedFrame::Control(c)),
    });
    out
}

/// Build a complete, valid packet the way a sender would.
pub fn make_packet(blk: u8, payload: &[u8], check: Check, stx: bool) -> Vec<u8> {
    let block = if stx { BLOCK_1024 } else { BLOCK_128 };
    let mut p = vec![if stx { STX } else { SOH }, blk, !blk];
    p.extend_from_slice(payload);
    p.resize(3 + block, SUB);
    match check {
        Check::Sum8 => p.push(frame::sum8(&p[3..3 + block])),
        Check::Crc16 => {
            let crc = frame::crc16(&p[3..3 + block]);
            p.push((crc >> 8) as u8);
            p.push(crc as u8);
        }
    }
    assert_eq!(p.len(), packet_len(p[0], check));
    p
}

/// Build a YMODEM header block (block 0) the way a sender would: SOH 00 FF
/// <name> 00 [<decimal size>] 00, zero padded, CRC16. `None` omits the size
/// (legacy header).
pub fn make_header(name: &[u8], size: Option<u64>) -> Vec<u8> {
    let mut p = vec![SOH, 0, 0xFF];
    p.extend_from_slice(name);
    p.push(0);
    if let Some(size) = size {
        p.extend_from_slice(size.to_string().as_bytes());
    }
    p.push(0);
    p.resize(3 + BLOCK_128, 0);
    let crc = frame::crc16(&p[3..3 + BLOCK_128]);
    p.push((crc >> 8) as u8);
    p.push(crc as u8);
    p
}

/// Strip XMODEM SUB padding (the engine delivers the whole block, padding
/// included -- it cannot tell payload from padding).
pub fn unpadded(mut v: Vec<u8>) -> Vec<u8> {
    while v.last() == Some(&SUB) {
        v.pop();
    }
    v
}

/// A tiny in-memory [`Io`]: records everything the engine emits/delivers,
/// serves a fixed source, and lets a test inspect events.
#[derive(Default)]
pub struct MemIo {
    pub emitted: Vec<u8>,
    pub sinked: Vec<u8>,
    pub source: Vec<u8>,
    pub source_pos: usize,
    pub events: Vec<Event>,
    /// If true, `data_source` fails, which the engine must reject with
    /// `Failure::Local(LocalReason::Io)` without sending EOT.
    pub broken_source: bool,
    /// If true, `emit` fails (engine aborts with `LocalReason::Io`).
    pub fail_emit: bool,
    /// If true, `data_sink` fails.
    pub fail_sink: bool,
    /// YMODEM: every header seen via `on_file`, in order.
    pub files: Vec<(Vec<u8>, Option<u64>)>,
    /// If true, `on_file` rejects every file (the engine must cancel with CAN).
    pub reject_files: bool,
}

impl MemIo {
    pub fn new(source: &[u8]) -> Self {
        Self {
            source: source.to_vec(),
            ..Default::default()
        }
    }

    /// Take all bytes emitted so far (drains).
    pub fn take(&mut self) -> Vec<u8> {
        core::mem::take(&mut self.emitted)
    }
}

impl Io for MemIo {
    fn emit(&mut self, bytes: &[u8]) -> Result<(), IoError> {
        if self.fail_emit {
            return Err(IoError);
        }
        self.emitted.extend_from_slice(bytes);
        Ok(())
    }
    fn data_sink(&mut self, data: &[u8]) -> Result<(), IoError> {
        if self.fail_sink {
            return Err(IoError);
        }
        self.sinked.extend_from_slice(data);
        Ok(())
    }
    fn data_source(&mut self, buf: &mut [u8]) -> Result<usize, IoError> {
        if self.broken_source {
            return Err(IoError);
        }
        let n = (self.source.len() - self.source_pos).min(buf.len());
        buf[..n].copy_from_slice(&self.source[self.source_pos..self.source_pos + n]);
        self.source_pos += n;
        Ok(n)
    }
    fn on_event(&mut self, event: Event) {
        self.events.push(event)
    }
    fn on_file(&mut self, name: &[u8], size: Option<u64>) -> bool {
        self.files.push((name.to_vec(), size));
        !self.reject_files
    }
}
