// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QHash>
#include <QSet>
#include <QString>

#include <optional>

namespace chatterino::detail {

bool isTwitchJoinConfirmation(const QString &command);

enum class MembershipAction {
    NoAction,
    QueueJoin,
    ScheduleRefresh,
    RejectJoin,
    Joined,
};

struct MembershipResult {
    MembershipAction action = MembershipAction::NoAction;
    bool joinedChanged{};
    bool sendPart{};
};

class TwitchChannelMembership
{
public:
    static constexpr qint64 JOIN_ACK_TIMEOUT_MS = 30LL * 1000;
    static constexpr qint64 JOIN_RETRY_DELAY_MS = 60LL * 1000;

    MembershipAction requestJoin(const QString &channelName, bool connected,
                                 qint64 now);
    /// Records that a JOIN is about to be written to the connection. Returns
    /// false when one is already in flight, which happens when a queued JOIN
    /// from before a reconnect is drained after the channel was queued again.
    bool markJoinSent(const QString &channelName, qint64 now);
    void cancelPendingJoin(const QString &channelName);
    MembershipResult leave(const QString &channelName);
    MembershipResult confirmJoined(const QString &channelName);
    MembershipResult confirmParted(const QString &channelName, qint64 now);
    QSet<QString> reset();
    MembershipResult removeChannel(const QString &channelName);

    bool wantsJoin(const QString &channelName) const;
    bool isPending(const QString &channelName) const;
    bool isJoined(const QString &channelName) const;
    std::optional<qint64> nextWakeAt() const;

private:
    QSet<QString> desired_;
    QSet<QString> pending_;
    QSet<QString> confirmed_;
    /// Channels Twitch may consider us a member of. Unlike joinSentAt_ this
    /// survives an unacknowledged JOIN, so a channel that was joined but whose
    /// USERSTATE and ROOMSTATE were lost is still PARTed when it goes idle.
    QSet<QString> possiblyJoined_;
    QHash<QString, qint64> joinSentAt_;
    QHash<QString, qint64> nextJoinAttempt_;
};

class TwitchChannelRetentionPolicy
{
public:
    static constexpr qint64 IDLE_TIMEOUT_MS = 2LL * 60 * 60 * 1000;

    void loadLastVisibleAt(const QString &channelName, qint64 timestamp);
    void loadAlwaysJoined(const QString &channelName);
    bool setAlwaysJoined(const QString &channelName, bool alwaysJoined);
    bool isAlwaysJoined(const QString &channelName) const;
    const QSet<QString> &alwaysJoinedChannels() const;

    void updateVisible(const QSet<QString> &visible, qint64 now);
    void prune(qint64 now);
    bool shouldKeepJoined(const QString &channelName, qint64 now) const;
    std::optional<qint64> nextExpiry(const QString &channelName) const;

    const QHash<QString, qint64> &lastVisibleAt() const;
    bool isDirty() const;
    void markSaved(qint64 now);
    qint64 lastSavedAt() const;

private:
    QSet<QString> alwaysJoined_;
    QHash<QString, qint64> lastVisibleAt_;
    QSet<QString> visible_;
    qint64 lastSavedAt_{};
    bool dirty_{};
};

}  // namespace chatterino::detail
