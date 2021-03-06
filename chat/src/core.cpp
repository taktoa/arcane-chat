#include "audiocall.hpp"
#include "core.hpp"
#include "utils.hpp"
#include "options.hpp"
#include "core_db.hpp"

#include <iostream>
#include <cassert>
#include <sstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <QDebug>
#include <QBuffer>
#include <QDataStream>
#include <QSocketNotifier>

using namespace chat;

namespace {
constexpr const char* bootstrap_address = "23.226.230.47";

constexpr int bootstrap_port = 33445;

constexpr const char* bootstrap_key =
    "A09162D68618E742FFBCA1C2C70385E6679604B2D80EA6E84AD0996A1AC8A074";

constexpr uint8_t arcane_lossy_packet_id = 211;
constexpr uint8_t arcane_lossless_packet_id = 171;

tox::LinkType convert_link_type(TOX_CONNECTION link_type) {
    switch(link_type) {
    case TOX_CONNECTION_NONE: return tox::LinkType::none;
    case TOX_CONNECTION_TCP: return tox::LinkType::tcp;
    case TOX_CONNECTION_UDP: return tox::LinkType::udp;
    default: Q_ASSERT(false); return tox::LinkType::none;
    }
}

tox::MessageType convert_message_type(TOX_MESSAGE_TYPE message_type) {
    switch(message_type) {
    case TOX_MESSAGE_TYPE_NORMAL: return tox::MessageType::normal;
    case TOX_MESSAGE_TYPE_ACTION: return tox::MessageType::action;
    default: Q_ASSERT(false); return tox::MessageType::normal;
    }
}

inline const uint8_t* str_to_bytes(const char* str) {
    return reinterpret_cast<const uint8_t*>(str);
}

inline uint8_t* str_to_bytes(char* str) {
    return reinterpret_cast<uint8_t*>(str);
}

QByteArray make_qba(const uint8_t* data, size_t length) {
    return QByteArray(reinterpret_cast<const char*>(data), length);
}

void callback_friend_message(Tox* tox,
                             uint32_t friend_number,
                             TOX_MESSAGE_TYPE type,
                             const uint8_t* message,
                             size_t length,
                             void* user_data) {
    Q_UNUSED(tox);
    Core* core = reinterpret_cast<Core*>(user_data);
    core->handle_message(friend_number, convert_message_type(type),
                         make_qba(message, length));
}

void callback_friend_lossy_packet(Tox* tox,
                                  uint32_t friend_number,
                                  const uint8_t* data,
                                  size_t length,
                                  void* user_data) {
    Q_UNUSED(tox);
    Core* core = reinterpret_cast<Core*>(user_data);
    auto fr = core->friends().find(friend_number);
    Q_ASSERT(fr != core->friends().end());

    if((data[0] == arcane_lossy_packet_id) && (length > 1)) {
        QByteArray arr = make_qba(data + 1, length - 1);
        core->handle_lossy_packet(*fr, arr);
    }
}

void callback_friend_lossless_packet(Tox* tox,
                                     uint32_t friend_number,
                                     const uint8_t* data,
                                     size_t length,
                                     void* user_data) {
    Q_UNUSED(tox);
    Core* core = reinterpret_cast<Core*>(user_data);
    auto fr = core->friends().find(friend_number);
    Q_ASSERT(fr != core->friends().end());

    if((data[0] == arcane_lossless_packet_id) && (length > 1)) {
        QByteArray arr = make_qba(data + 1, length - 1);
        core->handle_lossless_packet(*fr, arr);
    }
}

void callback_friend_name(Tox* tox, uint32_t friend_number, const uint8_t *name, size_t length, void *user_data) {
    Q_UNUSED(tox);
    Core* core = reinterpret_cast<Core*>(user_data);
    auto fr = core->friends().find(friend_number);
    Q_ASSERT(fr != core->friends().end());

    (*fr)->set_username(QString(make_qba(name,length)));
}

void callback_friend_typing(Tox* tox,
                            uint32_t friend_number,
                            bool is_typing,
                            void* user_data) {
    Q_UNUSED(tox);
    Core* core = reinterpret_cast<Core*>(user_data);
    Q_UNUSED(core);
    qDebug() << friend_number << is_typing << "is typing";
}

void callback_friend_request(Tox* tox,
                             const uint8_t* public_key,
                             const uint8_t* message,
                             size_t length,
                             void* user_data) {
    Q_UNUSED(tox);
    Core* core = reinterpret_cast<Core*>(user_data);
    std::string msg{message, message + length};
    // Accept any friend request sent to us
    core->friend_add_norequest(make_qba(public_key, TOX_PUBLIC_KEY_SIZE));
}

void callback_friend_connection_status(Tox* tox,
                                       uint32_t friend_number,
                                       TOX_CONNECTION connection_status,
                                       void* user_data) {
    Q_UNUSED(tox);
    Core* core = reinterpret_cast<Core*>(user_data);
    tox::LinkType link_type = convert_link_type(connection_status);
    auto fr = core->friends().find(friend_number);
    Q_ASSERT(fr != core->friends().end());

    core->handle_friend_connection_status(*fr, link_type);
    //std::cout << __func__ << " " << friend_number << " " << connection_status << "\n";
}

void callback_self_connection_status(Tox* tox,
                                     TOX_CONNECTION connection_status,
                                     void* user_data) {
    Core* core = reinterpret_cast<Core*>(user_data);
    uint8_t toxid[TOX_ADDRESS_SIZE];
    tox_self_get_address(tox, toxid);
    QString tox_printable_id = tox::utils::to_hex(toxid, sizeof(toxid));

    const char* msg = nullptr;

    switch(connection_status) {
    case TOX_CONNECTION_NONE:
        msg = "offline";
        std::cout << "connection lost\n";
        break;
    case TOX_CONNECTION_TCP:
        msg = "connected via tcp";
        std::cout << "tcp connection established\n";
        break;
    case TOX_CONNECTION_UDP:
        msg = "connected via udp";
        std::cout << "udp connection established\n";
        break;
    }

    if(msg != nullptr) {
        qDebug() << "status = " << msg << ", "
                  << "id = " << tox_printable_id;
    }

    core->save_state();
}
} // namespace

