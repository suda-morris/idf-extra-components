// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
//! Wire constants, checksums and a streaming framer.
//!
//! XMODEM travels over a UART: an *unframed byte stream*. The application
//! may hand us half a packet, several packets, or control bytes mixed with
//! packet bytes in one call. [`Framer`] reassembles that stream into complete
//! [`Frame`]s; the state machine in `modem.rs` never sees a partial packet.

/// Start of a 128-byte data block.
pub const SOH: u8 = 0x01;
/// Start of a 1024-byte data block (XMODEM-1K).
pub const STX: u8 = 0x02;
/// End of transmission.
pub const EOT: u8 = 0x04;
/// Acknowledge.
pub const ACK: u8 = 0x06;
/// Negative acknowledge / request checksum mode.
pub const NAK: u8 = 0x15;
/// Cancel (two consecutive bytes cancel a session).
pub const CAN: u8 = 0x18;
/// Padding byte for short data blocks.
pub const SUB: u8 = 0x1A;
/// Receiver request for CRC-16 mode ('C').
pub const CRC: u8 = 0x43;

pub const BLOCK_128: usize = 128;
pub const BLOCK_1024: usize = 1024;
/// Longest frame on the wire: header(3) + 1K payload + CRC16(2).
pub const MAX_FRAME_LEN: usize = 3 + BLOCK_1024 + 2;
const _: () = assert!(packet_len(STX, Check::Crc16) == MAX_FRAME_LEN);
const _: () = assert!(packet_len(STX, Check::Sum8) <= MAX_FRAME_LEN);
const _: () = assert!(packet_len(SOH, Check::Crc16) <= MAX_FRAME_LEN);

/// Size of a complete packet for a given start byte and check mode.
pub const fn packet_len(start: u8, check: Check) -> usize {
    3 + block_len(start) + check.trailer_len()
}

/// Payload length implied by a packet's start byte.
pub const fn block_len(start: u8) -> usize {
    if start == STX {
        BLOCK_1024
    } else {
        BLOCK_128
    }
}

/// Packet integrity mode, negotiated during the handshake.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Check {
    /// 8-bit arithmetic sum modulo 256 (classic XMODEM).
    Sum8,
    /// CRC-16/XMODEM, big-endian on the wire.
    Crc16,
}

impl Check {
    pub const fn trailer_len(self) -> usize {
        match self {
            Check::Sum8 => 1,
            Check::Crc16 => 2,
        }
    }

    /// The byte a receiver sends to request this mode during handshake.
    pub const fn request_byte(self) -> u8 {
        match self {
            Check::Sum8 => NAK,
            Check::Crc16 => CRC,
        }
    }
}

/// CRC-16/XMODEM: poly 0x1021, init 0, no reflection, no final xor.
/// This is the same algorithm `esp_crc16_be(~0, ..)` and the Python `xmodem`
/// package use, so CRC-mode transfers interoperate with both.
pub fn crc16(data: &[u8]) -> u16 {
    let mut crc: u16 = 0;
    for &b in data {
        crc ^= (b as u16) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 {
                (crc << 1) ^ 0x1021
            } else {
                crc << 1
            };
        }
    }
    crc
}

/// 8-bit checksum: the sum of the payload bytes modulo 256.
pub fn sum8(data: &[u8]) -> u8 {
    data.iter().fold(0u8, |acc, &b| acc.wrapping_add(b))
}

/// Verify a complete packet's integrity.
///
/// `packet` starts at the SOH/STX byte and includes the trailer. For CRC mode
/// the standard trick is used: CRC over payload + the two transmitted CRC bytes
/// must be zero, so the sender can append the CRC big-endian without inverting.
pub fn verify_packet(packet: &[u8], check: Check) -> bool {
    let Some(&start) = packet.first() else {
        return false;
    };
    if start != SOH && start != STX {
        return false;
    }
    let expected = packet_len(start, check);
    if packet.len() < expected {
        return false;
    }
    let packet = &packet[..expected];

    // Block number and its complement must agree.
    if packet[1] ^ packet[2] != 0xFF {
        return false;
    }

    let payload_end = 3 + block_len(start);
    let payload = &packet[3..payload_end];
    let trailer = &packet[payload_end..expected];
    match check {
        Check::Sum8 => sum8(payload) == trailer[0],
        Check::Crc16 => {
            let got = ((trailer[0] as u16) << 8) | trailer[1] as u16;
            crc16(payload) == got
        }
    }
}

/// A fully reassembled unit from the byte stream.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Frame<'a> {
    /// A complete data packet (including its trailer).
    Packet(&'a [u8]),
    /// One of the single-byte control tokens: SOH/STX-openers are handled
    /// inside the framer, so only these escape to the state machine.
    Control(u8),
}

/// Reassembles an unframed byte stream into packets and control bytes.
///
/// The framer owns a fixed [`MAX_FRAME_LEN`] buffer, so it needs no allocator.
/// It is deliberately independent of the protocol state machine: it only knows
/// how many bytes a packet starting with SOH/STX/CRC-mode is supposed to have.
pub struct Framer {
    buf: [u8; MAX_FRAME_LEN],
    len: usize,
    /// Expected total length of the packet currently being assembled (0 = none).
    expected: usize,
    /// Check mode for the current transfer (controls the trailer length).
    check: Check,
    /// A lone CAN is treated as line noise unless a second CAN follows; this
    /// records that we are waiting for the second one.
    pending_can: bool,
}

