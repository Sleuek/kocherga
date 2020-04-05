// This software is distributed under the terms of the MIT License.
// Copyright (c) 2020 Zubax Robotics.
// Author: Pavel Kirienko <pavel.kirienko@zubax.com>

#pragma once

#include "kocherga.hpp"

namespace kocherga::serial
{
namespace detail
{
static constexpr auto BitsPerByte = kocherga::detail::BitsPerByte;

/// Special byte stream values.
static constexpr std::uint8_t FrameDelimiter = 0x9E;
static constexpr std::uint8_t EscapePrefix   = 0x8E;

/// Reference values to check the header against.
static constexpr std::uint8_t                FrameFormatVersion = 0;
static constexpr std::array<std::uint8_t, 4> FrameIndexEOTReference{0, 0, 0, 0x80};

/// Size-optimized implementation of CRC32-C (Castagnoli).
class CRC32C
{
public:
    static constexpr std::size_t Size = 4;

    void update(const std::uint8_t b)
    {
        value_ ^= static_cast<std::uint32_t>(b);
        for (auto i = 0U; i < BitsPerByte; i++)
        {
            value_ = ((value_ & 1U) != 0) ? ((value_ >> 1U) ^ ReflectedPoly) : (value_ >> 1U);  // NOLINT
        }
    }

    [[nodiscard]] auto get() const { return value_ ^ Xor; }

    [[nodiscard]] auto getBytes() const -> std::array<std::uint8_t, Size>
    {
        const auto x = get();
        return {
            static_cast<std::uint8_t>(x >> (BitsPerByte * 0U)),
            static_cast<std::uint8_t>(x >> (BitsPerByte * 1U)),
            static_cast<std::uint8_t>(x >> (BitsPerByte * 2U)),
            static_cast<std::uint8_t>(x >> (BitsPerByte * 3U)),
        };
    }

    [[nodiscard]] auto isResidueCorrect() const { return value_ == Residue; }

private:
    static constexpr std::uint32_t Xor           = 0xFFFF'FFFFUL;
    static constexpr std::uint32_t ReflectedPoly = 0x82F6'3B78UL;
    static constexpr std::uint32_t Residue       = 0xB798'B438UL;

    std::uint32_t value_ = Xor;
};

struct Transfer
{
    struct Metadata
    {
        static constexpr std::uint8_t DefaultPriority      = 7U;  // Lowest.
        static constexpr NodeID       AnonymousNodeID      = 0xFFFFU;
        static constexpr PortID       DataSpecRequestMask  = 0x8000U;
        static constexpr PortID       DataSpecResponseMask = 0xC000U;

        std::uint8_t  priority    = DefaultPriority;
        NodeID        source      = AnonymousNodeID;
        NodeID        destination = AnonymousNodeID;
        std::uint16_t data_spec{};
        TransferID    transfer_id{};

        [[nodiscard]] auto isRequest() const -> std::optional<PortID>
        {
            if ((data_spec & DataSpecRequestMask) == DataSpecRequestMask)
            {
                return data_spec & static_cast<PortID>(~DataSpecRequestMask);
            }
            return {};
        }

