#include "chatd.h"
#include "chatdICrypto.h"
#include <base/cservices.h>
#include <gcmpp.h>
#include <retryHandler.h>
#include <libws_log.h>
#include <event2/dns.h>
#include <event2/dns_compat.h>
#include "base64.h"
#include <algorithm>
#include <random>

#ifdef __ANDROID__
    #include <sys/system_properties.h>
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #ifdef TARGET_OS_IPHONE
        #include <resolv.h>
    #endif
#endif

using namespace std;
using namespace promise;
using namespace karere;
#define CHATD_LOG_LISTENER_CALLS

#define ID_CSTR(id) id.toString().c_str()

// logging for a specific chatid - prepends the chatid and calls the normal logging macro
#define CHATID_LOG_DEBUG(fmtString,...) CHATD_LOG_DEBUG("%s: " fmtString, ID_CSTR(chatId()), ##__VA_ARGS__)
#define CHATID_LOG_WARNING(fmtString,...) CHATD_LOG_WARNING("%s: " fmtString, ID_CSTR(chatId()), ##__VA_ARGS__)
#define CHATID_LOG_ERROR(fmtString,...) CHATD_LOG_ERROR("%s: " fmtString, ID_CSTR(chatId()), ##__VA_ARGS__)

#ifdef CHATD_LOG_LISTENER_CALLS
    #define CHATD_LOG_LISTENER_CALL(fmtString,...) CHATID_LOG_DEBUG(fmtString, ##__VA_ARGS__)
#else
    #define CHATD_LOG_LISTENER_CALL(...)
#endif

#ifdef CHATD_LOG_CRYPTO_CALLS
    #define CHATD_LOG_CRYPTO_CALL(fmtString,...) CHATID_LOG_DEBUG(fmtString, ##__VA_ARGS__)
#else
    #define CHATD_LOG_CRYPTO_CALL(...)
#endif

#ifdef CHATD_LOG_DB_CALLS
    #define CHATD_LOG_DB_CALL(fmtString,...) CHATID_LOG_DEBUG(fmtString, ##__VA_ARGS__)
#else
    #define CHATD_LOG_DB_CALL(...)
#endif