Core::Core(QString path) : tox(nullptr), db(new CoreDb(path)), current_channel_(nullptr) {
    tox::options opts;
    uptime.start();
    uptime_offset = 0;
    opts.set_start_port(33445);
    opts.set_end_port(33445 + 100);
    {
        QByteArray new_savedata = db->get_data("savestate");
        if (new_savedata.size() > 0) {
            qDebug() << "loading new profile" << new_savedata.size();
            opts.set_savedata_type(tox::SaveDataType::tox_save);
            opts.set_savedata_data(new_savedata);
        } else {
            // qDebug() << "loading old profile";
            // std::ifstream state_file{savedata_path, std::ios::binary};
            // if(state_file.is_open()) {
            //     std::ostringstream ss;
            //     ss << state_file.rdbuf();
            //     std::string st = ss.str();
            //     opts.set_savedata_type(tox::SaveDataType::tox_save);
            //     opts.set_savedata_data(std::vector<uint8_t>(st.begin(), st.end()));
            // }
        }
    }
    TOX_ERR_NEW new_error;
    tox = tox_new(opts.get_underlying(), &new_error);
    if(!tox) {
        opts.set_ipv6_enabled(false);
        tox = tox_new(opts.get_underlying(), &new_error);
    }

    QSocketNotifier *notifier = new QSocketNotifier(tox_fd_udp(tox), QSocketNotifier::Read, this);
    connect(notifier, SIGNAL(activated(int)), this, SLOT(feed_tox(int)));

    uint8_t toxid[TOX_ADDRESS_SIZE];
    tox_self_get_address(tox, toxid);
    qDebug() << "my id is " << tox::utils::to_hex(toxid, TOX_ADDRESS_SIZE);
    QByteArray bootstrap_pub_key{tox::utils::from_hex(bootstrap_key)};
    tox_bootstrap(tox, bootstrap_address, bootstrap_port,
                  reinterpret_cast<const uint8_t*>(bootstrap_pub_key.data()), nullptr);

    tox_callback_friend_name(tox, callback_friend_name);
    tox_callback_friend_typing(tox, callback_friend_typing);
    tox_callback_friend_request(tox, callback_friend_request);
    tox_callback_friend_message(tox, callback_friend_message);
    tox_callback_friend_lossy_packet(tox, callback_friend_lossy_packet);
    tox_callback_friend_lossless_packet(tox, callback_friend_lossless_packet);
    tox_callback_friend_connection_status(tox, callback_friend_connection_status);
    tox_callback_self_connection_status(tox, callback_self_connection_status);


    int current_name_size = tox_self_get_name_size(tox);
    if (current_name_size) {
        uint8_t buffer[current_name_size];
        tox_self_get_name(tox, buffer);
        this->username = QString::fromStdString(std::string(reinterpret_cast<char*>(buffer),current_name_size));
    } else {
        std::string username = ({
            std::stringstream ss;
            ss << "arcane-" << rand();
            ss.str();
        });
        tox_self_set_name(tox, reinterpret_cast<const uint8_t*>(username.c_str()),
                          username.size(), nullptr);

        this->username = username.c_str();
        qDebug() << "username is now" << this->username;
    }

    iterator.setSingleShot(true);
    connect(&iterator, SIGNAL(timeout()), this, SLOT(check_tox()));
    iterator.setInterval(tox_iteration_interval(tox));
    iterator.start();

    int count = tox_self_get_friend_list_size(tox);
    uint32_t friends[count];
    uint8_t pubkey[TOX_PUBLIC_KEY_SIZE];
    TOX_ERR_FRIEND_GET_PUBLIC_KEY error;
    TOX_ERR_FRIEND_QUERY error2;

    tox_self_get_friend_list(tox, friends);

    qDebug() << "I have" << count << "friends!";

    for(int i = 0; i < count; i++) {
        tox_friend_get_public_key(tox, friends[i], pubkey, &error);
        assert(error == TOX_ERR_FRIEND_GET_PUBLIC_KEY_OK);
        size_t size = tox_friend_get_name_size(tox, friends[i], &error2);
        assert(error2 == TOX_ERR_FRIEND_QUERY_OK);
        uint8_t name[size];
        tox_friend_get_name(tox, friends[i], name, &error2);
        assert(error2 == TOX_ERR_FRIEND_QUERY_OK);

        TOX_CONNECTION link =
            tox_friend_get_connection_status(tox, friends[i], nullptr);

        tox::LinkType newlink = convert_link_type(link);

        Friend* f =
            new Friend{friends[i], make_qba(pubkey, TOX_PUBLIC_KEY_SIZE),
                       QString(make_qba(name, size)), newlink, this};

        this->friends_.insert(f->friend_number, f);
    }

    auto channels_in = db->get_channels();
    for (Channel *c : channels_in) {
        this->channels_.append(c);
    }

    QTimer *syncer = new QTimer(this);
    connect(syncer, SIGNAL(timeout()), this, SLOT(sync_clock()));
    syncer->setSingleShot(false);
    syncer->start(60000);
}