        [[nodiscard]] auto isResponse() const -> std::optional<PortID>
        {
            return ((data_spec & DataSpecResponseMask) == DataSpecResponseMask)
                       ? std::optional<PortID>(data_spec & static_cast<PortID>(~DataSpecResponseMask))
                       : std::nullopt;
        }
    };
    Metadata            meta{};
    std::size_t         payload_len = 0;
    const std::uint8_t* payload     = nullptr;
};

/// UAVCAN/serial stream parser. Extracts UAVCAN/serial frames from raw stream of bytes.
template <std::size_t MaxPayloadSize>
class StreamParser
{
public:
    /// If the byte completed a transfer, it will be returned.
    /// The returned object contains a pointer to the payload buffer memory. The memory is invalidated on the second
    /// call to update() after reception (the first call does not invalidate the memory).
    [[nodiscard]] auto update(const std::uint8_t stream_byte) -> std::optional<Transfer>
    {
        std::optional<Transfer> out;
        if (stream_byte == FrameDelimiter)
        {
            if (inside_ && (offset_ >= CRC32C::Size) && crc_.isResidueCorrect())
            {
                out = Transfer{
                    meta_,
                    offset_ - CRC32C::Size,
                    buf_.data(),
                };
            }
            reset();
            inside_ = true;
        }
        else if (inside_)  // Accept the byte
        {
            if (stream_byte == EscapePrefix)
            {
                if (unescape_)
                {
                    inside_ = false;  // Double escape cannot occur in a well-formed stream by design.
                }
                else
                {
                    unescape_ = true;
                }
            }
            else
            {
                const std::uint8_t bt = unescape_ ? static_cast<std::uint8_t>(~stream_byte) : stream_byte;
                unescape_             = false;
                crc_.update(bt);
                if (offset_ < HeaderSize)
                {
                    acceptHeader(bt);
                }
                else
                {
                    buf_.at(offset_) = bt;
                }
                ++offset_;
                if (offset_ >= buf_.size())
                {
                    inside_ = false;
                }
            }
        }
        else
        {
            (void) 0;  // Not inside a frame, drop the byte.
        }
        return out;
    }

    void reset()
    {
        offset_   = 0;
        unescape_ = false;
        inside_   = false;
        crc_      = {};
        meta_     = {};
    }

private:
    void acceptHeader(const std::uint8_t bt)
    {
        if ((OffsetVersion == offset_) && (bt != FrameFormatVersion))
        {
            inside_ = false;
        }
        if (OffsetPriority == offset_)
        {
            meta_.priority = bt;
        }
        acceptHeaderField(OffsetSource, meta_.source, bt);
        acceptHeaderField(OffsetDestination, meta_.destination, bt);
        acceptHeaderField(OffsetDataSpec, meta_.data_spec, bt);
        acceptHeaderField(OffsetTransferID, meta_.transfer_id, bt);
        if ((OffsetFrameIndexEOT.first <= offset_) && (offset_ <= OffsetFrameIndexEOT.second) &&
            (FrameIndexEOTReference.at(offset_ - OffsetFrameIndexEOT.first) != bt))
        {
            inside_ = false;
        }
        if (offset_ == (HeaderSize - 1U))
        {
            if (!crc_.isResidueCorrect())
            {
                inside_ = false;  // Header CRC error.
            }
            // At this point the header has been received and proven to be correct. Here, a generic implementation
            // would normally query the subscription list to see if the frame is interesting or it should be dropped;
            // also, the amount of dynamic memory that needs to be allocated for the payload would also be determined
            // at this moment. The main purpose of the header CRC is to permit such early-stage frame processing.
            // This specialized implementation requires none of that.
            crc_ = {};
        }
    }

    template <typename Field>
    void acceptHeaderField(const std::pair<std::size_t, std::size_t> range, Field& fld, const std::uint8_t bt) const
    {
        if ((range.first <= offset_) && (offset_ <= range.second))
        {
            fld |= static_cast<Field>(static_cast<Field>(bt) << (BitsPerByte * (offset_ - range.first)));
        }
    }

    static constexpr std::size_t HeaderSize = 32;
    // Header field offsets.
    static constexpr std::size_t                         OffsetVersion  = 0;
    static constexpr std::size_t                         OffsetPriority = 1;
    static constexpr std::pair<std::size_t, std::size_t> OffsetSource{2, 3};
    static constexpr std::pair<std::size_t, std::size_t> OffsetDestination{4, 5};
    static constexpr std::pair<std::size_t, std::size_t> OffsetDataSpec{6, 7};
    static constexpr std::pair<std::size_t, std::size_t> OffsetTransferID{16, 23};
    static constexpr std::pair<std::size_t, std::size_t> OffsetFrameIndexEOT{24, 27};

