// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/TwitchIrcServer.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/Common.hpp"
#include "common/Env.hpp"
#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/bttv/BttvLiveUpdates.hpp"
#include "providers/bttv/liveupdates/BttvLiveUpdateMessages.hpp"  // IWYU pragma: keep
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/irc/IrcConnection2.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"  // IWYU pragma: keep
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvEventAPI.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/PostToThread.hpp"
#include "util/RatelimitBucket.hpp"
#include "util/Twitch.hpp"

#include <IrcCommand>
#include <IrcMessage>
#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMetaEnum>
#include <QStringList>

#include <algorithm>
#include <cassert>
#include <functional>
#include <limits>
#include <mutex>

using namespace std::chrono_literals;

namespace {

// Ratelimits for joinBucket_
constexpr int JOIN_RATELIMIT_BUDGET = 18;
constexpr int JOIN_RATELIMIT_COOLDOWN = 12500;

constexpr qint64 CHANNEL_ACTIVITY_SAVE_INTERVAL_MS = 5LL * 60 * 1000;

using namespace chatterino;

bool isCurrentUser(const QString &nick)
{
    return QString::compare(
               nick,
               getApp()->getAccounts()->twitch.getCurrent()->getUserName(),
               Qt::CaseInsensitive) == 0;
}

void sendHelixMessage(const std::shared_ptr<TwitchChannel> &channel,
                      const QString &message, const QString &replyParentId = {})
{
    auto broadcasterID = channel->roomId();
    if (broadcasterID.isEmpty())
    {
        channel->addSystemMessage(
            "Sending messages in this channel isn't possible.");
        return;
    }

    getHelix()->sendChatMessage(
        {
            .broadcasterID = broadcasterID,
            .senderID =
                getApp()->getAccounts()->twitch.getCurrent()->getUserId(),
            .message = message,
            .replyParentMessageID = replyParentId,
        },
        [weak = std::weak_ptr(channel)](const auto &res) {
            auto chan = weak.lock();
            if (!chan)
            {
                return;
            }

            if (res.isSent)
            {
                return;
            }

            if (res.dropReason)
            {
                chan->addSystemMessage(res.dropReason->message);
            }
            else
            {
                chan->addSystemMessage("Your message was not sent.");
            }
        },
        [weak = std::weak_ptr(channel)](auto error, auto message) {
            auto chan = weak.lock();
            if (!chan)
            {
                return;
            }

            if (message.isEmpty())
            {
                message = "(empty message)";
            }

            using Error = decltype(error);

            auto errorMessage = [&]() -> QString {
                switch (error)
                {
                    case Error::MissingText:
                        return "You can't send an empty message.";
                    case Error::BadRequest:
                        return "Failed to send message: " + message;
                    case Error::Forbidden:
                        return "You are not allowed to send messages in this "
                               "channel.";
                    case Error::MessageTooLarge:
                        return "Your message was too long.";
                    case Error::UserMissingScope:
                        return "Missing required scope. Re-login with your "
                               "account and try again.";
                    case Error::Forwarded:
                        return message;
                    case Error::Unknown:
                    default:
                        return "Unknown error: " + message;
                }
            }();
            chan->addSystemMessage(errorMessage);
        });
}

}  // namespace

