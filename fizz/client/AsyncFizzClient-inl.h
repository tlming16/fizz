/*
 *  Copyright (c) 2018-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree.
 */

namespace fizz {
namespace client {

template <typename SM>
AsyncFizzClientT<SM>::AsyncFizzClientT(
    folly::AsyncTransportWrapper::UniquePtr socket,
    std::shared_ptr<const FizzClientContext> fizzContext,
    const std::shared_ptr<ClientExtensions>& extensions)
    : AsyncFizzBase(std::move(socket)),
      fizzContext_(std::move(fizzContext)),
      extensions_(extensions),
      visitor_(*this),
      fizzClient_(state_, transportReadBuf_, visitor_, this) {}

template <typename SM>
AsyncFizzClientT<SM>::AsyncFizzClientT(
    folly::EventBase* eventBase,
    std::shared_ptr<const FizzClientContext> fizzContext,
    const std::shared_ptr<ClientExtensions>& extensions)
    : AsyncFizzBase(
          folly::AsyncSocket::UniquePtr(new folly::AsyncSocket(eventBase))),
      fizzContext_(std::move(fizzContext)),
      extensions_(extensions),
      visitor_(*this),
      fizzClient_(state_, transportReadBuf_, visitor_, this) {}

template <typename SM>
void AsyncFizzClientT<SM>::connect(
    HandshakeCallback* callback,
    folly::Optional<std::string> hostname,
    std::chrono::milliseconds timeout) {
  auto pskIdentity = hostname;
  connect(
      callback,
      std::make_shared<DefaultCertificateVerifier>(VerificationContext::Client),
      std::move(hostname),
      std::move(pskIdentity),
      std::move(timeout));
}

template <typename SM>
void AsyncFizzClientT<SM>::connect(
    HandshakeCallback* callback,
    std::shared_ptr<const CertificateVerifier> verifier,
    folly::Optional<std::string> sni,
    folly::Optional<std::string> pskIdentity,
    std::chrono::milliseconds timeout) {
  DelayedDestruction::DestructorGuard dg(this);

  CHECK(callback);
  CHECK(!callback_);
  callback_ = callback;

  if (!transport_->good()) {
    folly::AsyncSocketException ase(
        folly::AsyncSocketException::NOT_OPEN,
        "handshake connect called but socket isn't open");
    deliverAllErrors(ase, false);
    return;
  }

  sni_ = sni;
  pskIdentity_ = pskIdentity;

  if (timeout != std::chrono::milliseconds::zero()) {
    startHandshakeTimeout(timeout);
  }

  startTransportReads();

  folly::Optional<CachedPsk> cachedPsk = folly::none;
  if (pskIdentity) {
    cachedPsk = fizzContext_->getPsk(*pskIdentity);
  }
  fizzClient_.connect(
      fizzContext_,
      std::move(verifier),
      std::move(sni),
      std::move(cachedPsk),
      extensions_);
}

template <typename SM>
void AsyncFizzClientT<SM>::connect(
    const folly::SocketAddress& connectAddr,
    folly::AsyncSocket::ConnectCallback* callback,
    std::shared_ptr<const CertificateVerifier> verifier,
    folly::Optional<std::string> sni,
    folly::Optional<std::string> pskIdentity,
    std::chrono::milliseconds totalTimeout,
    std::chrono::milliseconds socketTimeout,
    const folly::AsyncSocket::OptionMap& options,
    const folly::SocketAddress& bindAddr) {
  DelayedDestruction::DestructorGuard dg(this);

  CHECK(callback);
  CHECK(!callback_);
  callback_ = callback;

  verifier_ = std::move(verifier);
  sni_ = sni;
  pskIdentity_ = pskIdentity;

  if (totalTimeout != std::chrono::milliseconds::zero()) {
    startHandshakeTimeout(std::move(totalTimeout));
  }

  auto underlyingSocket =
      transport_->getUnderlyingTransport<folly::AsyncSocket>();
  if (underlyingSocket) {
    underlyingSocket->disableTransparentTls();
    underlyingSocket->connect(
        this,
        connectAddr,
        static_cast<int>(socketTimeout.count()),
        options,
        bindAddr);
  } else {
    folly::AsyncSocketException ase(
        folly::AsyncSocketException::BAD_ARGS,
        "could not find underlying socket");
    deliverAllErrors(ase, false);
  }
}

template <typename SM>
bool AsyncFizzClientT<SM>::good() const {
  return !error() && transport_->good();
}

template <typename SM>
bool AsyncFizzClientT<SM>::readable() const {
  return transport_->readable();
}

template <typename SM>
bool AsyncFizzClientT<SM>::connecting() const {
  return callback_ || transport_->connecting();
}

template <typename SM>
bool AsyncFizzClientT<SM>::error() const {
  return transport_->error() || fizzClient_.inErrorState();
}

template <typename SM>
folly::ssl::X509UniquePtr AsyncFizzClientT<SM>::getPeerCert() const {
  auto serverCert = getPeerCertificate();
  if (serverCert) {
    return serverCert->getX509();
  } else {
    return nullptr;
  }
}

template <typename SM>
const X509* AsyncFizzClientT<SM>::getSelfCert() const {
  auto cert = getSelfCertificate();
  if (cert) {
    return cert->getX509().get();
  } else {
    return nullptr;
  }
}

template <typename SM>
const Cert* AsyncFizzClientT<SM>::getPeerCertificate() const {
  return earlyDataState_ ? getState().earlyDataParams()->serverCert.get()
                         : getState().serverCert().get();
}

template <typename SM>
const Cert* AsyncFizzClientT<SM>::getSelfCertificate() const {
  return earlyDataState_ ? getState().earlyDataParams()->clientCert.get()
                         : getState().clientCert().get();
}

template <typename SM>
bool AsyncFizzClientT<SM>::isReplaySafe() const {
  return !earlyDataState_.hasValue();
}

template <typename SM>
void AsyncFizzClientT<SM>::setReplaySafetyCallback(
    folly::AsyncTransport::ReplaySafetyCallback* callback) {
  DCHECK(!callback || !isReplaySafe());
  replaySafetyCallback_ = callback;
}

template <typename SM>
std::string AsyncFizzClientT<SM>::getApplicationProtocol() noexcept {
  if (earlyDataState_) {
    if (getState().earlyDataParams()->alpn) {
      return *getState().earlyDataParams()->alpn;
    } else {
      return "";
    }
  } else {
    if (getState().alpn()) {
      return *getState().alpn();
    } else {
      return "";
    }
  }
}

template <typename SM>
void AsyncFizzClientT<SM>::close() {
  if (transport_->good()) {
    fizzClient_.appClose();
  } else {
    DelayedDestruction::DestructorGuard dg(this);
    folly::AsyncSocketException ase(
        folly::AsyncSocketException::END_OF_FILE, "socket closed locally");
    deliverAllErrors(ase, false);
    transport_->close();
  }
}

template <typename SM>
void AsyncFizzClientT<SM>::closeWithReset() {
  DelayedDestruction::DestructorGuard dg(this);
  if (transport_->good()) {
    fizzClient_.appClose();
  }
  folly::AsyncSocketException ase(
      folly::AsyncSocketException::END_OF_FILE, "socket closed locally");
  deliverAllErrors(ase, false);
  transport_->closeWithReset();
}

template <typename SM>
void AsyncFizzClientT<SM>::closeNow() {
  DelayedDestruction::DestructorGuard dg(this);
  if (transport_->good()) {
    fizzClient_.appClose();
  }
  folly::AsyncSocketException ase(
      folly::AsyncSocketException::END_OF_FILE, "socket closed locally");
  deliverAllErrors(ase, false);
  transport_->closeNow();
}

template <typename SM>
void AsyncFizzClientT<SM>::connectSuccess() noexcept {
  startTransportReads();

  folly::Optional<CachedPsk> cachedPsk = folly::none;
  if (pskIdentity_) {
    cachedPsk = fizzContext_->getPsk(*pskIdentity_);
  }
  fizzClient_.connect(
      fizzContext_,
      std::move(verifier_),
      sni_,
      std::move(cachedPsk),
      extensions_);
}

template <typename SM>
void AsyncFizzClientT<SM>::connectErr(
    const folly::AsyncSocketException& ex) noexcept {
  deliverAllErrors(ex, false);
}

template <typename SM>
void AsyncFizzClientT<SM>::writeAppData(
    folly::AsyncTransportWrapper::WriteCallback* callback,
    std::unique_ptr<folly::IOBuf>&& buf,
    folly::WriteFlags flags) {
  if (error()) {
    if (callback) {
      callback->writeErr(
          0,
          folly::AsyncSocketException(
              folly::AsyncSocketException::INVALID_STATE,
              "fizz app write in error state"));
    }
    return;
  }

  if (earlyDataState_) {
    auto size = buf->computeChainDataLength();
    if (!earlyDataState_->pendingAppWrites.empty() ||
        size > earlyDataState_->remainingEarlyData) {
      AppWrite w;
      w.callback = callback;
      w.data = std::move(buf);
      w.flags = flags;

      earlyDataState_->remainingEarlyData = 0;
      earlyDataState_->pendingAppWrites.push_back(std::move(w));
    } else {
      EarlyAppWrite w;
      w.callback = callback;
      w.data = std::move(buf);
      w.flags = flags;

      if (earlyDataRejectionPolicy_ ==
          EarlyDataRejectionPolicy::AutomaticResend) {
        // We need to call unshare() to make a copy of the data here since we
        // may need to resend it after we've already called writeSuccess().
        // Particularly when using the write and writev interfaces, the
        // application is allowed to delete the underlying buffer after getting
        // the write callback.
        auto writeCopy = w.data->clone();
        writeCopy->unshare();
        earlyDataState_->resendBuffer.append(std::move(writeCopy));
      }

      earlyDataState_->remainingEarlyData -= size;
      fizzClient_.earlyAppWrite(std::move(w));
    }
  } else {
    AppWrite w;
    w.callback = callback;
    w.data = std::move(buf);
    w.flags = flags;
    fizzClient_.appWrite(std::move(w));
  }
}

template <typename SM>
void AsyncFizzClientT<SM>::transportError(
    const folly::AsyncSocketException& ex) {
  DelayedDestruction::DestructorGuard dg(this);
  deliverAllErrors(ex);
}

template <typename SM>
void AsyncFizzClientT<SM>::transportDataAvailable() {
  fizzClient_.newTransportData();
}

template <typename SM>
void AsyncFizzClientT<SM>::deliverAllErrors(
    const folly::AsyncSocketException& ex,
    bool closeTransport) {
  DelayedDestruction::DestructorGuard dg(this);
  deliverHandshakeError(ex);

  if (replaySafetyCallback_) {
    replaySafetyCallback_ = nullptr;
  }

  while (earlyDataState_ && !earlyDataState_->pendingAppWrites.empty()) {
    auto w = std::move(earlyDataState_->pendingAppWrites.front());
    earlyDataState_->pendingAppWrites.pop_front();
    if (w.callback) {
      w.callback->writeErr(0, ex);
    }
  }
  fizzClient_.moveToErrorState(ex);
  deliverError(ex, closeTransport);
}

template <typename SM>
void AsyncFizzClientT<SM>::deliverHandshakeError(folly::exception_wrapper ex) {
  if (callback_) {
    cancelHandshakeTimeout();
    auto cb = *callback_;
    callback_ = folly::none;
    folly::variant_match(
        cb,
        [this, &ex](HandshakeCallback* callback) {
          callback->fizzHandshakeError(this, std::move(ex));
        },
        [&ex](folly::AsyncSocket::ConnectCallback* callback) {
          ex.handle(
              [callback](const folly::AsyncSocketException& ase) {
                callback->connectErr(ase);
              },
              [callback](const std::exception& stdEx) {
                folly::AsyncSocketException ase(
                    folly::AsyncSocketException::SSL_ERROR, stdEx.what());
                callback->connectErr(ase);
              },
              [callback](...) {
                folly::AsyncSocketException ase(
                    folly::AsyncSocketException::SSL_ERROR, "unknown error");
                callback->connectErr(ase);
              });
        });
  }
}

template <typename SM>
void AsyncFizzClientT<SM>::ActionMoveVisitor::operator()(DeliverAppData& data) {
  client_.deliverAppData(std::move(data.data));
}

template <typename SM>
void AsyncFizzClientT<SM>::ActionMoveVisitor::operator()(WriteToSocket& data) {
  client_.transport_->writeChain(
      data.callback, std::move(data.data), data.flags);
}

template <typename SM>
void AsyncFizzClientT<SM>::ActionMoveVisitor::operator()(
    ReportEarlyHandshakeSuccess& earlySuccess) {
  client_.earlyDataState_ = EarlyDataState();
  client_.earlyDataState_->remainingEarlyData = earlySuccess.maxEarlyDataSize;
  if (client_.callback_) {
    auto cb = *client_.callback_;
    client_.callback_ = folly::none;
    folly::variant_match(
        cb,
        [this](HandshakeCallback* callback) {
          callback->fizzHandshakeSuccess(&client_);
        },
        [](folly::AsyncSocket::ConnectCallback* callback) {
          callback->connectSuccess();
        });
  }
}

template <typename SM>
folly::Optional<folly::AsyncSocketException>
AsyncFizzClientT<SM>::handleEarlyReject() {
  switch (earlyDataRejectionPolicy_) {
    case EarlyDataRejectionPolicy::FatalConnectionError: {
      return folly::AsyncSocketException(
          folly::AsyncSocketException::EARLY_DATA_REJECTED,
          "fizz early data rejected");
    }
    case EarlyDataRejectionPolicy::AutomaticResend: {
      if (earlyParametersMatch(getState())) {
        if (!earlyDataState_->resendBuffer.empty()) {
          AppWrite resend;
          resend.data = earlyDataState_->resendBuffer.move();
          fizzClient_.appWrite(std::move(resend));
        }
      } else {
        return folly::AsyncSocketException(
            folly::AsyncSocketException::EARLY_DATA_REJECTED,
            "fizz early data rejected, could not be resent");
      }
      break;
    }
  }
  return folly::none;
}

template <typename SM>
void AsyncFizzClientT<SM>::ActionMoveVisitor::operator()(
    ReportHandshakeSuccess& success) {
  client_.cancelHandshakeTimeout();
  if (client_.earlyDataState_) {
    if (!success.earlyDataAccepted) {
      auto ex = client_.handleEarlyReject();
      if (ex) {
        if (client_.pskIdentity_) {
          client_.fizzContext_->removePsk(*client_.pskIdentity_);
        }
        client_.deliverAllErrors(*ex, false);
        client_.transport_->closeNow();
        return;
      }
    }

    while (!client_.earlyDataState_->pendingAppWrites.empty()) {
      auto w = std::move(client_.earlyDataState_->pendingAppWrites.front());
      client_.earlyDataState_->pendingAppWrites.pop_front();
      client_.fizzClient_.appWrite(std::move(w));
    }
    client_.earlyDataState_.clear();
  }
  if (client_.callback_) {
    auto cb = *client_.callback_;
    client_.callback_ = folly::none;
    folly::variant_match(
        cb,
        [this](HandshakeCallback* callback) {
          callback->fizzHandshakeSuccess(&client_);
        },
        [](folly::AsyncSocket::ConnectCallback* callback) {
          callback->connectSuccess();
        });
  }
  if (client_.replaySafetyCallback_) {
    auto callback = client_.replaySafetyCallback_;
    client_.replaySafetyCallback_ = nullptr;
    callback->onReplaySafe();
  }
}

template <typename SM>
void AsyncFizzClientT<SM>::ActionMoveVisitor::operator()(
    ReportEarlyWriteFailed& write) {
  // If the state machine reports that an early write happened after early data
  // was already rejected, we need to invoke some write callback so that the
  // write isn't leaked. For now we just call writeSuccess and let the actual
  // rejection or early data get sorted out after full handshake success.
  //
  // TODO: buffer these callbacks until full handshake success, and call
  //       writeSuccess/writeErr depending on whether we are treating rejection
  //       as a fatal error.
  if (write.write.callback) {
    write.write.callback->writeSuccess();
  }
}

template <typename SM>
void AsyncFizzClientT<SM>::ActionMoveVisitor::operator()(ReportError& error) {
  folly::AsyncSocketException ase(
      folly::AsyncSocketException::SSL_ERROR, error.error.what().toStdString());
  client_.deliverHandshakeError(std::move(error.error));
  client_.deliverAllErrors(ase);
}

template <typename SM>
void AsyncFizzClientT<SM>::ActionMoveVisitor::operator()(WaitForData&) {
  client_.fizzClient_.waitForData();

  if (client_.callback_) {
    // Make sure that the read callback is installed.
    client_.startTransportReads();
  }
}

template <typename SM>
void AsyncFizzClientT<SM>::ActionMoveVisitor::operator()(MutateState& mutator) {
  mutator(client_.state_);
}

template <typename SM>
void AsyncFizzClientT<SM>::ActionMoveVisitor::operator()(
    NewCachedPsk& newCachedPsk) {
  if (client_.pskIdentity_) {
    client_.fizzContext_->putPsk(
        *client_.pskIdentity_, std::move(newCachedPsk.psk));
  }
}

template <typename SM>
Buf AsyncFizzClientT<SM>::getEkm(
    folly::StringPiece label,
    const Buf& context,
    uint16_t length) const {
  return fizzClient_.getEkm(label, context, length);
}

template <typename SM>
Buf AsyncFizzClientT<SM>::getEarlyEkm(
    folly::StringPiece label,
    const Buf& context,
    uint16_t length) const {
  return fizzClient_.getEarlyEkm(label, context, length);
}

template <typename SM>
bool AsyncFizzClientT<SM>::pskResumed() const {
  return getState().pskMode().has_value();
}
} // namespace client
} // namespace fizz
