// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
//! `xymodem-proto` — a `no_std`, sans-IO XMODEM protocol engine.
//!
//! Pure software: no allocator, no clock, no hardware access. The caller feeds
//! received bytes with [`Engine::on_receive`], drives the state machine with
//! [`Engine::poll`], and observes output through an [`Io`] implementation.
//! See `modem.rs` for the model.
//!
//! A session carries an optional block-0 file header: with one it speaks
//! YMODEM (single file), without it classic XMODEM. That is a per-session
//! data choice, not a protocol mode baked into the engine at construction.
//!
//! Tests live in each module next to the code they cover (a `#[cfg(test)] mod
//! tests` at the bottom of the file), so a change and its proof sit together.

#![no_std]

pub mod frame;
pub mod io;
pub mod modem;

#[cfg(test)]
pub(crate) mod test_support;