namespace chatterino {

using namespace literals;

TwitchIrcServer::TwitchIrcServer()
    : whispersChannel(new Channel("/whispers", Channel::Type::TwitchWhispers))
    , mentionsChannel(new Channel("/mentions", Channel::Type::TwitchMentions))
    , liveChannel(new Channel("/live", Channel::Type::TwitchLive))
    , automodChannel(new Channel("/automod", Channel::Type::TwitchAutomod))
    , watchingChannel(Channel::getEmpty(), Channel::Type::TwitchWatching)
{
    // Initialize the connections
    // XXX: don't create write connection if there is no separate write connection.
    this->writeConnection_.reset(new IrcConnection);
    this->writeConnection_->moveToThread(
        QCoreApplication::instance()->thread());

    // Apply a leaky bucket rate limiting to JOIN messages. Membership is not
    // considered established until Twitch confirms the channel with USERSTATE
    // or ROOMSTATE.
    auto actuallyJoin = [&](QString message) {
        if (!this->channels.contains(message) || !this->readConnection_ ||
            !this->readConnection_->isConnected())
        {
            this->membership_.cancelPendingJoin(message);
            return;
        }

        if (!this->membership_.markJoinSent(
                message, QDateTime::currentMSecsSinceEpoch()))
        {
            return;
        }
        this->readConnection_->sendRaw("JOIN #" + message);
        this->scheduleChannelRetentionRefresh(
            detail::TwitchChannelMembership::JOIN_ACK_TIMEOUT_MS);
    };
    this->joinBucket_.reset(new RatelimitBucket(
        JOIN_RATELIMIT_BUDGET, JOIN_RATELIMIT_COOLDOWN, actuallyJoin, this));

    QObject::connect(this->writeConnection_.get(),
                     &Communi::IrcConnection::messageReceived, this,
                     &TwitchIrcServer::writeConnectionMessageReceived);
    this->signalHolder.managedConnect(
        this->writeConnection_->connectionLost, [this](bool timeout) {
            qCDebug(chatterinoIrc)
                << "Write connection reconnect requested. Timeout:" << timeout;
            this->writeConnection_->smartReconnect();
        });

    // Listen to read connection message signals
    this->readConnection_.reset(new IrcConnection);
    this->readConnection_->moveToThread(QCoreApplication::instance()->thread());

    QObject::connect(this->readConnection_.get(),
                     &Communi::IrcConnection::messageReceived, this,
                     &TwitchIrcServer::readConnectionMessageReceived);
    QObject::connect(this->readConnection_.get(),
                     &Communi::IrcConnection::privateMessageReceived, this,
                     &TwitchIrcServer::privateMessageReceived);
    QObject::connect(this->readConnection_.get(),
                     &Communi::IrcConnection::connected, this,
                     &TwitchIrcServer::onReadConnected);
    QObject::connect(this->readConnection_.get(),
                     &Communi::IrcConnection::disconnected, this,
                     &TwitchIrcServer::onDisconnected);
    this->signalHolder.managedConnect(
        this->readConnection_->connectionLost, [this](bool timeout) {
            qCDebug(chatterinoIrc)
                << "Read connection reconnect requested. Timeout:" << timeout;
            if (timeout)
            {
                // Show additional message since this is going to interrupt a
                // connection that is still "connected"
                this->addGlobalSystemMessage(
                    "Server connection timed out, reconnecting");
            }
            this->readConnection_->smartReconnect();
        });
    this->signalHolder.managedConnect(this->readConnection_->heartbeat, [this] {
        this->markChannelsConnected();
    });

    this->loadChannelActivity();
    this->loadPinnedChannels();

    this->channelRetentionTimer_.setSingleShot(true);
    QObject::connect(&this->channelRetentionTimer_, &QTimer::timeout, this,
                     [this] {
                         this->refreshChannelRetention();
                     });
}

void TwitchIrcServer::initialize()
{
    this->signalHolder.managedConnect(
        getApp()->getAccounts()->twitch.currentUserChanged, [this]() {
            postToThread([this] {
                this->connect();
            });
        });

    this->signalHolder.managedConnect(
        getApp()->getTwitchPubSub()->pointReward.redeemed, [this](auto &data) {
            QString channelId = data.value("channel_id").toString();
            if (channelId.isEmpty())
            {
                qCDebug(chatterinoApp)
                    << "Couldn't find channel id of point reward";
                return;
            }

            auto chan = this->getChannelOrEmptyByID(channelId);

            auto reward = ChannelPointReward(data);

            postToThread([chan, reward] {
                if (isAppAboutToQuit())
                {
                    return;
                }

                if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
                {
                    channel->addChannelPointReward(reward);
                }
            });
        });

    this->signalHolder.managedConnect(
        getApp()->getTwitchPubSub()->pinnedChatUpdates.pinned,
        [this](const QString &channelId) {
            auto chan = this->getChannelOrEmptyByID(channelId);
            postToThread([chan] {
                if (isAppAboutToQuit())
                {
                    return;
                }
                if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
                {
                    channel->refreshPinnedMessage();
                }
            });
        });

    this->signalHolder.managedConnect(
        getApp()->getTwitchPubSub()->pinnedChatUpdates.unpinned,
        [this](const QString &channelId) {
            auto chan = this->getChannelOrEmptyByID(channelId);
            postToThread([chan] {
                if (isAppAboutToQuit())
                {
                    return;
                }
                if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
                {
                    channel->clearPinnedMessage();
                }
            });
        });

    getSettings()->enableBTTVChannelEmotes.connect(
        [this] {
            this->reloadAllBTTVChannelEmotes();
        },
        this->signalHolder, false);
    getSettings()->enableFFZChannelEmotes.connect(
        [this] {
            this->reloadAllFFZChannelEmotes();
        },
        this->signalHolder, false);
    getSettings()->enableSevenTVChannelEmotes.connect(
        [this] {
            this->reloadAllSevenTVChannelEmotes();
        },
        this->signalHolder, false);

    this->signalHolder.managedConnect(
        getApp()->getWindows()->visibleChannelsChanged, [this] {
            this->scheduleChannelRetentionRefresh();
        });

    this->scheduleChannelRetentionRefresh();
}

void TwitchIrcServer::aboutToQuit()
{
    this->refreshChannelRetention(true);
    this->channelRetentionTimer_.stop();

    this->signalHolder.clear();

    this->channels.clear();
}

void TwitchIrcServer::loadChannelActivity()
{
    this->retentionPolicy_.markSaved(QDateTime::currentMSecsSinceEpoch());

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        getSettings()->twitchChannelLastActive.getValue().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        return;
    }

    const auto object = document.object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
    {
        if (!it.value().isDouble())
        {
            continue;
        }

        const auto timestamp = static_cast<qint64>(it.value().toDouble());
        if (timestamp <= 0)
        {
            continue;
        }

        this->retentionPolicy_.loadLastVisibleAt(cleanChannelName(it.key()),
                                                 timestamp);
    }
}

void TwitchIrcServer::saveChannelActivity()
{
    if (!this->retentionPolicy_.isDirty())
    {
        return;
    }

    QJsonObject object;
    const auto &lastVisibleAt = this->retentionPolicy_.lastVisibleAt();
    for (auto it = lastVisibleAt.constBegin(); it != lastVisibleAt.constEnd();
         ++it)
    {
        object.insert(it.key(), static_cast<double>(it.value()));
    }

    getSettings()->twitchChannelLastActive.setValue(QString::fromUtf8(
        QJsonDocument(object).toJson(QJsonDocument::Compact)));
    getSettings()->requestSave();

    this->retentionPolicy_.markSaved(QDateTime::currentMSecsSinceEpoch());
}

