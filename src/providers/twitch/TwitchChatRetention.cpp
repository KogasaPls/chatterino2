// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/TwitchChatRetention.hpp"

#include <algorithm>
#include <limits>

namespace chatterino::detail {

bool isTwitchJoinConfirmation(const QString &command)
{
    return command == "USERSTATE" || command == "ROOMSTATE";
}

MembershipAction TwitchChannelMembership::requestJoin(
    const QString &channelName, bool connected, qint64 now)
{
    this->desired_.insert(channelName);

    if (!connected || this->confirmed_.contains(channelName))
    {
        return MembershipAction::NoAction;
    }

    const auto retryAt = this->nextJoinAttempt_.value(channelName, 0);
    if (retryAt > now)
    {
        return MembershipAction::NoAction;
    }
    if (retryAt > 0)
    {
        this->nextJoinAttempt_.remove(channelName);
    }

    if (this->pending_.contains(channelName))
    {
        const auto sentAt = this->joinSentAt_.value(channelName, 0);
        if (sentAt <= 0 || now - sentAt < JOIN_ACK_TIMEOUT_MS)
        {
            return MembershipAction::NoAction;
        }

        this->pending_.remove(channelName);
        this->joinSentAt_.remove(channelName);
        this->nextJoinAttempt_.insert(channelName, now + JOIN_RETRY_DELAY_MS);
        return MembershipAction::ScheduleRefresh;
    }

    this->pending_.insert(channelName);
    return MembershipAction::QueueJoin;
}

bool TwitchChannelMembership::markJoinSent(const QString &channelName,
                                           qint64 now)
{
    if (!this->desired_.contains(channelName) ||
        !this->pending_.contains(channelName) ||
        this->confirmed_.contains(channelName))
    {
        this->pending_.remove(channelName);
        this->joinSentAt_.remove(channelName);
        return false;
    }

    if (this->joinSentAt_.contains(channelName))
    {
        return false;
    }

    this->joinSentAt_.insert(channelName, now);
    this->possiblyJoined_.insert(channelName);
    return true;
}

void TwitchChannelMembership::cancelPendingJoin(const QString &channelName)
{
    this->pending_.remove(channelName);
    this->joinSentAt_.remove(channelName);
}

MembershipResult TwitchChannelMembership::leave(const QString &channelName)
{
    const bool wasConfirmed = this->confirmed_.remove(channelName) != 0;
    const bool possiblyJoined = this->possiblyJoined_.remove(channelName) != 0;

    this->desired_.remove(channelName);
    this->pending_.remove(channelName);
    this->joinSentAt_.remove(channelName);
    this->nextJoinAttempt_.remove(channelName);

    return {
        .joinedChanged = wasConfirmed,
        .sendPart = wasConfirmed || possiblyJoined,
    };
}

MembershipResult TwitchChannelMembership::confirmJoined(
    const QString &channelName)
{
    this->pending_.remove(channelName);
    this->joinSentAt_.remove(channelName);
    this->nextJoinAttempt_.remove(channelName);

    if (!this->desired_.contains(channelName))
    {
        return {.action = MembershipAction::RejectJoin, .sendPart = true};
    }

    const bool inserted = !this->confirmed_.contains(channelName);
    this->confirmed_.insert(channelName);
    return {
        .action =
            inserted ? MembershipAction::Joined : MembershipAction::NoAction,
        .joinedChanged = inserted,
    };
}

MembershipResult TwitchChannelMembership::confirmParted(
    const QString &channelName, qint64 now)
{
    this->pending_.remove(channelName);
    this->joinSentAt_.remove(channelName);
    this->possiblyJoined_.remove(channelName);
    const bool wasConfirmed = this->confirmed_.remove(channelName) != 0;
    const bool desired = this->desired_.contains(channelName);

    if (desired)
    {
        this->nextJoinAttempt_.insert(channelName, now + JOIN_RETRY_DELAY_MS);
    }

    return {
        .action = desired ? MembershipAction::ScheduleRefresh
                          : MembershipAction::NoAction,
        .joinedChanged = wasConfirmed,
    };
}

QSet<QString> TwitchChannelMembership::reset()
{
    auto previouslyConfirmed = this->confirmed_;
    this->pending_.clear();
    this->confirmed_.clear();
    this->possiblyJoined_.clear();
    this->joinSentAt_.clear();
    this->nextJoinAttempt_.clear();
    return previouslyConfirmed;
}

MembershipResult TwitchChannelMembership::removeChannel(
    const QString &channelName)
{
    return this->leave(channelName);
}

bool TwitchChannelMembership::wantsJoin(const QString &channelName) const
{
    return this->desired_.contains(channelName);
}

bool TwitchChannelMembership::isPending(const QString &channelName) const
{
    return this->pending_.contains(channelName);
}

bool TwitchChannelMembership::isJoined(const QString &channelName) const
{
    return this->confirmed_.contains(channelName);
}

std::optional<qint64> TwitchChannelMembership::nextWakeAt() const
{
    qint64 wakeAt = std::numeric_limits<qint64>::max();
    for (auto it = this->joinSentAt_.constBegin();
         it != this->joinSentAt_.constEnd(); ++it)
    {
        if (it.value() > 0)
        {
            wakeAt = std::min(wakeAt, it.value() + JOIN_ACK_TIMEOUT_MS);
        }
    }
    for (auto it = this->nextJoinAttempt_.constBegin();
         it != this->nextJoinAttempt_.constEnd(); ++it)
    {
        wakeAt = std::min(wakeAt, it.value());
    }
    if (wakeAt == std::numeric_limits<qint64>::max())
    {
        return std::nullopt;
    }
    return wakeAt;
}

void TwitchChannelRetentionPolicy::loadLastVisibleAt(const QString &channelName,
                                                     qint64 timestamp)
{
    this->lastVisibleAt_.insert(channelName, timestamp);
}

void TwitchChannelRetentionPolicy::loadAlwaysJoined(const QString &channelName)
{
    this->alwaysJoined_.insert(channelName);
}

bool TwitchChannelRetentionPolicy::setAlwaysJoined(const QString &channelName,
                                                   bool alwaysJoined)
{
    const bool wasAlwaysJoined = this->alwaysJoined_.contains(channelName);
    if (wasAlwaysJoined == alwaysJoined)
    {
        return false;
    }
    if (alwaysJoined)
    {
        this->alwaysJoined_.insert(channelName);
    }
    else
    {
        this->alwaysJoined_.remove(channelName);
    }
    return true;
}

bool TwitchChannelRetentionPolicy::isAlwaysJoined(
    const QString &channelName) const
{
    return this->alwaysJoined_.contains(channelName);
}

const QSet<QString> &TwitchChannelRetentionPolicy::alwaysJoinedChannels() const
{
    return this->alwaysJoined_;
}

void TwitchChannelRetentionPolicy::updateVisible(const QSet<QString> &visible,
                                                 qint64 now)
{
    if (visible == this->visible_)
    {
        return;
    }

    auto changed = visible;
    changed.unite(this->visible_);
    for (const auto &channelName : changed)
    {
        this->lastVisibleAt_.insert(channelName, now);
    }
    this->visible_ = visible;
    this->dirty_ = true;
}

void TwitchChannelRetentionPolicy::prune(qint64 now)
{
    for (auto it = this->lastVisibleAt_.begin();
         it != this->lastVisibleAt_.end();)
    {
        if (!this->visible_.contains(it.key()) &&
            (it.value() <= 0 || now - it.value() > IDLE_TIMEOUT_MS))
        {
            it = this->lastVisibleAt_.erase(it);
            this->dirty_ = true;
        }
        else
        {
            ++it;
        }
    }
}

bool TwitchChannelRetentionPolicy::shouldKeepJoined(const QString &channelName,
                                                    qint64 now) const
{
    const auto lastVisible = this->lastVisibleAt_.value(channelName, 0);
    return this->alwaysJoined_.contains(channelName) ||
           this->visible_.contains(channelName) ||
           (lastVisible > 0 && now - lastVisible <= IDLE_TIMEOUT_MS);
}

std::optional<qint64> TwitchChannelRetentionPolicy::nextExpiry(
    const QString &channelName) const
{
    if (this->alwaysJoined_.contains(channelName) ||
        this->visible_.contains(channelName))
    {
        return std::nullopt;
    }
    const auto lastVisible = this->lastVisibleAt_.value(channelName, 0);
    if (lastVisible <= 0)
    {
        return std::nullopt;
    }
    return lastVisible + IDLE_TIMEOUT_MS + 1;
}

const QHash<QString, qint64> &TwitchChannelRetentionPolicy::lastVisibleAt()
    const
{
    return this->lastVisibleAt_;
}

bool TwitchChannelRetentionPolicy::isDirty() const
{
    return this->dirty_;
}

void TwitchChannelRetentionPolicy::markSaved(qint64 now)
{
    this->lastSavedAt_ = now;
    this->dirty_ = false;
}

qint64 TwitchChannelRetentionPolicy::lastSavedAt() const
{
    return this->lastSavedAt_;
}

}  // namespace chatterino::detail
