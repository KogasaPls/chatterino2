// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>

namespace chatterino {

class TwitchChannel;

namespace detail {

enum class RoomIdLookupFailure {
    /// The request itself failed or timed out. The channel may well exist, so
    /// these are retried for as long as the channel is open.
    Transient,
    /// The request succeeded but Twitch did not return the login, so the
    /// channel most likely does not exist. These stop after a few attempts.
    ChannelMissing,
};

std::optional<std::chrono::milliseconds> twitchChannelRoomIdRetryDelay(
    int failedAttempts, RoomIdLookupFailure failure);
size_t twitchChannelRoomIdBatchSize(size_t pending);

class TwitchRoomIdRequestTracker
{
public:
    int begin();
    bool finish(int requestId);
    bool isCurrent(int requestId) const;
    bool isResolving() const;

private:
    bool resolving_{};
    int currentRequestId_{};
};

void resolveTwitchChannelRoomId(std::weak_ptr<TwitchChannel> channel,
                                int failedAttempts = 0);

}  // namespace detail
}  // namespace chatterino
