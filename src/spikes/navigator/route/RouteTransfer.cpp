#include "RouteTransfer.h"

namespace navigator {
namespace {

inline uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// Reflected IEEE CRC-32 (polynomial 0xEDB88320), identical to the Swift
// CRC32.checksum and to RoutePackageV1's internal CRC: init 0xFFFFFFFF, final
// XOR 0xFFFFFFFF. Tableless and allocation-free. The same primitive backs two
// distinct checks that must not be conflated:
//   * the internal trailer CRC stored as a package's last four bytes, computed
//     over the content that precedes it (the golden fixture: 0x4CB0201C); and
//   * the START whole-package transport CRC, computed over every transmitted
//     package byte *including* that four-byte trailer. For a correctly formed
//     package the trailer is the content's IEEE CRC, so this whole-file CRC is
//     the constant residue 0x2144DF1C - it is the transport's redundancy check
//     over the bytes as sent, not a content checksum (validateRoutePackageV1
//     still validates the inner content CRC).
class Crc32 {
 public:
  void update(const uint8_t* data, uint32_t length) {
    for (uint32_t i = 0; i < length; ++i) {
      crc_ ^= data[i];
      for (unsigned bit = 0; bit < 8; ++bit) {
        const uint32_t mask = 0u - (crc_ & 1u);
        crc_ = (crc_ >> 1) ^ (0xEDB88320u & mask);
      }
    }
  }

  uint32_t value() const { return crc_ ^ 0xFFFFFFFFu; }