void Core::set_username(QString username) {
    TOX_ERR_SET_INFO error;
    QByteArray raw = qPrintable(username);
    tox_self_set_name(tox, reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), &error);
    if (error != TOX_ERR_SET_INFO_OK) {
        qDebug() << "error changing nick" << error;
        return;
    }
    this->username = username;
}

static qint64 normalize(qint64 x) {
    if (x < 0) return -1 * x;
    else return x;
}

void Core::sync_clock() {
    qint64 total_offset = 0;
    int online = 0;
    int limit = 10;
    if (uptime_offset == 0) limit = 5;

    for (Friend *fr : friends_) {
        if (fr->connection == tox::LinkType::none) continue;
        qDebug() << fr->get_username() << fr->offset.toString();
        if (fr->offset.stddev() > (1000000000l * 10)) continue; // stddev over 1 sec
        if (fr->offset.count() < limit) continue;
        if (fr->offset.stddev() > normalize(fr->offset.average())) {
            qDebug() << "too much jitter";
            continue;
        }
        total_offset += fr->offset.average();
        online++;
    }
    if (online == 0) return;
    qint64 average = total_offset / online;
    qDebug() << "average offset" << util::Stats::shorten(average);
    if (uptime_offset == 0) {
        shift_clock(average);
    } else {
        // once within 10ms, it will slow down the drift rate
        if ((average > -10000000) & (average < 10000000)) {
            shift_clock(average / 10);
        } else {
            shift_clock(average / 2);
        }
    }
}

void Core::shift_clock(qint64 offset) {
    uptime_offset -= offset;
    qDebug() << "shifting" << util::Stats::shorten(offset);
    for (Friend *fr : friends_) {
        if (fr->connection == tox::LinkType::none) continue;
        fr->offset.shift(offset);
    }
}

Core::~Core() {
    save_state();
    tox_kill(tox);
    delete db;
}

void Core::feed_tox(int sock) {
    tox_iterate(tox, this);
    iterator.setInterval(tox_iteration_interval(tox));
    iterator.start();
}

