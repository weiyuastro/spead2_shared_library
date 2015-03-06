#!/usr/bin/env python
import spead2
import spead2.send
import sys
import logging
import numpy as np

logging.basicConfig(level=logging.INFO)

thread_pool = spead2.ThreadPool()
stream = spead2.send.UdpStream(thread_pool,
    "localhost", 8888, 48, spead2.BUG_COMPAT_PYSPEAD_0_5_2, 1500, 1e7)
del thread_pool

item = spead2.Item(0x1234, 'foo', 'a foo item', shape=(4, 5), dtype=np.int32)
item.value = np.zeros([4, 5], np.int32)
heap = spead2.send.Heap(1, spead2.BUG_COMPAT_PYSPEAD_0_5_2)
heap.add_descriptor(item)
heap.add_item(item.id, item)
stream.send_heap(heap)
