/* Copyright 2016, 2019-2020 SKA South Africa
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

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include <spead2/common_features.h>
#if SPEAD2_USE_IBV

#include <cassert>
#include <utility>
#include <memory>
#include <algorithm>
#include <set>
#include <boost/noncopyable.hpp>
#include <spead2/common_ibv.h>
#include <spead2/common_memory_allocator.h>
#include <spead2/common_raw_packet.h>
#include <spead2/common_logging.h>
#include <spead2/send_udp_ibv.h>
#include <spead2/send_stream.h>
#include <spead2/send_writer.h>

namespace spead2
{
namespace send
{

namespace
{

static constexpr int max_sge = 4;

/**
 * Stream using Infiniband Verbs for acceleration. Only IPv4 multicast
 * with an explicit source address is supported.
 */
class udp_ibv_writer : public writer
{
private:
    struct memory_region
    {
        const void *ptr;
        std::size_t size;
        ibv_mr_t mr;

        memory_region(const ibv_pd_t &pd, const void *ptr, std::size_t size);
        memory_region(const void *ptr, std::size_t size);

        bool operator<(const memory_region &other) const
        {
            // We order by decreasing address so that lower_bound will give us the containing region
            return ptr > other.ptr;
        }

        bool contains(const memory_region &other) const
        {
            return ptr <= other.ptr
                && static_cast<const std::uint8_t *>(ptr) + size
                >= static_cast<const std::uint8_t *>(other.ptr) + other.size;
        }
    };

    struct slot : public boost::noncopyable
    {
        ibv_send_wr wr{};
        ibv_sge sge[max_sge]{};
        ethernet_frame frame;
        stream::queue_item *item = nullptr;
        bool last;   ///< Last packet in the heap
    };

    const std::size_t n_slots;
    const std::size_t target_batch;
    boost::asio::ip::udp::socket socket; // used only to assign a source UDP port
    boost::asio::ip::udp::endpoint source;
    const std::vector<boost::asio::ip::udp::endpoint> endpoints;
    std::vector<mac_address> mac_addresses; ///< MAC addresses corresponding to endpoints
    memory_allocator::pointer buffer;
    rdma_event_channel_t event_channel;
    rdma_cm_id_t cm_id;
    ibv_pd_t pd;
    ibv_comp_channel_t comp_channel;
    boost::asio::posix::stream_descriptor comp_channel_wrapper;
    ibv_cq_t send_cq, recv_cq;
    ibv_qp_t qp;
    ibv_mr_t mr;     ///< Memory region for internal buffer
    std::set<memory_region> memory_regions;   ///< User-registered memory regions
    std::unique_ptr<slot[]> slots;
    std::size_t head = 0, tail = 0;
    std::size_t available;
    const int max_poll;

    static ibv_qp_t
    create_qp(const ibv_pd_t &pd, const ibv_cq_t &send_cq, const ibv_cq_t &recv_cq,
              std::size_t n_slots);

    /// Modify the QP with a rate limit, returning true on success
    static bool setup_hw_rate(const ibv_qp_t &qp, const stream_config &config);

    /**
     * Clear out the completion queue and return slots to the queue.
     * It will stop after freeing up @ref target_batch slots or
     * find no completions @ref max_poll times.
     *
     * Returns @c true if there are possibly more completions still in the
     * queue.
     */
    bool reap();

    /**
     * Schedule a call to wakeup when it should check for space in the buffer again.
     */
    void wait_for_space();

    virtual void wakeup() override final;

public:
    udp_ibv_writer(
        io_service_ref io_service,
        const stream_config &config,
        const udp_ibv_stream_config &udp_ibv_config);

    virtual std::size_t get_num_substreams() const override final { return endpoints.size(); }
};

udp_ibv_writer::memory_region::memory_region(
    const ibv_pd_t &pd, const void *ptr, std::size_t size)
    : ptr(ptr), size(size), mr(pd, const_cast<void *>(ptr), size, 0)
{
}

udp_ibv_writer::memory_region::memory_region(
    const void *ptr, std::size_t size)
    : ptr(ptr), size(size)
{
}

static constexpr int header_length =
    ethernet_frame::min_size + ipv4_packet::min_size + udp_packet::min_size;

ibv_qp_t udp_ibv_writer::create_qp(
    const ibv_pd_t &pd, const ibv_cq_t &send_cq, const ibv_cq_t &recv_cq, std::size_t n_slots)
{
    ibv_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.send_cq = send_cq.get();
    attr.recv_cq = recv_cq.get();
    attr.qp_type = IBV_QPT_RAW_PACKET;
    attr.cap.max_send_wr = n_slots;
    attr.cap.max_recv_wr = 0;
    attr.cap.max_send_sge = max_sge;
    attr.cap.max_recv_sge = 0;
    attr.sq_sig_all = 0;
    return ibv_qp_t(pd, &attr);
}

