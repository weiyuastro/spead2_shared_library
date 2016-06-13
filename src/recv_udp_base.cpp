/* Copyright 2016 SKA South Africa
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 */

#include <cstddef>
#include <cstdint>
#include "recv_reader.h"
#include "recv_packet.h"
#include "recv_stream.h"
#include "recv_udp_base.h"
#include "common_logging.h"

namespace spead2
{
namespace recv
{

constexpr std::size_t udp_reader_base::default_max_size;

bool udp_reader_base::process_one_packet(const std::uint8_t *data, std::size_t length, std::size_t max_size)
{
    bool stopped = false;
    if (length <= max_size && length > 0)
    {
        // If it's bigger, the packet might have been truncated
        packet_header packet;
        std::size_t size = decode_packet(packet, data, length);
        if (size == length)
        {
            get_stream_base().add_packet(packet);
            if (get_stream_base().is_stopped())
            {
                log_debug("UDP reader: end of stream detected");
                stopped = true;
            }
        }
        else if (size != 0)
        {
            log_info("discarding packet due to size mismatch (%1% != %2%)",
                     size, length);
        }
    }
    else if (length > max_size)
        log_info("dropped packet due to truncation");
    return stopped;
}

} // namespace recv
} // namespace spead2