void TwitchIrcServer::loadPinnedChannels()
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        getSettings()->twitchAlwaysConnectedChannels.getValue().toUtf8(),
        &error);
    if (error.error != QJsonParseError::NoError || !document.isArray())
    {
        return;
    }

    for (const auto &value : document.array())
    {
        if (!value.isString())
        {
            continue;
        }

        const auto channelName = cleanChannelName(value.toString());
        if (!channelName.isEmpty() && !channelName.startsWith("/"))
        {
            this->retentionPolicy_.loadAlwaysJoined(channelName);
        }
    }
}

void TwitchIrcServer::savePinnedChannels()
{
    QStringList channelNames;
    const auto &alwaysJoined = this->retentionPolicy_.alwaysJoinedChannels();
    channelNames.reserve(alwaysJoined.size());
    for (const auto &channelName : alwaysJoined)
    {
        channelNames.push_back(channelName);
    }
    channelNames.sort(Qt::CaseInsensitive);

    getSettings()->twitchAlwaysConnectedChannels.setValue(QString::fromUtf8(
        QJsonDocument(QJsonArray::fromStringList(channelNames))
            .toJson(QJsonDocument::Compact)));
    getSettings()->requestSave();
}

void TwitchIrcServer::joinChannel(const QString &channelName)
{
    if (channelName.startsWith("/"))
    {
        return;
    }

    const bool connected =
        this->readConnection_ && this->readConnection_->isConnected();
    switch (this->membership_.requestJoin(channelName, connected,
                                          QDateTime::currentMSecsSinceEpoch()))
    {
        case detail::MembershipAction::QueueJoin:
            this->joinBucket_->send(channelName);
            break;
        case detail::MembershipAction::ScheduleRefresh:
            this->scheduleChannelRetentionRefresh(
                detail::TwitchChannelMembership::JOIN_RETRY_DELAY_MS);
            break;
        default:
            break;
    }
}

void TwitchIrcServer::leaveChannel(const QString &channelName)
{
    const auto result = this->membership_.leave(channelName);
    if (result.joinedChanged)
    {
        this->channelJoinedChanged.invoke(channelName, false);

        auto channel = this->getChannelOrEmpty(channelName);
        if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
        {
            twitchChannel->markDisconnected();
        }
    }

    if (result.sendPart && this->readConnection_ &&
        this->readConnection_->isConnected())
    {
        this->readConnection_->sendRaw("PART #" + channelName);
    }
}

bool TwitchIrcServer::confirmChannelJoined(const QString &dirtyChannelName)
{
    const auto channelName = cleanChannelName(dirtyChannelName);
    const auto result = this->membership_.confirmJoined(channelName);
    if (result.action == detail::MembershipAction::RejectJoin)
    {
        // A queued JOIN can race with the channel becoming inactive. Do not
        // expose a transient connected state; immediately undo the late JOIN.
        if (this->readConnection_ && this->readConnection_->isConnected())
        {
            this->readConnection_->sendRaw("PART #" + channelName);
        }
        return false;
    }

    if (result.joinedChanged)
    {
        this->channelJoinedChanged.invoke(channelName, true);
        this->addChannelConnectedMessage(this->getChannelOrEmpty(channelName));
    }
    return true;
}

bool TwitchIrcServer::confirmChannelParted(const QString &dirtyChannelName)
{
    const auto channelName = cleanChannelName(dirtyChannelName);
    const auto result = this->membership_.confirmParted(
        channelName, QDateTime::currentMSecsSinceEpoch());
    if (result.joinedChanged)
    {
        this->channelJoinedChanged.invoke(channelName, false);
    }

    if (result.action == detail::MembershipAction::ScheduleRefresh)
    {
        this->scheduleChannelRetentionRefresh(
            detail::TwitchChannelMembership::JOIN_RETRY_DELAY_MS);
    }

    auto channel = this->getChannelOrEmpty(channelName);
    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        twitchChannel->markDisconnected();
    }

    return !this->membership_.wantsJoin(channelName);
}

bool TwitchIrcServer::handleOwnJoinMessage(const QString &dirtyChannelName)
{
    auto channel = this->getChannelOrEmpty(dirtyChannelName);
    auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get());
    if (!twitchChannel)
    {
        return false;
    }

    // Retention joins and parts channels as they come in and out of view, so
    // upstream's per-join system message would repeat alongside the
    // connected/reconnected message that confirmChannelJoined already adds.
    twitchChannel->joined.invoke();
    return true;
}

void TwitchIrcServer::resetChannelMembership()
{
    for (const auto &channelName : this->membership_.reset())
    {
        this->channelJoinedChanged.invoke(channelName, false);
    }
}

void TwitchIrcServer::addChannelConnectedMessage(const ChannelPtr &channel)
{
    if (!channel || channel->isEmpty())
    {
        return;
    }

    auto connectedMsg = makeSystemMessage("connected");
    connectedMsg->flags.set(MessageFlag::ConnectedMessage);
    auto reconnected = makeSystemMessage("reconnected");
    reconnected->flags.set(MessageFlag::ConnectedMessage);

    MessagePtr last = channel->getLastMessage();
    const bool replaceMessage =
        last && last->flags.has(MessageFlag::DisconnectedMessage);

    if (replaceMessage)
    {
        channel->replaceMessage(last, reconnected);
    }
    else
    {
        channel->addMessage(connectedMsg, MessageContext::Original);
    }
}

