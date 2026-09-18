# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Unlicense OR CC0-1.0

"""Compare the PNG written by the example against a golden image.

The example runs on the host, so the application and the test share the file
system: the test only has to wait for the application to exit and then read the
`screenshot.png` the application wrote. No serial dump, no base64 and no PPM
conversion is involved.
"""

import logging
import zlib
from dataclasses import dataclass
from pathlib import Path

import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize

PNG_NAME = 'screenshot.png'
GOLDEN_NAME = 'golden_result.png'
EXPECTED_WIDTH = 240
EXPECTED_HEIGHT = 240
CHANNELS_RGB = 3
CHANNELS_RGBA = 4

PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'
PNG_COLOR_TYPE_RGB = 2
PNG_COLOR_TYPE_RGBA = 6
PNG_COLOR_TYPE_PALETTE = 3
PNG_INTERLACE_NONE = 0
PNG_IHDR_LENGTH = 13  # width(4) + height(4) + bit_depth(1) + color_type(1) + compression(1) + filter(1) + interlace(1)
# The application logs the file it wrote, so the test does not have to guess
# whether the file came from a previous run.
SAVED_PNG_PATTERN = r'Saved \d+x\d+ PNG image \(\d+ bytes\) to \'(?P<path>[^\']+)\''


@dataclass(frozen=True)
class PngImage:
    width: int
    height: int
    channels: int
    pixels: bytes


def _parse_png_header(png_bytes: bytes) -> tuple[int, int, int]:
    """Return (width, height, channels) of an 8 bit per channel PNG."""
    if not png_bytes.startswith(PNG_SIGNATURE):
        raise ValueError('Not a PNG file')

    # The IHDR chunk is always the first chunk: length(4) + type(4) + data + crc(4).
    chunk_length = int.from_bytes(png_bytes[8:12], 'big')
    chunk_type = png_bytes[12:16]
    if chunk_type != b'IHDR' or chunk_length != PNG_IHDR_LENGTH:
        raise ValueError(f'Unexpected first PNG chunk: {chunk_type!r}')

    data = png_bytes[16:16 + PNG_IHDR_LENGTH]
    width = int.from_bytes(data[0:4], 'big')
    height = int.from_bytes(data[4:8], 'big')
    bit_depth = data[8]
    color_type = data[9]
    interlace = data[12]

    if bit_depth != 8:
        raise ValueError(f'Unsupported PNG bit depth: {bit_depth}')
    if interlace != PNG_INTERLACE_NONE:
        raise ValueError('Interlaced PNG files are not supported')
    if color_type == PNG_COLOR_TYPE_PALETTE:
        raise ValueError('The PNG must not be palette based')
    if color_type not in (PNG_COLOR_TYPE_RGB, PNG_COLOR_TYPE_RGBA):
        raise ValueError(f'Unsupported PNG color type: {color_type}')

    return width, height, CHANNELS_RGB if color_type == PNG_COLOR_TYPE_RGB else CHANNELS_RGBA


def _read_png_image(path: Path) -> PngImage:
    """Decode the subset of PNG the component writes: 8 bit RGB or RGBA, no interlace."""
    png_bytes = path.read_bytes()
    width, height, channels = _parse_png_header(png_bytes)

    # Decode every IDAT chunk, then undo the filters of the scanlines. That needs
    # the standard library only, so the test has no third party dependency.
    idat = bytearray()
    offset = 8
    while offset < len(png_bytes):
        chunk_length = int.from_bytes(png_bytes[offset:offset + 4], 'big')
        chunk_type = png_bytes[offset + 4:offset + 8]
        chunk_data = png_bytes[offset + 8:offset + 8 + chunk_length]
        if chunk_type == b'IDAT':
            idat += chunk_data
        elif chunk_type == b'IEND':
            break
        offset += 12 + chunk_length

    if not idat:
        raise ValueError(f'{path} has no image data')

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    if len(raw) != (stride + 1) * height:
        raise ValueError(f'Unexpected amount of image data in {path}: {len(raw)} bytes')

    pixels = bytearray(stride * height)
    previous = bytearray(stride)
    position = 0
    for y in range(height):
        filter_type = raw[position]
        position += 1
        line = bytearray(raw[position:position + stride])
        position += stride

        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up = previous[x]
            up_left = previous[x - channels] if x >= channels else 0
            if filter_type == 0:
                pass
            elif filter_type == 1:
                line[x] = (line[x] + left) & 0xFF
            elif filter_type == 2:
                line[x] = (line[x] + up) & 0xFF
            elif filter_type == 3:
                line[x] = (line[x] + ((left + up) >> 1)) & 0xFF
            elif filter_type == 4:
                estimate = left + up - up_left
                distances = (abs(estimate - left), abs(estimate - up), abs(estimate - up_left))
                nearest = (left, up, up_left)[distances.index(min(distances))]
                line[x] = (line[x] + nearest) & 0xFF
            else:
                raise ValueError(f'Unsupported PNG filter type {filter_type} in {path}')

        pixels[y * stride:(y + 1) * stride] = line
        previous = line

    return PngImage(width=width, height=height, channels=channels, pixels=bytes(pixels))