 private:
  uint32_t crc_ = 0xFFFFFFFFu;
};

}  // namespace

void RouteTransferStatus::writeEnvelope(uint8_t out[7]) const {
  out[0] = static_cast<uint8_t>(code);
  out[1] = static_cast<uint8_t>(routeId & 0xFFu);
  out[2] = static_cast<uint8_t>((routeId >> 8) & 0xFFu);
  out[3] = static_cast<uint8_t>((routeId >> 16) & 0xFFu);
  out[4] = static_cast<uint8_t>((routeId >> 24) & 0xFFu);
  out[5] = static_cast<uint8_t>(received & 0xFFu);
  out[6] = static_cast<uint8_t>((received >> 8) & 0xFFu);
}

RouteTransferStatus RouteTransfer::makeStatus(RouteTransferCode code) const {
  RouteTransferStatus status;
  status.code = code;
  status.routeId = active_ ? routeId_ : 0;
  status.received = active_ ? static_cast<uint16_t>(received_) : 0;
  return status;
}

void RouteTransfer::clearSession() {
  active_ = false;
  routeId_ = 0;
  totalLength_ = 0;
  expectedCrc_ = 0;
  received_ = 0;
}

void RouteTransfer::abortTransfer(RouteSink& sink) {
  if (active_) {
    sink.abandon();
  }
  clearSession();
}

RouteTransferStatus RouteTransfer::handleFrame(const uint8_t* frame, uint32_t frameLength, RouteSink& sink,
                                               RouteIndex& candidate) {
  if (frame == nullptr || frameLength == 0 || frameLength > kMaxFrameBytes) {
    return makeStatus(RouteTransferCode::RouteInvalid);
  }
  switch (frame[0]) {
    case kRouteOpcodeStart:
      return handleStart(frame, frameLength, sink);
    case kRouteOpcodeChunk:
      return handleChunk(frame, frameLength, sink);
    case kRouteOpcodeCommit:
      return handleCommit(frame, frameLength, sink, candidate);
    default:
      return makeStatus(RouteTransferCode::RouteInvalid);
  }
}

RouteTransferStatus RouteTransfer::handleStart(const uint8_t* frame, uint32_t frameLength, RouteSink& sink) {
  // START while a session is already active is rejected; the original session
  // is left untouched so its remaining chunks can still arrive.
  if (active_) {
    return makeStatus(RouteTransferCode::RouteInvalid);
  }
  if (frameLength != kStartFrameBytes) {
    return makeStatus(RouteTransferCode::RouteInvalid);
  }
  const uint32_t routeId = readU32(frame + 1);
  const uint32_t totalLength = readU32(frame + 5);
  const uint32_t expectedCrc = readU32(frame + 9);
  // The transport ceiling is the Route Package v1 ceiling (65,535); an empty
  // package is rejected outright (there is nothing to transfer).
  if (totalLength == 0 || totalLength > kRoutePackageV1MaxBytes) {
    return makeStatus(RouteTransferCode::RouteInvalid);
  }
  if (!sink.begin(totalLength)) {
    // Storage could not open the staged transaction (I/O failure or directory
    // quota rejection); no session starts and the previous active route is
    // untouched. begin() may have left a partial route.tmp behind, so ask the
    // sink to abandon it best-effort. Echo the attempted id for the phone.
    sink.abandon();
    RouteTransferStatus status;
    status.code = RouteTransferCode::RouteStorageFailed;
    status.routeId = routeId;
    status.received = 0;
    return status;
  }
  routeId_ = routeId;
  totalLength_ = totalLength;
  expectedCrc_ = expectedCrc;
  received_ = 0;
  active_ = true;
  return makeStatus(RouteTransferCode::RouteReady);
}

RouteTransferStatus RouteTransfer::handleChunk(const uint8_t* frame, uint32_t frameLength, RouteSink& sink) {
  if (!active_) {
    return makeStatus(RouteTransferCode::RouteInvalid);
  }
  // A CHUNK must carry at least one payload byte: header is 9 bytes, so a
  // frame of <= 9 bytes is truncated or zero-payload and rejected.
  if (frameLength <= kChunkHeaderBytes) {
    return makeStatus(RouteTransferCode::RouteInvalid);
  }
  const uint32_t routeId = readU32(frame + 1);
  const uint32_t offset = readU32(frame + 5);
  const uint32_t payloadLength = frameLength - kChunkHeaderBytes;
  if (routeId != routeId_) {
    return makeStatus(RouteTransferCode::RouteInvalid);
  }

  if (offset < received_) {
    // Wholly within already-staged bytes: idempotent only when the staged file
    // verifies the exact bytes (never append twice). Anything that extends past
    // the staged end is a partial overlap and is rejected.
    if (static_cast<uint64_t>(offset) + payloadLength > received_) {
      return makeStatus(RouteTransferCode::RouteInvalid);
    }
    switch (sink.matchesStaged(offset, frame + kChunkHeaderBytes, payloadLength)) {
      case RouteSink::VerifyResult::Match:
        return makeStatus(RouteTransferCode::RouteProgress);  // idempotent duplicate
      case RouteSink::VerifyResult::Differ:
        return makeStatus(RouteTransferCode::RouteInvalid);  // differing duplicate
      case RouteSink::VerifyResult::ReadFailed: {
        const RouteTransferStatus failed = makeStatus(RouteTransferCode::RouteStorageFailed);
        abortTransfer(sink);
        return failed;
      }
    }
    return makeStatus(RouteTransferCode::RouteInvalid);  // unreachable; exhaustive switch
  }
  if (offset > received_) {
    return makeStatus(RouteTransferCode::RouteInvalid);  // gap: not contiguous
  }
  // offset == received_: contiguous append. Overshoot past the declared
  // package end is rejected before any byte is written.
  if (static_cast<uint64_t>(offset) + payloadLength > totalLength_) {
    return makeStatus(RouteTransferCode::RouteInvalid);
  }
  if (!sink.append(frame + kChunkHeaderBytes, payloadLength)) {
    const RouteTransferStatus failed = makeStatus(RouteTransferCode::RouteStorageFailed);
    abortTransfer(sink);
    return failed;
  }
  received_ += payloadLength;
  return makeStatus(RouteTransferCode::RouteProgress);
}

RouteTransferStatus RouteTransfer::handleCommit(const uint8_t* frame, uint32_t frameLength, RouteSink& sink,
                                                RouteIndex& candidate) {
  if (!active_) {
    return makeStatus(RouteTransferCode::RouteInvalid);
  }
  if (frameLength != kCommitFrameBytes) {
    return makeStatus(RouteTransferCode::RouteInvalid);
  }
  if (readU32(frame + 1) != routeId_) {
    return makeStatus(RouteTransferCode::RouteInvalid);
  }
  // Commit before every declared byte is staged is rejected; the session stays
  // active so the missing chunks can still arrive and a later COMMIT can pass.
  if (received_ != totalLength_) {
    return makeStatus(RouteTransferCode::RouteInvalid);
  }

  const RouteTransferStatus accepted = makeStatus(RouteTransferCode::RouteAccepted);

  if (!sink.flush()) {
    const RouteTransferStatus failed = makeStatus(RouteTransferCode::RouteStorageFailed);
    abortTransfer(sink);
    return failed;
  }

  // Whole-package transport CRC pass: stream every staged byte - the full
  // transmitted package, content plus its trailing four-byte internal CRC -
  // through a transient <= 1,024-byte work buffer and compare with the CRC32
  // declared in START. The package's own internal content CRC is validated
  // separately by validateRoutePackageV1 below; this pass is the transport's
  // redundancy check over the bytes as sent (for a correctly formed package it
  // evaluates to the constant IEEE residue, 0x2144DF1C for the golden
  // fixture). A stalled read (got == 0) or a source that reports more bytes
  // than requested (got > want, a RouteByteSource contract violation) is a
  // storage failure: the buffer must not be consumed and the count must not
  // advance. A CRC mismatch is a protocol/package rejection.
  {
    uint8_t work[kRoutePackageV1WorkBufferBytes];
    RouteByteSource& source = sink.stagedSource();
    const uint32_t staged = source.size();
    if (staged != totalLength_) {
      const RouteTransferStatus failed = makeStatus(RouteTransferCode::RouteStorageFailed);
      abortTransfer(sink);
      return failed;
    }
    Crc32 crc;
    uint32_t done = 0;
    while (done < staged) {
      const uint32_t want =
          staged - done < kRoutePackageV1WorkBufferBytes ? staged - done : kRoutePackageV1WorkBufferBytes;
      const uint32_t got = source.read(done, work, want);
      if (got == 0 || got > want) {
        const RouteTransferStatus failed = makeStatus(RouteTransferCode::RouteStorageFailed);
        abortTransfer(sink);
        return failed;
      }
      crc.update(work, got);
      done += got;
    }
    if (crc.value() != expectedCrc_) {
      const RouteTransferStatus failed = makeStatus(RouteTransferCode::RouteInvalid);
      abortTransfer(sink);
      return failed;
    }
  }

  // Streamed Route Package v1 validation from the staged file. `candidate` is
  // the caller-owned staging/validation workspace, NOT the caller's live
  // active index: a successful decode writes it even when COMMIT later fails
  // at publish, so the caller may consume or swap it only after RouteAccepted.
  // validateRoutePackageV1 leaves `candidate` untouched on every rejection it
  // reports; any non-Ok status here is a decode failure (the CRC pass already
  // proved the file readable).
  if (validateRoutePackageV1(sink.stagedSource(), candidate) != DecodeStatus::Ok) {
    const RouteTransferStatus failed = makeStatus(RouteTransferCode::RouteInvalid);
    abortTransfer(sink);
    return failed;
  }

  // The decoded package's header route id must agree with the START route id;
  // a package for a different route is rejected before publish. `candidate`
  // already holds the decode at this point (expected for the workspace), and
  // every rejection path cleans route.tmp while the previous active route
  // stays usable.
  if (candidate.routeId != routeId_) {
    const RouteTransferStatus failed = makeStatus(RouteTransferCode::RouteInvalid);
    abortTransfer(sink);
    return failed;
  }

  // Transactional promotion: on failure the store restores the previous active
  // route (or keeps it recoverably at route.bak); either way it stays usable.
  if (!sink.publish()) {
    const RouteTransferStatus failed = makeStatus(RouteTransferCode::RouteStorageFailed);
    abortTransfer(sink);
    return failed;
  }
  clearSession();
  return accepted;
}

}  // namespace navigator