#define CALL_LISTENER(methodName,...)                                                           \
    do {                                                                                        \
      try {                                                                                     \
          CHATD_LOG_LISTENER_CALL("Calling Listener::" #methodName "()");                       \
          mListener->methodName(__VA_ARGS__);                                                   \
      } catch(std::exception& e) {                                                              \
          CHATD_LOG_WARNING("Exception thrown from Listener::" #methodName "():\n%s", e.what());\
      }                                                                                         \
    } while(0)

#define CALL_CRYPTO(methodName,...)                                                             \
    do {                                                                                        \
      try {                                                                                     \
          CHATD_LOG_CRYPTO_CALL("Calling ICrypto::" #methodName "()");                          \
          mCrypto->methodName(__VA_ARGS__);                                                     \
      } catch(std::exception& e) {                                                              \
          CHATD_LOG_WARNING("Exception thrown from ICrypto::" #methodName "():\n%s", e.what()); \
      }                                                                                         \
    } while(0)

#define CALL_DB(methodName,...)                                                           \
    do {                                                                                        \
      try {                                                                                     \
          CHATD_LOG_DB_CALL("Calling DbInterface::" #methodName "()");                               \
          mDbInterface->methodName(__VA_ARGS__);                                                   \
      } catch(std::exception& e) {                                                              \
          CHATID_LOG_ERROR("Exception thrown from DbInterface::" #methodName "():\n%s", e.what());\
      }                                                                                         \
    } while(0)

#ifndef CHATD_ASYNC_MSG_CALLBACKS
    #define CHATD_ASYNC_MSG_CALLBACKS 1
#endif

namespace chatd
{

// message storage subsystem
// the message buffer can grow in two directions and is always contiguous, i.e. there are no "holes"
// there is no guarantee as to ordering

ws_base_s Client::sWebsocketContext;
bool Client::sWebsockCtxInitialized = false;

Client::Client(Id userId)
:mUserId(userId)
{
    if (!sWebsockCtxInitialized)
    {
        ws_global_init(&sWebsocketContext, services_get_event_loop(), services_dns_eventbase,
        [](struct bufferevent* bev, void* userp)
        {
            marshallCall([bev, userp]()
            {
                //CHATD_LOG_DEBUG("Read event");
                ws_read_callback(bev, userp);
            });
        },
        [](struct bufferevent* bev, short events, void* userp)
        {
            marshallCall([bev, events, userp]()
            {
                //CHATD_LOG_DEBUG("Buffer event 0x%x", events);
                ws_event_callback(bev, events, userp);
            });
        },
        [](int fd, short events, void* userp)
        {
            marshallCall([events, userp]()
            {
                //CHATD_LOG_DEBUG("Timer %p event", userp);
                ws_handle_marshall_timer_cb(0, events, userp);
            });
        });
//        ws_set_log_cb(ws_default_log_cb);
//        ws_set_log_level(LIBWS_TRACE);
        sWebsockCtxInitialized = true;
    }
}

#define checkLibwsCall(call, opname) \
    do {                             \
        int _cls_ret = (call);       \
        if (_cls_ret) throw std::runtime_error("Websocket error " +std::to_string(_cls_ret) + \
        " on operation " #opname);   \
    } while(0)

//Stale event from a previous connect attempt?
#define ASSERT_NOT_ANOTHER_WS(event)    \
    if (ws != self->mWebSocket) {       \
        CHATD_LOG_WARNING("Websocket '" event "' callback: ws param is not equal to self->mWebSocket, ignoring"); \
    }

Chat& Client::createChat(Id chatid, int shardNo, const std::string& url,
    Listener* listener, const karere::SetOfIds& users, ICrypto* crypto, uint32_t chatCreationTs)
{
    auto chatit = mChatForChatId.find(chatid);
    if (chatit != mChatForChatId.end())
    {
        CHATD_LOG_WARNING("Client::createChat: Chat with chatid %s already exists, returning existing instance", ID_CSTR(chatid));
        return *chatit->second;
    }

    // instantiate a Connection object for this shard if needed
    Connection* conn;
    auto it = mConnections.find(shardNo);
    if (it == mConnections.end())
    {
        conn = new Connection(*this, shardNo);
        mConnections.emplace(std::piecewise_construct,
            std::forward_as_tuple(shardNo), std::forward_as_tuple(conn));
    }
    else
    {
        conn = it->second.get();
    }

    if (!url.empty())
    {
        conn->mUrl.parse(url);
    }
    // map chatid to this shard
    mConnectionForChatId[chatid] = conn;

    // always update the URL to give the API an opportunity to migrate chat shards between hosts
    Chat* chat = new Chat(*conn, chatid, listener, users, chatCreationTs, crypto);
    // add chatid to the connection's chatids
    conn->mChatIds.insert(chatid);
    mChatForChatId.emplace(chatid, std::shared_ptr<Chat>(chat));
    return *chat;
}
void Client::sendKeepalive()
{
    for (auto& conn: mConnections)
    {
        conn.second->sendKeepalive(mKeepaliveType);
    }
}

void Client::notifyUserIdle()
{
    if (mKeepaliveType == OP_KEEPALIVEAWAY)
        return;
    mKeepaliveType = OP_KEEPALIVEAWAY;
    sendKeepalive();
}

void Client::notifyUserActive()
{
    if (mKeepaliveType == OP_KEEPALIVE)
        return;
    mKeepaliveType = OP_KEEPALIVE;
    sendKeepalive();
}

void Chat::connect(const std::string& url)
{
    // attempt a connection ONLY if this is a new shard.
    if (mConnection.state() == Connection::kStateNew)
    {
        mConnection.reconnect(url)
        .fail([this](const promise::Error& err)
        {
            CHATID_LOG_ERROR("Error connecting to server: %s", err.what());
        });
    }
    else if (mConnection.isOnline())
    {
        login();
    }
}

void Chat::disconnect()
{
    disable(true);
    setOnlineState(kChatStateOffline);
}

void Chat::login()
{
    ChatDbInfo info;
    mDbInterface->getHistoryInfo(info);
    mOldestKnownMsgId = info.oldestDbId;
    if (mOldestKnownMsgId) //if we have local history
        joinRangeHist(info);
    else
        join();
}

void Connection::websockConnectCb(ws_t ws, void* arg)
{
    Connection* self = static_cast<Connection*>(arg);
    ASSERT_NOT_ANOTHER_WS("connect");
    CHATD_LOG_DEBUG("Chatd connected to shard %d", self->mShardNo);
    ::marshallCall([self]()
    {
        self->mState = kStateConnected;
        assert(!self->mConnectPromise.done());
        self->mConnectPromise.resolve();
    });
}

void Connection::websockCloseCb(ws_t ws, int errcode, int errtype, const char *preason,
                                size_t reason_len, void *arg)
{
    auto self = static_cast<Connection*>(arg);
    ASSERT_NOT_ANOTHER_WS("close/error");
    std::string reason;
    if (preason)
        reason.assign(preason, reason_len);

    //we don't want to initiate websocket reconnect from within a websocket callback
    marshallCall([self, reason, errcode, errtype]()
    {
        self->onSocketClose(errcode, errtype, reason);
    });
}

void Connection::onSocketClose(int errcode, int errtype, const std::string& reason)
{
    CHATD_LOG_WARNING("Socket close on connection to shard %d. Reason: %s",
        mShardNo, reason.c_str());
    if (errtype == WS_ERRTYPE_DNS)
    {
        CHATD_LOG_WARNING("->DNS error: forcing libevent to re-read /etc/resolv.conf");
        evdns_base_clear_host_addresses(services_dns_eventbase);
        //if we didn't have our network interface up at app startup, and resolv.conf is
        //genereated dynamically, dns may never work unless we re-read the resolv.conf file
#ifdef _WIN32
        evdns_config_windows_nameservers();
#elif defined (__ANDROID__)
        char server[PROP_VALUE_MAX];
        if (__system_property_get("net.dns1", server) > 0) {
            evdns_base_nameserver_ip_add(services_dns_eventbase, server);
        }
#elif defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
        struct __res_state res;
        res_ninit(&res);
        union res_sockaddr_union addrs[MAXNS];
        int count = res_getservers(&res, addrs, MAXNS);
        if (count > 0) {
            if (addrs->sin.sin_family == AF_INET) {
                if (!addrs->sin.sin_port) {
                    addrs->sin.sin_port = 53;
                }
                evdns_base_nameserver_sockaddr_add(services_dns_eventbase, (struct sockaddr*)(&addrs->sin), sizeof(struct sockaddr_in), 0);
            } else if (addrs->sin6.sin6_family == AF_INET6) {
                if (!addrs->sin6.sin6_port) {
                    addrs->sin6.sin6_port = 53;
                }
                evdns_base_nameserver_sockaddr_add(services_dns_eventbase, (struct sockaddr*)(&addrs->sin6), sizeof(struct sockaddr_in6), 0);
            } else {
                fprintf(stderr, "Unknown address family for DNS server.");
            }
        }
        res_nclose(&res);
#else
        evdns_base_resolv_conf_parse(services_dns_eventbase,
            DNS_OPTIONS_ALL & (~DNS_OPTION_SEARCH), "/etc/resolv.conf");
#endif
    }
    disableInactivityTimer();
    for (auto& chatid: mChatIds)
    {
        auto& chat = mClient.chats(chatid);
        chat.onDisconnect();
    }

    if (mTerminating)
    {
        if (!mDisconnectPromise.done())
            mDisconnectPromise.resolve(); //may delete this
        return;
    }

    if (mState < kStateLoggedIn) //tell retry controller that the connect attempt failed
    {
        assert(!mLoginPromise.done());
        mConnectPromise.reject(reason, errcode, errtype);
        mLoginPromise.reject(reason, errcode, errtype);
    }
    else
    {
        CHATD_LOG_DEBUG("Socket close and state is not kLoggedIn (but %d), start retry controller", mState);
        mState = kStateDisconnected;
        reconnect(); //start retry controller
    }
}

void Connection::disableInactivityTimer()
{
    if (mInactivityTimer)
    {
        cancelInterval(mInactivityTimer);
        mInactivityTimer = 0;
    }
}
bool Connection::sendKeepalive(uint8_t opcode)
{
    CHATD_LOG_DEBUG("shard %d: send %s", mShardNo, Command::opcodeToStr(opcode));
    return sendBuf(Command(opcode));
}

Promise<void> Connection::reconnect(const std::string& url)
{
    try
    {
        if (mState >= kStateConnecting) //would be good to just log and return, but we have to return a promise
            throw std::runtime_error(std::string("Already connecting/connected to shard ")+std::to_string(mShardNo));
        if (!url.empty())
        {
            mUrl.parse(url);
        }
        else
        {
            if (!mUrl.isValid())
                throw std::runtime_error("No valid URL provided and current URL is not valid");
        }

        mState = kStateConnecting;
        return retry("chatd", [this](int no)
        {
            reset();
            mConnectPromise = Promise<void>();
            mLoginPromise = Promise<void>();
            mDisconnectPromise = Promise<void>();
            CHATD_LOG_DEBUG("Chatd connecting to shard %d...", mShardNo);
            checkLibwsCall((ws_init(&mWebSocket, &Client::sWebsocketContext)), "create socket");
            ws_set_onconnect_cb(mWebSocket, &websockConnectCb, this);
            ws_set_onclose_cb(mWebSocket, &websockCloseCb, this);
            ws_set_onmsg_cb(mWebSocket,
            [](ws_t ws, char *msg, uint64_t len, int binary, void *arg)
            {
                Connection* self = static_cast<Connection*>(arg);
                ASSERT_NOT_ANOTHER_WS("message");
                self->mInactivityBeats = 0;
                self->execCommand(StaticBuffer(msg, len));
            }, this);

            if (mUrl.isSecure)
            {
                ws_set_ssl_state(mWebSocket, LIBWS_SSL_SELFSIGNED);
            }
            for (auto& chatid: mChatIds)
            {
                auto& chat = mClient.chats(chatid);
                if (!chat.isDisabled())
                    chat.setOnlineState(kChatStateConnecting);
            }
            checkLibwsCall((ws_connect(mWebSocket, mUrl.host.c_str(), mUrl.port, (mUrl.path).c_str(), services_http_use_ipv6)), "connect");
            return mConnectPromise
            .then([this]() -> promise::Promise<void>
            {
                assert(mState >= kStateConnected);
                enableInactivityTimer();
                return rejoinExistingChats();
            });
        }, nullptr, 0, 0, KARERE_RECONNECT_DELAY_MAX, KARERE_RECONNECT_DELAY_INITIAL);
    }
    KR_EXCEPTION_TO_PROMISE(kPromiseErrtype_chatd);
}

void Connection::enableInactivityTimer()
{
    if (mInactivityTimer)
        return;

    mInactivityTimer = setInterval([this]()
    {
        if (mInactivityBeats++ > 3)
        {
            mState = kStateDisconnected;
            disableInactivityTimer();
            CHATD_LOG_WARNING("Connection to shard %d inactive for too long, reconnecting...",
                mShardNo);
            reconnect();
        }
    }, 10000);
}

promise::Promise<void> Connection::disconnect(int timeoutMs) //should be graceful disconnect
{
    mTerminating = true;
    if (!mWebSocket)
    {
        onSocketClose(0, 0, "terminating");
        return promise::Void();
    }
    auto wptr = getDelTracker();
    setTimeout([this, wptr]()
    {
        if (wptr.deleted())
            return;
        if (!mDisconnectPromise.done())
            mDisconnectPromise.resolve();
    }, timeoutMs);
    ws_close(mWebSocket);
    return mDisconnectPromise;
}

void Connection::retryPendingConnection()
{
    if (mUrl.isValid())
    {
        mState = kStateDisconnected;
        disableInactivityTimer();
        CHATD_LOG_WARNING("Retrying pending connenction...");
        reconnect();
    }
}

promise::Promise<void> Client::disconnect()
{
    std::vector<Promise<void>> promises;
    for (auto& conn: mConnections)
    {
        promises.push_back(conn.second->disconnect());
    }
    return promise::when(promises);
}

void Client::retryPendingConnections()
{
    for (auto& conn: mConnections)
    {
        conn.second->retryPendingConnection();
    }
}

void Connection::reset() //immediate disconnect
{
    if (!mWebSocket)
        return;

    ws_close_immediately(mWebSocket);
    ws_destroy(&mWebSocket);
    assert(!mWebSocket);
}

bool Connection::sendBuf(Buffer&& buf)
{
    if (!isOnline())
        return false;
//WARNING: ws_send_msg_ex() is destructive to the buffer - it applies the websocket mask directly
//Copy the data to preserve the original
    auto rc = ws_send_msg_ex(mWebSocket, buf.buf(), buf.dataSize(), 1);
    buf.free(); //just in case, as it's content is xor-ed with the websock datamask so it's unusable
    bool result = (!rc && isOnline());
    return result;
}
bool Chat::sendCommand(Command&& cmd)
{
    if (krLoggerWouldLog(krLogChannel_chatd, krLogLevelDebug))
        logSend(cmd);
    bool result = mConnection.sendBuf(std::move(cmd));
    if (!result)
        CHATID_LOG_DEBUG("  Can't send, we are offline");
    return result;
}

bool Chat::sendCommand(const Command& cmd)
{
    Buffer buf(cmd.buf(), cmd.dataSize());
    if (krLoggerWouldLog(krLogChannel_chatd, krLogLevelDebug))
        logSend(cmd);
    auto result = mConnection.sendBuf(std::move(buf));
    if (!result)
        CHATD_LOG_DEBUG("  Can't send, we are offline");
    return result;
}
void Chat::logSend(const Command& cmd)
{
    auto op = cmd.opcode();
    switch (op)
    {
        case OP_NEWMSG:
        {
            auto& msgcmd = static_cast<const MsgCommand&>(cmd);
            krLoggerLog(krLogChannel_chatd, krLogLevelDebug, "%s: send NEWMSG - msgxid: %s\n",
                ID_CSTR(mChatId), ID_CSTR(msgcmd.msgid()));
            break;
        }
        case OP_MSGUPD:
        {
            auto& msgcmd = static_cast<const MsgCommand&>(cmd);
            krLoggerLog(krLogChannel_chatd, krLogLevelDebug, "%s: send MSGUPD - msgid: %s\n",
                ID_CSTR(mChatId), ID_CSTR(msgcmd.msgid()));
            break;
        }
        case OP_MSGUPDX:
        {
            auto& msgcmd = static_cast<const MsgCommand&>(cmd);
            krLoggerLog(krLogChannel_chatd, krLogLevelDebug, "%s: send MSGUPDX - msgxid: %s, tsdelta: %hu\n",
                ID_CSTR(mChatId), ID_CSTR(msgcmd.msgid()), msgcmd.updated());
            break;
        }
        case OP_NEWKEY:
        {
            //auto& keycmd = static_cast<const KeyCommand&>(cmd);
            krLoggerLog(krLogChannel_chatd, krLogLevelDebug, "%s: send NEWKEY\n",
                        ID_CSTR(mChatId));
            break;
        }
        default:
        {
            krLoggerLog(krLogChannel_chatd, krLogLevelDebug, "%s: send %s\n", ID_CSTR(mChatId), cmd.opcodeName());
            break;
        }
    }
}

// rejoin all open chats after reconnection (this is mandatory)
promise::Promise<void> Connection::rejoinExistingChats()
{
    for (auto& chatid: mChatIds)
    {
        try
        {
            Chat& chat = mClient.chats(chatid);
            if (!chat.isDisabled())
                chat.login();
        }
        catch(std::exception& e)
        {
            mLoginPromise.reject(std::string("rejoinExistingChats: Exception: ")+e.what());
        }
    }
    return mLoginPromise;
}

// send JOIN
void Chat::join()
{
//We don't have any local history, otherwise joinRangeHist() would be called instead of this
//Reset handshake state, as we may be reconnecting
    assert(mConnection.isOnline());
    mUserDump.clear();
    setOnlineState(kChatStateJoining);
    mServerFetchState = kHistNotFetching;
    //we don't have local history, so mHistSendSource may be None or Server.
    //In both cases this will not block history messages being sent to app
    mServerOldHistCbEnabled = false;
    sendCommand(Command(OP_JOIN) + mChatId + mClient.mUserId + (int8_t)PRIV_NOCHANGE);
    requestHistoryFromServer(-initialHistoryFetchCount);
}

void Chat::onJoinRejected()
{
    CHATID_LOG_WARNING("JOIN was rejected, setting chat offline and disabling it");
    mServerFetchState = kHistNotFetching;
    setOnlineState(kChatStateOffline);
    disable(true);
}

void Chat::onDisconnect()
{
    if (mServerOldHistCbEnabled && (mServerFetchState & kHistFetchingOldFromServer))
    {
        //app has been receiving old history from server, but we are now
        //about to receive new history (if any), so notify app about end of
        //old history
        CALL_LISTENER(onHistoryDone, kHistSourceServer);
    }
    mServerFetchState = kHistNotFetching;
    setOnlineState(kChatStateOffline);
}

HistSource Chat::getHistory(unsigned count)
{
    if (isNotifyingOldHistFromServer())
    {
        return kHistSourceServer;
    }
    if ((mNextHistFetchIdx == CHATD_IDX_INVALID) && !empty())
    {
        //start from newest message and go backwards
        mNextHistFetchIdx = highnum();
    }

    Idx countSoFar = 0;
    if (mNextHistFetchIdx != CHATD_IDX_INVALID)
    {
        assert(mNextHistFetchIdx <= highnum());
        auto end = lownum()-1;
        if (mNextHistFetchIdx > end) //we are in the RAM range
        {
            CHATID_LOG_DEBUG("Fetching history(%u) from RAM...", count);
            Idx fetchEnd = mNextHistFetchIdx - count;
            if (fetchEnd < end)
            {
                fetchEnd = end;
            }

            for (Idx i = mNextHistFetchIdx; i > fetchEnd; i--)
            {
                auto& msg = at(i);
                CALL_LISTENER(onRecvHistoryMessage, i, msg, getMsgStatus(msg, i), true);
            }
            countSoFar = mNextHistFetchIdx - fetchEnd;
            mNextHistFetchIdx -= countSoFar;
            if (countSoFar >= (int)count)
            {
                CALL_LISTENER(onHistoryDone, kHistSourceRam);
                return kHistSourceRam;
            }
        }
    }

    // more than what is available in RAM is requested
    auto nextSource = getHistoryFromDbOrServer(count - countSoFar);
    if (nextSource == kHistSourceNone) //no history in db and server
    {
        auto source = (countSoFar > 0) ? kHistSourceRam : kHistSourceNone;
        CALL_LISTENER(onHistoryDone, source);
        return source;
    }
    if (nextSource == kHistSourceDb)
    {
        CALL_LISTENER(onHistoryDone, kHistSourceDb);
    }
    return nextSource;
}

HistSource Chat::getHistoryFromDbOrServer(unsigned count)
{
    if (mHasMoreHistoryInDb)
    {
        CHATID_LOG_DEBUG("Fetching history(%u) from db...", count);
        getHistoryFromDb(count);
        return kHistSourceDb;
    }
    else //have to fetch history from server
    {
        mServerOldHistCbEnabled = true;
        if (mHaveAllHistory)
        {
            CHATID_LOG_DEBUG("getHistoryFromDbOrServer: No more history exists");
            return kHistSourceNone;
        }
        if (mServerFetchState & kHistOldFlag)
        {
            CHATID_LOG_DEBUG("getHistoryFromDbOrServer: Need more history, and server history fetch is already in progress, will get next messages from there");
        }
        else
        {
            if (!mConnection.isOnline())
                return kHistSourceServerOffline;

            CHATID_LOG_DEBUG("Fetching history(%u) from server...", count);
            requestHistoryFromServer(-count);
        }
        return kHistSourceServer;
    }
}

void Chat::requestHistoryFromServer(int32_t count)
{
    assert(mConnection.isOnline());
    mLastServerHistFetchCount = mLastHistDecryptCount = 0;
    mServerFetchState = (count > 0)
        ? kHistFetchingNewFromServer
        : kHistFetchingOldFromServer;

    sendCommand(Command(OP_HIST) + mChatId + count);
}

Chat::Chat(Connection& conn, Id chatid, Listener* listener,
    const karere::SetOfIds& initialUsers, uint32_t chatCreationTs,
    ICrypto* crypto)
    : mConnection(conn), mClient(conn.mClient), mChatId(chatid),
      mListener(listener), mUsers(initialUsers), mCrypto(crypto),
      mLastMsgTs(chatCreationTs)
{
    assert(mChatId);
    assert(mListener);
    assert(mCrypto);
    assert(!mUsers.empty());
    mNextUnsent = mSending.begin();
    //we don't use CALL_LISTENER here because if init() throws, then something is wrong and we should not continue
    mListener->init(*this, mDbInterface);
    CALL_CRYPTO(setUsers, &mUsers);
    assert(mDbInterface);
    ChatDbInfo info;
    mDbInterface->getHistoryInfo(info);
    mOldestKnownMsgId = info.oldestDbId;
    mLastSeenId = info.lastSeenId;
    mLastReceivedId = info.lastRecvId;
    mLastSeenIdx = mDbInterface->getIdxOfMsgid(mLastSeenId);
    mLastReceivedIdx = mDbInterface->getIdxOfMsgid(mLastReceivedId);

    if ((mHaveAllHistory = mDbInterface->haveAllHistory()))
    {
        CHATID_LOG_DEBUG("All backward history of chat is available locally");
    }

    if (!mOldestKnownMsgId)
    {
        //no history in db
        mHasMoreHistoryInDb = false;
        mForwardStart = CHATD_IDX_RANGE_MIDDLE;
        CHATID_LOG_DEBUG("Db has no local history for chat");
        loadAndProcessUnsent();
    }
    else
    {
        assert(info.newestDbIdx != CHATD_IDX_INVALID);
        mHasMoreHistoryInDb = true;
        mForwardStart = info.newestDbIdx + 1;
        CHATID_LOG_DEBUG("Db has local history: %s - %s (middle point: %u)",
            ID_CSTR(info.oldestDbId), ID_CSTR(info.newestDbId), mForwardStart);
        loadAndProcessUnsent();
        getHistoryFromDb(1); //to know if we have the latest message on server, we must at least load the latest db message
    }
}
Chat::~Chat()
{
    CALL_LISTENER(onDestroy); //we don't delete because it may have its own idea of its lifetime (i.e. it could be a GUI class)
    try { delete mCrypto; }
    catch(std::exception& e)
    { CHATID_LOG_ERROR("EXCEPTION from ICrypto destructor: %s", e.what()); }
    mCrypto = nullptr;
    clear();
    try { delete mDbInterface; }
    catch(std::exception& e)
    { CHATID_LOG_ERROR("EXCEPTION from DbInterface destructor: %s", e.what()); }
    mDbInterface = nullptr;
}

Idx Chat::getHistoryFromDb(unsigned count)
{
    assert(mHasMoreHistoryInDb); //we are within the db range
    std::vector<Message*> messages;
    CALL_DB(fetchDbHistory, lownum()-1, count, messages);
    for (auto msg: messages)
    {
        msgIncoming(false, msg, true); //increments mLastHistFetch/DecryptCount, may reset mHasMoreHistoryInDb if this msgid == mLastKnownMsgid
    }
    if (mNextHistFetchIdx == CHATD_IDX_INVALID)
    {
        mNextHistFetchIdx = mForwardStart - 1 - messages.size();
    }
    else
    {
        mNextHistFetchIdx -= messages.size();
    }
    CALL_LISTENER(onHistoryDone, kHistSourceDb);

    // If we haven't yet seen the message with the last-seen msgid, then all messages
    // in the buffer (and in the loaded range) are unseen - so we just loaded
    // more unseen messages
    if ((messages.size() < count) && mHasMoreHistoryInDb)
        throw std::runtime_error(mChatId.toString()+": Db says it has no more messages, but we still haven't seen mOldestKnownMsgId of "+std::to_string((int64_t)mOldestKnownMsgId.val));
    return (Idx)messages.size();
}

#define READ_ID(varname, offset)\
    assert(offset==pos-base); Id varname(buf.read<uint64_t>(pos)); pos+=sizeof(uint64_t)
#define READ_CHATID(offset)\
    assert(offset==pos-base); chatid = buf.read<uint64_t>(pos); pos+=sizeof(uint64_t)

#define READ_32(varname, offset)\
    assert(offset==pos-base); uint32_t varname(buf.read<uint32_t>(pos)); pos+=4
#define READ_16(varname, offset)\
    assert(offset==pos-base); uint16_t varname(buf.read<uint16_t>(pos)); pos+=2
#define READ_8(varname, offset)\
    assert(offset==pos-base); uint8_t varname(buf.read<uint8_t>(pos)); pos+=1

// inbound command processing
// multiple commands can appear as one WebSocket frame, but commands never cross frame boundaries
// CHECK: is this assumption correct on all browsers and under all circumstances?
void Connection::execCommand(const StaticBuffer& buf)
{
    size_t pos = 0;
//IMPORTANT: Increment pos before calling the command handler, because the handler may throw, in which
//case the next iteration will not advance and will execute the same command again, resulting in
//infinite loop
    while (pos < buf.dataSize())
    {
      char opcode = buf.buf()[pos];
      Id chatid;
      try
      {
        pos++;
#ifndef NDEBUG
        size_t base = pos;
#endif
//        CHATD_LOG_DEBUG("RECV %s", Command::opcodeToStr(opcode));
        switch (opcode)
        {
            case OP_KEEPALIVE:
            {
                //CHATD_LOG_DEBUG("Server heartbeat received");
                sendKeepalive(mClient.mKeepaliveType);
                break;
            }
            case OP_BROADCAST:
            {
                READ_CHATID(0);
                READ_ID(userid, 8);
                READ_8(bcastType, 16);
                auto& chat = mClient.chats(chatid);
                chat.handleBroadcast(userid, bcastType);
                break;
            }
            case OP_JOIN:
            {
                READ_CHATID(0);
                READ_ID(userid, 8);
                Priv priv = (Priv)buf.read<int8_t>(pos);
                pos++;
                CHATD_LOG_DEBUG("%s: recv JOIN - user '%s' with privilege level %d",
                                ID_CSTR(chatid), ID_CSTR(userid), priv);
                auto& chat =  mClient.chats(chatid);
                if (priv == PRIV_NOTPRESENT)
                    chat.onUserLeave(userid);
                else
                    chat.onUserJoin(userid, priv);
                break;
            }
            case OP_OLDMSG:
            case OP_NEWMSG:
            case OP_MSGUPD:
            {
                READ_CHATID(0);
                READ_ID(userid, 8);
                READ_ID(msgid, 16);
                READ_32(ts, 24);
                READ_16(updated, 28);
                READ_32(keyid, 30);
                READ_32(msglen, 34);
                const char* msgdata = buf.readPtr(pos, msglen);
                pos += msglen;
                CHATD_LOG_DEBUG("%s: recv %s - msgid: '%s', from user '%s' with keyid %x",
                    ID_CSTR(chatid), Command::opcodeToStr(opcode), ID_CSTR(msgid),
                    ID_CSTR(userid), keyid);

                std::unique_ptr<Message> msg(new Message(msgid, userid, ts, updated, msgdata, msglen, false, keyid));
                msg->setEncrypted(1);
                Chat& chat = mClient.chats(chatid);
                if (opcode == OP_MSGUPD)
                {
                    chat.onMsgUpdated(msg.release());
                }
                else
                {
                    chat.msgIncoming((opcode == OP_NEWMSG), msg.release(), false);
                }
                break;
            }
            case OP_SEEN:
            {
            //TODO: why do we test the whole buffer's len to determine the current command's len?
            //buffer may contain other commands following it
                READ_CHATID(0);
                READ_ID(msgid, 8);
                CHATD_LOG_DEBUG("%s: recv SEEN - msgid: '%s'",
                                ID_CSTR(chatid), ID_CSTR(msgid));
                mClient.chats(chatid).onLastSeen(msgid);
                break;
            }
            case OP_RECEIVED:
            {
                READ_CHATID(0);
                READ_ID(msgid, 8);
                CHATD_LOG_DEBUG("%s: recv RECEIVED - msgid: '%s'", ID_CSTR(chatid), ID_CSTR(msgid));
                mClient.chats(chatid).onLastReceived(msgid);
                break;
            }
            case OP_RETENTION:
            {
                READ_CHATID(0);
                READ_ID(userid, 8);
                READ_32(period, 16);
                CHATD_LOG_DEBUG("%s: recv RETENTION by user '%s' to %u second(s)",
                                ID_CSTR(chatid), ID_CSTR(userid), period);
                break;
            }
            case OP_MSGID:
            {
                READ_ID(msgxid, 0);
                READ_ID(msgid, 8);
                if (!msgid)
                {
                    CHATD_LOG_ERROR("MSGID with zero message id received, ignoring");
                    break;
                }
                mClient.onMsgAlreadySent(msgxid, msgid);
                break;
            }
            case OP_NEWMSGID:
            {
                READ_ID(msgxid, 0);
                READ_ID(msgid, 8);
                mClient.msgConfirm(msgxid, msgid);
                break;
            }
/*            case OP_RANGE:
            {
                READ_CHATID(0);
                READ_ID(oldest, 8);
                READ_ID(newest, 16);
                CHATD_LOG_DEBUG("%s: recv RANGE - (%s - %s)",
                                ID_CSTR(chatid), ID_CSTR(oldest), ID_CSTR(newest));
                auto& msgs = mClient.chats(chatid);
                if (msgs.onlineState() == kChatStateJoining)
                    msgs.initialFetchHistory(newest);
                break;
            }
*/
            case OP_REJECT:
            {
                READ_CHATID(0);
                READ_ID(id, 8);
                READ_8(op, 16);
                READ_8(reason, 17);
                CHATD_LOG_WARNING("%s: recv REJECT of %s: id='%s', reason: %hu",
                    ID_CSTR(chatid), Command::opcodeToStr(op), ID_CSTR(id), reason);
                auto& chat = mClient.chats(chatid);
                if (op == OP_NEWMSG) // the message was rejected
                {
                    chat.msgConfirm(id, Id::null());
                }
                else if ((op == OP_MSGUPD) || (op == OP_MSGUPDX))
                {
                    chat.rejectMsgupd(id, reason);
                }
                else if (op == OP_JOIN)
                {
                    chat.onJoinRejected();
                }
                else
                {
                    chat.rejectGeneric(op);
                    //TODO: Implement
                }
                break;
            }
            case OP_HISTDONE:
            {
                READ_CHATID(0);
                CHATD_LOG_DEBUG("%s: recv HISTDONE - history retrieval finished", ID_CSTR(chatid));
                mClient.chats(chatid).onHistDone();
                break;
            }
            case OP_KEYID:
            {
                READ_CHATID(0);
                READ_32(keyxid, 8);
                READ_32(keyid, 12);
                CHATD_LOG_DEBUG("%s: recv KEYID %u", ID_CSTR(chatid), keyid);
                mClient.chats(chatid).keyConfirm(keyxid, keyid);
                break;
            }
            case OP_NEWKEY:
            {
                READ_CHATID(0);
                pos += 4; //skip dummy 32bit keyid
                READ_32(totalLen, 12);
                const char* keys = buf.readPtr(pos, totalLen);
                pos+=totalLen;
                CHATD_LOG_DEBUG("%s: recv NEWKEY", ID_CSTR(chatid));
                mClient.chats(chatid).onNewKeys(StaticBuffer(keys, totalLen));
                break;
            }
            default:
            {
                CHATD_LOG_ERROR("Unknown opcode %d, ignoring all subsequent commands", opcode);
                return;
            }
        }
      }
      catch(BufferRangeError& e)
      {
            CHATD_LOG_ERROR("%s: Buffer bound check error while parsing %s:\n\t%s\n\tAborting command processing", ID_CSTR(chatid), Command::opcodeToStr(opcode), e.what());
            return;
      }
      catch(std::exception& e)
      {
            CHATD_LOG_ERROR("%s: Exception while processing incoming %s: %s", ID_CSTR(chatid), Command::opcodeToStr(opcode), e.what());
      }
    }
}

void Chat::onNewKeys(StaticBuffer&& keybuf)
{
    uint16_t keylen = 0;
    for(size_t pos = 0; pos < keybuf.dataSize(); pos+=(14+keylen))
    {
        Id userid(keybuf.read<uint64_t>(pos));
        uint32_t keyid = keybuf.read<uint32_t>(pos+8);
        keylen = keybuf.read<uint16_t>(pos+12);
        CHATID_LOG_DEBUG(" sending key %d with length %zu to crypto module", keyid, keybuf.dataSize());
        mCrypto->onKeyReceived(keyid, userid, mClient.userId(),
            keybuf.readPtr(pos+14, keylen), keylen);
    }
}

void Chat::onHistDone()
{
    // We may be fetching from memory and db because of a resetHistFetch()
    // while fetching from server. In that case, we don't notify about
    // fetched messages and onHistDone()

    if (isFetchingFromServer()) //HISTDONE is received for new history or after JOINRANGEHIST
    {
        onFetchHistDone();
    }
    if(mOnlineState == kChatStateJoining)
    {
        onJoinComplete();
    }
}

void Chat::onFetchHistDone()
{
    assert(isFetchingFromServer());

    //resetHistFetch() may have been called while fetching from server,
    //so state may be fetching-from-ram or fetching-from-db
    bool fetchingOld = (mServerFetchState & kHistOldFlag);
    if (fetchingOld)
    {
        mServerFetchState = (mDecryptOldHaltedAt != CHATD_IDX_INVALID)
            ? kHistDecryptingOld : kHistNotFetching;
        if (mLastServerHistFetchCount <= 0)
        {
            //server returned zero messages
            assert((mDecryptOldHaltedAt == CHATD_IDX_INVALID) && (mDecryptNewHaltedAt == CHATD_IDX_INVALID));
            mHaveAllHistory = true;
            CALL_DB(setHaveAllHistory);
            CHATID_LOG_DEBUG("Start of history reached");
            //last text msg stuff
            if (mLastTextMsg.isFetching())
            {
                mLastTextMsg.clear();
                notifyLastTextMsg();
            }
        }
    }
    else
    {
        mServerFetchState = (mDecryptNewHaltedAt != CHATD_IDX_INVALID)
            ? kHistDecryptingNew : kHistNotFetching;
    }

    if (mServerFetchState == kHistNotFetching) //if not still decrypting
    {
        if (fetchingOld && mServerOldHistCbEnabled)
        {
            //we are forwarding to the app the history we are receiving from
            //server. Tell app that is complete.
            CALL_LISTENER(onHistoryDone, kHistSourceServer);
        }
        if (mLastSeenIdx == CHATD_IDX_INVALID)
            CALL_LISTENER(onUnreadChanged);
    }

    // handle last text message fetching
    if (mLastTextMsg.isFetching())
    {
        assert(!mHaveAllHistory); //if we reach start of history, mLastTextMsg.state() will be set to kNone
        CHATID_LOG_DEBUG("No text message seen yet, fetching more history from server");
        getHistory(16);
    }
}

void Chat::loadAndProcessUnsent()
{
    assert(mSending.empty());
    CALL_DB(loadSendQueue, mSending);
    if (mSending.empty())
        return;
    mNextUnsent = mSending.begin();
    replayUnsentNotifications();

    //last text message stuff
    for (auto it = mSending.rbegin(); it!=mSending.rend(); it++)
    {
        if (it->msg->isText())
        {
            onLastTextMsgUpdated(*it->msg);
            return;
        }
    }
}

void Chat::resetListenerState()
{
    resetGetHistory();
    replayUnsentNotifications();
    loadManualSending();
}

void Chat::replayUnsentNotifications()
{
    for (auto it = mSending.begin(); it != mSending.end(); it++)
    {
        auto& item = *it;
        if (item.opcode() == OP_NEWMSG)
        {
            CALL_LISTENER(onUnsentMsgLoaded, *item.msg);
        }
        else if (item.opcode() == OP_MSGUPD)
        {
            CHATID_LOG_DEBUG("Adding a pending edit of msgid %s", ID_CSTR(item.msg->id()));
            mPendingEdits[item.msg->id()] = item.msg;
            CALL_LISTENER(onUnsentEditLoaded, *item.msg, false);
        }
        else if (item.opcode() == OP_MSGUPDX)
        {
            //in case of MSGUPDX, when msgModify posted it, it must have updated
            //the text of the original message with that msgxid in the send queue.
            //So we can technically do without the this
            //'else if (item.opcode == OP_MSGUPDX)' case. However, if we don't tell
            //the GUI there is an actual edit pending, (although it may be a dummy one,
            //because the pending NEWMSG in the send queue was updated with the new msg),
            //it will display a normal pending outgoing message without any sign
            //of an edit. Then, when it receives the MSGUPD confirmation, it will
            //suddenly flash an indicator that the message was edited, which may be
            //confusing to the user.
            CHATID_LOG_DEBUG("Adding a pending edit of msgxid %s", ID_CSTR(item.msg->id()));
            CALL_LISTENER(onUnsentEditLoaded, *item.msg, true);
        }
    }
}

void Chat::loadManualSending()
{
    std::vector<ManualSendItem> items;
    CALL_DB(loadManualSendItems, items);
    for (auto& item: items)
    {
        CALL_LISTENER(onManualSendRequired, item.msg, item.rowid, item.reason);
    }
}
Message* Chat::getMsgByXid(Id msgxid)
{
    for (auto& item: mSending)
    {
        if (!item.msg)
            continue;
        //id() of MSGUPD messages is a real msgid, not a msgxid
        if ((item.msg->id() == msgxid) && (item.opcode() != OP_MSGUPD))
        {
            assert(item.msg->isSending());
            return item.msg;
        }
    }
    return nullptr;
}

bool Chat::haveAllHistoryNotified() const
{
    if (!mHaveAllHistory || mHasMoreHistoryInDb)
        return false;

    return (mNextHistFetchIdx < lownum());
}

uint64_t Chat::generateRefId(const ICrypto* aCrypto)
{
    uint64_t ts = time(nullptr);
    uint64_t rand;
    aCrypto->randomBytes(&rand, sizeof(rand));
    return (ts & 0x0000000000ffffff) | (rand << 40);
}

Message* Chat::msgSubmit(const char* msg, size_t msglen, unsigned char type, void* userp)
{
    // write the new message to the message buffer and mark as in sending state
    auto message = new Message(makeRandomId(), client().userId(), time(NULL),
        0, msg, msglen, true, CHATD_KEYID_INVALID, type, userp);
    message->backRefId = generateRefId(mCrypto);
    msgSubmit(message);
    return message;
}
void Chat::msgSubmit(Message* msg)
{
    assert(msg->isSending());
    assert(msg->keyid == CHATD_KEYID_INVALID);
    postMsgToSending(OP_NEWMSG, msg);

    // last text msg stuff
    if (msg->isText())
    {
        onLastTextMsgUpdated(*msg);
    }
    onMsgTimestamp(msg->ts);
}

void Chat::createMsgBackRefs(Message& msg)
{
#ifndef _MSC_VER
    static std::uniform_int_distribution<uint8_t>distrib(0, 0xff);
#else
//MSVC has a bug - no char template argument allowed
    static std::uniform_int_distribution<uint32_t>distrib(0,0xff);
#endif

    static std::random_device rd;
    std::vector<SendingItem*> sendingIdx;
    sendingIdx.reserve(mSending.size());
    for (auto& item: mSending)
    {
        sendingIdx.push_back(&item);
    }
    Idx maxEnd = mSending.size()+size();
    if (maxEnd <= 0)
        return;
    Idx start = 0;
    for (size_t i=0; i<7; i++)
    {
        Idx end = 1 << i;
        if (end > maxEnd)
            end = maxEnd;
        //backward offset range is [start - end)
        Idx range = (end - start);
        assert(range >= 0);
        Idx back =  (range > 1)
            ? (start + (distrib(rd) % range))
            : (start);
        uint64_t backref = (back < (Idx)mSending.size()) //reference a not-yet confirmed message
            ? (sendingIdx[mSending.size()-1-back]->msg->backRefId)
            : (at(highnum()-(back-mSending.size())).backRefId);
        msg.backRefs.push_back(backref);
        if (end == maxEnd)
            return;
        start = end;
    }
}

Chat::SendingItem* Chat::postMsgToSending(uint8_t opcode, Message* msg)
{
    mSending.emplace_back(opcode, msg, mUsers);
    CALL_DB(saveMsgToSending, mSending.back());
    if (mNextUnsent == mSending.end())
    {
        mNextUnsent--;
    }
//The mSending queue should not change, as we never delete items upon sending, only upon confirmation
#ifndef NDEBUG
    auto save = &mSending.back();
#endif
    flushOutputQueue();
    assert(&mSending.back() == save);
    return &mSending.back();
}

bool Chat::sendKeyAndMessage(std::pair<MsgCommand*, KeyCommand*> cmd)
{
    assert(cmd.first);
    if (cmd.second)
    {
        cmd.second->setChatId(mChatId);
        if (!sendCommand(*cmd.second))
            return false;
    }
    return sendCommand(*cmd.first);
}

bool Chat::msgEncryptAndSend(OutputQueue::iterator it)
{
    auto msg = it->msg;
    //opcode can be NEWMSG, MSGUPD or MSGUPDX
    assert(msg->id());
    if (it->opcode() == OP_NEWMSG && msg->backRefs.empty())
        createMsgBackRefs(*msg);

    if (mEncryptionHalted || (mOnlineState != kChatStateOnline))
        return false;

    auto msgCmd = new MsgCommand(it->opcode(), mChatId, client().userId(),
         msg->id(), msg->ts, msg->updated, msg->keyid);

    CHATD_LOG_CRYPTO_CALL("Calling ICrypto::encrypt()");
    auto pms = mCrypto->msgEncrypt(it->msg, msgCmd);
    if (pms.succeeded())
        return sendKeyAndMessage(pms.value());

    mEncryptionHalted = true;
    CHATID_LOG_DEBUG("Can't encrypt message immediately, halting output");
    auto rowid = mSending.front().rowid;
    pms.then([this, rowid](std::pair<MsgCommand*, KeyCommand*> result)
    {
        assert(mEncryptionHalted);
        assert(!mSending.empty());
        assert(mSending.front().rowid == rowid);

        sendKeyAndMessage(result);
        mEncryptionHalted = false;
        flushOutputQueue();
    });

    pms.fail([this, msg, msgCmd](const promise::Error& err)
    {
        CHATID_LOG_ERROR("ICrypto::encrypt error encrypting message %s: %s", ID_CSTR(msg->id()), err.what());
        delete msgCmd;
        return err;
    });
    return false;
    //we don't sent a msgStatusChange event to the listener, as the GUI should initialize the
    //message's status with something already, so it's redundant.
    //The GUI should by default show it as sending
}

// Can be called for a message in history or a NEWMSG,MSGUPD,MSGUPDX message in sending queue
Message* Chat::msgModify(Message& msg, const char* newdata, size_t newlen, void* userp)
{
    uint32_t age = time(NULL) - msg.ts;
    if (age > CHATD_MAX_EDIT_AGE)
    {
        CHATID_LOG_DEBUG("msgModify: Denying edit of msgid %s because message is too old", ID_CSTR(msg.id()));
        return nullptr;
    }
    if (msg.isSending()) //update the not yet sent(or at least not yet confirmed) original as well, trying to avoid sending the original content
    {
        SendingItem* item = nullptr;
        for (auto& loopItem: mSending)
        {
            if (loopItem.msg->id() == msg.id())
            {
                item = &loopItem;
                break;
            }
        }
        assert(item);
        if ((item->opcode() == OP_MSGUPD) || (item->opcode() == OP_MSGUPDX))
        {
            item->msg->updated = age + 1;
        }
        msg.assign((void*)newdata, newlen);
        CALL_DB(updateMsgPlaintextInSending, item->rowid, msg);
    } //end msg.isSending()
    auto upd = new Message(msg.id(), msg.userid, msg.ts, age+1, newdata, newlen,
        msg.isSending(), msg.keyid, msg.type, userp);
    postMsgToSending(upd->isSending() ? OP_MSGUPDX : OP_MSGUPD, upd);
    onMsgTimestamp(msg.ts+age);
    return upd;
}

void Chat::onLastReceived(Id msgid)
{
    mLastReceivedId = msgid;
    CALL_DB(setLastReceived, msgid);
    auto it = mIdToIndexMap.find(msgid);
    if (it == mIdToIndexMap.end())
    { // we don't have that message in the buffer yet, so we don't know its index
        Idx idx = mDbInterface->getIdxOfMsgid(msgid);
        if (idx != CHATD_IDX_INVALID)
        {
            if ((mLastReceivedIdx != CHATD_IDX_INVALID) && (idx < mLastReceivedIdx))
            {
                CHATID_LOG_ERROR("onLastReceived: Tried to set the index to an older message, ignoring");
                CHATID_LOG_DEBUG("highnum() = %zu, mLastReceivedIdx = %zu, idx = %zu", highnum(), mLastReceivedIdx, idx);
            }
            else
            {
                mLastReceivedIdx = idx;
            }
        }
        return; //last-received is behind history in memory, so nothing to notify about
    }

    auto idx = it->second;
    if (idx == mLastReceivedIdx)
        return; //probably set from db
    if (at(idx).userid != mClient.mUserId)
    {
        CHATID_LOG_WARNING("Last-received pointer points to a message not by us,"
            " possibly the pointer was set incorrectly");
    }
    //notify about messages that become 'received'
    Idx notifyOldest;
    if (mLastReceivedIdx != CHATD_IDX_INVALID) //we have a previous last-received index, notify user about received messages
    {
        if (mLastReceivedIdx > idx)
        {
            CHATID_LOG_ERROR("onLastReceived: Tried to set the index to an older message, ignoring");
            CHATID_LOG_DEBUG("highnum() = %zu, mLastReceivedIdx = %zu, idx = %zu", highnum(), mLastReceivedIdx, idx);
            return;
        }
        notifyOldest = mLastReceivedIdx + 1;
        auto low = lownum();
        if (notifyOldest < low)
        { // mLastReceivedIdx may point to a message in db, older than what we have in RAM
            notifyOldest = low;
        }
        mLastReceivedIdx = idx;
    }
    else
    {
        // No mLastReceivedIdx - notify all messages in RAM
        mLastReceivedIdx = idx;
        notifyOldest = lownum();
    }
    for (Idx i=notifyOldest; i<=mLastReceivedIdx; i++)
    {
        auto& msg = at(i);
        if (msg.userid == mClient.mUserId)
        {
            CALL_LISTENER(onMessageStatusChange, i, Message::kDelivered, msg);
        }
    }
}

void Chat::onLastSeen(Id msgid)
{
    mLastSeenId = msgid;
    CALL_DB(setLastSeen, msgid);
    auto it = mIdToIndexMap.find(msgid);
    if (it == mIdToIndexMap.end())
    {
        Idx idx = mDbInterface->getIdxOfMsgid(msgid);
        //last seen is older than our history, so all history in memory is 'unseen', even if there was a previous, older last-seen
        if (idx != CHATD_IDX_INVALID)
        {
            if ((mLastSeenIdx != CHATD_IDX_INVALID) && (idx < mLastSeenIdx))
            {
                CHATD_LOG_WARNING("onLastSeen: Setting last seen index to an older message");
            }
            mLastSeenIdx = idx;
        }
    }
    else
    {
        auto idx = it->second;
        if (idx == mLastSeenIdx)
            return; //we may have set it from db already
        if(at(idx).userid == mClient.mUserId)
        {
            CHATD_LOG_WARNING("Last-seen points to a message by us, possibly the pointer was not set properly");
        }
        //notify about messages that have become 'seen'
        Idx notifyOldest;
        if (mLastSeenIdx != CHATD_IDX_INVALID)
        {
            if (idx < mLastSeenIdx)
            {
                CHATID_LOG_WARNING("onLastSeen: Setting last seen index to an older "
                    "message: current idx: %d, new: %d", mLastSeenIdx, idx);
            }
            notifyOldest = mLastSeenIdx + 1;
            auto low = lownum();
            if (notifyOldest < low)
            {
                notifyOldest = low;
            }
            mLastSeenIdx = idx;
        }
        else
        {
            mLastSeenIdx = idx;
            notifyOldest = lownum();
        }
        for (Idx i=notifyOldest; i<=mLastSeenIdx; i++)
        {
            auto& msg = at(i);
            if (msg.userid != mClient.mUserId)
            {
                CALL_LISTENER(onMessageStatusChange, i, Message::kSeen, msg);
            }
        }
    }
    CALL_LISTENER(onUnreadChanged);
}

bool Chat::setMessageSeen(Idx idx)
{
    assert(idx != CHATD_IDX_INVALID);
    if ((mLastSeenIdx != CHATD_IDX_INVALID) && (idx <= mLastSeenIdx))
        return false;

    auto& msg = at(idx);
    if (msg.userid == mClient.mUserId)
    {
        CHATID_LOG_DEBUG("Asked to mark own message %s as seen, ignoring", ID_CSTR(msg.id()));
        return false;
    }
    CHATID_LOG_DEBUG("setMessageSeen: Setting last seen msgid to %s", ID_CSTR(msg.id()));
    sendCommand(Command(OP_SEEN) + mChatId + msg.id());

    Idx notifyStart;
    if (mLastSeenIdx == CHATD_IDX_INVALID)
    {
        notifyStart = lownum()-1;
    }
    else
    {
        Idx lowest = lownum()-1;
        notifyStart = (mLastSeenIdx < lowest) ? lowest : mLastSeenIdx;
    }
    mLastSeenIdx = idx;
    Idx highest = highnum();
    Idx notifyEnd = (mLastSeenIdx > highest) ? highest : mLastSeenIdx;

    for (Idx i=notifyStart+1; i<=notifyEnd; i++)
    {
        auto& m = at(i);
        if (m.userid != mClient.mUserId)
        {
            CALL_LISTENER(onMessageStatusChange, i, Message::kSeen, m);
        }
    }
    CALL_LISTENER(onUnreadChanged);
    return true;
}

bool Chat::setMessageSeen(Id msgid)
{
    auto it = mIdToIndexMap.find(msgid);
    if (it == mIdToIndexMap.end())
    {
        CHATID_LOG_WARNING("setMessageSeen: unknown msgid '%s'", ID_CSTR(msgid));
        return false;
    }
    return setMessageSeen(it->second);
}

int Chat::unreadMsgCount() const
{
    if (mLastSeenIdx == CHATD_IDX_INVALID)
    {
        Message* msg;
        if (!empty() && ((msg = newest())->type == Message::kMsgTruncate))
        {
            assert(size() == 1);
            return (msg->userid != client().userId()) ? 1 : 0;
        }
        else
        {
            return -mDbInterface->getPeerMsgCountAfterIdx(CHATD_IDX_INVALID);
        }
    }
    else if (mLastSeenIdx < lownum())
    {
        return mDbInterface->getPeerMsgCountAfterIdx(mLastSeenIdx);
    }

    Idx first = mLastSeenIdx+1;
    unsigned count = 0;
    auto last = highnum();
    for (Idx i=first; i<=last; i++)
    {
        auto& msg = at(i);
        if (msg.userid != mClient.userId())
            count++;
    }
    return count;
}

void Chat::flushOutputQueue(bool fromStart)
{
//We assume that if fromStart is set, then we have to set mIgnoreKeyAcks
//Indeed, if we flush the send queue from the start, this means that
//the crypto module would get out of sync with the I/O sequence, which means
//that it must have been reset/freshly initialized, and we have to skip
//the KEYID responses for the keys we flush from the output queue
    if(mEncryptionHalted || !mConnection.isOnline())
        return;

    if (fromStart)
        mNextUnsent = mSending.begin();

    if (mNextUnsent == mSending.end())
        return;

    while (mNextUnsent != mSending.end())
    {
        ManualSendReason reason =
             (manualResendWhenUserJoins() && !mNextUnsent->isEdit() && (mNextUnsent->recipients < mUsers))
            ? kManualSendUsersChanged : kManualSendInvalidReason;

        if ((reason == kManualSendInvalidReason) && (time(NULL) - mNextUnsent->msg->ts > CHATD_MAX_EDIT_AGE))
            reason = kManualSendTooOld;

        if (reason != kManualSendInvalidReason)
        {
            auto start = mNextUnsent;
            mNextUnsent = mSending.end();
            // Too old message or edit, or group composition has changed.
            // Move it and all following items as well
            for (auto it = start; it != mSending.end();)
            {
                auto erased = it;
                it++;
                moveItemToManualSending(erased, reason);
            }
            CALL_CRYPTO(resetSendKey);
            return;
        }

        //kickstart encryption
        //return true if we encrypted and sent at least one message
        if (!msgEncryptAndSend(mNextUnsent++))
            return;
    }
}

void Chat::moveItemToManualSending(OutputQueue::iterator it, ManualSendReason reason)
{
    CALL_DB(deleteItemFromSending, it->rowid);
    CALL_DB(saveItemToManualSending, *it, reason);
    CALL_LISTENER(onManualSendRequired, it->msg, it->rowid, reason); //GUI should put this message at end of that list of messages requiring 'manual' resend
    it->msg = nullptr; //don't delete the Message object, it will be owned by the app
    mSending.erase(it);
}

void Chat::removeManualSend(uint64_t rowid)
{
    if (!mDbInterface->deleteManualSendItem(rowid))
        throw std::runtime_error("Unknown manual send id");
}

// after a reconnect, we tell the chatd the oldest and newest buffered message
void Chat::joinRangeHist(const ChatDbInfo& dbInfo)
{
    assert(mConnection.isOnline());
    assert(dbInfo.oldestDbId && dbInfo.newestDbId);
    mUserDump.clear();
    setOnlineState(kChatStateJoining);
    mServerOldHistCbEnabled = false;
    mServerFetchState = kHistFetchingNewFromServer;
    CHATID_LOG_DEBUG("Sending JOINRANGEHIST based on app db: %s - %s",
            dbInfo.oldestDbId.toString().c_str(), dbInfo.newestDbId.toString().c_str());
    sendCommand(Command(OP_JOINRANGEHIST) + mChatId + dbInfo.oldestDbId + dbInfo.newestDbId);
}

void Client::msgConfirm(Id msgxid, Id msgid)
{
    // CHECK: is it more efficient to keep a separate mapping of msgxid to messages?
    for (auto& chat: mChatForChatId)
    {
        if (chat.second->msgConfirm(msgxid, msgid) != CHATD_IDX_INVALID)
            return;
    }
    CHATD_LOG_DEBUG("msgConfirm: No chat knows about message transaction id %s", ID_CSTR(msgxid));
}

//called when MSGID is received
bool Client::onMsgAlreadySent(Id msgxid, Id msgid)
{
    for (auto& chat: mChatForChatId)
    {
        if (chat.second->msgAlreadySent(msgxid, msgid))
            return true;
    }
    return false;
}
bool Chat::msgAlreadySent(Id msgxid, Id msgid)
{
    auto msg = msgRemoveFromSending(msgxid, msgid);
    if (!msg)
        return false;

    CHATID_LOG_DEBUG("recv MSGID: '%s' -> '%s'", ID_CSTR(msgxid), ID_CSTR(msgid));
    CALL_LISTENER(onMessageRejected, *msg, 0);
    delete msg;
    return true;
}

Message* Chat::msgRemoveFromSending(Id msgxid, Id msgid)
{
    // as msgConirm() is tried on all chatids, it's normal that we don't have
    // the message, so no error logging of error, just return invalid index
    if (mSending.empty())
        return nullptr;

    auto& item = mSending.front();
    if (item.opcode() != OP_NEWMSG)
    {
//        CHATID_LOG_DEBUG("msgConfirm: sendQueue doesnt start with NEWMSG, but with %s", Command::opcodeToStr(item.opcode()));
        return nullptr;
    }
    if (item.msg->id() != msgxid)
    {
//        CHATID_LOG_DEBUG("msgConfirm: sendQueue starts with NEWMSG, but the msgxid is different");
        return nullptr;
    }

    if (mNextUnsent == mSending.begin())
        mNextUnsent++; //because we remove the first element

    if (!msgid)
    {
        moveItemToManualSending(mSending.begin(), (mOwnPrivilege == PRIV_RDONLY)
            ? kManualSendNoWriteAccess
            : kManualSendGeneralReject); //deletes item
        return nullptr;
    }
    auto msg = item.msg;
    item.msg = nullptr;
    assert(msg);
    assert(msg->isSending());

    CALL_DB(deleteItemFromSending, item.rowid);
    mSending.pop_front(); //deletes item
    return msg;
}

// msgid can be 0 in case of rejections
Idx Chat::msgConfirm(Id msgxid, Id msgid)
{
    Message* msg = msgRemoveFromSending(msgxid, msgid);
    if (!msg)
        return CHATD_IDX_INVALID;

    CHATID_LOG_DEBUG("recv NEWMSGID: '%s' -> '%s'", ID_CSTR(msgxid), ID_CSTR(msgid));
    //put into history
    msg->setId(msgid, false);
    push_forward(msg);
    auto idx = mIdToIndexMap[msgid] = highnum();
    CALL_DB(addMsgToHistory, *msg, idx);
    //update any following MSGUPDX-s referring to this msgxid
    for (auto& item: mSending)
    {
        if (item.msg->id() == msgxid)
        {
            assert(item.opcode() == OP_MSGUPDX);
            CALL_DB(sendingItemMsgupdxToMsgupd, item, msgid);
            item.msg->setId(msgid, false);
            item.setOpcode(OP_MSGUPD);
        }
    }
    CALL_LISTENER(onMessageConfirmed, msgxid, *msg, idx);

    // last text message stuff
    if (mLastTextMsg.idx() == CHATD_IDX_INVALID)
    {
        if (mLastTextMsg.xid() != msgxid) //it's another message
        {
            onLastTextMsgUpdated(*msg, idx);
        }
        else
        { //it's the same message - set its index, and don't notify again
            mLastTextMsg.confirm(idx, msgid);
            if (!mLastTextMsg.mIsNotified)
                notifyLastTextMsg();
        }
    }
    else if (idx > mLastTextMsg.idx())
    {
        onLastTextMsgUpdated(*msg, idx);
    }
    else if (idx == mLastTextMsg.idx() && !mLastTextMsg.mIsNotified)
    {
        notifyLastTextMsg();
    }
    return idx;
}

void Chat::keyConfirm(KeyId keyxid, KeyId keyid)
{
    if (keyxid != 0xffffffff)
    {
        CHATID_LOG_ERROR("keyConfirm: Key transaction id != 0xffffffff, continuing anyway");
    }
    if (mSending.empty())
    {
        CHATID_LOG_ERROR("keyConfirm: Sending queue is empty");
        return;
    }
    CALL_CRYPTO(onKeyConfirmed, keyxid, keyid);
}

void Chat::rejectMsgupd(Id id, uint8_t serverReason)
{
    if (mSending.empty())
        throw std::runtime_error("rejectMsgupd: Send queue is empty");
    auto& front = mSending.front();
    auto opcode = front.opcode();
    if (opcode != OP_MSGUPD && opcode != OP_MSGUPDX)
        throw std::runtime_error(std::string("rejectMsgupd: Front of send queue does not match - expected opcode MSGUPD or MSGUPDX, actual opcode: ")
        +Command::opcodeToStr(opcode));
    auto& msg = *front.msg;
    if (msg.id() != id)
        throw std::runtime_error("rejectMsgupd: Message msgid/msgxid does not match the one at the front of send queue");

    /* Server reason:
        0 - insufficient privs or not in chat
        1 - message is not your own or you are outside the time window
        2 - message did not change (with same content)
    */
    if (serverReason == 2)
    {
        CALL_LISTENER(onEditRejected, msg, kManualSendEditNoChange);
        CALL_DB(deleteItemFromSending, mSending.front().rowid);
        mSending.pop_front();
    }
    else
    {
        moveItemToManualSending(mSending.begin(), (serverReason == 0)
            ? kManualSendNoWriteAccess : kManualSendTooOld);
    }
}

template<bool mustBeInSending>
void Chat::rejectGeneric(uint8_t opcode)
{
    if (!mustBeInSending)
        return;

    if (mSending.empty())
    {
        throw std::runtime_error("rejectGeneric(mustBeInSending): Send queue is empty");
    }
    if (mSending.front().opcode() != opcode)
    {
        throw std::runtime_error("rejectGeneric(mustBeInSending): Rejected command is not at the front of the send queue");
    }
    CALL_DB(deleteItemFromSending, mSending.front().rowid);
    mSending.pop_front();
}

void Chat::onMsgUpdated(Message* cipherMsg)
{
//first, if it was us who updated the message confirm the update by removing any
//queued msgupds from sending, even if they are not the same edit (i.e. a received
//MSGUPD from another client with out user will cancel any pending edit by our client
    if (cipherMsg->userid == client().userId())
    {
        for (auto it = mSending.begin(); it != mSending.end(); )
        {
            auto& item = *it;
            if (((item.opcode() != OP_MSGUPD) && (item.opcode() != OP_MSGUPDX))
                || (item.msg->id() != cipherMsg->id()))
            {
                it++;
                continue;
            }
            //erase item
            CALL_DB(deleteItemFromSending, item.rowid);
            auto erased = it;
            it++;
            mPendingEdits.erase(cipherMsg->id());
            mSending.erase(erased);
        }
    }
    mCrypto->msgDecrypt(cipherMsg)
    .then([this](Message* msg)
    {
        assert(!msg->isEncrypted());
        //update in db
        CALL_DB(updateMsgInHistory, msg->id(), *msg);
        //update in memory, if loaded
        auto msgit = mIdToIndexMap.find(msg->id());
        Idx idx;
        uint8_t prevType;
        if (msgit != mIdToIndexMap.end())
        {
            idx = msgit->second;
            auto& histmsg = at(idx);
            prevType = histmsg.type;
            histmsg.takeFrom(std::move(*msg));
            histmsg.updated = msg->updated;
            histmsg.type = msg->type;
            histmsg.userid = msg->userid;
            // msg.ts is zero - chatd doesn't send the original timestamp
            CALL_LISTENER(onMessageEdited, histmsg, idx);
            //last text msg stuff
            if ((mLastTextMsg.idx() == idx) && (msg->type != Message::kMsgTruncate))
            {
                //our last text message was edited
                if (histmsg.isText()) //same message, but with updated contents
                {
                    onLastTextMsgUpdated(histmsg, idx);
                }
                else //our last text msg was deleted or changed to management
                {    //message, find another one
                    findAndNotifyLastTextMsg();
                }
            }
        }
        else
        {
            idx = CHATD_IDX_INVALID;
            prevType = Message::kMsgInvalid;
        }

        if (msg->type == Message::kMsgTruncate)
        {
            if (prevType != Message::kMsgTruncate)
            {
                handleTruncate(*msg, idx);
            }
            else
            {
                CHATID_LOG_DEBUG("Skipping replayed truncate MSGUPD");
            }
        }
    })
    .fail([this, cipherMsg](const promise::Error& err)
    {
        CHATID_LOG_ERROR("Error decrypting edit of message %s: %s",
            ID_CSTR(cipherMsg->id()), err.what());
    });
}
void Chat::handleTruncate(const Message& msg, Idx idx)
{
// chatd may re-send a MSGUPD at login, if there are no newer messages in the
// chat. We have to be prepared to handle this, i.e. handleTruncate() must
// be idempotent.
// However, handling the SEEN and RECEIVED pointers in in a replayed truncate
// is a bit tricky, because if they point to the truncate point (i.e. idx)
// normally we would set them in a way that makes the truncate management message
// at the truncation point unseen. But in case of a replay, we don't want it
// to be unseen, as this will reset the unread message count to '1+' every time
// the client connects, until someoone posts a new message in the chat.
// To avoid this, we have to detect the replay. But if we detect it, we can actually
// avoid the whole replay (even the idempotent part), and just bail out.

    CHATID_LOG_DEBUG("Truncating chat history before msgid %s, idx %d, fwdStart %d", ID_CSTR(msg.id()), idx, mForwardStart);
    CALL_DB(truncateHistory, msg);
    if (idx != CHATD_IDX_INVALID)
    {
        //GUI must detach and free any resources associated with
        //messages older than the one specified
        CALL_LISTENER(onHistoryTruncated, msg, idx);
        deleteMessagesBefore(idx);
        if (mLastSeenIdx != CHATD_IDX_INVALID)
        {
            if (mLastSeenIdx <= idx)
            {
                //if we haven't seen even messages before the truncation point,
                //now we will have not seen any message after the truncation
                mLastSeenIdx = CHATD_IDX_INVALID;
                mLastSeenId = 0;
                CALL_DB(setLastSeen, 0);
            }
        }
        if (mLastReceivedIdx != CHATD_IDX_INVALID)
        {
            if (mLastReceivedIdx <= idx)
            {
                mLastReceivedIdx = CHATD_IDX_INVALID;
                mLastReceivedId = 0;
                CALL_DB(setLastReceived, 0);
            }
        }
    }

    ChatDbInfo info;
    mDbInterface->getHistoryInfo(info);
    mOldestKnownMsgId = info.oldestDbId;
    if (mOldestKnownMsgId)
    {
        mHasMoreHistoryInDb = (at(lownum()).id() != mOldestKnownMsgId);
    }
    else
    {
        mHasMoreHistoryInDb = false;
    }
    CALL_LISTENER(onUnreadChanged);
    findAndNotifyLastTextMsg();
}

Id Chat::makeRandomId()
{
    static std::uniform_int_distribution<uint64_t>distrib(0, 0xffffffffffffffff);
    static std::random_device rd;
    return distrib(rd);
}

void Chat::deleteMessagesBefore(Idx idx)
{
    //delete everything before idx, but not including idx
    if (idx > mForwardStart)
    {
        mBackwardList.clear();
        auto delCount = idx-mForwardStart;
        mForwardList.erase(mForwardList.begin(), mForwardList.begin()+delCount);
        mForwardStart += delCount;
    }
    else
    {
        mBackwardList.erase(mBackwardList.begin()+mForwardStart-idx, mBackwardList.end());
    }
}

Message::Status Chat::getMsgStatus(const Message& msg, Idx idx) const
{
    assert(idx != CHATD_IDX_INVALID);
    if (msg.userid == mClient.mUserId)
    {
        if (msg.isSending())
            return Message::kSending;

        // Check if we have an unconfirmed edit
        for (auto& item: mSending)
        {
            if (item.msg->id() == msg.id())
            {
                auto op = item.opcode();
                if (op == OP_MSGUPD || op == OP_MSGUPDX)
                    return Message::kSending;
            }
        }
        if (idx <= mLastReceivedIdx)
            return Message::kDelivered;
        else
        {
            return Message::kServerReceived;
        }
    } //message is from a peer
    else
    {
        if (mLastSeenIdx == CHATD_IDX_INVALID)
            return Message::kNotSeen;
        return (idx <= mLastSeenIdx) ? Message::kSeen : Message::kNotSeen;
    }
}
/* We have 3 stages:
 - add to history buffer, allocating an index
 - decrypt - may not happen asynchronous if crypto needs to fetch stuff from network.
 Also the message may be undecryptable - in this case continue as normal, but message's
 isEncrypted() flag will be set to true, so GUI can decide how to show it
 - add message to history db (including its isEncrypted() state(), handle received
 and seen pointers, call old/new message user callbacks. This may be executed for
 messages that are actually encrypted - app must be prepared for that
*/
Idx Chat::msgIncoming(bool isNew, Message* message, bool isLocal)
{
    assert((isLocal && !isNew) || !isLocal);
    auto msgid = message->id();
    assert(msgid);
    Idx idx;

    if (isNew)
    {
        push_forward(message);
        idx = highnum();
        if (!mOldestKnownMsgId)
            mOldestKnownMsgId = msgid;
    }
    else
    {
        if (!isLocal)
        {
            //history message older than the oldest we have
            assert(isFetchingFromServer());
            assert(message->isEncrypted() == 1);
            mLastServerHistFetchCount++;
            if (mHasMoreHistoryInDb)
            { //we have db history that is not loaded, so we determine the index
              //by the db, and don't add the message to RAM
                idx = mDbInterface->getOldestIdx()-1;
            }
            else
            {
                //all history is in RAM, determine the index from RAM
                push_back(message);
                idx = lownum();
            }
            //shouldn't we update this only after we save the msg to db?
            mOldestKnownMsgId = msgid;
        }
        else //local history message - load from DB to RAM
        {
            push_back(message);
            idx = lownum();
            if (msgid == mOldestKnownMsgId)
            //we have just processed the oldest message from the db
                mHasMoreHistoryInDb = false;
        }
    }
    mIdToIndexMap[msgid] = idx;
    handleLastReceivedSeen(msgid);
    msgIncomingAfterAdd(isNew, isLocal, *message, idx);
    return idx;
}

bool Chat::msgIncomingAfterAdd(bool isNew, bool isLocal, Message& msg, Idx idx)
{
    if (isLocal)
    {
        assert(!msg.isEncrypted());
        msgIncomingAfterDecrypt(isNew, true, msg, idx);
        return true;
    }
    else
    {
        assert(msg.isEncrypted() == 1); //no decrypt attempt was made
    }

    try
    {
        mCrypto->handleLegacyKeys(msg);
    }
    catch(std::exception& e)
    {
        CHATID_LOG_WARNING("handleLegacyKeys threw error: %s\n"
            "Queued messages for decrypt: %d - %d. Ignoring", e.what(),
            mDecryptOldHaltedAt, idx);
    }

    if (at(idx).isEncrypted() != 1)
    {
        CHATID_LOG_DEBUG("handleLegacyKeys already decrypted msg %s, bailing out", ID_CSTR(msg.id()));
        return true;
    }

    if (isNew)
    {
        if (mDecryptNewHaltedAt != CHATD_IDX_INVALID)
        {
            CHATID_LOG_DEBUG("Decryption of new messages is halted, message queued for decryption");
            return false;
        }
    }
    else
    {
        if (mDecryptOldHaltedAt != CHATD_IDX_INVALID)
        {
            CHATID_LOG_DEBUG("Decryption of old messages is halted, message queued for decryption");
            return false;
        }
    }
    CHATD_LOG_CRYPTO_CALL("Calling ICrypto::decrypt()");
    auto pms = mCrypto->msgDecrypt(&msg);
    if (pms.succeeded())
    {
        assert(!msg.isEncrypted());
        msgIncomingAfterDecrypt(isNew, false, msg, idx);
        return true;
    }

    CHATID_LOG_DEBUG("Decryption could not be done immediately, halting for next messages");
    if (isNew)
        mDecryptNewHaltedAt = idx;
    else
        mDecryptOldHaltedAt = idx;

    auto message = &msg;
    pms.fail([this, message, idx](const promise::Error& err) -> promise::Promise<Message*>
    {
        assert(message->isEncrypted() == 1);
        message->setEncrypted(2);
        if ((err.type() != SVCRYPTO_ERRTYPE) ||
            (err.code() != SVCRYPTO_ENOKEY))
        {
            CHATID_LOG_ERROR(
                "Unrecoverable decrypt error at message %s(idx %d):'%s'\n"
                "Message will not be decrypted", ID_CSTR(message->id()), idx, err.toString().c_str());
        }
        else
        {
            //we have a normal situation where a message was sent just before a user joined, so it will be undecryptable
            //TODO: assert chatroom is not 1on1
            CHATID_LOG_WARNING("No key to decrypt message %s, possibly message was sent just before user joined", ID_CSTR(message->id()));
        }
        return message;
    })
    .then([this, isNew, isLocal, idx](Message* message)
    {
#ifndef NDEBUG
        if (isNew)
            assert(mDecryptNewHaltedAt == idx);
        else
            assert(mDecryptOldHaltedAt == idx);
#endif
        msgIncomingAfterDecrypt(isNew, false, *message, idx);
        if (isNew)
        {
            // Decrypt the rest - try to decrypt immediately (synchromously),
            // so that order is guaranteed. Bail out of the loop at the first
            // message that can't be decrypted immediately(msgIncomingAfterAdd()
            // returns false). Will continue when the delayed decrypt finishes

            auto first = mDecryptNewHaltedAt + 1;
            mDecryptNewHaltedAt = CHATD_IDX_INVALID;
            auto last = highnum();
            for (Idx i = first; i <= last; i++)
            {
                if (!msgIncomingAfterAdd(isNew, false, at(i), i))
                    break;
            }
            if ((mServerFetchState == kHistDecryptingNew) &&
                (mDecryptNewHaltedAt == CHATD_IDX_INVALID)) //all messages decrypted
            {
                mServerFetchState = kHistNotFetching;
            }
        }
        else
        {
            // Old history
            // Decrypt the rest synchronously, bail out on first that can't
            // decrypt synchonously.
            // Local messages are always decrypted, this is handled
            // at the start of this func

            assert(!isLocal);
            auto first = mDecryptOldHaltedAt - 1;
            mDecryptOldHaltedAt = CHATD_IDX_INVALID;
            auto last = lownum();
            for (Idx i = first; i >= last; i--)
            {
                if (!msgIncomingAfterAdd(isNew, false, at(i), i))
                    break;
            }
            if ((mServerFetchState == kHistDecryptingOld) &&
                (mDecryptOldHaltedAt == CHATD_IDX_INVALID))
            {
                mServerFetchState = kHistNotFetching;
                if (mServerOldHistCbEnabled)
                {
                    CALL_LISTENER(onHistoryDone, kHistSourceServer);
                }
            }
        }
    });

    return false; //decrypt was not done immediately
}

// Save to history db, handle received and seen pointers, call new/old message user callbacks
void Chat::msgIncomingAfterDecrypt(bool isNew, bool isLocal, Message& msg, Idx idx)
{
    assert(idx != CHATD_IDX_INVALID);
    if (!isNew)
    {
        mLastHistDecryptCount++;
    }
    auto msgid = msg.id();
    if (!isLocal)
    {
        assert(msg.isEncrypted() != 1); //either decrypted or error
        if (!msg.empty() && (*msg.buf() == 0)) //'special' message - attachment etc
        {
            if (msg.dataSize() < 2)
                CHATID_LOG_ERROR("Malformed special message received - starts with null char received, but its length is 1. Assuming type of normal message");
            else
                msg.type = msg.buf()[1];
        }

        verifyMsgOrder(msg, idx);
        CALL_DB(addMsgToHistory, msg, idx);
        if ((msg.userid != mClient.mUserId) &&
           ((mLastReceivedIdx == CHATD_IDX_INVALID) || (idx > mLastReceivedIdx)))
        {
            sendCommand(Command(OP_RECEIVED) + mChatId + msgid);
        }
    }

    auto status = getMsgStatus(msg, idx);
    if (isNew)
    {
        CALL_LISTENER(onRecvNewMessage, idx, msg, status);
    }
    else
    {
        // old message
        // local messages are obtained on-demand, so if isLocal,
        // then always send to app
        if (isLocal || mServerOldHistCbEnabled)
        {
            CALL_LISTENER(onRecvHistoryMessage, idx, msg, status, isLocal);
        }
    }

    if (isNew || (mLastSeenIdx == CHATD_IDX_INVALID))
        CALL_LISTENER(onUnreadChanged);

    //handle last text message
    if (msg.isText())
    {
        if ((mLastTextMsg.state() != LastTextMsgState::kHave) //we don't have any last-text-msg yet, just use any
        || (mLastTextMsg.idx() == CHATD_IDX_INVALID) //current last-text-msg is a pending send, always override it
        || (idx > mLastTextMsg.idx())) //we have a newer message
        {
            onLastTextMsgUpdated(msg, idx);
        }
        else if (idx == mLastTextMsg.idx() && !mLastTextMsg.mIsNotified)
        { //we have already updated mLastTextMsg because app called
          //lastTextMessage() from the onRecvXXX callback, but we haven't done
          //onLastTextMessageUpdated() with it
            notifyLastTextMsg();
        }
    }
    onMsgTimestamp(msg.ts);
}

void Chat::onMsgTimestamp(uint32_t ts)
{
    if (ts <= mLastMsgTs)
        return;
    mLastMsgTs = ts;
    CALL_LISTENER(onLastMessageTsUpdated, ts);
}

void Chat::verifyMsgOrder(const Message& msg, Idx idx)
{
    if (!msg.backRefId)
        return;
    if (!mRefidToIdxMap.emplace(msg.backRefId, idx).second)
    {
        CALL_LISTENER(onMsgOrderVerificationFail, msg, idx, "A message with that backrefId "+std::to_string(msg.backRefId)+" already exists");
        return;
    }
    for (auto refid: msg.backRefs)
    {
        auto it = mRefidToIdxMap.find(refid);
        if (it == mRefidToIdxMap.end())
            continue;
        Idx targetIdx = it->second;
        if (targetIdx >= idx)
        {
            CALL_LISTENER(onMsgOrderVerificationFail, msg, idx, "Message order verification failed, possible history tampering");
            return;
        }
    }
}

void Chat::handleLastReceivedSeen(Id msgid)
{
    //normally the indices will not be set if mLastXXXId == msgid, as there will be only
    //one chance to set the idx (we receive the msg only once).
    if (msgid == mLastSeenId) //we didn't have the message when we received the last seen id
    {
        CHATID_LOG_DEBUG("Received the message with the last-seen msgid '%s', "
            "setting the index pointer to it", ID_CSTR(msgid));
        onLastSeen(msgid);
    }
    if (mLastReceivedId == msgid)
    {
        //we didn't have the message when we received the last received msgid pointer,
        //and now we just received the message - set the index pointer
        CHATID_LOG_DEBUG("Received the message with the last-received msgid '%s', "
            "setting the index pointer to it", ID_CSTR(msgid));
        onLastReceived(msgid);
    }
}

void Chat::onUserJoin(Id userid, Priv priv)
{
    if (userid == client().userId())
        mOwnPrivilege = priv;
    if (mOnlineState == kChatStateJoining)
    {
        mUserDump.insert(userid);
    }
    else if (mOnlineState == kChatStateOnline)
    {
        mUsers.insert(userid);
        CALL_CRYPTO(onUserJoin, userid);
        CALL_LISTENER(onUserJoin, userid, priv);
    }
    else
    {
        throw std::runtime_error("onUserJoin received while not joining and not online");
    }
}

void Chat::onUserLeave(Id userid)
{
    if (mOnlineState != kChatStateOnline)
        throw std::runtime_error("onUserLeave received while not online");

    mUsers.erase(userid);
    CALL_CRYPTO(onUserLeave, userid);
    CALL_LISTENER(onUserLeave, userid);
}

void Connection::notifyLoggedIn()
{
    if (mLoginPromise.done())
        return;
    mState = kStateLoggedIn;
    assert(mConnectPromise.succeeded());
    mLoginPromise.resolve();
}

void Chat::onJoinComplete()
{
    mConnection.notifyLoggedIn();
    if (mUsers != mUserDump)
    {
        mUsers.swap(mUserDump);
        CALL_CRYPTO(setUsers, &mUsers);
    }
    mUserDump.clear();
    mEncryptionHalted = false;
    auto unconfirmedKeyCmd = mCrypto->unconfirmedKeyCmd();
    if (unconfirmedKeyCmd)
    {
        CHATID_LOG_DEBUG("Re-sending unconfirmed key");
        sendCommand(*unconfirmedKeyCmd);
    }

    setOnlineState(kChatStateOnline);
    flushOutputQueue(true); //flush encrypted messages

    if (mIsFirstJoin)
    {
        mIsFirstJoin = false;
        if (!mLastTextMsg.isValid())
        {
            CHATID_LOG_DEBUG("onJoinComplete: Haven't received a text message during join, getting last text message on-demand");
            findAndNotifyLastTextMsg();
        }
    }
}

void Chat::resetGetHistory()
{
    mNextHistFetchIdx = CHATD_IDX_INVALID;
    mServerOldHistCbEnabled = false;
}

void Chat::setOnlineState(ChatState state)
{
    if (state == mOnlineState)
        return;
    mOnlineState = state;
    CHATID_LOG_DEBUG("Online state changed to %s", chatStateToStr(mOnlineState));
    CALL_CRYPTO(onOnlineStateChange, state);
    CALL_LISTENER(onOnlineStateChange, state);
}

void Chat::onLastTextMsgUpdated(const Message& msg, Idx idx)
{
    //idx == CHATD_IDX_INVALID when we notify about a message in the send queue
    //either (msg.isSending() && idx-is-invalid) or (!msg.isSending() && index-is-valid)
    assert(!((idx == CHATD_IDX_INVALID) ^ msg.isSending()));
    assert(!msg.empty());
    mLastTextMsg.assign(msg, idx);
    notifyLastTextMsg();
}

void Chat::notifyLastTextMsg()
{
    CALL_LISTENER(onLastTextMessageUpdated, mLastTextMsg);
    mLastTextMsg.mIsNotified = true;
}

uint8_t Chat::lastTextMessage(LastTextMsg*& msg)
{
    if (mLastTextMsg.isValid())
    {
        msg = &mLastTextMsg;
        return LastTextMsgState::kHave;
    }
    if (mLastTextMsg.isFetching())
        return LastTextMsgState::kFetching;

    findLastTextMsg();
    if (mLastTextMsg.isValid())
    {
        msg = &mLastTextMsg;
        return LastTextMsgState::kHave;
    }

    msg = nullptr;
    if ((mOnlineState == kChatStateJoining) || (mServerFetchState & kHistFetchingOldFromServer))
    {
        CHATID_LOG_DEBUG("getLastTextMsg: We are joining or fetch is in progress");
        return LastTextMsgState::kFetching;
    }
    else
    {
        return mLastTextMsg.state();
    }
}

void Chat::findLastTextMsg()
{
    if (!mSending.empty())
    {
        for (auto it = mSending.rbegin(); it!= mSending.rend(); it++)
        {
            assert(it->msg);
            auto& msg = *it->msg;
            if (msg.isText())
            {
                mLastTextMsg.assign(msg, CHATD_IDX_INVALID);
                CHATID_LOG_DEBUG("lastTextMessage: Text message found in send queue");
                return;
            }
        }
    }
    if (!empty())
    {
        //check in ram
        auto low = lownum();
        for (Idx i=highnum(); i >= low; i--)
        {
            auto& msg = at(i);
            if (msg.isText())
            {
                mLastTextMsg.assign(msg, i);
                CHATID_LOG_DEBUG("lastTextMessage: Text message found in RAM");
                return;
            }
        }
        //check in db
        CALL_DB(getLastTextMessage, lownum()-1, mLastTextMsg);
        if (mLastTextMsg.isValid())
        {
            CHATID_LOG_DEBUG("lastTextMessage: Text message found in DB");
            return;
        }
    }
    if (mHaveAllHistory)
    {
        CHATID_LOG_DEBUG("lastTextMessage: No text message in whole history");
        assert(!mLastTextMsg.isValid());
        return;
    }

    //we are empty or there is no text messsage in ram or db - fetch from server
    if (mOnlineState != kChatStateOnline)
    {
//      CHATID_LOG_DEBUG("lastTextMesage: We are not online, can't fetch messages from server");
        return;
    }
    CHATID_LOG_DEBUG("lastTextMessage: No text message found locally, fetching more history from server");
    mServerOldHistCbEnabled = false;
    requestHistoryFromServer(-16);
    mLastTextMsg.setState(LastTextMsgState::kFetching);
}

void Chat::findAndNotifyLastTextMsg()
{
    auto wptr = weakHandle();
    marshallCall([wptr, this]() //prevent re-entrancy
    {
        if (wptr.deleted())
            return;
        findLastTextMsg();
        if (mLastTextMsg.state() == LastTextMsgState::kFetching)
            return;
        notifyLastTextMsg();
    });

}

void Chat::sendTypingNotification()
{
    sendCommand(Command(OP_BROADCAST) + mChatId + karere::Id::null() +(uint8_t)Command::kBroadcastUserTyping);
}

void Chat::handleBroadcast(karere::Id from, uint8_t type)
{
    if (type == Command::kBroadcastUserTyping)
        CALL_LISTENER(onUserTyping, from);
}

bool Chat::manualResendWhenUserJoins() const
{
    return mClient.manualResendWhenUserJoins();
}

void Client::leave(Id chatid)
{
    auto conn = mConnectionForChatId.find(chatid);
    if (conn == mConnectionForChatId.end())
    {
        CHATD_LOG_ERROR("Client::leave: Unknown chat %s", ID_CSTR(chatid));
        return;
    }
    conn->second->mChatIds.erase(chatid);
    mConnectionForChatId.erase(conn);
    mChatForChatId.erase(chatid);
}

#define RET_ENUM_NAME(name) case OP_##name: return #name;
const char* Command::opcodeToStr(uint8_t opcode)
{
    switch (opcode)
    {
        RET_ENUM_NAME(KEEPALIVE);
        RET_ENUM_NAME(JOIN);
        RET_ENUM_NAME(OLDMSG);
        RET_ENUM_NAME(NEWMSG);
        RET_ENUM_NAME(MSGUPD);
        RET_ENUM_NAME(SEEN);
        RET_ENUM_NAME(RECEIVED);
        RET_ENUM_NAME(RETENTION);
        RET_ENUM_NAME(HIST);
        RET_ENUM_NAME(RANGE);
        RET_ENUM_NAME(NEWMSGID);
        RET_ENUM_NAME(REJECT);
        RET_ENUM_NAME(BROADCAST);
        RET_ENUM_NAME(HISTDONE);
        RET_ENUM_NAME(NEWKEY);
        RET_ENUM_NAME(KEYID);
        RET_ENUM_NAME(JOINRANGEHIST);
        RET_ENUM_NAME(MSGUPDX);
        RET_ENUM_NAME(MSGID);
        RET_ENUM_NAME(KEEPALIVEAWAY);
        default: return "(invalid opcode)";
    }
}
const char* Message::statusNames[] =
{
  "Sending", "SendingManual", "ServerReceived", "ServerRejected", "Delivered", "NotSeen", "Seen"
};
}