void Core::check_tox() {
    tox_iterate(tox, this);
    iterator.setInterval(tox_iteration_interval(tox));
    iterator.start();
}

void Core::handle_message(uint32_t friend_number,
                          tox::MessageType type,
                          QByteArray message) {
    QString text(message);
    bool is_action = (type == tox::MessageType::action);
    for(chat::Friend* f : friends_) {
        if(f->friend_number == friend_number) {
            emit on_message(f, is_action, text);
            f->new_message(is_action, message);
        }
    }
}

void Core::handle_lossless_packet(Friend* fr, QByteArray message) {
    emit on_lossless_packet(fr, message);
}

void Core::handle_lossy_packet(Friend* fr, QByteArray message) {
    QByteArray payload;

    emit on_lossy_packet(fr, message);

    uint8_t header_size = message[0];
    QByteArray header = message.mid(1,header_size);
    arcane::network::RpcMessage rpc;
    rpc.ParseFromString(header.toStdString());
    uint32_t typecode = rpc.method_id();
    payload = message.mid(1+header_size, rpc.data_size());

    switch (typecode) {
    case arcane::network::call_start: {
        auto it = calls_.find(fr);
        if (it == calls_.end()) {
            CallControl *cc = new CallControl(this,fr);
            cc->show();
            cc->start();
            calls_.insert(fr,cc);
        } else {
            qDebug() << "call acked";
            (*it)->start();
        }
        break; }
    case arcane::network::call_data: {
        arcane::network::CallData data;
        data.ParseFromString(payload.toStdString());
        fr->on_other(data.sent());
        auto it = calls_.find(fr);
        if (it != calls_.end()) {
            (*it)->packet(QByteArray::fromStdString(data.data()));
        }
        break; }
    case arcane::network::call_stop: {
        auto it = calls_.find(fr);
        if (it != calls_.end()) {
            (*it)->stop();
            //delete *it;
            //calls.remove(fr);
        }
        break; }
    case arcane::network::ping: {
        arcane::network::PingPayload data;
        data.ParseFromString(payload.toStdString());
        data.set_received(get_uptime());
        send_packet(fr, arcane::network::pong, &data);
        fr->on_ping(data.sent(), QByteArray::fromStdString(data.data()));
        break; }
    case arcane::network::pong: {
        arcane::network::PingPayload data;
        data.ParseFromString(payload.toStdString());
        fr->on_pong(data.sent(), data.received(), QByteArray::fromStdString(data.data()));
        emit on_pong(fr, data.sent(), data.received(), QByteArray::fromStdString(data.data()));
        break; }
    }
}

void Core::open_call_control(Friend *fr) {
    auto it = calls_.find(fr);
    if (it == calls_.end()) {
        CallControl *cc = new CallControl(this,fr);
        cc->show();
        //cc->start();
        calls_.insert(fr,cc);
    } else {
        qDebug() << "call acked";
    }
}

void Core::handle_friend_connection_status(Friend* fr, tox::LinkType link) {
    fr->set_connection(link);
}

void Core::send_message(uint32_t friend_number, bool action, QString message) {
    TOX_ERR_FRIEND_SEND_MESSAGE error;
    QByteArray bytes = message.toUtf8();
    uint8_t* msg = reinterpret_cast<uint8_t*>(bytes.data());
    size_t size = bytes.size();
    tox_friend_send_message(tox, friend_number, TOX_MESSAGE_TYPE_NORMAL, msg,
                            size, &error);
}

void Core::save_state() {
    size_t size = tox_get_savedata_size(tox);
    QByteArray savedata;
    savedata.resize(size);
    tox_get_savedata(tox, reinterpret_cast<uint8_t*>(savedata.data()));
    db->set_data("savestate", savedata);
}

void Core::friend_add_norequest(const QByteArray public_key) {
    TOX_ERR_FRIEND_ADD error;
    Friend* f;
    uint32_t friend_number = tox_friend_add_norequest(
        tox, reinterpret_cast<const uint8_t*>(public_key.data()), &error);
    switch(error) {
    case TOX_ERR_FRIEND_ADD_OK:
        f = new Friend{friend_number, public_key, QString(),
                       tox::LinkType::none, this};

        friends_.insert(f->friend_number, f);
        emit on_new_friend(f);

        break;
    case TOX_ERR_FRIEND_ADD_ALREADY_SENT: qDebug() << "already sent"; break;
    case TOX_ERR_FRIEND_ADD_BAD_CHECKSUM: qDebug() << "crc error"; break;
    default: qDebug() << "error code: " << error;
    }
    save_state();
}