bool udp_ibv_writer::setup_hw_rate(const ibv_qp_t &qp, const stream_config &config)
{
#if SPEAD2_USE_IBV_HW_RATE_LIMIT
    ibv_device_attr_ex attr;
    if (ibv_query_device_ex(qp->context, nullptr, &attr) != 0)
    {
        log_debug("Not using HW rate limiting because ibv_query_device_ex failed");
        return false;
    }
    if (!ibv_is_qpt_supported(attr.packet_pacing_caps.supported_qpts, IBV_QPT_RAW_PACKET)
        || attr.packet_pacing_caps.qp_rate_limit_max == 0)
    {
        log_debug("Not using HW rate limiting because it is not supported by the device");
        return false;
    }
    // User rate is Bps, for UDP payload. Convert to rate for Ethernet frames, in kbps.
    std::size_t frame_size = config.get_max_packet_size() + 42;
    double overhead = double(frame_size) / config.get_max_packet_size();
    double rate_kbps = config.get_rate() * 8e-3 * overhead;
    if (rate_kbps < attr.packet_pacing_caps.qp_rate_limit_min
        || rate_kbps > attr.packet_pacing_caps.qp_rate_limit_max)
    {
        log_debug("Not using HW rate limiting because the HW does not support the rate");
        return false;
    }

    ibv_qp_rate_limit_attr limit_attr = {};
    limit_attr.rate_limit = rate_kbps;
    limit_attr.typical_pkt_sz = frame_size;
    /* Using config.get_max_burst_size() would cause much longer bursts than
     * necessary if the user did not explicitly turn down the default.
     * Experience with ConnectX-5 shows that it can limit bursts to a single
     * packet with little loss in rate accuracy. If max_burst_sz is set to less
     * than typical_pkt_size it is ignored and a default is used.
     */
    limit_attr.max_burst_sz = frame_size;
    if (ibv_modify_qp_rate_limit(qp.get(), &limit_attr) != 0)
    {
        log_debug("Not using HW rate limiting because ibv_modify_qp_rate_limit failed");
        return false;
    }

    return true;
#else
    log_debug("Not using HW rate limiting because support was not found at compile time");
    return false;
#endif
}

bool udp_ibv_writer::reap()
{
    ibv_wc wc;
    int retries = max_poll;
    int heaps = 0;
    std::size_t min_available = std::min(n_slots, available + target_batch);
    // TODO: this could probably be faster with the cq_ex polling API
    // because it could avoid locking.
    while (available < min_available)
    {
        int done = send_cq.poll(1, &wc);
        if (done == 0)
        {
            retries--;
            if (retries <= 0)
                break;
            else
                continue;
        }

        boost::system::error_code ec;
        if (wc.status != IBV_WC_SUCCESS)
        {
            log_warning("Work Request failed with code %1%", wc.status);
            // TODO: create some mapping from ibv_wc_status to system error codes
            ec = boost::system::error_code(EIO, boost::asio::error::get_system_category());
        }
        int batch = wc.wr_id;
        for (int i = 0; i < batch; i++)
        {
            const slot *s = &slots[head];
            stream::queue_item *item = s->item;
            if (ec)
            {
                if (!item->result)
                    item->result = ec;
            }
            else
            {
                for (int j = 0; j < s->wr.num_sge; j++)
                    item->bytes_sent += s->sge[j].length;
                item->bytes_sent -= header_length;
            }
            heaps += s->last;
            if (++head == n_slots)
                head = 0;
        }
        available += batch;
    }
    if (heaps > 0)
        heaps_completed(heaps);
    return available < n_slots && retries > 0;
}

void udp_ibv_writer::wait_for_space()
{
    if (comp_channel)
    {
        send_cq.req_notify(false);
        auto handler = [this](const boost::system::error_code &, size_t)
        {
            ibv_cq *event_cq;
            void *event_cq_context;
            // This should be non-blocking, since we were woken up, but
            // spurious wakeups have been observed.
            while (comp_channel.get_event(&event_cq, &event_cq_context))
                send_cq.ack_events(1);
            wakeup();
        };
        comp_channel_wrapper.async_read_some(boost::asio::null_buffers(), handler);
    }
    else
        post_wakeup();
}

