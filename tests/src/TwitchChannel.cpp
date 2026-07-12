// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/TwitchChannel.hpp"

#include "controllers/accounts/AccountController.hpp"
#include "messages/Message.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/Logging.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "providers/twitch/TwitchRoomIdResolver.hpp"
#include "Test.hpp"

#include <QDateTime>
#include <QString>

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

namespace chatterino {

namespace {

class LiveUpdateApplication : public mock::BaseApplication
{
public:
    AccountController *getAccounts() override
    {
        return &this->accounts;
    }

    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }

    ILogging *getChatLogger() override
    {
        return &this->logging;
    }

    AccountController accounts;
    mock::MockTwitchIrcServer twitch;
    mock::EmptyLogging logging;
};

}  // namespace

TEST(TwitchChannel, LiveUpdateGrouping)
{
    LiveUpdateApplication app;
    auto channel = std::make_shared<TwitchChannel>("testchannel");
    const auto addEmote = [&](const QString &name, const QDateTime &time) {
        channel->addOrReplaceLiveUpdatesAddRemove(true, "BTTV", {}, name, time);
    };
    const auto time =
        QDateTime::fromString("1984-04-20T21:37:00Z", Qt::ISODate);

    addEmote("FirstEmote", time);
    ASSERT_EQ(channel->countMessages(), 1);
    auto first = channel->getLastMessage();
    EXPECT_EQ(first->serverReceivedTime, time);

    // Grouping considers the date, not only the time.
    const auto nextDay = time.addDays(1);
    addEmote("SecondEmote", nextDay);
    ASSERT_EQ(channel->countMessages(), 2);
    EXPECT_EQ(channel->getMessageSnapshot().at(0), first);
    EXPECT_FALSE(first->messageText.contains("SecondEmote"));

    // An update less than 5 seconds later joins the group.
    addEmote("ThirdEmote", nextDay.addSecs(4));
    ASSERT_EQ(channel->countMessages(), 2);
    auto grouped = channel->getLastMessage();
    EXPECT_TRUE(grouped->messageText.contains("SecondEmote"));
    EXPECT_TRUE(grouped->messageText.contains("ThirdEmote"));
    EXPECT_EQ(grouped->serverReceivedTime, nextDay.addSecs(4));

    // Updates more than five seconds apart start a new group.
    addEmote("FourthEmote", nextDay.addSecs(10));
    EXPECT_EQ(channel->countMessages(), 3);
}

}  // namespace chatterino

namespace chatterino::detail {

using namespace std::chrono_literals;

TEST(TwitchChannelDetail_isUnknownCommand, good)
{
    // clang-format off
    std::vector<QString> cases{
        "/me hello",
        ".me hello",
        "/ hello",
        ". hello",
        "/ /hello",
        ". .hello",
        "/ .hello",
        ". /hello",
        ".", // this results in an empty message but not in an error (twitchdev/issues#1019)
        "/me",
        ".me",
        "..",
        "...",
        "....",
        "",
        "foo",
        "a",
        "!",
        ". .",
        ". ..",
        ".. ..",
        ".. .",
        "/ /",
        "/ .",
        ". /",
        ". ./",
        ".. /",
        ".. me",
        ". me",
    };
    // clang-format on

    for (const auto &input : cases)
    {
        ASSERT_FALSE(isUnknownCommand(input))
            << input << " should not be considered an unknown command";
    }
}

TEST(TwitchChannelDetail_isUnknownCommand, bad)
{
    // clang-format off
    std::vector<QString> cases{
        "/badcommand",
        ".badcommand",
        "/badcommand hello",
        ".badcommand hello",
        "/@badcommand hello",
        ".@badcommand hello",
        "/bann username ban reason",
        "/bann username",
        "//",
        "./",
        "./me",
        "./w",
        "/.",
        "/.me",
        "/.w",
        "/,me",
    };
    // clang-format on

    for (const auto &input : cases)
    {
        ASSERT_TRUE(isUnknownCommand(input))
            << input << " should be considered an unknown command";
    }
}

TEST(TwitchChannelDetail_roomIdRetryDelay, retriesTransientFailuresWithBackoff)
{
    using enum RoomIdLookupFailure;

    EXPECT_EQ(twitchChannelRoomIdRetryDelay(0, Transient), 5s);
    EXPECT_EQ(twitchChannelRoomIdRetryDelay(1, Transient), 30s);
    EXPECT_EQ(twitchChannelRoomIdRetryDelay(2, Transient), 2min);
    EXPECT_EQ(twitchChannelRoomIdRetryDelay(3, Transient), 5min);
}

TEST(TwitchChannelDetail_roomIdRetryDelay, keepsRetryingWhileRequestsFail)
{
    using enum RoomIdLookupFailure;

    // A Helix or network outage must not permanently cost a channel its live
    // status, so transient failures never stop being retried.
    EXPECT_EQ(twitchChannelRoomIdRetryDelay(5, Transient), 5min);
    EXPECT_EQ(twitchChannelRoomIdRetryDelay(500, Transient), 5min);
}

TEST(TwitchChannelDetail_roomIdRetryDelay, givesUpOnChannelsTwitchDoesNotKnow)
{
    using enum RoomIdLookupFailure;

    EXPECT_EQ(twitchChannelRoomIdRetryDelay(4, ChannelMissing), 5min);
    EXPECT_EQ(twitchChannelRoomIdRetryDelay(5, ChannelMissing), std::nullopt);
}

TEST(TwitchChannelDetail_roomIdBatchSize, capsRequestsAtHelixLimit)
{
    EXPECT_EQ(twitchChannelRoomIdBatchSize(0), 0);
    EXPECT_EQ(twitchChannelRoomIdBatchSize(1), 1);
    EXPECT_EQ(twitchChannelRoomIdBatchSize(100), 100);
    EXPECT_EQ(twitchChannelRoomIdBatchSize(101), 100);
}

TEST(TwitchChannelDetail_roomIdRequestTracker, ignoresLateRequestCompletion)
{
    TwitchRoomIdRequestTracker requests;
    const auto timedOut = requests.begin();
    EXPECT_TRUE(requests.finish(timedOut));

    const auto current = requests.begin();
    EXPECT_FALSE(requests.finish(timedOut));
    EXPECT_TRUE(requests.isCurrent(current));
    EXPECT_TRUE(requests.finish(current));
    EXPECT_FALSE(requests.isResolving());
}

}  // namespace chatterino::detail
