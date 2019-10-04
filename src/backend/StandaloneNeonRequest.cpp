/*
 * This File is part of Davix, The IO library for HTTP based protocols
 * Copyright (C) CERN 2019
 * Author: Georgios Bitzes <georgios.bitzes@cern.ch>
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

#include "StandaloneNeonRequest.hpp"
#include <status/davixstatusrequest.hpp>
#include <core/ContentProvider.hpp>
#include <neon/neonsession.hpp>
#include <ne_redirect.h>
#include <ne_request.h>

namespace Davix {

//------------------------------------------------------------------------------
// Wrapper class for neon sessions - setup and tear down hooks
//------------------------------------------------------------------------------
class NeonSessionWrapper {
public:
    NeonSessionWrapper(StandaloneNeonRequest* r, NEONSessionFactory &factory, const Uri &uri, const RequestParams &p, DavixError **err)
        : _r(r) {
        _sess = factory.provideNEONSession(uri, p, err);

        if(_sess && _sess->get_ne_sess() != NULL){
            ne_hook_pre_send(_sess->get_ne_sess(), NeonSessionWrapper::runHookPreSend, (void*) this);
            ne_hook_post_headers(_sess->get_ne_sess(), NeonSessionWrapper::runHookPreReceive, (void*) this);
        }
    }

    virtual ~NeonSessionWrapper() {
        if(_sess->get_ne_sess() != NULL){
            ne_unhook_pre_send(_sess->get_ne_sess(), NeonSessionWrapper::runHookPreSend, (void*) this);
            ne_unhook_post_headers(_sess->get_ne_sess(), NeonSessionWrapper::runHookPreReceive, (void*) this);
        }
    }

    ne_session* get_ne_sess() {
        return _sess->get_ne_sess();
    }

    bool isRecycledSession() const {
        return _sess->isRecycledSession();
    }

    void do_not_reuse_this_session() {
        _sess->do_not_reuse_this_session();
    }

private:
    static void runHookPreSend(ne_request *r, void *userdata, ne_buffer *header) {
      (void) r;

      NeonSessionWrapper* wrapper = (NeonSessionWrapper*) userdata;
      BoundHooks &boundHooks = wrapper->_r->_bound_hooks;
      if(boundHooks.presendHook) {
        std::string header_line(header->data, (header->used)-1);
        boundHooks.presendHook(header_line);
      }
    }

    static void runHookPreReceive(ne_request *r, void *userdata, const ne_status *status) {
      (void) r;

      NeonSessionWrapper* wrapper = (NeonSessionWrapper*) userdata;
      BoundHooks &boundHooks = wrapper->_r->_bound_hooks;
      if(boundHooks.prereceiveHook){
        std::ostringstream header_line;
        HeaderVec headers;
        wrapper->_r->getAnswerHeaders(headers);
        header_line << "HTTP/"<< status->major_version << '.' << status->minor_version
                    << ' ' << status->code << ' ' << status->reason_phrase << '\n';

        boundHooks.prereceiveHook(header_line.str(), headers, status->code);
      }
    }

    std::unique_ptr<NEONSession> _sess;
    StandaloneNeonRequest* _r;
};

//------------------------------------------------------------------------------
// Callback for libneon to use the content provider
//------------------------------------------------------------------------------
static ssize_t content_provider_callback(void* userdata, char* buffer, size_t buflen) {
    ContentProvider *provider = static_cast<ContentProvider*>(userdata);
    return provider->pullBytes(buffer, buflen);
}

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
StandaloneNeonRequest::StandaloneNeonRequest(NEONSessionFactory &sessionFactory, bool reuseSession,
  const BoundHooks &boundHooks, const Uri &uri, const std::string &verb, const RequestParams &params,
  const std::vector<HeaderLine> &headers, int reqFlag, ContentProvider *contentProvider,
  Chrono::TimePoint deadline)

: _session_factory(sessionFactory), _reuse_session(reuseSession), _bound_hooks(boundHooks),
  _uri(uri), _verb(verb), _params(params), _state(RequestState::kNotStarted),
  _headers(headers), _req_flag(reqFlag), _content_provider(contentProvider),
  _deadline(deadline), _neon_req(NULL), _total_read_size(0), _last_read(-1) {}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
StandaloneNeonRequest::~StandaloneNeonRequest() {
  if(_neon_req) {
    ne_request_destroy(_neon_req);
    _neon_req = NULL;
  }
}

//------------------------------------------------------------------------------
// Start request - calling this multiple times will do nothing.
//------------------------------------------------------------------------------
void StandaloneNeonRequest::startRequest(DavixError **err) {
  if(_state != RequestState::kNotStarted) {
    return;
  }

  //----------------------------------------------------------------------------
  // Have we timed out already?
  //----------------------------------------------------------------------------
  if(checkTimeout(err)) {
    markCompleted();
    return;
  }

  //----------------------------------------------------------------------------
  // Retrieve a session, create request
  //----------------------------------------------------------------------------
  DavixError* tmp_err = NULL;
  _session.reset(new NeonSessionWrapper(this, _session_factory, _uri, _params, &tmp_err));

  if(tmp_err){
    _session.reset();
    DavixError::propagateError(err, tmp_err);
    return;
  }

  _neon_req = ne_request_create(_session->get_ne_sess(), _verb.c_str(), _uri.getPathAndQuery().c_str());

  //----------------------------------------------------------------------------
  // Setup headers
  //----------------------------------------------------------------------------
  std::copy(_params.getHeaders().begin(), _params.getHeaders().end(), std::back_inserter(_headers));

  for(size_t i = 0; i < _headers.size(); i++) {
    ne_add_request_header(_neon_req, _headers[i].first.c_str(),  _headers[i].second.c_str());
  }

  //----------------------------------------------------------------------------
  // Setup flags
  //----------------------------------------------------------------------------
  ne_set_request_flag(_neon_req, NE_REQFLAG_EXPECT100, _params.get100ContinueSupport() &&
            (_req_flag & RequestFlag::SupportContinue100));
  ne_set_request_flag(_neon_req, NE_REQFLAG_IDEMPOTENT, _req_flag & RequestFlag::IdempotentRequest);

  if( (_req_flag & RequestFlag::SupportContinue100) == true) {
    _session->do_not_reuse_this_session();
  }

  //----------------------------------------------------------------------------
  // Setup HTTP body
  //----------------------------------------------------------------------------
  if(_content_provider) {
    _content_provider->rewind();
    ne_set_request_body_provider(_neon_req, _content_provider->getSize(),
      content_provider_callback, _content_provider);
  }

  //----------------------------------------------------------------------------
  // We're off to go
  //----------------------------------------------------------------------------
  _state = RequestState::kStarted;
}

//------------------------------------------------------------------------------
// Check if timeout has passed
//------------------------------------------------------------------------------
bool StandaloneNeonRequest::checkTimeout(DavixError **err) {
  if(_deadline.isValid() && _deadline < Chrono::Clock(Chrono::Clock::Monolitic).now()) {
    std::ostringstream ss;
    ss << "timeout of " << _params.getOperationTimeout()->tv_sec << "s";
    DavixError::setupError(err, davix_scope_http_request(), StatusCode::OperationTimeout, ss.str());
    return true;
  }

  return false;
}

//------------------------------------------------------------------------------
// Major read function - read a block of max_size bytes (at max) into buffer.
//------------------------------------------------------------------------------
dav_ssize_t StandaloneNeonRequest::readBlock(char* buffer, dav_size_t max_size, DavixError** err) {

  if(!_neon_req) {
    DavixError::setupError(err, davix_scope_http_request(), StatusCode::AlreadyRunning, "Request has not been started yet");
    return -1;
  }

  if(max_size ==0) {
    return 0;
  }

  if(checkTimeout(err)) {
    markCompleted();
    return -1;
  }

  _last_read = ne_read_response_block(_neon_req, buffer, max_size);
  if(_last_read < 0) {
    DavixError::setupError(err, davix_scope_http_request(), StatusCode::ConnectionProblem, "Invalid read in request");
    _session->do_not_reuse_this_session();
    markCompleted();
    return -1;
  }

  DAVIX_SLOG(DAVIX_LOG_TRACE, DAVIX_LOG_HTTP, "StandaloneNeonRequestNeonRequest::readBlock read {} bytes", _last_read);

  _total_read_size += _last_read;
  return _last_read;
}

//------------------------------------------------------------------------------
// Check request state
//------------------------------------------------------------------------------
RequestState StandaloneNeonRequest::getState() const {
  return _state;
}

//------------------------------------------------------------------------------
// Get a specific response header
//------------------------------------------------------------------------------
bool StandaloneNeonRequest::getAnswerHeader(const std::string &header_name, std::string &value) const {
  if(_neon_req){
    const char* answer_content = ne_get_response_header(_neon_req, header_name.c_str());
    if(answer_content) {
      value = answer_content;
      return true;
    }
  }

  return false;
}

//------------------------------------------------------------------------------
// Get all response headers
//------------------------------------------------------------------------------
size_t StandaloneNeonRequest::getAnswerHeaders( HeaderVec & vec_headers) const {
  if(_neon_req) {
    void * handle = NULL;
    const char* name = NULL, *value = NULL;
    while( (handle = ne_response_header_iterate(_neon_req, handle, &name, &value)) != NULL){
      vec_headers.push_back(std::pair<std::string, std::string>(name, value));
    }
  }

  return vec_headers.size();
}

//------------------------------------------------------------------------------
// Mark request as completed, release any resources
//------------------------------------------------------------------------------
void StandaloneNeonRequest::markCompleted() {
  if(_state == RequestState::kFinished) {
    return;
  }

  _state = RequestState::kFinished;
  ne_end_request(_neon_req);
  _session.reset();
}


}