void udp_ibv_writer::wakeup()
{
    bool more_cqe = reap();
    if (available < target_batch)
    {
        wait_for_space();
    }
    else
    {
        std::size_t i;
        packet_result result;
        slot *prev = nullptr;
        slot *first = &slots[tail];
        for (i = 0; i < target_batch; i++)
        {
            transmit_packet data;
            result = get_packet(data);
            if (result != packet_result::SUCCESS)
                break;

            slot *s = &slots[tail];
            std::size_t payload_size = data.size;
            ipv4_packet ipv4 = s->frame.payload_ipv4();
            ipv4.total_length(payload_size + udp_packet::min_size + ipv4.header_length());
            udp_packet udp = ipv4.payload_udp();
            udp.length(payload_size + udp_packet::min_size);
            if (get_num_substreams() > 1)
            {
                const std::size_t substream_index = data.item->substream_index;
                const auto &endpoint = endpoints[substream_index];
                s->frame.destination_mac(mac_addresses[substream_index]);
                ipv4.destination_address(endpoint.address().to_v4());
                udp.destination_port(endpoint.port());
            }
            ipv4.update_checksum();
            packet_buffer payload = udp.payload();
            s->wr.num_sge = 1;
            // TODO: addr and lkey can be fixed by constructor
            s->sge[0].addr = (uintptr_t) s->frame.data();
            s->sge[0].lkey = mr->lkey;
            s->sge[0].length = payload.data() - s->frame.data();
            std::uint8_t *copy_target = payload.data();
            /* It may appear that we require strictly less than (because we're
             * using one SGE for the IP/UDP header), but the first buffer in
             * data.pkt.buffers will always be able to merge with it.
             *
             * This is a conservative estimate, because other merges are
             * possible (particularly if not all items fall into registered
             * ranges), but the cost of doing two passes to check for this
             * case would be expensive.
             */
            bool can_skip_copy = data.pkt.buffers.size() <= max_sge;
            for (const auto &buffer : data.pkt.buffers)
            {
                ibv_sge cur;
                const uint8_t *ptr = boost::asio::buffer_cast<const uint8_t *>(buffer);
                cur.length = boost::asio::buffer_size(buffer);
                // Check if it belongs to a user-registered region
                memory_region cmp(ptr, cur.length);
                std::set<memory_region>::const_iterator it;
                if (can_skip_copy
                    && (it = memory_regions.lower_bound(cmp)) != memory_regions.end()
                    && it->contains(cmp))
                {
                    cur.addr = (uintptr_t) ptr;
                    cur.lkey = it->mr->lkey;  // TODO: cache the lkey to avoid pointer lookup?
                }
                else
                {
                    // We have to copy it
                    cur.addr = (uintptr_t) copy_target;
                    cur.lkey = mr->lkey;
                    std::memcpy(copy_target, ptr, cur.length);
                    copy_target += cur.length;
                }
                ibv_sge &prev = s->sge[s->wr.num_sge - 1];
                if (prev.lkey == cur.lkey && prev.addr + prev.length == cur.addr)
                {
                    // Can merge with the previous one
                    prev.length += cur.length;
                }
                else
                {
                    // Have to create a new one.
                    s->sge[s->wr.num_sge++] = cur;
                }
            }
            s->wr.next = nullptr;
            s->wr.send_flags = 0;
            s->item = data.item;
            s->last = data.last;
            if (prev != nullptr)
                prev->wr.next = &s->wr;
            prev = s;

            if (++tail == n_slots)
                tail = 0;
            available--;
        }

        if (i > 0)
        {
            prev->wr.wr_id = i;
            prev->wr.send_flags = IBV_SEND_SIGNALED;
            qp.post_send(&first->wr);
        }

        if (i > 0 || more_cqe)
        {
            /* There may be more completions immediately available (either existing
             * ones, or for the packets we've just posted).
             */
            post_wakeup();
        }
        else if (available < n_slots)
        {
            /* We ran out of packets and completions, but we need to monitor
             * the CQ for future completions. If the result is SLEEP then we
             * could call sleep() instead (and ideally we'd wait for whichever
             * happens first). However, if we get SLEEP then we're using
             * software rate limiting, which means we're likely to see some
             * completions before it's time to wake up again (because the
             * hardware will complete packets at line rate).
             */
            wait_for_space();
        }
        else if (result == packet_result::SLEEP)
            sleep();
        else
        {
            assert(result == packet_result::EMPTY);
            request_wakeup();
        }
    }
}

static std::size_t calc_n_slots(const stream_config &config, std::size_t buffer_size)
{
    return std::max(std::size_t(1), buffer_size / (config.get_max_packet_size() + header_length));
}