void TwitchIrcServer::scheduleChannelRetentionRefresh(qint64 delayMs)
{
    const auto boundedDelay = static_cast<int>(
        std::clamp<qint64>(delayMs, 0, std::numeric_limits<int>::max()));

    if (!this->channelRetentionTimer_.isActive() ||
        this->channelRetentionTimer_.remainingTime() < 0 ||
        boundedDelay < this->channelRetentionTimer_.remainingTime())
    {
        this->channelRetentionTimer_.start(boundedDelay);
    }
}

void TwitchIrcServer::refreshChannelRetention(bool forceSave)
{
    assertInGuiThread();

    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto visibleNames = getApp()->getWindows()->getVisibleChannelNames();
    QSet<QString> visible;
    for (const auto &channelName : visibleNames)
    {
        if (!channelName.startsWith("/"))
        {
            visible.insert(channelName);
        }
    }

    this->retentionPolicy_.updateVisible(visible, now);
    this->retentionPolicy_.prune(now);

    std::vector<ChannelPtr> activeChannels;
    {
        std::lock_guard lock(this->channelMutex);

        activeChannels.reserve(this->channels.size());
        for (const auto &weak : this->channels)
        {
            if (auto channel = weak.lock())
            {
                activeChannels.push_back(channel);
            }
        }
    }

    std::ranges::stable_partition(activeChannels, [&](const auto &channel) {
        return visible.contains(channel->getName());
    });

    qint64 nextWakeAt = std::numeric_limits<qint64>::max();
    for (const auto &channel : activeChannels)
    {
        const auto &channelName = channel->getName();
        if (channelName.startsWith("/"))
        {
            continue;
        }

        if (this->retentionPolicy_.shouldKeepJoined(channelName, now))
        {
            this->joinChannel(channelName);
            if (const auto expiry =
                    this->retentionPolicy_.nextExpiry(channelName))
            {
                nextWakeAt = std::min(nextWakeAt, *expiry);
            }
        }
        else
        {
            this->leaveChannel(channelName);
        }
    }

    if (const auto membershipWakeAt = this->membership_.nextWakeAt())
    {
        nextWakeAt = std::min(nextWakeAt, *membershipWakeAt);
    }

    if (forceSave || now - this->retentionPolicy_.lastSavedAt() >=
                         CHANNEL_ACTIVITY_SAVE_INTERVAL_MS)
    {
        this->saveChannelActivity();
    }
    else if (this->retentionPolicy_.isDirty())
    {
        nextWakeAt =
            std::min(nextWakeAt, this->retentionPolicy_.lastSavedAt() +
                                     CHANNEL_ACTIVITY_SAVE_INTERVAL_MS);
    }

    if (nextWakeAt != std::numeric_limits<qint64>::max())
    {
        this->scheduleChannelRetentionRefresh(
            std::max<qint64>(0, nextWakeAt - now));
    }
}

void TwitchIrcServer::initializeConnection(IrcConnection *connection,
                                           ConnectionType type)
{
    std::shared_ptr<TwitchAccount> account =
        getApp()->getAccounts()->twitch.getCurrent();

    qCDebug(chatterinoTwitch) << "logging in as" << account->getUserName();

    // twitch.tv/tags enables IRCv3 tags on messages. See https://dev.twitch.tv/docs/irc/tags
    // twitch.tv/commands enables a bunch of miscellaneous command capabilities. See https://dev.twitch.tv/docs/irc/commands
    // twitch.tv/membership enables the JOIN/PART/NAMES commands. See https://dev.twitch.tv/docs/irc/membership
    // This is enabled so we receive USERSTATE messages when joining channels / typing messages, along with the other command capabilities
    QStringList caps{"twitch.tv/tags", "twitch.tv/commands"};
    if (type != ConnectionType::Write)
    {
        caps.push_back("twitch.tv/membership");
    }

    connection->network()->setSkipCapabilityValidation(true);
    connection->network()->setRequestedCapabilities(caps);

    QString username = account->getUserName();
    QString oauthToken = account->getOAuthToken();

    if (!oauthToken.startsWith("oauth:"))
    {
        oauthToken.prepend("oauth:");
    }

    connection->setUserName(username);
    connection->setNickName(username);
    connection->setRealName(username);

    if (!account->isAnon())
    {
        connection->setPassword(oauthToken);
    }

    // https://dev.twitch.tv/docs/irc#connecting-to-the-twitch-irc-server
    // SSL disabled: irc://irc.chat.twitch.tv:6667 (or port 80)
    // SSL enabled: irc://irc.chat.twitch.tv:6697 (or port 443)
    connection->setHost(Env::get().twitchServerHost);
    connection->setPort(Env::get().twitchServerPort);
    connection->setSecure(Env::get().twitchServerSecure);

    connection->open();
}

std::shared_ptr<Channel> TwitchIrcServer::createChannel(
    const QString &channelName)
{
    auto channel = std::make_shared<TwitchChannel>(channelName);
    channel->initialize();

    // We can safely ignore these signal connections since the TwitchIrcServer is only
    // ever destroyed when the full Application state is about to be destroyed, at which point
    // no Channel's should live
    // NOTE: CHANNEL_LIFETIME
    std::ignore = channel->sendMessageSignal.connect(
        [this, channel = std::weak_ptr(channel)](auto &msg, bool &sent) {
            auto c = channel.lock();
            if (!c)
            {
                return;
            }
            this->onMessageSendRequested(c, msg, sent);
        });
    std::ignore = channel->sendReplySignal.connect(
        [this, channel = std::weak_ptr(channel)](auto &msg, auto &replyId,
                                                 bool &sent) {
            auto c = channel.lock();
            if (!c)
            {
                return;
            }
            this->onReplySendRequested(c, msg, replyId, sent);
        });

    return channel;
}

