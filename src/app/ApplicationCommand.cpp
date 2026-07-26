/*
 * Copyright (c) 2026 HsingYun (iakext@gmail.com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "ApplicationCommand.h"

#include <QtEndian>
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace uwf::app {

namespace {

constexpr std::array<char, 4> kMagic{'U', 'W', 'F', 'C'};
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kResultFixedPayloadSize = 48;
constexpr std::size_t kMaximumPayloadSize = 64 * 1024;
constexpr std::string_view kTruncatedDetailSuffix = "\n…";

enum class FrameKind : std::uint16_t {
  Request = 1,
  Result = 2,
  Progress = 3,
};

template <typename Integer>
void appendInteger(QByteArray& bytes, const Integer value) {
  static_assert(std::is_integral_v<Integer>);
  const Integer littleEndian = qToLittleEndian(value);
  bytes.append(reinterpret_cast<const char*>(&littleEndian), static_cast<qsizetype>(sizeof(littleEndian)));
}

template <typename Integer>
Integer readInteger(const QByteArray& bytes, const std::size_t offset) {
  static_assert(std::is_integral_v<Integer>);
  if (offset > static_cast<std::size_t>(bytes.size()) || sizeof(Integer) > static_cast<std::size_t>(bytes.size()) - offset) {
    throw ApplicationCommandProtocolError("application command frame is truncated");
  }
  return qFromLittleEndian<Integer>(reinterpret_cast<const uchar*>(bytes.constData() + static_cast<qsizetype>(offset)));
}

QByteArray makeFrame(const FrameKind kind, const std::uint64_t requestId, const QByteArray& payload) {
  if (payload.size() < 0 || static_cast<std::size_t>(payload.size()) > kMaximumPayloadSize) {
    throw ApplicationCommandProtocolError("application command payload is too large");
  }

  QByteArray bytes;
  bytes.reserve(static_cast<qsizetype>(kHeaderSize + static_cast<std::size_t>(payload.size())));
  bytes.append(kMagic.data(), static_cast<qsizetype>(kMagic.size()));
  appendInteger(bytes, kProtocolVersion);
  appendInteger(bytes, static_cast<std::uint16_t>(kind));
  appendInteger(bytes, requestId);
  appendInteger(bytes, static_cast<std::uint32_t>(payload.size()));
  appendInteger(bytes, std::uint32_t{0});
  bytes.append(payload);
  return bytes;
}

struct Frame {
  FrameKind kind;
  std::uint64_t requestId;
  QByteArray payload;
};

std::optional<Frame> decodeFrame(const QByteArray& bytes) {
  if (static_cast<std::size_t>(bytes.size()) < kHeaderSize) return std::nullopt;
  if (!std::equal(kMagic.begin(), kMagic.end(), bytes.constData())) {
    throw ApplicationCommandProtocolError("application command frame has an invalid signature");
  }
  if (readInteger<std::uint16_t>(bytes, 4) != kProtocolVersion) {
    throw ApplicationCommandProtocolError("application command protocol version is unsupported");
  }
  if (readInteger<std::uint32_t>(bytes, 20) != 0) {
    throw ApplicationCommandProtocolError("application command frame reserved field is nonzero");
  }

  const auto rawKind = readInteger<std::uint16_t>(bytes, 6);
  if (rawKind != static_cast<std::uint16_t>(FrameKind::Request) && rawKind != static_cast<std::uint16_t>(FrameKind::Result) &&
      rawKind != static_cast<std::uint16_t>(FrameKind::Progress)) {
    throw ApplicationCommandProtocolError("application command frame kind is invalid");
  }
  const auto payloadSize = readInteger<std::uint32_t>(bytes, 16);
  if (payloadSize > kMaximumPayloadSize) throw ApplicationCommandProtocolError("application command payload is too large");
  const std::size_t totalSize = kHeaderSize + payloadSize;
  if (static_cast<std::size_t>(bytes.size()) < totalSize) return std::nullopt;
  if (static_cast<std::size_t>(bytes.size()) != totalSize) {
    throw ApplicationCommandProtocolError("application command connection contains trailing data");
  }

  return Frame{static_cast<FrameKind>(rawKind), readInteger<std::uint64_t>(bytes, 8),
               bytes.mid(static_cast<qsizetype>(kHeaderSize), static_cast<qsizetype>(payloadSize))};
}

std::uint64_t checkedSize(const std::size_t value) {
  if (value > std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("application command counter is too large");
  return static_cast<std::uint64_t>(value);
}

std::size_t checkedCounter(const std::uint64_t value) {
  if (value > std::numeric_limits<std::size_t>::max()) throw ApplicationCommandProtocolError("application command counter is out of range");
  return static_cast<std::size_t>(value);
}

void validateResult(const ApplicationCommandResult& result) {
  if (result.committedFiles > result.discoveredFiles || result.skippedFiles > result.discoveredFiles - result.committedFiles) {
    throw ApplicationCommandProtocolError("application command result accounts for more files than were discovered");
  }
  if (result.outcome == ApplicationCommandOutcome::Succeeded && result.failedFiles != 0) {
    throw ApplicationCommandProtocolError("a successful application command result cannot contain failures");
  }
  if (result.outcome == ApplicationCommandOutcome::CompletedWithFailures && result.failedFiles == 0) {
    throw ApplicationCommandProtocolError("an application command result marked with failures must contain at least one failure");
  }
  if (result.outcome == ApplicationCommandOutcome::ContinuationApproved && result.failedFiles == 0) {
    throw ApplicationCommandProtocolError("an approved continuation must preserve the failure that required user confirmation");
  }
  if (result.outcome == ApplicationCommandOutcome::PreshutdownReleased && result.failedFiles == 0) {
    throw ApplicationCommandProtocolError("a degraded preshutdown release must preserve its infrastructure failure");
  }
}

QByteArray encodeBoundedDetail(const QString& text) {
  QByteArray detail = text.toUtf8();
  const auto limit = static_cast<qsizetype>(kMaximumPayloadSize - kResultFixedPayloadSize);
  if (detail.size() <= limit) return detail;

  const QByteArray suffix(kTruncatedDetailSuffix.data(), static_cast<qsizetype>(kTruncatedDetailSuffix.size()));
  qsizetype prefixSize = limit - suffix.size();
  // 截断位置若落在 UTF-8 continuation byte 中间，就退回到该码点的首字节。
  // 传输层只缩短诊断文本；批次计数和最终状态保持原值。
  while (prefixSize > 0 && (static_cast<unsigned char>(detail.at(prefixSize)) & 0xC0U) == 0x80U) --prefixSize;
  detail.truncate(prefixSize);
  detail.append(suffix);
  return detail;
}

}  // namespace

QByteArray encodeCommandRequest(const ApplicationCommandRequest& request) {
  switch (request.kind) {
    case ApplicationCommandKind::Activate:
    case ApplicationCommandKind::EnsureRunning:
    case ApplicationCommandKind::CommitStage:
      break;
    default:
      throw ApplicationCommandProtocolError("application command kind is invalid");
  }
  QByteArray payload;
  appendInteger(payload, static_cast<std::uint16_t>(request.kind));
  appendInteger(payload, std::uint16_t{0});
  return makeFrame(FrameKind::Request, request.requestId, payload);
}

QByteArray encodeCommandProgress(const std::uint64_t requestId, const ApplicationCommandProgress& progress) {
  QByteArray payload;
  payload.reserve(16);
  appendInteger(payload, checkedSize(progress.processed));
  appendInteger(payload, checkedSize(progress.total));
  return makeFrame(FrameKind::Progress, requestId, payload);
}

QByteArray encodeCommandResult(const std::uint64_t requestId, const ApplicationCommandResult& result) {
  switch (result.outcome) {
    case ApplicationCommandOutcome::Succeeded:
    case ApplicationCommandOutcome::CompletedWithFailures:
    case ApplicationCommandOutcome::Rejected:
    case ApplicationCommandOutcome::Failed:
    case ApplicationCommandOutcome::ContinuationApproved:
    case ApplicationCommandOutcome::PreshutdownReleased:
      break;
    default:
      throw ApplicationCommandProtocolError("application command outcome is invalid");
  }
  validateResult(result);
  const QByteArray detail = encodeBoundedDetail(result.detail);

  QByteArray payload;
  payload.reserve(static_cast<qsizetype>(kResultFixedPayloadSize + static_cast<std::size_t>(detail.size())));
  appendInteger(payload, static_cast<std::uint16_t>(result.outcome));
  appendInteger(payload, std::uint16_t{0});
  appendInteger(payload, checkedSize(result.discoveredFiles));
  appendInteger(payload, checkedSize(result.committedFiles));
  appendInteger(payload, checkedSize(result.skippedFiles));
  appendInteger(payload, checkedSize(result.skippedEntries));
  appendInteger(payload, checkedSize(result.failedFiles));
  appendInteger(payload, static_cast<std::uint32_t>(detail.size()));
  payload.append(detail);
  return makeFrame(FrameKind::Result, requestId, payload);
}

std::optional<std::pair<std::uint64_t, ApplicationCommandProgress>> decodeCommandProgress(const QByteArray& bytes) {
  const auto frame = decodeFrame(bytes);
  if (!frame) return std::nullopt;
  if (frame->kind != FrameKind::Progress || frame->payload.size() != 16) {
    throw ApplicationCommandProtocolError("application command progress payload is invalid");
  }
  ApplicationCommandProgress progress{checkedCounter(readInteger<std::uint64_t>(frame->payload, 0)),
                                      checkedCounter(readInteger<std::uint64_t>(frame->payload, 8))};
  // total=0 表示目录扫描阶段，此时 processed 是持续增长的已发现文件数；
  // total 非零后进入提交阶段，processed 不得越过该批次的最终文件数。
  if (progress.total != 0 && progress.processed > progress.total) {
    throw ApplicationCommandProtocolError("application command progress exceeds its total");
  }
  return std::pair{frame->requestId, progress};
}

std::optional<ApplicationCommandRequest> decodeCommandRequest(const QByteArray& bytes) {
  const auto frame = decodeFrame(bytes);
  if (!frame) return std::nullopt;
  if (frame->kind != FrameKind::Request || frame->payload.size() != 4) {
    throw ApplicationCommandProtocolError("application command request payload is invalid");
  }
  const auto rawKind = readInteger<std::uint16_t>(frame->payload, 0);
  if (readInteger<std::uint16_t>(frame->payload, 2) != 0) {
    throw ApplicationCommandProtocolError("application command request reserved field is nonzero");
  }
  switch (rawKind) {
    case static_cast<std::uint16_t>(ApplicationCommandKind::Activate):
    case static_cast<std::uint16_t>(ApplicationCommandKind::EnsureRunning):
    case static_cast<std::uint16_t>(ApplicationCommandKind::CommitStage):
      return ApplicationCommandRequest{static_cast<ApplicationCommandKind>(rawKind), frame->requestId};
    default:
      throw ApplicationCommandProtocolError("application command request kind is invalid");
  }
}

std::optional<std::pair<std::uint64_t, ApplicationCommandResult>> decodeCommandResult(const QByteArray& bytes) {
  const auto frame = decodeFrame(bytes);
  if (!frame) return std::nullopt;
  if (frame->kind != FrameKind::Result || static_cast<std::size_t>(frame->payload.size()) < kResultFixedPayloadSize) {
    throw ApplicationCommandProtocolError("application command result payload is invalid");
  }

  const auto rawOutcome = readInteger<std::uint16_t>(frame->payload, 0);
  if (readInteger<std::uint16_t>(frame->payload, 2) != 0) {
    throw ApplicationCommandProtocolError("application command result reserved field is nonzero");
  }
  switch (rawOutcome) {
    case static_cast<std::uint16_t>(ApplicationCommandOutcome::Succeeded):
    case static_cast<std::uint16_t>(ApplicationCommandOutcome::CompletedWithFailures):
    case static_cast<std::uint16_t>(ApplicationCommandOutcome::Rejected):
    case static_cast<std::uint16_t>(ApplicationCommandOutcome::Failed):
    case static_cast<std::uint16_t>(ApplicationCommandOutcome::ContinuationApproved):
    case static_cast<std::uint16_t>(ApplicationCommandOutcome::PreshutdownReleased):
      break;
    default:
      throw ApplicationCommandProtocolError("application command result outcome is invalid");
  }

  const std::uint32_t detailSize = readInteger<std::uint32_t>(frame->payload, 44);
  if (kResultFixedPayloadSize + detailSize != static_cast<std::size_t>(frame->payload.size())) {
    throw ApplicationCommandProtocolError("application command result detail length is invalid");
  }
  const QByteArray detail = frame->payload.mid(static_cast<qsizetype>(kResultFixedPayloadSize), static_cast<qsizetype>(detailSize));
  const QString decodedDetail = QString::fromUtf8(detail);
  if (decodedDetail.toUtf8() != detail) {
    throw ApplicationCommandProtocolError("application command result detail is not valid UTF-8");
  }

  ApplicationCommandResult result;
  result.outcome = static_cast<ApplicationCommandOutcome>(rawOutcome);
  result.discoveredFiles = checkedCounter(readInteger<std::uint64_t>(frame->payload, 4));
  result.committedFiles = checkedCounter(readInteger<std::uint64_t>(frame->payload, 12));
  result.skippedFiles = checkedCounter(readInteger<std::uint64_t>(frame->payload, 20));
  result.skippedEntries = checkedCounter(readInteger<std::uint64_t>(frame->payload, 28));
  result.failedFiles = checkedCounter(readInteger<std::uint64_t>(frame->payload, 36));
  result.detail = decodedDetail;
  validateResult(result);
  return std::pair{frame->requestId, std::move(result)};
}

std::size_t applicationCommandFrameSize(const QByteArray& bytes) {
  if (static_cast<std::size_t>(bytes.size()) < kHeaderSize) return 0;
  if (!std::equal(kMagic.begin(), kMagic.end(), bytes.constData())) {
    throw ApplicationCommandProtocolError("application command frame has an invalid signature");
  }
  const auto payloadSize = readInteger<std::uint32_t>(bytes, 16);
  if (payloadSize > kMaximumPayloadSize) throw ApplicationCommandProtocolError("application command payload is too large");
  return kHeaderSize + payloadSize;
}

ApplicationCommandMessageKind applicationCommandMessageKind(const QByteArray& bytes) {
  const auto frame = decodeFrame(bytes);
  if (!frame) throw ApplicationCommandProtocolError("application command frame is incomplete");
  switch (frame->kind) {
    case FrameKind::Request:
      return ApplicationCommandMessageKind::Request;
    case FrameKind::Progress:
      return ApplicationCommandMessageKind::Progress;
    case FrameKind::Result:
      return ApplicationCommandMessageKind::Result;
  }
  throw ApplicationCommandProtocolError("application command frame kind is invalid");
}

}  // namespace uwf::app
