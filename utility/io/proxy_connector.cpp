// Copyright 2019 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "proxy_connector.h"
#include "utility/io/tcpstream.h"

#include "utility/config.h"
#include "utility/helpers.h"

#ifndef LOG_VERBOSE_ENABLED
    #define LOG_VERBOSE_ENABLED 0
#endif
#include "utility/logger.h"

namespace beam {
namespace io {

ProxyConnector::ProxyConnector(Reactor& r) :
    _reactor(r),
    _connectRequestsPool(config().get_int("io.connect_pool_size", 16, 0, 512)) {};

ProxyConnector::~ProxyConnector() {
    if (!_connectRequests.empty())
    {
        LOG_ERROR() << "proxy connect requests were not cancelled";
    }
}

ProxyConnector::ProxyConnectRequest::~ProxyConnectRequest() {
    LOG_DEBUG() << "~ProxyConnectRequest(); Tag: " << tag;
}

/**
 * @return Lambda called on tcp connection to proxy server.
 * Used to hold context and identify ProxyConnectRequest.
 */
ProxyConnector::OnConnect ProxyConnector::create_connection(
        uint64_t tag,
        Address destination,
        const OnConnect& on_proxy_establish,
        int timeoutMsec,
        bool isTls) {

    // is_tag_free
    assert(_connectRequests.count(tag) == 0);
    
    ProxyConnectRequest* request_ptr = _connectRequestsPool.alloc();
    request_ptr->tag = tag;
    request_ptr->destination = destination;
    request_ptr->on_connection_establish = OnConnect(on_proxy_establish);
    request_ptr->on_proxy_response = OnResponse();
    request_ptr->timeoutMsec = timeoutMsec;
    request_ptr->isTls = isTls;
    _connectRequests[tag] = request_ptr;

    // Lambda used to hold context
    return [this](uint64_t tag, std::unique_ptr<TcpStream>&& new_stream, ErrorCode errorCode) {
        on_tcp_connect(tag, std::move(new_stream), errorCode);
    };
}

void ProxyConnector::release_connection(uint64_t tag, Result res) {
    // TODO: proxy refact to modern pointers
    // TODO: check if @stream destructor is called properly
    LOG_DEBUG() << "release_connection(" << tag << ")";
    
    ProxyConnectRequest* req_ptr = _connectRequests[tag];

    if (!res) {
        req_ptr->on_connection_establish(tag, TcpStream::Ptr(), res.error());
        LOG_DEBUG() << "Proxy connection error: " << error_str(res.error());
    }
    req_ptr->~ProxyConnectRequest();
    _connectRequestsPool.release(req_ptr);
    _connectRequests.erase(tag);
}

void ProxyConnector::on_tcp_connect(uint64_t tag, std::unique_ptr<TcpStream>&& new_stream, ErrorCode errorCode) {
    if (_connectRequests.count(tag) == 0) {
        LOG_DEBUG() << "Proxy connection undefined. Tag: " << tag;
        return; // ~TcpStream() has to close stream
    }
    if (errorCode != EC_OK) {
        LOG_DEBUG() << "Proxy server connection error. Tag: " << tag << ". Error: " << errorCode;
        release_connection(tag, make_unexpected(errorCode));
        return;
    }

    LOG_DEBUG() << "Connected to proxy server. Tag: " << tag;

    // ProxyConnectRequest owns tcp stream instance during negotiation with proxy server.
    _connectRequests[tag]->stream = std::move(new_stream);
    send_auth_methods(tag);
}

void ProxyConnector::send_auth_methods(uint64_t tag) {
    // set callback for proxy auth method response
    _connectRequests[tag]->on_proxy_response = &ProxyConnector::on_auth_method_resp;
    _connectRequests[tag]->stream->enable_read(
        /* Lambda used to hold pointer ProxyConnector instance and ProxyConnectRequest tag
         * in TcpStream cause enable_read() doesn't pass info about
         * stream it was executed on.
         */
        [this, tag](ErrorCode errorCode, void *data, size_t size) {
            return _connectRequests[tag]->on_proxy_response(*this, tag, errorCode, data, size);
        });

    // send authentication methods to proxy server
    auto auth_request = Socks5_Protocol::makeAuthRequest(
        Socks5_Protocol::AuthMethod::NO_AUTHENTICATION_REQUIRED);
    Result res = _connectRequests[tag]->stream->write(auth_request.data(), auth_request.size());
    {
        std::string req = beam::to_hex(auth_request.data(), auth_request.size());
        LOG_DEBUG() << "Write to proxy " << sizeof auth_request << " bytes: " << req;
    }
    if (!res)
    {
        //make_unexpected(EC_ENOTCONN)
        LOG_DEBUG() << "AuthRequest to proxy error: " << error_str(res.error());
        release_connection(tag, res);
    }
}

bool ProxyConnector::on_auth_method_resp(uint64_t tag, ErrorCode errorCode, void *data, size_t size) {
    if (_connectRequests.count(tag) == 0) {
        LOG_DEBUG() << "Proxy connection undefined. Tag: " << tag;
        return false;
    }
    if (errorCode != EC_OK) {
        LOG_DEBUG() << "Proxy auth method response error. Tag: " << tag << ". Error: " << errorCode;
        release_connection(tag, make_unexpected(errorCode));
        return false;
    }
    if (data && size) {
        std::string response = beam::to_hex(data, size);
        LOG_DEBUG() << "Received from proxy " << size << " bytes: " << response;

        Socks5_Protocol::AuthMethod method;
        if (!Socks5_Protocol::parseAuthResp(data, method) ||
            method != Socks5_Protocol::AuthMethod::NO_AUTHENTICATION_REQUIRED) {
            LOG_ERROR() << "Proxy required unsupported auth method.";
            release_connection(tag, make_unexpected(EC_PROXY_AUTH_ERROR));
        }
        // No authentication required
        send_connect_request(tag);        
    }
    else {
        LOG_DEBUG() << "Proxy auth method response error. size: " << size;
        release_connection(tag, make_unexpected(EC_PROXY_AUTH_ERROR));
        return false;
    }
    return true;
}

void ProxyConnector::send_connect_request(uint64_t tag) {
    // set callback for proxy connection request reply
    _connectRequests[tag]->on_proxy_response = &ProxyConnector::on_connect_resp;
    
    auto conn_req = Socks5_Protocol::makeRequest(
        _connectRequests[tag]->destination,
        Socks5_Protocol::Command::CONNECT);

    // Send connection request to proxy server
    Result res = _connectRequests[tag]->stream->write(conn_req.data(), conn_req.size());
    {
        std::string req = beam::to_hex(conn_req.data(), conn_req.size());
        LOG_DEBUG() << "Write to proxy " << sizeof conn_req << " bytes: " << req;
    }
    if (!res)
    {
        LOG_ERROR() << "Write to proxy error: " << error_str(res.error());
        release_connection(tag, res);
    }
}

bool ProxyConnector::on_connect_resp(uint64_t tag, ErrorCode errorCode, void *data, size_t size) {
    if (_connectRequests.count(tag) == 0) {
        LOG_DEBUG() << "Proxy connection undefined. Tag: " << tag;
        return false;
    }
    if (errorCode != EC_OK) {
        LOG_DEBUG() << "Proxy connection response. Tag: " << tag << ". Error: " << errorCode;
        release_connection(tag, make_unexpected(errorCode));
        return false;
    }
    if (data && size) {
        std::string response = beam::to_hex(data, size);
        LOG_DEBUG() << "Received from proxy " << size << " bytes: " << response;

        if (Socks5_Protocol::parseReply(data) != Socks5_Protocol::Reply::OK) {
            LOG_DEBUG() << "Proxy destination connect error.";
            release_connection(tag, make_unexpected(EC_PROXY_REPL_ERROR));
            return false;
        }
        on_connection_established(tag);
        return true;
    }
    LOG_DEBUG() << "Proxy connection response error. size: " << size;
    release_connection(tag, make_unexpected(EC_PROXY_REPL_ERROR));
    return false;
}

void ProxyConnector::on_connection_established(uint64_t tag) {
    ProxyConnectRequest* request = _connectRequests[tag];
    TcpStream::Ptr stream;

    if (request->isTls) {
        if (!create_ssl_context()) {
            release_connection(tag, make_result(EC_SSL_ERROR));
        }
        TcpStream* sslStreamPtr = new SslStream(_sslContext);
        _reactor.move_stream(sslStreamPtr, request->stream.get());
        stream.reset(sslStreamPtr);
    }
    else {
        request->stream->disable_read();
        stream = std::move(request->stream);
    }

    // if (_connectTimer)
    //     _connectTimer->cancel(request->tag);

    // request->on_proxy_response.~OnResponse();
    request->on_connection_establish(tag, std::move(stream), EC_OK);
    // request->on_connection_establish.~OnConnect();
    release_connection(tag, make_result(EC_OK));
}

std::array<uint8_t,3> Socks5_Protocol::makeAuthRequest(AuthMethod method) {
    return {
        ProtoVersion,
        0x01,   /// The NMETHODS field contains the number of method identifier octets
                /// that appear in the METHODS field.
        static_cast<uint8_t>(method)
    };
}

bool Socks5_Protocol::parseAuthResp(void* data, AuthMethod& outMethod) {
    uint8_t* resp = static_cast<uint8_t*>(data);
    if (resp[0] != ProtoVersion)
        return false;
    outMethod = static_cast<AuthMethod>(resp[1]);
    return true;
}

std::array<uint8_t,10> Socks5_Protocol::makeRequest(const Address& addr, Command cmd) {
    const uint32_t ip = addr.ip();
    const uint16_t port = addr.port();
    return std::array<uint8_t,10> {
        ProtoVersion,
        static_cast<uint8_t>(cmd),
        Reserved,
        static_cast<uint8_t>(AddrType::IP_V4),
        static_cast<uint8_t>(ip >> 24),
        static_cast<uint8_t>((ip >> 16) & 0xFF),
        static_cast<uint8_t>((ip >> 8) & 0xFF),
        static_cast<uint8_t>(ip & 0xFF),
        static_cast<uint8_t>((port >> 8) & 0xFF),
        static_cast<uint8_t>(port & 0xFF)
    };
}

Socks5_Protocol::Reply Socks5_Protocol::parseReply(void* data) {
    uint8_t* resp = static_cast<uint8_t*>(data);
    if (resp[0] != ProtoVersion || resp[2] != Reserved)
        return Reply::SOCKS_FAIL;
    // server bound address
    // AddrType addrType = static_cast<AddrType>(resp[3]);
    // if (addrType == AddrType::IP_V4) {
    //     uint32_t ip = resp[4];
    //     ip <<= 8;
    //     ip += resp[5];
    //     ip <<= 8;
    //     ip += resp[6];
    //     ip <<= 8;
    //     ip += resp[7];
    //     uint16_t port = resp[8];
    //     port <<= 8;
    //     port += resp[9];
    // }
    return static_cast<Reply>(resp[1]);
}

}   // namespace beam
}   // namespace io