void TwitchIrcServer::privateMessageReceived(
    Communi::IrcPrivateMessage *message)
{
    IrcMessageHandler::instance().handlePrivMessage(message, *this);
}

void TwitchIrcServer::readConnectionMessageReceived(
    Communi::IrcMessage *message)
{
    if (message->type() == Communi::IrcMessage::Type::Private)
    {
        // We already have a handler for private messages
        return;
    }

    const QString &command = message->command();

    auto &handler = IrcMessageHandler::instance();

    if (detail::isTwitchJoinConfirmation(command))
    {
        this->confirmChannelJoined(message->parameter(0));
    }

    // Below commands are normally enabled through the twitch.tv/membership CAP
    // REQ. We still handle them for fake/debug messages and compatibility.
    if (command == "JOIN")
    {
        if (isCurrentUser(message->nick()) &&
            this->handleOwnJoinMessage(message->parameter(0)))
        {
            return;
        }
        handler.handleJoinMessage(message);
    }
    else if (command == "PART")
    {
        if (isCurrentUser(message->nick()) &&
            this->confirmChannelParted(message->parameter(0)))
        {
            // Retention-initiated PARTs are normal and must not be presented as
            // a ban/removal event by IrcMessageHandler.
            return;
        }
        handler.handlePartMessage(message);
    }
    else if (command == "USERSTATE")
    {
        // Received USERSTATE upon JOINing a channel
        handler.handleUserStateMessage(message);
    }
    else if (command == "ROOMSTATE")
    {
        // Received ROOMSTATE upon JOINing a channel
        handler.handleRoomStateMessage(message);
    }
    else if (command == "CLEARCHAT")
    {
        handler.handleClearChatMessage(message);
    }
    else if (command == "CLEARMSG")
    {
        handler.handleClearMessageMessage(message);
    }
    else if (command == "USERNOTICE")
    {
        handler.handleUserNoticeMessage(message, *this);
    }
    else if (command == "NOTICE")
    {
        handler.handleNoticeMessage(
            static_cast<Communi::IrcNoticeMessage *>(message));
    }
    else if (command == "WHISPER")
    {
        handler.handleWhisperMessage(message);
    }
    else if (command == "RECONNECT")
    {
        this->addGlobalSystemMessage(
            "Twitch Servers requested us to reconnect, reconnecting");
        this->markChannelsConnected();
        this->connect();
    }
}

void TwitchIrcServer::writeConnectionMessageReceived(
    Communi::IrcMessage *message)
{
    const QString &command = message->command();

    auto &handler = IrcMessageHandler::instance();
    // Below commands enabled through the twitch.tv/commands CAP REQ
    if (command == "USERSTATE")
    {
        // Received USERSTATE upon sending PRIVMSG messages
        handler.handleUserStateMessage(message);
    }
    else if (command == "NOTICE")
    {
        // List of expected NOTICE messages on write connection
        // https://git.kotmisia.pl/Mm2PL/docs/src/branch/master/irc_msg_ids.md#command-results
        handler.handleNoticeMessage(
            static_cast<Communi::IrcNoticeMessage *>(message));
    }
    else if (command == "RECONNECT")
    {
        this->addGlobalSystemMessage(
            "Twitch Servers requested us to reconnect, reconnecting");
        this->connect();
    }
}

void TwitchIrcServer::onReadConnected()
{
    this->resetChannelMembership();
    this->refreshChannelRetention(true);
}

void TwitchIrcServer::onDisconnected()
{
    std::lock_guard<std::mutex> lock(this->channelMutex);

    MessageBuilder b(systemMessage, "disconnected");
    b->flags.set(MessageFlag::DisconnectedMessage);
    auto disconnectedMsg = b.release();

    for (std::weak_ptr<Channel> &weak : this->channels.values())
    {
        std::shared_ptr<Channel> chan = weak.lock();
        if (!chan)
        {
            continue;
        }

        // Retention leaves most channels parted. Upstream uses an empty room
        // ID as the "never joined" sentinel, which no longer holds here
        // because room IDs are resolved over Helix before the first JOIN.
        if (!this->membership_.isJoined(chan->getName()))
        {
            continue;
        }

        chan->addMessage(disconnectedMsg, MessageContext::Original);

        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->markDisconnected();
        }
    }

    this->resetChannelMembership();
}

std::shared_ptr<Channel> TwitchIrcServer::getCustomChannel(
    const QString &channelName)
{
    if (channelName == "/whispers")
    {
        return this->whispersChannel;
    }

    if (channelName == "/mentions")
    {
        return this->mentionsChannel;
    }

    if (channelName == "/live")
    {
        return this->liveChannel;
    }

    if (channelName == "/automod")
    {
        return this->automodChannel;
    }

    static auto getTimer = [this](ChannelPtr channel, int msBetweenMessages,
                                  bool addInitialMessages) {
        if (addInitialMessages)
        {
            for (auto i = 0; i < 1000; i++)
            {
                channel->addSystemMessage(QString::number(i + 1));
            }
        }

        auto *timer = new QTimer;
        QObject::connect(timer, &QTimer::timeout, this, [channel] {
            channel->addSystemMessage(QTime::currentTime().toString());
        });
        timer->start(msBetweenMessages);
        return timer;
    };

    if (channelName == "$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 500, true);

        return channel;
    }
    if (channelName == "$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 500, false);

        return channel;
    }
    if (channelName == "$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 250, true);

        return channel;
    }
    if (channelName == "$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 250, false);

        return channel;
    }
    if (channelName == "$$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 100, true);

        return channel;
    }
    if (channelName == "$$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 100, false);

        return channel;
    }
    if (channelName == "$$$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 50, true);

        return channel;
    }
    if (channelName == "$$$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 50, false);

        return channel;
    }
    if (channelName == "$$$$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 25, true);

        return channel;
    }
    if (channelName == "$$$$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 25, false);

        return channel;
    }

    return nullptr;
}

