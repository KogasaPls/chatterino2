// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/TwitchRoomIdResolver.hpp"

#include "common/QLogging.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchChannel.hpp"

#include <QApplicationStatic>
#include <QTimer>

#include <algorithm>
#include <iterator>
#include <vector>

namespace chatterino::detail {

using namespace std::chrono_literals;

std::optional<std::chrono::milliseconds> twitchChannelRoomIdRetryDelay(
    int failedAttempts, RoomIdLookupFailure failure)
{
    constexpr int MISSING_CHANNEL_ATTEMPTS = 5;
    if (failure == RoomIdLookupFailure::ChannelMissing &&
        failedAttempts >= MISSING_CHANNEL_ATTEMPTS)
    {
        return std::nullopt;
    }

    switch (std::max(failedAttempts, 0))
    {
        case 0:
            return 5s;
        case 1:
            return 30s;
        case 2:
            return 2min;
        default:
            return 5min;
    }
}

size_t twitchChannelRoomIdBatchSize(size_t pending)
{
    return std::min<size_t>(pending, 100);
}

int TwitchRoomIdRequestTracker::begin()
{
    this->resolving_ = true;
    return ++this->currentRequestId_;
}

bool TwitchRoomIdRequestTracker::finish(int requestId)
{
    if (!this->isCurrent(requestId))
    {
        return false;
    }
    this->resolving_ = false;
    return true;
}

bool TwitchRoomIdRequestTracker::isCurrent(int requestId) const
{
    return this->resolving_ && requestId == this->currentRequestId_;
}

bool TwitchRoomIdRequestTracker::isResolving() const
{
    return this->resolving_;
}

namespace {

class RoomIdResolveQueue;
RoomIdResolveQueue *roomIdResolveQueue();

class RoomIdResolveQueue : public QObject
{
public:
    struct Entry {
        std::weak_ptr<TwitchChannel> channel;
        int failedAttempts{};
    };

    RoomIdResolveQueue()
    {
        this->batchTimer_.setSingleShot(true);
        this->batchTimer_.setInterval(250);
        QObject::connect(&this->batchTimer_, &QTimer::timeout, this, [this] {
            this->makeNextRequest();
        });
    }

    void enqueue(Entry entry)
    {
        this->pending_.push_back(std::move(entry));
        if (!this->requests_.isResolving() && !this->batchTimer_.isActive())
        {
            this->batchTimer_.start();
        }
    }

private:
    static void scheduleRetry(std::weak_ptr<TwitchChannel> channel,
                              int failedAttempts, RoomIdLookupFailure failure)
    {
        auto retryDelay =
            twitchChannelRoomIdRetryDelay(failedAttempts, failure);
        if (!retryDelay)
        {
            return;
        }

        auto *queue = roomIdResolveQueue();
        QTimer::singleShot(*retryDelay, queue, [channel, failedAttempts] {
            auto locked = channel.lock();
            if (!locked || !locked->roomId().isEmpty())
            {
                return;
            }

            roomIdResolveQueue()->enqueue({channel, failedAttempts + 1});
        });
    }

    void makeNextRequest()
    {
        if (this->pending_.empty() || this->requests_.isResolving())
        {
            return;
        }
        const auto requestId = this->requests_.begin();

        const auto count = twitchChannelRoomIdBatchSize(this->pending_.size());
        std::vector<Entry> batch(
            std::make_move_iterator(this->pending_.begin()),
            std::make_move_iterator(this->pending_.begin() +
                                    static_cast<std::ptrdiff_t>(count)));
        this->pending_.erase(
            this->pending_.begin(),
            this->pending_.begin() + static_cast<std::ptrdiff_t>(count));

        QStringList logins;
        for (const auto &entry : batch)
        {
            if (auto channel = entry.channel.lock())
            {
                logins.append(channel->getName());
            }
        }

        if (logins.isEmpty())
        {
            this->requests_.finish(requestId);
            this->makeNextRequest();
            return;
        }

        QTimer::singleShot(REQUEST_TIMEOUT, this, [this, batch, requestId] {
            if (!this->requests_.isCurrent(requestId))
            {
                return;
            }

            qCWarning(chatterinoTwitch) << "Room ID lookup timed out; retrying";
            for (const auto &entry : batch)
            {
                scheduleRetry(entry.channel, entry.failedAttempts,
                              RoomIdLookupFailure::Transient);
            }
            this->requests_.finish(requestId);
            this->makeNextRequest();
        });

        getHelix()->fetchUsers(
            {}, logins,
            [this, batch, requestId](const std::vector<HelixUser> &users) {
                const bool current = this->requests_.isCurrent(requestId);
                for (const auto &entry : batch)
                {
                    auto channel = entry.channel.lock();
                    if (!channel)
                    {
                        continue;
                    }

                    auto user = std::find_if(
                        users.begin(), users.end(), [&](const auto &candidate) {
                            return candidate.login.compare(
                                       channel->getName(),
                                       Qt::CaseInsensitive) == 0;
                        });
                    if (user != users.end())
                    {
                        channel->setRoomIdFromHelix(user->id);
                        channel->updateDisplayName(user->displayName);
                    }
                    else if (current)
                    {
                        scheduleRetry(entry.channel, entry.failedAttempts,
                                      RoomIdLookupFailure::ChannelMissing);
                    }
                }

                if (current)
                {
                    this->requests_.finish(requestId);
                    this->makeNextRequest();
                }
            },
            [this, batch, requestId] {
                if (!this->requests_.isCurrent(requestId))
                {
                    return;
                }
                for (const auto &entry : batch)
                {
                    scheduleRetry(entry.channel, entry.failedAttempts,
                                  RoomIdLookupFailure::Transient);
                }
                this->requests_.finish(requestId);
                this->makeNextRequest();
            });
    }

    static constexpr std::chrono::seconds REQUEST_TIMEOUT{30};

    std::vector<Entry> pending_;
    TwitchRoomIdRequestTracker requests_;
    QTimer batchTimer_;
};

Q_APPLICATION_STATIC(RoomIdResolveQueue, roomIdResolveQueueHolder)

RoomIdResolveQueue *roomIdResolveQueue()
{
    return roomIdResolveQueueHolder();
}

}  // namespace

void resolveTwitchChannelRoomId(std::weak_ptr<TwitchChannel> channel,
                                int failedAttempts)
{
    if (auto locked = channel.lock(); !locked || !locked->roomId().isEmpty())
    {
        return;
    }
    roomIdResolveQueue()->enqueue({std::move(channel), failedAttempts});
}

}  // namespace chatterino::detail