    std::size_t offset_   = 0;
    bool        unescape_ = false;
    bool        inside_   = false;
    CRC32C      crc_;

    Transfer::Metadata meta_;

    std::array<std::uint8_t, MaxPayloadSize + CRC32C::Size> buf_{};
};

/// Sends a transfer without intermediate buffering.
/// Callback is of type (std::uint8_t) -> bool whose semantics reflects ISerialPort::send().
/// Callback shall not be an std::function<> to avoid heap allocation.
template <typename Callback>
[[nodiscard]] inline auto transmit(const Callback& send_byte, const Transfer& tr) -> bool
{
    CRC32C     crc;
    const auto out = [&send_byte, &crc](const std::uint8_t b) {
        crc.update(b);
        if ((b == detail::FrameDelimiter) || (b == detail::EscapePrefix))
        {
            return send_byte(detail::EscapePrefix) && send_byte(static_cast<std::uint8_t>(~b));
        }
        return send_byte(b);
    };
    const auto out2 = [&out](const std::uint16_t bb) {
        return out(static_cast<std::uint8_t>(bb)) && out(static_cast<std::uint8_t>(bb >> BitsPerByte));
    };
    bool ok = send_byte(FrameDelimiter) && out(FrameFormatVersion) && out(tr.meta.priority) && out2(tr.meta.source) &&
              out2(tr.meta.destination) && out2(tr.meta.data_spec);
    for (auto i = 0U; i < sizeof(std::uint64_t); i++)
    {
        ok = ok && out(0);
    }
    auto tmp_transfer_id = tr.meta.transfer_id;
    for (auto i = 0U; i < sizeof(std::uint64_t); i++)
    {
        ok = ok && out(static_cast<std::uint8_t>(tmp_transfer_id));
        tmp_transfer_id >>= BitsPerByte;
    }
    for (const auto x : FrameIndexEOTReference)
    {
        ok = ok && out(x);
    }
    for (const auto x : crc.getBytes())
    {
        ok = ok && out(x);
    }
    crc      = {};
    auto ptr = tr.payload;
    for (std::size_t i = 0U; i < tr.payload_len; i++)
    {
        ok = ok && out(*ptr);
        ++ptr;
    }
    for (const auto x : crc.getBytes())
    {
        ok = ok && out(x);
    }
    return ok && send_byte(FrameDelimiter);
}

}  // namespace detail

/// Bridges Kocherga/serial with the platform-specific serial port implementation.
/// Implement this and pass a reference to SerialNode.
class ISerialPort
{
public:
    /// Receive a single byte from the RX queue without blocking, if available. Otherwise, return an empty option.
    [[nodiscard]] virtual auto receive() -> std::optional<std::uint8_t> = 0;

    /// Send a single byte into the TX queue without blocking if there is free space available.
    /// Return true if enqueued or sent successfully; return false if no space available.
    [[nodiscard]] virtual auto send(const std::uint8_t b) -> bool = 0;

    virtual ~ISerialPort()           = default;
    ISerialPort()                    = default;
    ISerialPort(const ISerialPort&)  = delete;
    ISerialPort(const ISerialPort&&) = delete;
    auto operator=(const ISerialPort&) -> ISerialPort& = delete;
    auto operator=(const ISerialPort &&) -> ISerialPort& = delete;
};

/// Kocherga node implementing the UAVCAN/serial transport.
class SerialNode : public kocherga::INode
{
public:
    explicit SerialNode(ISerialPort& port) : port_(port) {}

    /// Resets the state of the frame parser. Call it when the communication channel is reinitialized.
    void reset() { stream_parser_.reset(); }

private:
    void poll(IReactor& reactor, const std::chrono::microseconds uptime) override
    {
        for (auto i = 0U; i < MaxBytesToProcessPerPoll; i++)
        {
            if (auto bt = port_.receive())
            {
                if (const auto tr = stream_parser_.update(*bt))
                {
                    processReceivedTransfer(reactor, uptime, *tr);
                }
            }
            else
            {
                break;
            }
        }
    }