def _drop_unused_alpha(image: PngImage) -> PngImage:
    """Compare an RGBA image by its color channels when the alpha is opaque."""
    if image.channels != CHANNELS_RGBA:
        return image

    alpha = image.pixels[3::4]
    if any(value != alpha[0] for value in alpha):
        return image

    rgb = bytearray()
    for offset in range(0, len(image.pixels), CHANNELS_RGBA):
        rgb += image.pixels[offset:offset + CHANNELS_RGB]
    return PngImage(width=image.width, height=image.height, channels=CHANNELS_RGB, pixels=bytes(rgb))


def _first_difference(result: PngImage, golden: PngImage) -> str:
    for offset, (actual, expected) in enumerate(zip(result.pixels, golden.pixels)):
        if actual != expected:
            pixel = offset // result.channels
            channel = offset % result.channels
            return (f'first difference at pixel ({pixel % result.width}, {pixel // result.width}), '
                    f'channel {channel}: rendered {actual}, golden {expected}')
    return 'the images are equal'


@pytest.mark.host_test
@idf_parametrize('target', ['linux'], indirect=['target'])
def test_lvgl_host_sdl_example(dut: Dut) -> None:
    # The application writes the frame into its working directory, prints the
    # path it used and exits. pytest-embedded runs it on the host, so the file
    # can be read directly: there is no serial stream in between.
    dut.expect(r'Host SDL panel created')
    log_line = dut.expect(SAVED_PNG_PATTERN).group('path').decode()
    dut.expect_exact('LVGL host SDL example done.')

    result_path = Path(log_line)
    assert result_path.is_file(), (
        f'The application did not write {result_path}. '
        'Relative paths are relative to the working directory of the application.'
    )

    result_image = _read_png_image(result_path)
    golden_path = Path(__file__).with_name(GOLDEN_NAME)
    assert golden_path.is_file(), (
        f'Golden image {GOLDEN_NAME} not found. Run this test once, inspect {result_path}, '
        f'and copy it next to this pytest script as {GOLDEN_NAME}.'
    )
    golden_image = _read_png_image(golden_path)

    assert (result_image.width, result_image.height) == (EXPECTED_WIDTH, EXPECTED_HEIGHT), (
        f'Unexpected PNG geometry: {result_image.width}x{result_image.height}'
    )
    assert (result_image.width, result_image.height) == (golden_image.width, golden_image.height), (
        f'Rendered frame is {result_image.width}x{result_image.height}, '
        f'golden image is {golden_image.width}x{golden_image.height}'
    )

    comparable_result = _drop_unused_alpha(result_image)
    comparable_golden = _drop_unused_alpha(golden_image)
    assert comparable_result.pixels == comparable_golden.pixels, (
        f'Rendered frame does not match {GOLDEN_NAME}: {_first_difference(comparable_result, comparable_golden)}. '
        f'Inspect {result_path} in the build directory.'
    )

    logging.info('Rendered frame matches %s', golden_path)
    # Leave the rendered frame of a failing run next to the test report, so it
    # can be inspected without rebuilding the example.
    logging.info('Rendered frame: %s', result_path.resolve())