static std::size_t calc_target_batch(const stream_config &config, std::size_t n_slots)
{
    std::size_t packet_size = config.get_max_packet_size() + header_length;
    return std::max(std::size_t(1), std::min(n_slots / 4, 262144 / packet_size));
}

udp_ibv_writer::udp_ibv_writer(
    io_service_ref io_service,
    const stream_config &config,
    const udp_ibv_stream_config &udp_ibv_config)
    : writer(std::move(io_service), config),
    n_slots(calc_n_slots(config, udp_ibv_config.get_buffer_size())),
    target_batch(calc_target_batch(config, n_slots)),
    socket(get_io_service(), boost::asio::ip::udp::v4()),
    endpoints(udp_ibv_config.get_endpoints()),
    cm_id(event_channel, nullptr, RDMA_PS_UDP),
    comp_channel_wrapper(get_io_service()),
    available(n_slots),
    max_poll(udp_ibv_config.get_max_poll())
{
    if (endpoints.empty())
        throw std::invalid_argument("endpoints is empty");
    mac_addresses.reserve(endpoints.size());
    for (const auto &endpoint : endpoints)
        mac_addresses.push_back(multicast_mac(endpoint.address()));
    const boost::asio::ip::address &interface_address = udp_ibv_config.get_interface_address();
    if (interface_address.is_unspecified())
        throw std::invalid_argument("interface address must be specified");
    // Check that registered memory regions don't overlap
    auto config_regions = udp_ibv_config.get_memory_regions();
    std::sort(config_regions.begin(), config_regions.end());
    for (std::size_t i = 1; i < config_regions.size(); i++)
        if (static_cast<const std::uint8_t *>(config_regions[i - 1].first)
            + config_regions[i - 1].second > config_regions[i].first)
            throw std::invalid_argument("memory regions overlap");

    socket.bind(boost::asio::ip::udp::endpoint(interface_address, 0));
    // Re-compute buffer_size as a whole number of slots
    const std::size_t max_raw_size = config.get_max_packet_size() + header_length;
    std::size_t buffer_size = n_slots * max_raw_size;

    cm_id.bind_addr(interface_address);
    pd = ibv_pd_t(cm_id);
    int comp_vector = udp_ibv_config.get_comp_vector();
    if (comp_vector >= 0)
    {
        comp_channel = ibv_comp_channel_t(cm_id);
        comp_channel_wrapper = comp_channel.wrap(get_io_service());
        send_cq = ibv_cq_t(cm_id, n_slots, nullptr,
                           comp_channel, comp_vector % cm_id->verbs->num_comp_vectors);
    }
    else
        send_cq = ibv_cq_t(cm_id, n_slots, nullptr);
    recv_cq = ibv_cq_t(cm_id, 1, nullptr);
    qp = create_qp(pd, send_cq, recv_cq, n_slots);
    qp.modify(IBV_QPS_INIT, cm_id->port_num);
    qp.modify(IBV_QPS_RTR);
    qp.modify(IBV_QPS_RTS);

    if (config.get_allow_hw_rate() && config.get_rate() > 0.0)
    {
        if (setup_hw_rate(qp, config))
            enable_hw_rate();
    }

    std::shared_ptr<mmap_allocator> allocator = std::make_shared<mmap_allocator>(0, true);
    buffer = allocator->allocate(max_raw_size * n_slots, nullptr);
    mr = ibv_mr_t(pd, buffer.get(), buffer_size, IBV_ACCESS_LOCAL_WRITE);
    for (const auto &region : udp_ibv_config.get_memory_regions())
        memory_regions.emplace(pd, region.first, region.second);
    slots.reset(new slot[n_slots]);
    // We fill in the destination details for the first endpoint. If there are
    // multiple endpoints, they'll get updated for each packet.
    mac_address source_mac = interface_mac(interface_address);
    for (std::size_t i = 0; i < n_slots; i++)
    {
        slots[i].frame = ethernet_frame(buffer.get() + i * max_raw_size, max_raw_size);
        memset(&slots[i].sge, 0, sizeof(slots[i].sge));
        slots[i].wr.sg_list = slots[i].sge;
        slots[i].wr.opcode = IBV_WR_SEND;
        slots[i].frame.destination_mac(mac_addresses[0]);
        slots[i].frame.source_mac(source_mac);
        slots[i].frame.ethertype(ipv4_packet::ethertype);
        ipv4_packet ipv4 = slots[i].frame.payload_ipv4();
        ipv4.version_ihl(0x45);  // IPv4, 20 byte header
        // total_length will change later to the actual packet size
        ipv4.total_length(config.get_max_packet_size() + ipv4_packet::min_size + udp_packet::min_size);
        ipv4.flags_frag_off(ipv4_packet::flag_do_not_fragment);
        ipv4.ttl(udp_ibv_config.get_ttl());
        ipv4.protocol(udp_packet::protocol);
        ipv4.source_address(interface_address.to_v4());
        ipv4.destination_address(endpoints[0].address().to_v4());
        udp_packet udp = ipv4.payload_udp();
        udp.source_port(socket.local_endpoint().port());
        udp.destination_port(endpoints[0].port());
        udp.length(config.get_max_packet_size() + udp_packet::min_size);
        udp.checksum(0);
    }
}

} // anonymous namespace

