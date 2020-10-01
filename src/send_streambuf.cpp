/* Copyright 2015, 2019-2020 SKA South Africa
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

#include <streambuf>
#include <spead2/send_streambuf.h>

namespace spead2
{
namespace send
{

void streambuf_writer::wakeup()
{
    constexpr int max_batch = 64;
    int heaps = 0;
    packet_result result;
    for (int i = 0; i < max_batch; i++)
    {
        transmit_packet data;
        result = get_packet(data);
        if (result != packet_result::SUCCESS)
            break;

        for (const auto &buffer : data.pkt.buffers)
        {
            std::size_t buffer_size = boost::asio::buffer_size(buffer);
            std::size_t written = streambuf.sputn(boost::asio::buffer_cast<const char *>(buffer), buffer_size);
            data.item->bytes_sent += written;
            if (written != buffer_size)
            {
                if (!data.item->result)
                    data.item->result = boost::asio::error::eof;
                break;
            }
        }
        if (data.last)
            heaps++;
    }

    if (heaps > 0)
        heaps_completed(heaps);

    switch (result)
    {
    case packet_result::SLEEP:
        sleep();
        break;
    case packet_result::EMPTY:
        request_wakeup();
        break;
    case packet_result::SUCCESS:
        post_wakeup();
        break;
    }
}

streambuf_writer::streambuf_writer(
    io_service_ref io_service,
    std::streambuf &streambuf,
    const stream_config &config)
    : writer(std::move(io_service), config), streambuf(streambuf)
{
}

streambuf_stream::streambuf_stream(
    io_service_ref io_service,
    std::streambuf &streambuf,
    const stream_config &config)
    : stream(std::unique_ptr<writer>(new streambuf_writer(
        std::move(io_service), streambuf, config)))
{
}

} // namespace send
} // namespace spead2