    void processReceivedTransfer(IReactor& reactor, const std::chrono::microseconds uptime, const detail::Transfer& tr)
    {
        if (const auto resp_id = tr.meta.isResponse())
        {
            if (pending_request_meta_ && local_node_id_)
            {
                const bool match = (resp_id == pending_request_meta_->service_id) &&
                                   (tr.meta.source == pending_request_meta_->server_node_id) &&
                                   (tr.meta.destination == *local_node_id_) &&
                                   (tr.meta.transfer_id == pending_request_meta_->transfer_id);
                if (match)
                {
                    reactor.processResponse(tr.payload_len, tr.payload);
                    pending_request_meta_.reset();
                }
            }
        }
        else if (const auto req_id = tr.meta.isRequest())
        {
            if (local_node_id_ && (tr.meta.destination == (*local_node_id_)))
            {
                std::array<std::uint8_t, MaxSerializedRepresentationSize> buf{};
                if (const auto size =
                        reactor.processRequest(*req_id, tr.meta.source, tr.payload_len, tr.payload, buf.data()))
                {
                    detail::Transfer::Metadata meta{};
                    meta.priority    = tr.meta.priority;
                    meta.source      = *local_node_id_;
                    meta.destination = tr.meta.source;
                    meta.data_spec   = static_cast<PortID>(*req_id) | detail::Transfer::Metadata::DataSpecResponseMask;
                    meta.transfer_id = tr.meta.transfer_id;
                    (void) transmit({meta, *size, buf.data()});
                }
            }
        }
        else
        {
            (void) uptime;
        }
    }

    [[nodiscard]] auto sendRequest(const ServiceID           service_id,
                                   const NodeID              server_node_id,
                                   const TransferID          transfer_id,
                                   const std::size_t         payload_length,
                                   const std::uint8_t* const payload) -> bool override
    {
        if (local_node_id_)
        {
            detail::Transfer::Metadata meta{};
            meta.source      = *local_node_id_;
            meta.destination = server_node_id;
            meta.data_spec   = static_cast<PortID>(service_id) | detail::Transfer::Metadata::DataSpecRequestMask;
            meta.transfer_id = transfer_id;
            if (transmit({meta, payload_length, payload}))
            {
                pending_request_meta_ = PendingRequestMetadata{
                    server_node_id,
                    static_cast<PortID>(service_id),
                    transfer_id,
                };
                return true;
            }
        }
        return false;
    }

    void cancelRequest() override { pending_request_meta_.reset(); }

    auto publishMessage(const SubjectID           subject_id,
                        const TransferID          transfer_id,
                        const std::size_t         payload_length,
                        const std::uint8_t* const payload) -> bool override
    {
        if (local_node_id_)
        {
            detail::Transfer::Metadata meta{};
            meta.source      = *local_node_id_;
            meta.data_spec   = static_cast<PortID>(subject_id);
            meta.transfer_id = transfer_id;
            return transmit({meta, payload_length, payload});
        }
        return false;
    }

    [[nodiscard]] auto transmit(const detail::Transfer& tr) -> bool
    {
        return detail::transmit([this](const std::uint8_t b) { return port_.send(b); }, tr);
    }

    struct PendingRequestMetadata
    {
        NodeID     server_node_id{};
        PortID     service_id{};
        TransferID transfer_id{};
    };

    static constexpr auto MaxBytesToProcessPerPoll = MaxSerializedRepresentationSize * 3U;

    ISerialPort&                                          port_;
    detail::StreamParser<MaxSerializedRepresentationSize> stream_parser_;
    std::optional<NodeID>                                 local_node_id_;
    std::optional<PendingRequestMetadata>                 pending_request_meta_;
};

}  // namespace kocherga::serial