void TwitchIrcServer::forEachChannelAndSpecialChannels(
    std::function<void(ChannelPtr)> func)
{
    this->forEachChannel(func);

    func(this->whispersChannel);
    func(this->mentionsChannel);
    func(this->liveChannel);
    func(this->automodChannel);
}

std::shared_ptr<Channel> TwitchIrcServer::getChannelOrEmptyByID(
    const QString &channelId)
{
    std::lock_guard<std::mutex> lock(this->channelMutex);

    for (const auto &weakChannel : this->channels)
    {
        auto channel = weakChannel.lock();
        if (!channel)
        {
            continue;
        }

        auto twitchChannel = std::dynamic_pointer_cast<TwitchChannel>(channel);
        if (!twitchChannel)
        {
            continue;
        }

        if (twitchChannel->roomId() == channelId &&
            twitchChannel->getName().count(':') < 2)
        {
            return twitchChannel;
        }
    }

    return Channel::getEmpty();
}

bool TwitchIrcServer::prepareToSend(
    const std::shared_ptr<TwitchChannel> &channel)
{
    std::lock_guard<std::mutex> guard(this->lastMessageMutex_);

    auto &lastMessage = channel->hasHighRateLimit() ? this->lastMessageMod_
                                                    : this->lastMessagePleb_;
    size_t maxMessageCount = channel->hasHighRateLimit() ? 99 : 19;
    auto minMessageOffset = (channel->hasHighRateLimit() ? 100ms : 1100ms);

    auto now = std::chrono::steady_clock::now();

    // check if you are sending messages too fast
    if (!lastMessage.empty() && lastMessage.back() + minMessageOffset > now)
    {
        if (this->lastErrorTimeSpeed_ + 30s < now)
        {
            channel->addSystemMessage("You are sending messages too quickly.");

            this->lastErrorTimeSpeed_ = now;
        }
        return false;
    }

    // remove messages older than 30 seconds
    while (!lastMessage.empty() && lastMessage.front() + 32s < now)
    {
        lastMessage.pop();
    }

    // check if you are sending too many messages
    if (lastMessage.size() >= maxMessageCount)
    {
        if (this->lastErrorTimeAmount_ + 30s < now)
        {
            channel->addSystemMessage("You are sending too many messages.");

            this->lastErrorTimeAmount_ = now;
        }
        return false;
    }

    lastMessage.push(now);
    return true;
}

void TwitchIrcServer::onMessageSendRequested(
    const std::shared_ptr<TwitchChannel> &channel, const QString &message,
    bool &sent)
{
    sent = false;

    bool canSend = this->prepareToSend(channel);
    if (!canSend)
    {
        return;
    }

    if (getSettings()->shouldSendHelixChat())
    {
        sendHelixMessage(channel, message);
    }
    else
    {
        this->sendMessage(channel->getName(), message);
    }

    sent = true;
}

void TwitchIrcServer::onReplySendRequested(
    const std::shared_ptr<TwitchChannel> &channel, const QString &message,
    const QString &replyId, bool &sent)
{
    sent = false;

    bool canSend = this->prepareToSend(channel);
    if (!canSend)
    {
        return;
    }

    if (getSettings()->shouldSendHelixChat())
    {
        sendHelixMessage(channel, message, replyId);
    }
    else
    {
        this->sendRawMessage("@reply-parent-msg-id=" + replyId + " PRIVMSG #" +
                             channel->getName() + " :" + message);
    }
    sent = true;
}

const IndirectChannel &TwitchIrcServer::getWatchingChannel() const
{
    return this->watchingChannel;
}

void TwitchIrcServer::setWatchingChannel(ChannelPtr newWatchingChannel)
{
    assertInGuiThread();

    this->watchingChannel.reset(newWatchingChannel);
}

ChannelPtr TwitchIrcServer::getWhispersChannel() const
{
    return this->whispersChannel;
}

ChannelPtr TwitchIrcServer::getMentionsChannel() const
{
    return this->mentionsChannel;
}

ChannelPtr TwitchIrcServer::getLiveChannel() const
{
    return this->liveChannel;
}

ChannelPtr TwitchIrcServer::getAutomodChannel() const
{
    return this->automodChannel;
}

QString TwitchIrcServer::getLastUserThatWhisperedMe() const
{
    return this->lastUserThatWhisperedMe.get();
}

void TwitchIrcServer::setLastUserThatWhisperedMe(const QString &user)
{
    assertInGuiThread();

    this->lastUserThatWhisperedMe.set(user);
}