static void validate_endpoint(const boost::asio::ip::udp::endpoint &endpoint)
{
    if (!endpoint.address().is_v4() || !endpoint.address().is_multicast())
        throw std::invalid_argument("endpoint is not an IPv4 multicast address");
}

static void validate_memory_region(const udp_ibv_stream_config::memory_region &region)
{
    if (region.second == 0)
        throw std::invalid_argument("memory region must have non-zero size");
}

udp_ibv_stream_config::udp_ibv_stream_config()
    : buffer_size(udp_ibv_stream::default_buffer_size),
    ttl(1),
    comp_vector(0),
    max_poll(udp_ibv_stream::default_max_poll)
{
}

udp_ibv_stream_config &udp_ibv_stream_config::set_endpoints(
    const std::vector<boost::asio::ip::udp::endpoint> &endpoints)
{
    for (const auto &endpoint : endpoints)
        validate_endpoint(endpoint);
    this->endpoints = endpoints;
    return *this;
}

udp_ibv_stream_config &udp_ibv_stream_config::add_endpoint(
    const boost::asio::ip::udp::endpoint &endpoint)
{
    validate_endpoint(endpoint);
    endpoints.push_back(endpoint);
    return *this;
}

udp_ibv_stream_config &udp_ibv_stream_config::set_interface_address(
    const boost::asio::ip::address &interface_address)
{
    if (!interface_address.is_v4())
        throw std::invalid_argument("interface address is not an IPv4 address");
    this->interface_address = interface_address;
    return *this;
}

udp_ibv_stream_config &udp_ibv_stream_config::set_buffer_size(std::size_t buffer_size)
{
    if (buffer_size == 0)
        this->buffer_size = udp_ibv_stream::default_buffer_size;
    else
        this->buffer_size = buffer_size;
    return *this;
}

udp_ibv_stream_config &udp_ibv_stream_config::set_ttl(std::uint8_t ttl)
{
    this->ttl = ttl;
    return *this;
}

udp_ibv_stream_config &udp_ibv_stream_config::set_comp_vector(int comp_vector)
{
    this->comp_vector = comp_vector;
    return *this;
}

udp_ibv_stream_config &udp_ibv_stream_config::set_max_poll(int max_poll)
{
    if (max_poll <= 1)
        throw std::invalid_argument("max_poll must be at least 1");
    this->max_poll = max_poll;
    return *this;
}

udp_ibv_stream_config &udp_ibv_stream_config::set_memory_regions(const std::vector<memory_region> &memory_regions)
{
    for (const memory_region &region : memory_regions)
        validate_memory_region(region);
    this->memory_regions = memory_regions;
    return *this;
}

udp_ibv_stream_config &udp_ibv_stream_config::add_memory_region(const void *ptr, std::size_t size)
{
    memory_region region(ptr, size);
    validate_memory_region(region);
    memory_regions.push_back(region);
    return *this;
}

constexpr std::size_t udp_ibv_stream::default_buffer_size;
constexpr int udp_ibv_stream::default_max_poll;

udp_ibv_stream::udp_ibv_stream(
    io_service_ref io_service,
    const boost::asio::ip::udp::endpoint &endpoint,
    const stream_config &config,
    const boost::asio::ip::address &interface_address,
    std::size_t buffer_size,
    int ttl,
    int comp_vector,
    int max_poll)
    : udp_ibv_stream(
        std::move(io_service),
        config,
        udp_ibv_stream_config()
            .add_endpoint(endpoint)
            .set_interface_address(interface_address)
            .set_buffer_size(buffer_size)
            .set_ttl(ttl)
            .set_comp_vector(comp_vector)
            .set_max_poll(max_poll)
    )
{
}

udp_ibv_stream::udp_ibv_stream(
    io_service_ref io_service,
    const stream_config &config,
    const udp_ibv_stream_config &udp_ibv_config)
    : stream(std::unique_ptr<writer>(new udp_ibv_writer(
        std::move(io_service), config, udp_ibv_config)))
{
}

} // namespace send
} // namespace spead2

#endif // SPEAD2_USE_IBV
