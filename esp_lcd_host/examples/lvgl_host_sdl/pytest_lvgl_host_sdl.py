# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Unlicense OR CC0-1.0

import hashlib
import logging
import re
from dataclasses import dataclass
from pathlib import Path

import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize

# The example draws a static screen, so every run produces exactly the same
# image. The frame is checked in two independent ways:
#   - the binary PPM the example writes is compared against the committed golden
#     image, hash by hash, and
#   - the PNG written by the component is checked against the properties it must
#     have (geometry, RGB8 color type and pixel payload) and hashed, so a decoder
#     is not needed.
PPM_NAME = 'lvgl_host_sdl_result.ppm'
PNG_NAME = 'screenshot.png'
GOLDEN_NAME = 'golden_result.ppm'
EXPECTED_WIDTH = 240
EXPECTED_HEIGHT = 240
RGB888_BYTES_PER_PIXEL = 3

PPM_MAGIC = b'P6'
PPM_MAX_VALUE = b'255'
PPM_HEADER_RE = re.compile(rb'^P6\s+(?P<width>\d+)\s+(?P<height>\d+)\s+(?P<max_value>\d+)\s')

PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'
PNG_COLOR_TYPE_RGB = 2
PNG_COLOR_TYPE_PALETTE = 3
PNG_INTERLACE_NONE = 0
PNG_IHDR_END = 13  # width(4) + height(4) + bit_depth(1) + color_type(1) + compression(1) + filter(1) + interlace(1)


@dataclass(frozen=True)
class PngHeader:
    width: int
    height: int
    bit_depth: int
    color_type: int
    interlace: int


def parse_png_header(png_bytes: bytes) -> PngHeader:
    if not png_bytes.startswith(PNG_SIGNATURE):
        raise ValueError('Not a PNG file')

    # The IHDR chunk is always the first chunk: length(4) + type(4) + data + crc(4).
    chunk_length = int.from_bytes(png_bytes[8:12], 'big')
    chunk_type = png_bytes[12:16]
    if chunk_type != b'IHDR' or chunk_length != PNG_IHDR_END:
        raise ValueError(f'Unexpected first PNG chunk: {chunk_type!r}')

    data = png_bytes[16:16 + PNG_IHDR_END]
    return PngHeader(
        width=int.from_bytes(data[0:4], 'big'),
        height=int.from_bytes(data[4:8], 'big'),
        bit_depth=data[8],
        color_type=data[9],
        interlace=data[12],
    )


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_ppm(path: Path) -> tuple[int, int, bytes]:
    ppm_bytes = path.read_bytes()
    header_match = PPM_HEADER_RE.match(ppm_bytes)
    if not header_match:
        raise ValueError(f'{path} is not a supported binary PPM (P6, maxval 255) file')

    width = int(header_match.group('width'))
    height = int(header_match.group('height'))
    if width <= 0 or height <= 0:
        raise ValueError(f'Unsupported PPM dimensions in {path}')
    if header_match.group('max_value') != PPM_MAX_VALUE:
        raise ValueError(f'Unsupported PPM max value in {path}')

    pixels = ppm_bytes[header_match.end():]
    expected_size = width * height * RGB888_BYTES_PER_PIXEL
    if len(pixels) != expected_size:
        raise ValueError(f'Expected {expected_size} PPM pixel bytes in {path}, got {len(pixels)}')
    return width, height, pixels


def save_png_artifact(png_bytes: bytes, output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        output_path.write_bytes(png_bytes)
    except OSError:
        logging.exception('Failed to save PNG artifact to %s', output_path)
        return

    logging.info('Saved PNG image to %s', output_path)


@pytest.mark.host_test
@idf_parametrize('target', ['linux'], indirect=['target'])
def test_lvgl_host_sdl_example(dut: Dut) -> None:
    # SDL creates the preview window when the panel is created; without a display
    # in the environment the application logs the failure and keeps going with
    # the framebuffer only.
    dut.expect(r'Host SDL panel created')
    png_bytes = dut.expect(rb'(?P<png>\x89PNG\r\n\x1a\n.*?IEND\xae\x42\x60\x82)', timeout=10).group('png')
    dut.expect(r'Reference frame written to')
    dut.expect_exact('LVGL host SDL example done.')

    # Save the PNG the component produced, so a human can look at it in CI (and
    # regenerate the golden image when the UI changes intentionally).
    save_png_artifact(png_bytes, Path(dut.logdir) / PNG_NAME)

    header = parse_png_header(png_bytes)
    assert (header.width, header.height) == (EXPECTED_WIDTH, EXPECTED_HEIGHT), (
        f'Unexpected PNG geometry: {header.width}x{header.height}'
    )
    # The component writes an 8 bit per channel RGB image for a BGR24 panel. The
    # example UI is fully opaque, so no alpha channel is added and the PNG is not
    # palette based (a mismatch would mean the pixels are not the raw frame).
    assert (header.bit_depth, header.color_type, header.interlace) == (
        8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE
    ), f'Unexpected PNG format: bit depth {header.bit_depth}, color type {header.color_type}'
    assert header.color_type != PNG_COLOR_TYPE_PALETTE, 'The PNG must not be palette based'

    # The example writes the frame it rendered itself as a PPM. Compare it with
    # the golden image to make sure the UI, LVGL and the flush path are unchanged.
    result_path = Path(__file__).with_name(PPM_NAME)
    golden_path = Path(__file__).with_name(GOLDEN_NAME)
    assert golden_path.is_file(), (
        f'Golden image {GOLDEN_NAME} not found. Run this test once, inspect {PPM_NAME} '
        f'in the test log directory, and copy it next to this pytest script as {GOLDEN_NAME}.'
    )

    width, height, pixels = load_ppm(result_path)
    golden = load_ppm(golden_path)
    assert (width, height) == (golden[0], golden[1]), (
        f'Rendered image is {width}x{height}, golden image is {golden[0]}x{golden[1]}'
    )
    assert sha256(pixels) == sha256(golden[2]), (
        'Rendered frame does not match the golden image '
        f'(rendered sha256 {sha256(pixels)}, golden sha256 {sha256(golden[2])})'
    )