void TwitchIrcServer::initEventAPIs(BttvLiveUpdates *bttvLiveUpdates,
                                    SeventvEventAPI *seventvEventAPI)
{
    assertInGuiThread();

    if (bttvLiveUpdates != nullptr)
    {
        this->signalHolder.managedConnect(
            bttvLiveUpdates->signals_.emoteAdded, [&](const auto &data) {
                auto chan = this->getChannelOrEmptyByID(data.channelID);

                postToThread(
                    [chan, data] {
                        if (auto *channel =
                                dynamic_cast<TwitchChannel *>(chan.get()))
                        {
                            channel->addBttvEmote(data);
                        }
                    },
                    this);
            });
        this->signalHolder.managedConnect(
            bttvLiveUpdates->signals_.emoteUpdated, [&](const auto &data) {
                auto chan = this->getChannelOrEmptyByID(data.channelID);

                postToThread(
                    [chan, data] {
                        if (auto *channel =
                                dynamic_cast<TwitchChannel *>(chan.get()))
                        {
                            channel->updateBttvEmote(data);
                        }
                    },
                    this);
            });
        this->signalHolder.managedConnect(
            bttvLiveUpdates->signals_.emoteRemoved, [&](const auto &data) {
                auto chan = this->getChannelOrEmptyByID(data.channelID);

                postToThread(
                    [chan, data] {
                        if (auto *channel =
                                dynamic_cast<TwitchChannel *>(chan.get()))
                        {
                            channel->removeBttvEmote(data);
                        }
                    },
                    this);
            });
    }
    else
    {
        qCDebug(chatterinoBttv)
            << "Skipping initialization of Live Updates as it's disabled";
    }

    if (seventvEventAPI != nullptr)
    {
        this->signalHolder.managedConnect(
            seventvEventAPI->signals_.emoteAdded, [this](const auto &data) {
                postToThread(
                    [this, data] {
                        this->forEachSeventvEmoteSet(
                            data.emoteSetID, [data](TwitchChannel &chan) {
                                chan.addSeventvEmote(data);
                            });
                    },
                    this);
            });
        this->signalHolder.managedConnect(
            seventvEventAPI->signals_.emoteUpdated, [this](const auto &data) {
                postToThread(
                    [this, data] {
                        this->forEachSeventvEmoteSet(
                            data.emoteSetID, [data](TwitchChannel &chan) {
                                chan.updateSeventvEmote(data);
                            });
                    },
                    this);
            });
        this->signalHolder.managedConnect(
            seventvEventAPI->signals_.emoteRemoved, [this](const auto &data) {
                postToThread(
                    [this, data] {
                        this->forEachSeventvEmoteSet(
                            data.emoteSetID, [data](TwitchChannel &chan) {
                                chan.removeSeventvEmote(data);
                            });
                    },
                    this);
            });
        this->signalHolder.managedConnect(
            seventvEventAPI->signals_.userUpdated, [this](const auto &data) {
                // TODO: is it fine if this is called in non-gui-thread?
                this->forEachSeventvUser(data.userID,
                                         [data](TwitchChannel &chan) {
                                             chan.updateSeventvUser(data);
                                         });
            });
    }
    else
    {
        qCDebug(chatterinoSeventvEventAPI)
            << "Skipping initialization as the EventAPI is disabled";
    }
}

void TwitchIrcServer::reloadAllBTTVChannelEmotes()
{
    this->forEachChannel([](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->refreshBTTVChannelEmotes(false);
        }
    });
}

void TwitchIrcServer::reloadAllFFZChannelEmotes()
{
    this->forEachChannel([](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->refreshFFZChannelEmotes(false);
        }
    });
}

void TwitchIrcServer::reloadAllSevenTVChannelEmotes()
{
    this->forEachChannel([](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->refreshSevenTVChannelEmotes(false);
        }
    });
}

void TwitchIrcServer::forEachSeventvEmoteSet(
    const QString &emoteSetId, std::function<void(TwitchChannel &)> func)
{
    this->forEachChannel([emoteSetId, func](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get());
            channel->seventvEmoteSetID() == emoteSetId)
        {
            func(*channel);
        }
    });
}
void TwitchIrcServer::forEachSeventvUser(
    const QString &userId, std::function<void(TwitchChannel &)> func)
{
    this->forEachChannel([userId, func](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get());
            channel->seventvUserID() == userId)
        {
            func(*channel);
        }
    });
}

void TwitchIrcServer::dropSeventvChannel(const QString &userID,
                                         const QString &emoteSetID)
{
    if (!getApp()->getSeventvEventAPI())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(this->channelMutex);

    // ignore empty values
    bool skipUser = userID.isEmpty();
    bool skipSet = emoteSetID.isEmpty();

    bool foundUser = skipUser;
    bool foundSet = skipSet;
    for (std::weak_ptr<Channel> &weak : this->channels)
    {
        ChannelPtr chan = weak.lock();
        if (!chan)
        {
            continue;
        }

        auto *channel = dynamic_cast<TwitchChannel *>(chan.get());
        if (!foundSet && channel->seventvEmoteSetID() == emoteSetID)
        {
            foundSet = true;
        }
        if (!foundUser && channel->seventvUserID() == userID)
        {
            foundUser = true;
        }

        if (foundSet && foundUser)
        {
            break;
        }
    }

    if (!foundUser)
    {
        getApp()->getSeventvEventAPI()->unsubscribeUser(userID);
    }
    if (!foundSet)
    {
        getApp()->getSeventvEventAPI()->unsubscribeEmoteSet(emoteSetID);
    }
}

void TwitchIrcServer::markChannelsConnected()
{
    this->forEachChannel([this](const ChannelPtr &chan) {
        if (!this->membership_.isJoined(chan->getName()))
        {
            return;
        }

        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->markConnected();
        }
    });
}

