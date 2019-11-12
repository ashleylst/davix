/*
 * This File is part of Davix, The IO library for HTTP based protocols
 * Copyright (C) CERN 2013
 * Author: Adrien Devresse <adrien.devresse@cern.ch>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
*/

#include <string>
#include <algorithm>
#include <mutex>
#include <davix_internal.hpp>
#include "neonsessionfactory.hpp"

#include <utils/davix_logger_internal.hpp>

namespace Davix {

static std::once_flag neon_once;

static void init_neon(){
    ne_sock_init();
}

NEONSessionFactory::NEONSessionFactory() {
    std::call_once(neon_once, &init_neon);
    DAVIX_SLOG(DAVIX_LOG_TRACE, DAVIX_LOG_CORE, "HTTP/SSL Session caching {}", (_session_caching?"ENABLED":"DISABLED"));
}

NEONSessionFactory::~NEONSessionFactory(){
    _session_pool.clear();
}

inline std::string davix_session_uri_rewrite(const Uri & u){
    std::string proto = u.getProtocol();
    if(proto.compare(0,4, "http") ==0
            || proto.compare(0,2, "s3") == 0
            || proto.compare(0,3, "dav") == 0
            || proto.compare(0, 6, "gcloud") == 0){
        proto.assign("http");
        if(*(u.getProtocol().rbegin()) == 's')
            proto.append("s");
        return proto;
    }
    return std::string();
}

//------------------------------------------------------------------------------
// Create a NEONSession.
//------------------------------------------------------------------------------
std::unique_ptr<NEONSession> NEONSessionFactory::provideNEONSession(const Uri &uri, const RequestParams &params, DavixError **err) {
    ne_session_ptr internal_session = createNeonSession(params, uri, err);
    if(!internal_session) {
      return {};
    }

    return std::unique_ptr<NEONSession>(new NEONSession(*this, std::move(internal_session), uri, params, err));
}

ne_session_ptr NEONSessionFactory::createNeonSession(const RequestParams & params, const Uri & uri, DavixError **err){
    if(uri.getStatus() == StatusCode::OK){
        std::string scheme = davix_session_uri_rewrite(uri);
        if(scheme.size() > 0){
            return create_recycled_session(params, scheme, uri.getHost(), httpUriGetPort(uri));
        }
    }

    DavixError::setupError(err, davix_scope_http_request(), StatusCode::UriParsingError, fmt::format("impossible to parse {}, not a valid HTTP, S3 or Webdav URL", uri.getString()));
    return ne_session_ptr();
}

void NEONSessionFactory::storeNeonSession(ne_session_ptr sess){
    internal_release_session_handle(std::move(sess));
}

ne_session_ptr NEONSessionFactory::create_session(const RequestParams & params, const std::string & protocol, const std::string &host, unsigned int port){
    ne_session *se;
    se = ne_session_create(protocol.c_str(), host.c_str(), (int) port);

    const Uri* proxy = params.getProxyServer();
    if(se != NULL && proxy != NULL){
        DAVIX_SLOG(DAVIX_LOG_TRACE, DAVIX_LOG_HTTP, " configure mandatory proxy to {}", proxy->getString().c_str());
        const enum ne_sock_sversion version = ((proxy->getProtocol().compare("socks5") ==0)?NE_SOCK_SOCKSV5:NE_SOCK_SOCKSV4);
        const int port_proxy = ((proxy->getPort() ==0)?1080:(proxy->getPort()));
        const std::string & userinfo = proxy->getUserInfo();
        std::string user, password;
        std::string::const_iterator delimiter = std::find(userinfo.begin(), userinfo.end(), ':');

        if(delimiter != userinfo.end()){
            user.assign(std::string(userinfo.begin(), delimiter));
            password.assign((delimiter+1), userinfo.end());
            ne_session_socks_proxy(se, version, proxy->getHost().c_str(), port_proxy, user.c_str(), password.c_str());
        }else{
            ne_session_socks_proxy(se, version, proxy->getHost().c_str(),  port_proxy, NULL, NULL);
        }

    }
    //ne_ssl_trust_default_ca(se); not stable in neon on epel 5
    return {se, ne_session_destroy};
}

ne_session_ptr NEONSessionFactory::create_recycled_session(const RequestParams & params, const std::string &protocol, const std::string &host, unsigned int port){

    if(params.getKeepAlive()){
        ne_session_ptr out;
        if(_session_pool.retrieve(create_map_keys_from_URL(protocol, host, port), out)) {
            DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_HTTP, "cached ne_session found ! taken from cache ");
            return out;
        }
    }
    DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_HTTP, "no cached ne_session, create a new one ");
    return create_session(params, protocol, host, port);
}

void NEONSessionFactory::internal_release_session_handle(ne_session_ptr sess){
    // clear sensitive data
    std::string sess_key;
    sess_key.append(ne_get_scheme(sess.get())).append(ne_get_server_hostport(sess.get()));

    DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_HTTP, "add old session to cache {}", sess_key.c_str());
    _session_pool.insert(sess_key, sess);
}

std::string create_map_keys_from_URL(const std::string & protocol, const std::string &host, unsigned int port){
    std::string host_port;
    if( (strcmp(protocol.c_str(), "http") ==0 && port == 80)
            || ( strcmp(protocol.c_str(), "https") ==0 && port == 443)){
        host_port = fmt::format("{}{}", protocol, host);
    }else
        host_port = fmt::format("{}{}:{}", protocol, host, port);
    DAVIX_SLOG(DAVIX_LOG_DEBUG, DAVIX_LOG_HTTP, " creating session keys... {}", host_port);
    return host_port;
}

//------------------------------------------------------------------------------
// Check if session caching is disabled from environment variables
//------------------------------------------------------------------------------
static bool isSessionCachingDisabled(){
  return ( getenv("DAVIX_DISABLE_SESSION_CACHING") != NULL);
}

//------------------------------------------------------------------------------
// Set caching on or off
//------------------------------------------------------------------------------
void NEONSessionFactory::setSessionCaching(bool caching) {
  std::lock_guard<std::mutex> lock(_session_caching_mtx);
  _session_caching = caching && !isSessionCachingDisabled();
}

//------------------------------------------------------------------------------
// Get caching status
//------------------------------------------------------------------------------
bool NEONSessionFactory::getSessionCaching() const {
  std::lock_guard<std::mutex> lock(_session_caching_mtx);
  return _session_caching;
}

} // namespace Davix
