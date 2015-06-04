# Copyright 2015 SKA South Africa
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU Lesser General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option) any
# later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
# details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

from __future__ import division, print_function
import spead2
import spead2.send as send
import struct
import binascii
import numpy as np
import weakref
from nose.tools import *
from .defines import *


def hexlify(data):
    """Turns a byte string into human-readable hex dump"""
    if isinstance(data, list):
        return [hexlify(x) for x in data]
    chunks = []
    for i in range(0, len(data), 8):
        part = data[i : min(i + 8, len(data))]
        chunks.append(b':'.join([binascii.hexlify(part[i : i+1]) for i in range(len(part))]))
    return b' '.join(chunks)

def encode_be(size, value):
    """Encodes `value` as big-endian in `size` bytes"""
    assert size <= 8
    packed = struct.pack('>Q', value)
    return packed[8 - size:]


class Flavour(spead2.Flavour):
    def __init__(self, version, item_pointer_bits, heap_address_bits, bug_compat=0):
        super(Flavour, self).__init__(version, item_pointer_bits, heap_address_bits, bug_compat)

    def make_header(self, num_items):
        address_size = self.heap_address_bits // 8
        item_size = 8 - address_size
        return struct.pack(
            '>Q',
            0x5304000000000000 | (address_size << 32) | (item_size << 40) | num_items)

    def make_immediate(self, item_id, value):
        return struct.pack('>Q', 2**63 | (item_id << self.heap_address_bits) | value)

    def make_address(self, item_id, address):
        return struct.pack('>Q', (item_id << self.heap_address_bits) | address)

    def make_shape(self, shape):
        # TODO: extend for bug_compat flavours
        assert not (self.bug_compat &
                    (spead2.BUG_COMPAT_DESCRIPTOR_WIDTHS | spead2.BUG_COMPAT_SHAPE_BIT_1))
        ans = []
        for size in shape:
            if size < 0:
                ans.append(struct.pack('B', 1))
                ans.append(encode_be(self.heap_address_bits // 8, 0))
            else:
                ans.append(struct.pack('B', 0))
                ans.append(encode_be(self.heap_address_bits // 8, size))
        return b''.join(ans)

    def make_format(self, format):
        # TODO: extend for bug_compat flavours
        assert not self.bug_compat & spead2.BUG_COMPAT_DESCRIPTOR_WIDTHS
        ans = []
        for (code, length) in format:
            ans.append(struct.pack('B', ord(code)))
            ans.append(encode_be(8 - self.heap_address_bits // 8, length))
        return b''.join(ans)

    def items_to_bytes(self, items, descriptors=None, max_packet_size=1500):
        if descriptors is None:
            descriptors = items
        heap = send.Heap(0x123456, self)
        for descriptor in descriptors:
            heap.add_descriptor(descriptor)
        for item in items:
            heap.add_item(item)
        gen = send.PacketGenerator(heap, max_packet_size)
        return list(gen)


def offset_generator(fields):
    offset = 0
    yield offset
    for field in fields:
        offset += len(field)
        yield offset


class TestEncode(object):
    """Test heap encoding of various data"""

    def __init__(self):
        self.flavour = Flavour(4, 64, 48, 0)

    def test_empty(self):
        """An empty heap must still generate a packet"""
        expected = [
            b''.join([
                self.flavour.make_header(5),
                self.flavour.make_immediate(HEAP_CNT_ID, 0x123456),
                self.flavour.make_immediate(HEAP_LENGTH_ID, 1),
                self.flavour.make_immediate(PAYLOAD_OFFSET_ID, 0),
                self.flavour.make_immediate(PAYLOAD_LENGTH_ID, 1),
                self.flavour.make_address(NULL_ID, 0),
                struct.pack('B', 0)])
        ]
        packet = self.flavour.items_to_bytes([])
        assert_equal(hexlify(expected), hexlify(packet))

    def test_lifetime(self):
        """Heap must hold references to item values"""
        item = spead2.Item(id=0x2345, name='name', description='description',
                           shape=(2, 3), dtype=np.uint16)
        item.value = np.array([[6, 7, 8], [10, 11, 12000]], dtype=np.uint16)
        weak = weakref.ref(item.value)
        heap = send.Heap(0x123456, self.flavour)
        heap.add_item(item)
        del item
        packets = list(send.PacketGenerator(heap, 1472))
        assert_is_not_none(weak())
        del heap
        assert_is_none(weak())

    def make_descriptor_numpy(self, id, name, description, shape, dtype_str, fortran_order):
        payload_fields = [
            b'name',
            b'description',
            b'',
            self.flavour.make_shape(shape),
            "{{'descr': {!r}, 'fortran_order': {!r}, 'shape': {!r}}}".format(
                str(dtype_str), bool(fortran_order), tuple(shape)).encode()
        ]
        payload = b''.join(payload_fields)
        offsets = offset_generator(payload_fields)
        descriptor = b''.join([
            self.flavour.make_header(10),
            self.flavour.make_immediate(HEAP_CNT_ID, 1),
            self.flavour.make_immediate(HEAP_LENGTH_ID, len(payload)),
            self.flavour.make_immediate(PAYLOAD_OFFSET_ID, 0),
            self.flavour.make_immediate(PAYLOAD_LENGTH_ID, len(payload)),
            self.flavour.make_immediate(DESCRIPTOR_ID_ID, id),
            self.flavour.make_address(DESCRIPTOR_NAME_ID, next(offsets)),
            self.flavour.make_address(DESCRIPTOR_DESCRIPTION_ID, next(offsets)),
            self.flavour.make_address(DESCRIPTOR_FORMAT_ID, next(offsets)),
            self.flavour.make_address(DESCRIPTOR_SHAPE_ID, next(offsets)),
            self.flavour.make_address(DESCRIPTOR_DTYPE_ID, next(offsets)),
            payload
        ])
        return descriptor

    def make_descriptor_fallback(self, id, name, description, shape, format):
        payload_fields = [
            b'name',
            b'description',
            self.flavour.make_format(format),
            self.flavour.make_shape(shape),
        ]
        payload = b''.join(payload_fields)
        offsets = offset_generator(payload_fields)
        descriptor = b''.join([
            self.flavour.make_header(9),
            self.flavour.make_immediate(HEAP_CNT_ID, 1),
            self.flavour.make_immediate(HEAP_LENGTH_ID, len(payload)),
            self.flavour.make_immediate(PAYLOAD_OFFSET_ID, 0),
            self.flavour.make_immediate(PAYLOAD_LENGTH_ID, len(payload)),
            self.flavour.make_immediate(DESCRIPTOR_ID_ID, id),
            self.flavour.make_address(DESCRIPTOR_NAME_ID, next(offsets)),
            self.flavour.make_address(DESCRIPTOR_DESCRIPTION_ID, next(offsets)),
            self.flavour.make_address(DESCRIPTOR_FORMAT_ID, next(offsets)),
            self.flavour.make_address(DESCRIPTOR_SHAPE_ID, next(offsets)),
            payload
        ])
        return descriptor

    def test_numpy_simple(self):
        """A single numpy-format item with descriptor"""
        id = 0x2345
        shape = (2, 3)
        data = np.array([[6, 7, 8], [10, 11, 12000]], dtype=np.uint16)
        payload_fields = [
            self.make_descriptor_numpy(id, 'name', 'description', shape, '<u2', False),
            struct.pack('<6H', 6, 7, 8, 10, 11, 12000)
        ]
        payload = b''.join(payload_fields)
        offsets = offset_generator(payload_fields)
        expected = [
            b''.join([
                self.flavour.make_header(6),
                self.flavour.make_immediate(HEAP_CNT_ID, 0x123456),
                self.flavour.make_immediate(HEAP_LENGTH_ID, len(payload)),
                self.flavour.make_immediate(PAYLOAD_OFFSET_ID, 0),
                self.flavour.make_immediate(PAYLOAD_LENGTH_ID, len(payload)),
                self.flavour.make_address(DESCRIPTOR_ID, next(offsets)),
                self.flavour.make_address(id, next(offsets)),
                payload
            ])
        ]

        item = spead2.Item(id=id, name='name', description='description',
                           shape=shape, dtype=np.uint16)
        item.value = data
        packet = self.flavour.items_to_bytes([item])
        assert_equal(hexlify(expected), hexlify(packet))

    def test_numpy_noncontiguous(self):
        """A numpy item with a discontiguous item value is sent correctly"""
        id = 0x2345
        shape = (2, 3)
        store = np.array([[6, 7, 8, 0, 1], [10, 11, 12000, 2, 3], [9, 9, 9, 9, 9]], dtype=np.uint16)
        data = store[:2, :3]
        payload = struct.pack('<6H', 6, 7, 8, 10, 11, 12000)
        expected = [
            b''.join([
                self.flavour.make_header(5),
                self.flavour.make_immediate(HEAP_CNT_ID, 0x123456),
                self.flavour.make_immediate(HEAP_LENGTH_ID, len(payload)),
                self.flavour.make_immediate(PAYLOAD_OFFSET_ID, 0),
                self.flavour.make_immediate(PAYLOAD_LENGTH_ID, len(payload)),
                self.flavour.make_address(id, 0),
                payload
            ])
        ]
        item = spead2.Item(id=id, name='name', description='description',
                           shape=shape, dtype=np.uint16)
        item.value = data
        packet = self.flavour.items_to_bytes([item], [])
        assert_equal(hexlify(expected), hexlify(packet))

    def test_numpy_fortran_order(self):
        """A numpy item with Fortran-order descriptor must be sent in Fortran order"""
        id = 0x2345
        shape = (2, 3)
        data = np.array([[6, 7, 8], [10, 11, 12000]], order='F')
        assert_false(data.flags.c_contiguous)
        payload_fields = [
            self.make_descriptor_numpy(id, 'name', 'description', shape, '<u2', True),
            struct.pack('<6H', 6, 10, 7, 11, 8, 12000)
        ]
        payload = b''.join(payload_fields)
        offsets = offset_generator(payload_fields)
        expected = [
            b''.join([
                self.flavour.make_header(6),
                self.flavour.make_immediate(HEAP_CNT_ID, 0x123456),
                self.flavour.make_immediate(HEAP_LENGTH_ID, len(payload)),
                self.flavour.make_immediate(PAYLOAD_OFFSET_ID, 0),
                self.flavour.make_immediate(PAYLOAD_LENGTH_ID, len(payload)),
                self.flavour.make_address(DESCRIPTOR_ID, next(offsets)),
                self.flavour.make_address(id, next(offsets)),
                payload
            ])
        ]
        item = spead2.Item(id=id, name='name', description='description',
                           shape=shape, dtype=np.uint16, order='F')
        item.value = data
        packet = self.flavour.items_to_bytes([item])
        assert_equal(hexlify(expected), hexlify(packet))

    def test_fallback_types(self):
        """Send an array with mixed types and strange packing"""
        id = 0x2345
        format = [('b', 1), ('c', 7), ('f', 32)]
        shape = (2,)
        data = [(True, 'y', 1.0), (False, 'n', -1.0)]
        payload_fields = [
            self.make_descriptor_fallback(id, 'name', 'description', shape, format),
            b'\xF9\x3F\x80\x00\x00' + b'n\xBF\x80\x00\x00'
        ]
        payload = b''.join(payload_fields)
        offsets = offset_generator(payload_fields)
        expected = [
            b''.join([
                self.flavour.make_header(6),
                self.flavour.make_immediate(HEAP_CNT_ID, 0x123456),
                self.flavour.make_immediate(HEAP_LENGTH_ID, len(payload)),
                self.flavour.make_immediate(PAYLOAD_OFFSET_ID, 0),
                self.flavour.make_immediate(PAYLOAD_LENGTH_ID, len(payload)),
                self.flavour.make_address(DESCRIPTOR_ID, next(offsets)),
                self.flavour.make_address(id, next(offsets)),
                payload
            ])
        ]
        item = spead2.Item(id=id, name='name', description='description',
                           dtype=None, shape=shape, format=format)
        item.value = data
        packet = self.flavour.items_to_bytes([item])
        assert_equal(hexlify(expected), hexlify(packet))

    def test_small_fixed(self):
        """Sending a small item with fixed shape must use an immediate."""
        id = 0x2345
        data = 0x7654
        payload = struct.pack('>I', data)
        expected = [
            b''.join([
                self.flavour.make_header(6),
                self.flavour.make_immediate(HEAP_CNT_ID, 0x123456),
                self.flavour.make_immediate(HEAP_LENGTH_ID, 1),
                self.flavour.make_immediate(PAYLOAD_OFFSET_ID, 0),
                self.flavour.make_immediate(PAYLOAD_LENGTH_ID, 1),
                self.flavour.make_immediate(id, data),
                self.flavour.make_address(NULL_ID, 0),
                struct.pack('B', 0)
            ])
        ]
        item = spead2.Item(id=id, name='name', description='description',
                           dtype=None, shape=(), format=[('u', 16)])
        item.value = data
        packet = self.flavour.items_to_bytes([item], [])
        assert_equal(hexlify(expected), hexlify(packet))

    def test_small_variable(self):
        """Sending a small item with dynamic shape must not use an immediate."""
        id = 0x2345
        shape = (1, None)
        data = np.array([[4, 5]], dtype=np.uint8)
        payload = struct.pack('>2B', 4, 5)
        expected = [
            b''.join([
                self.flavour.make_header(5),
                self.flavour.make_immediate(HEAP_CNT_ID, 0x123456),
                self.flavour.make_immediate(HEAP_LENGTH_ID, len(payload)),
                self.flavour.make_immediate(PAYLOAD_OFFSET_ID, 0),
                self.flavour.make_immediate(PAYLOAD_LENGTH_ID, len(payload)),
                self.flavour.make_address(id, 0),
                payload
            ])
        ]
        item = spead2.Item(id=id, name='name', description='description',
                           shape=shape, dtype=None, format=[('u', 8)])
        item.value = data
        packet = self.flavour.items_to_bytes([item], [])
        assert_equal(hexlify(expected), hexlify(packet))
