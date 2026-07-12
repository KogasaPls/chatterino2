// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/TwitchChatRetention.hpp"

#include <gtest/gtest.h>

namespace chatterino::detail {

using enum MembershipAction;

TEST(TwitchIrcCapabilities, ConfirmsJoinsFromStateMessagesOnly)
{
    EXPECT_TRUE(isTwitchJoinConfirmation("USERSTATE"));
    EXPECT_TRUE(isTwitchJoinConfirmation("ROOMSTATE"));
    EXPECT_FALSE(isTwitchJoinConfirmation("JOIN"));
}

TEST(TwitchChannelMembership, RejectsLateJoinAfterChannelBecomesUndesired)
{
    TwitchChannelMembership membership;

    EXPECT_EQ(membership.requestJoin("forsen", true, 1000), QueueJoin);
    EXPECT_TRUE(membership.markJoinSent("forsen", 1000));
    EXPECT_TRUE(membership.leave("forsen").sendPart);

    const auto confirmation = membership.confirmJoined("forsen");
    EXPECT_EQ(confirmation.action, RejectJoin);
    EXPECT_FALSE(membership.isJoined("forsen"));
}

TEST(TwitchChannelMembership, RetriesJoinAfterAcknowledgementTimeout)
{
    TwitchChannelMembership membership;

    EXPECT_EQ(membership.requestJoin("forsen", true, 1000), QueueJoin);
    EXPECT_TRUE(membership.markJoinSent("forsen", 1000));
    EXPECT_EQ(membership.requestJoin("forsen", true, 31000), ScheduleRefresh);
    EXPECT_EQ(membership.nextWakeAt(), 91000);
    EXPECT_EQ(membership.requestJoin("forsen", true, 90999), NoAction);
    EXPECT_EQ(membership.requestJoin("forsen", true, 91000), QueueJoin);
    EXPECT_EQ(membership.nextWakeAt(), std::nullopt);
}

TEST(TwitchChannelMembership, PartsChannelWhoseJoinWasNeverAcknowledged)
{
    TwitchChannelMembership membership;

    EXPECT_EQ(membership.requestJoin("forsen", true, 1000), QueueJoin);
    EXPECT_TRUE(membership.markJoinSent("forsen", 1000));

    // Twitch never sent USERSTATE or ROOMSTATE, so the join is retried.
    const auto afterTimeout =
        1000 + TwitchChannelMembership::JOIN_ACK_TIMEOUT_MS + 1;
    EXPECT_EQ(membership.requestJoin("forsen", true, afterTimeout),
              ScheduleRefresh);

    // Twitch may still consider us a member, so hiding the channel now has to
    // send a PART even though nothing was ever confirmed.
    const auto result = membership.leave("forsen");
    EXPECT_TRUE(result.sendPart);
    EXPECT_FALSE(result.joinedChanged);
}

TEST(TwitchChannelMembership, DoesNotSendASecondJoinForAQueuedChannel)
{
    TwitchChannelMembership membership;

    EXPECT_EQ(membership.requestJoin("forsen", true, 1000), QueueJoin);
    EXPECT_TRUE(membership.markJoinSent("forsen", 1000));

    // A JOIN left over in the rate limit bucket from before a reconnect must
    // not put a duplicate on the wire.
    EXPECT_FALSE(membership.markJoinSent("forsen", 1100));
}

TEST(TwitchChannelMembership, DoesNotPartAChannelWhoseJoinNeverLeftTheBucket)
{
    TwitchChannelMembership membership;

    EXPECT_EQ(membership.requestJoin("forsen", true, 1000), QueueJoin);

    const auto result = membership.leave("forsen");
    EXPECT_FALSE(result.sendPart);
    EXPECT_FALSE(membership.markJoinSent("forsen", 1100));
}

TEST(TwitchChannelMembership, ResetsConfirmedChannelsOnReconnect)
{
    TwitchChannelMembership membership;
    membership.requestJoin("forsen", true, 1000);
    membership.markJoinSent("forsen", 1000);
    EXPECT_EQ(membership.confirmJoined("forsen").action, Joined);

    EXPECT_EQ(membership.reset(), QSet<QString>{"forsen"});
    EXPECT_FALSE(membership.isJoined("forsen"));
    EXPECT_EQ(membership.requestJoin("forsen", true, 2000), QueueJoin);
}

TEST(TwitchChannelMembership, CancelsQueuedJoinWhenConnectionDrops)
{
    TwitchChannelMembership membership;
    EXPECT_EQ(membership.requestJoin("forsen", true, 1000), QueueJoin);

    membership.cancelPendingJoin("forsen");

    EXPECT_TRUE(membership.wantsJoin("forsen"));
    EXPECT_FALSE(membership.isPending("forsen"));
    EXPECT_EQ(membership.requestJoin("forsen", true, 2000), QueueJoin);
}

TEST(TwitchChannelRetentionPolicy, RetainsRecentlyVisibleChannelsUntilTimeout)
{
    TwitchChannelRetentionPolicy policy;
    policy.updateVisible(QSet<QString>{"forsen"}, 1000);
    policy.updateVisible({}, 2000);

    EXPECT_TRUE(policy.shouldKeepJoined("forsen", 2000));
    EXPECT_EQ(policy.nextExpiry("forsen"),
              2000 + TwitchChannelRetentionPolicy::IDLE_TIMEOUT_MS + 1);
    EXPECT_FALSE(policy.shouldKeepJoined(
        "forsen", 2000 + TwitchChannelRetentionPolicy::IDLE_TIMEOUT_MS + 1));
}

TEST(TwitchChannelRetentionPolicy, AlwaysJoinedChannelDoesNotExpire)
{
    TwitchChannelRetentionPolicy policy;
    policy.setAlwaysJoined("forsen", true);

    EXPECT_TRUE(policy.shouldKeepJoined(
        "forsen", TwitchChannelRetentionPolicy::IDLE_TIMEOUT_MS * 2));
    EXPECT_EQ(policy.nextExpiry("forsen"), std::nullopt);
}

}  // namespace chatterino::detail
