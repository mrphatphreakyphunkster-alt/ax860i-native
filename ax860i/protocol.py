# SPDX-License-Identifier: GPL-2.0-only
"""Wire codec and monitoring-only command construction (no vendor dependencies)."""
import math


class ProtocolError(Exception):
    pass


class FramingError(ProtocolError):
    pass


class DeadlineError(ProtocolError):
    pass


class AckError(ProtocolError):
    pass


def symbol(nibble):
    # Four bits become four pairs, 0 -> 01, 1 -> 10.
    return sum((2 if nibble & (1 << i) else 1) << (2*i) for i in range(4))


def decode_symbol(byte):
    """Decode the observed lookup's low nibble without copying its table.

    The least significant pair can be 00 for a start/header marker; the
    observed lookup assigns it the same bit value as 01. 11 is invalid.
    """
    value = 0
    for i in range(4):
        pair = (byte >> (i*2)) & 3
        if pair == 2:
            value |= 1 << i
        elif pair != 1 and not (i == 0 and pair == 0):
            raise FramingError(f"invalid encoded symbol 0x{byte:02x}")
    return value


def encode(payload):
    return bytes([0x54]) + bytes(s for b in payload for s in
                               (symbol(b & 15), symbol(b >> 4))) + b'\0'


def decode_response(frame):
    if len(frame) < 2 or frame[-1] != 0 or (len(frame)-2) % 2:
        raise FramingError(f"malformed response: {frame.hex()}")
    # HasError's final ceq inverts the comparison: 7 means SUCCESS.
    # Requests start 54, successful responses normally start a8.
    status = decode_symbol(frame[0])
    if status >> 1 != 7:
        raise FramingError(f"response header status {status:#x}: {frame.hex()}")
    return bytes(decode_symbol(frame[i]) | (decode_symbol(frame[i+1]) << 4)
                 for i in range(1, len(frame)-1, 2))


class FrameBuffer:
    def __init__(self, maximum=256):
        self.data = bytearray()
        self.maximum = maximum

    def feed(self, data):
        self.data.extend(data)
        if len(self.data) > self.maximum:
            raise FramingError("response exceeds bounded frame buffer")

    def pop(self):
        end = self.data.find(0)
        if end < 0:
            return None
        frame = bytes(self.data[:end+1])
        del self.data[:end+1]
        return frame


def ack_status(payload):
    if payload is None:
        return 255
    if not payload or payload[0] == 0:
        return 0
    return 128 if payload[0] == 128 else 255


def read_setup(register, count):
    if not 0 <= register <= 255 or not 1 <= count <= 32:
        raise ValueError("invalid read register/count")
    return bytes([0x13, 3, 6, 1, 7, count, register])


def selector_write(register, value):
    # Intentionally no generic PMBus write interface.
    if not ((register == 0 and 0 <= value <= 4) or
            (register == 0xe7 and 1 <= value <= 8)):
        raise ValueError("only monitoring page/output selectors may be written")
    return bytes([0x13, 1, 4, 2, register, value])


def linear11(data, windows_quirk=False):
    if len(data) != 2:
        raise ProtocolError("LINEAR11 requires exactly two bytes")
    word = int.from_bytes(data, 'little')
    exponent = word >> 11
    if exponent >= 16:
        exponent -= 32
    mantissa = word & 2047
    if mantissa >= 1024:
        mantissa -= 2048
    if windows_quirk and mantissa & 1:
        mantissa += 1
    return math.ldexp(float(mantissa), exponent)


def parse_model(data):
    if len(data) != 7:
        raise ProtocolError("model must be a seven-byte read")
    return data.split(b'\0', 1)[0].decode('utf-8', errors='strict').strip()


def parse_version(data):
    if len(data) != 2:
        raise ProtocolError("dongle version must contain two bytes")
    return f"{data[1] >> 4}.{data[1] & 15}"


def ieee_divide(a, b):
    if b != 0:
        return a / b
    if a == 0 or math.isnan(a):
        return math.nan
    return math.copysign(math.inf, a * math.copysign(1, b))


def windows_power_in(output_w, input_v):
    if input_v >= 180:
        estimate = .00006501415 * output_w * output_w + (.9914832 * output_w) * 1.035 + 11.86228
    else:
        estimate = .00007803814 * output_w * output_w + (.9856293 * output_w) * 1.035 + 12.32274
    ratio = ieee_divide(output_w, estimate)
    if ratio > .99:
        estimate = ieee_divide(output_w, .99)
    elif ratio < .4:
        estimate = ieee_divide(output_w, .4)
    return estimate


def displayed_power(output_w, input_v, measured_w):
    estimate = windows_power_in(output_w, input_v)
    return estimate if abs(estimate) > .001 else measured_w


def efficiency(output_w, input_w):
    return 0.0 if abs(input_w) < .001 else 100 * ieee_divide(output_w, input_w)