impl Framer {
    pub const fn new(check: Check) -> Self {
        Self {
            buf: [0; MAX_FRAME_LEN],
            len: 0,
            expected: 0,
            check,
            pending_can: false,
        }
    }

    /// Switch/check mode (used when a receiver falls back from CRC to SUM8).
    pub fn set_check(&mut self, check: Check) {
        self.check = check;
        // A half-assembled packet under the old mode is meaningless now.
        self.reset_packet();
    }

    /// Drop any partially accumulated packet (e.g. after an error/purge).
    pub fn reset_packet(&mut self) {
        self.len = 0;
        self.expected = 0;
        self.pending_can = false;
    }

    /// Feed a chunk of the byte stream. `f` is called once per complete frame,
    /// in wire order. Returns the number of frames delivered.
    ///
    /// The callback receives a `Frame::Packet` borrowing the framer's internal
    /// buffer; the state machine must consume it before the next `push` call.
    ///
    /// No error type is needed: `expected` is always `packet_len(...)` which is
    /// at most [`MAX_FRAME_LEN`] == `buf.len()`, so the buffer can never overflow.
    pub fn push<F>(&mut self, data: &[u8], mut f: F) -> usize
    where
        F: FnMut(Frame<'_>),
    {
        let mut delivered = 0;
        for &b in data {
            self.push_one(b, &mut f, &mut delivered);
        }
        delivered
    }

    fn push_one<F>(&mut self, b: u8, f: &mut F, delivered: &mut usize)
    where
        F: FnMut(Frame<'_>),
    {
        // --- We are in the middle of a packet: accumulate until `expected`. ---
        if self.expected != 0 {
            // `expected <= MAX_FRAME_LEN` by construction; this only guards a
            // future change to packet_len()/MAX_FRAME_LEN.
            debug_assert!(self.len < self.buf.len());
            self.buf[self.len] = b;
            self.len += 1;
            if self.len == self.expected {
                let packet = &self.buf[..self.len];
                f(Frame::Packet(packet));
                *delivered += 1;
                self.reset_packet();
            }
            return;
        }

        // --- Idle: interpret a control byte or start a packet. ---
        if self.pending_can {
            if b == CAN {
                self.pending_can = false;
                f(Frame::Control(CAN));
                *delivered += 1;
                // Two CANs with only line-noise between them: cancel.
                return;
            }
            if matches!(b, SOH | STX | EOT | ACK | NAK | CRC) {
                // A real frame started; the first CAN was noise.
                self.pending_can = false;
            } else {
                // Junk between CANs (UART noise). Keep waiting for the second
                // CAN so "CAN, 0x00, CAN" still cancels.
                return;
            }
        }

        match b {
            SOH | STX => {
                self.buf[0] = b;
                self.len = 1;
                self.expected = packet_len(b, self.check);
            }
            CAN => {
                // Hold it: a second CAN turns it into a cancel, otherwise noise.
                self.pending_can = true;
            }
            EOT | ACK | NAK | CRC => {
                f(Frame::Control(b));
                *delivered += 1;
            }
            _ => {
                // Stray byte on an idle line (line noise, or a SUB padding byte
                // from a preceding block). XMODEM has no framing around it; the
                // protocol's timeout/retry logic is the recovery mechanism.
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Tests. Kept next to the code they cover: the framer and the checksums are
// pure functions over byte slices, and their edge cases (chunk boundaries,
// half packets, stray CANs) only make sense right here.
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    extern crate std;
    use std::vec;
    use std::vec::Vec;

    use super::*;
    use crate::test_support::{make_packet, own_frames, OwnedFrame};

    #[test]
    fn crc16_known_vector() {
        // CRC-16/XMODEM check value for "123456789" is 0x31C3.
        assert_eq!(crc16(b"123456789"), 0x31C3);
    }

    #[test]
    fn crc16_over_data_plus_crc_is_zero() {
        let d = b"123456789";
        let c = crc16(d);
        let mut packet = d.to_vec();
        packet.push((c >> 8) as u8);
        packet.push(c as u8);
        assert_eq!(crc16(&packet), 0);
    }

    #[test]
    fn sum8_wraps_mod_256() {
        assert_eq!(sum8(&[0xFF, 0x01]), 0x00);
        assert_eq!(sum8(&[1, 2, 3, 4]), 10);
    }

    #[test]
    fn verify_packet_accepts_good_and_rejects_corrupt() {
        let p = make_packet(1, b"hello", Check::Crc16, false);
        assert!(verify_packet(&p, Check::Crc16));
        let mut bad = p.clone();
        bad[10] ^= 0xFF; // flip a payload byte
        assert!(!verify_packet(&bad, Check::Crc16));

        let s = make_packet(2, b"world", Check::Sum8, false);
        assert!(verify_packet(&s, Check::Sum8));
        // The wrong check mode must fail.
        assert!(!verify_packet(&s, Check::Crc16));
    }

    #[test]
    fn verify_packet_rejects_bad_block_complement() {
        let mut p = make_packet(7, b"x", Check::Crc16, false);
        p[2] = p[1]; // complement no longer matches
        assert!(!verify_packet(&p, Check::Crc16));
    }

    #[test]
    fn packet_len_matches_wire_size() {
        assert_eq!(packet_len(SOH, Check::Sum8), 132);
        assert_eq!(packet_len(SOH, Check::Crc16), 133);
        assert_eq!(packet_len(STX, Check::Crc16), 1029);
    }

    #[test]
    fn framer_reassembles_a_packet_split_arbitrarily() {
        let p = make_packet(1, b"stream", Check::Crc16, false);
        let mut f = Framer::new(Check::Crc16);
        let mut got: Vec<Vec<u8>> = Vec::new();
        // Feed in tiny, awkward chunks to prove no assumption about boundaries.
        let mut i = 0;
        for step in [1usize, 2, 5, 3, 7, 1].iter().cycle() {
            if i >= p.len() {
                break;
            }
            let end = (i + step).min(p.len());
            f.push(&p[i..end], |fr| match fr {
                Frame::Packet(b) => got.push(b.to_vec()),
                Frame::Control(_) => panic!("unexpected control byte"),
            });
            i = end;
        }
        assert_eq!(got, vec![p]);
    }

    #[test]
    fn framer_handles_several_packets_and_control_bytes_in_one_chunk() {
        let p1 = make_packet(1, b"A", Check::Sum8, false);
        let p2 = make_packet(2, b"B", Check::Sum8, false);
        let mut stream = Vec::new();
        stream.extend_from_slice(&[NAK, EOT]); // control bytes before
        stream.extend_from_slice(&p1);
        stream.push(ACK); // control byte between packets
        stream.extend_from_slice(&p2);
        stream.push(EOT);

        let mut f = Framer::new(Check::Sum8);
        let frames = own_frames(&mut f, &stream);
        let (controls, packets) = split_frames(frames);
        assert_eq!(controls, vec![NAK, EOT, ACK, EOT]);
        assert_eq!(packets, vec![p1, p2]);
    }

    #[test]
    fn framer_treats_double_can_as_cancel_and_single_can_as_noise() {
        let mut f = Framer::new(Check::Crc16);
        // A lone CAN followed by a real control byte: the CAN is dropped.
        let (controls, _) = split_frames(own_frames(&mut f, &[CAN, ACK]));
        assert_eq!(controls, vec![ACK]);

        let mut f2 = Framer::new(Check::Crc16);
        let (cancels, _) = split_frames(own_frames(&mut f2, &[CAN, CAN]));
        assert_eq!(cancels, vec![CAN]);
    }

    #[test]
    fn framer_can_then_noise_then_can_is_cancel() {
        let mut f = Framer::new(Check::Crc16);
        let (cancels, _) = split_frames(own_frames(&mut f, &[CAN, 0x00, 0xFF, CAN]));
        assert_eq!(cancels, vec![CAN]);
    }

    #[test]
    fn framer_switch_check_mode_drops_half_packet() {
        let mut f = Framer::new(Check::Crc16);
        // Start a 128-byte CRC packet but stop halfway.
        let p = make_packet(1, b"x", Check::Crc16, false);
        f.push(&p[..10], |_| {});
        // Falling back to SUM8 invalidates the partial packet.
        f.set_check(Check::Sum8);
        let s = make_packet(1, b"y", Check::Sum8, false);
        let (_, packets) = split_frames(own_frames(&mut f, &s));
        assert_eq!(packets, vec![s]);
    }

    #[test]
    fn framer_garbage_does_not_panic_or_overflow() {
        // Deterministic junk: the recovery path is timeout/NAK, so the only
        // invariant here is that arbitrary bytes cannot overflow the buffer
        // or panic. cargo test runs this with debug_assert on.
        let mut rng = 0xC0FFEE41u32;
        for check in [Check::Sum8, Check::Crc16] {
            let mut f = Framer::new(check);
            let mut buf = [0u8; 64];
            for _ in 0..8_000 {
                rng = rng.wrapping_mul(1664525).wrapping_add(1013904223);
                let n = (rng as usize) % buf.len();
                for b in buf.iter_mut().take(n) {
                    rng = rng.wrapping_mul(1664525).wrapping_add(1013904223);
                    *b = (rng >> 24) as u8;
                }
                f.push(&buf[..n], |fr| {
                    if let Frame::Packet(p) = fr {
                        assert!(p.len() <= MAX_FRAME_LEN);
                        assert_eq!(p.len(), packet_len(p[0], check));
                    }
                });
            }
        }
    }

    fn split_frames(frames: Vec<OwnedFrame>) -> (Vec<u8>, Vec<Vec<u8>>) {
        let mut controls = Vec::new();
        let mut packets = Vec::new();
        for f in frames {
            match f {
                OwnedFrame::Control(c) => controls.push(c),
                OwnedFrame::Packet(p) => packets.push(p),
            }
        }
        (controls, packets)
    }
}