void TwitchIrcServer::addFakeMessage(const QString &data)
{
    assertInGuiThread();

    auto *fakeMessage = Communi::IrcMessage::fromData(
        data.toUtf8(), this->readConnection_.get());

    if (fakeMessage->command() == "PRIVMSG")
    {
        this->privateMessageReceived(
            static_cast<Communi::IrcPrivateMessage *>(fakeMessage));
    }
    else
    {
        this->readConnectionMessageReceived(fakeMessage);
    }
}

void TwitchIrcServer::addGlobalSystemMessage(const QString &messageText)
{
    std::lock_guard<std::mutex> lock(this->channelMutex);

    MessageBuilder b(systemMessage, messageText);
    auto message = b.release();

    for (std::weak_ptr<Channel> &weak : this->channels.values())
    {
        std::shared_ptr<Channel> chan = weak.lock();
        if (!chan)
        {
            continue;
        }

        chan->addMessage(message, MessageContext::Original);
    }
}

void TwitchIrcServer::forEachChannel(std::function<void(ChannelPtr)> func)
{
    std::lock_guard<std::mutex> lock(this->channelMutex);

    for (std::weak_ptr<Channel> &weak : this->channels.values())
    {
        ChannelPtr chan = weak.lock();
        if (!chan)
        {
            continue;
        }

        func(chan);
    }
}

void TwitchIrcServer::connect()
{
    assertInGuiThread();

    this->disconnect();

    {
        std::scoped_lock l(this->connectionMutex_);
        initializeConnection(this->writeConnection_.get(),
                             ConnectionType::Write);
        initializeConnection(this->readConnection_.get(), ConnectionType::Read);
    }
}

void TwitchIrcServer::disconnect()
{
    std::lock_guard<std::mutex> locker(this->connectionMutex_);

    this->readConnection_->close();
    this->writeConnection_->close();
}

void TwitchIrcServer::sendMessage(const QString &channelName,
                                  const QString &message)
{
    this->sendRawMessage("PRIVMSG #" + channelName + " :" + message);
}

void TwitchIrcServer::sendRawMessage(const QString &rawMessage)
{
    std::lock_guard<std::mutex> locker(this->connectionMutex_);

    this->writeConnection_->sendRaw(rawMessage);
}

ChannelPtr TwitchIrcServer::getOrAddChannel(const QString &dirtyChannelName)
{
    auto channelName = cleanChannelName(dirtyChannelName);

    // try get channel
    ChannelPtr chan = this->getChannelOrEmpty(channelName);
    if (chan != Channel::getEmpty())
    {
        return chan;
    }

    std::lock_guard<std::mutex> lock(this->channelMutex);

    // value doesn't exist
    chan = this->createChannel(channelName);
    auto *twitchChannel = dynamic_cast<TwitchChannel *>(chan.get());
    if (!chan || !twitchChannel)
    {
        return Channel::getEmpty();
    }

    this->channels.insert(channelName, chan);
    this->signalHolder.managedConnect(twitchChannel->destroyed, [this,
                                                                 channelName] {
        // fourtf: issues when the server itself is destroyed

        qCDebug(chatterinoIrc) << "[TwitchIrcServer::addChannel]" << channelName
                               << "was destroyed";
        this->channels.remove(channelName);
        this->scheduleChannelRetentionRefresh();

        const auto membership = this->membership_.removeChannel(channelName);
        if (membership.joinedChanged)
        {
            this->channelJoinedChanged.invoke(channelName, false);
        }

        if (membership.sendPart && this->readConnection_ &&
            this->readConnection_->isConnected())
        {
            // HACK(mm2pl): This prevents custom invalid twitch channels used by plugins from being joined
            if (!channelName.startsWith("/"))
            {
                this->readConnection_->sendRaw("PART #" + channelName);
            }
        }
    });

    // Normal channels are joined only after becoming visible. A channel
    // explicitly pinned by the user is an exception and joins immediately.
    if (this->retentionPolicy_.isAlwaysJoined(channelName))
    {
        this->joinChannel(channelName);
    }

    this->scheduleChannelRetentionRefresh();
    return chan;
}

ChannelPtr TwitchIrcServer::getChannelOrEmpty(const QString &dirtyChannelName)
{
    auto channelName = cleanChannelName(dirtyChannelName);

    std::lock_guard<std::mutex> lock(this->channelMutex);

    // try get special channel
    ChannelPtr chan = this->getCustomChannel(channelName);
    if (chan)
    {
        return chan;
    }

    // value exists
    auto it = this->channels.find(channelName);
    if (it != this->channels.end())
    {
        chan = it.value().lock();

        if (chan)
        {
            return chan;
        }
    }

    return Channel::getEmpty();
}

bool TwitchIrcServer::isChannelJoined(const QString &dirtyChannelName) const
{
    return this->membership_.isJoined(cleanChannelName(dirtyChannelName));
}

bool TwitchIrcServer::isChannelPinned(const QString &dirtyChannelName) const
{
    return this->retentionPolicy_.isAlwaysJoined(
        cleanChannelName(dirtyChannelName));
}

void TwitchIrcServer::setChannelPinned(const QString &dirtyChannelName,
                                       bool pinned)
{
    assertInGuiThread();

    const auto channelName = cleanChannelName(dirtyChannelName);
    if (channelName.isEmpty() || channelName.startsWith("/"))
    {
        return;
    }

    if (!this->retentionPolicy_.setAlwaysJoined(channelName, pinned))
    {
        return;
    }

    this->savePinnedChannels();

    if (pinned)
    {
        this->joinChannel(channelName);
    }
    else
    {
        // Return to the normal visibility and idle-timeout policy.
        this->refreshChannelRetention();
    }
}

}  // namespace chatterino