void Core::friend_add(const QByteArray tox_id, std::string message) {
    TOX_ERR_FRIEND_ADD error;
    Friend* f;
    uint32_t friend_number = tox_friend_add(tox,
                                            str_to_bytes(tox_id.data()),
                                            str_to_bytes(message.c_str()),
                                            message.size(),
                                            &error);
    switch(error) {
    case TOX_ERR_FRIEND_ADD_OK:
        f = new Friend {
            friend_number,
            tox_id,
            QString(),
            tox::LinkType::none,
            this
        };

        friends_.insert(f->friend_number, f);
        emit on_new_friend(f);

        break;
    case TOX_ERR_FRIEND_ADD_ALREADY_SENT: qDebug() << "already sent"; break;
    case TOX_ERR_FRIEND_ADD_BAD_CHECKSUM: qDebug() << "crc error"; break;
    default: qDebug() << "error code: " << error;
    }
    save_state();
}

QString enum_to_string(TOX_ERR_FRIEND_CUSTOM_PACKET error) {
    switch (error) {
    case TOX_ERR_FRIEND_CUSTOM_PACKET_TOO_LONG:
        return "packet too long";
    case TOX_ERR_FRIEND_CUSTOM_PACKET_SENDQ:
        return "sendq overflow";
    default:
        return QString("unknown %1").arg(error);
    }
}

void Core::send_lossy_packet(Friend* fr, QByteArray data) {
    TOX_ERR_FRIEND_CUSTOM_PACKET error;
    auto packet = data.prepend(arcane_lossy_packet_id);
    tox_friend_send_lossy_packet(
        tox, fr->friend_number, reinterpret_cast<const uint8_t*>(packet.data()),
        packet.length(), &error);
    if (error != TOX_ERR_FRIEND_CUSTOM_PACKET_OK) {
        QString msg = enum_to_string(error);
        qDebug() << "send packet error" << msg;
    }
}

void Core::send_lossless_packet(Friend* fr, QByteArray data) {
    TOX_ERR_FRIEND_CUSTOM_PACKET error;
    auto packet = data.prepend(arcane_lossless_packet_id);
    tox_friend_send_lossless_packet(
        tox, fr->friend_number, reinterpret_cast<const uint8_t*>(packet.data()),
                packet.length(), &error);
    if (error != TOX_ERR_FRIEND_CUSTOM_PACKET_OK) {
        QString msg = enum_to_string(error);
        qDebug() << "send packet error" << msg;
    }
}

void Core::send_packet(Friend *fr, arcane::network::Methods methodid, ::google::protobuf::Message *payload) {
    arcane::network::RpcMessage msg;
    msg.set_method_id(methodid);
    QByteArray data;
    if (payload) {
        std::string tmp;
        payload->SerializeToString(&tmp);
        data = QByteArray::fromStdString(tmp);
        msg.set_data_size(data.size());
    }
    std::string packet1;
    msg.SerializeToString(&packet1);
    QByteArray header = QByteArray::fromStdString(packet1);

    QByteArray packet;
    packet.append((uint8_t) header.size());
    packet.append(header);
    packet.append(data);
    //qDebug() << packet.toHex();
    send_lossy_packet(fr, packet);
}

void Core::call_start(Friend *fr) {
    send_packet(fr, arcane::network::call_start);
}

void Core::call_data(Friend *fr, QByteArray data) {
    arcane::network::CallData datapacket;
    datapacket.set_data(data.toStdString());
    datapacket.set_sent(get_uptime());
    send_packet(fr, arcane::network::call_data, &datapacket);
}

void Core::call_stop(Friend *fr) {
    send_packet(fr, arcane::network::call_stop);
}

qint64 Core::get_uptime() {
    return uptime.nsecsElapsed() + uptime_offset;
}

void Core::send_ping(Friend *fr, QByteArray payload) {
    arcane::network::PingPayload p;
    p.set_sent(get_uptime());
    p.set_data(payload.toStdString());
    send_packet(fr, arcane::network::ping, &p);
}

void Core::add_owned_channel(Channel *channel) {
    channels_.append(channel);
    db->save_channel(channel);
}

void Core::join_channel(Channel *channel) {
    if (current_channel_) {
        current_channel_->remove_member(this);
    }
    channel->add_member(this);
    current_channel_ = channel;
}
