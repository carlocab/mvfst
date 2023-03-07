/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Chrono.h>
#include <folly/Optional.h>
#include <folly/io/async/AsyncTimeout.h>
#include <quic/QuicConstants.h>
#include <quic/codec/Types.h>
#include <quic/common/TimeUtil.h>
#include <quic/congestion_control/CongestionController.h>
#include <quic/flowcontrol/QuicFlowController.h>
#include <quic/logging/QLoggerConstants.h>
#include <quic/observer/SocketObserverTypes.h>
#include <quic/state/QuicStateFunctions.h>
#include <quic/state/SimpleFrameFunctions.h>
#include <quic/state/StateData.h>

namespace quic {

// Forward-declaration
bool hasAckDataToWrite(const QuicConnectionStateBase& conn);
WriteDataReason hasNonAckDataToWrite(const QuicConnectionStateBase& conn);

std::chrono::microseconds calculatePTO(const QuicConnectionStateBase& conn);

/**
 * Whether conn is having persistent congestion.
 *
 * Persistent congestion requires a time period much longer than crypto timer.
 * This means no handshake packet should be in a persistent congestion range.
 * Thus persistent congestion is per pnSpace, and it's AppData space only.
 *
 */
bool isPersistentCongestion(
    folly::Optional<std::chrono::microseconds> pto,
    TimePoint lostPeriodStart,
    TimePoint lostPeriodEnd,
    const CongestionController::AckEvent& ack) noexcept;

inline std::ostream& operator<<(
    std::ostream& os,
    const LossState::AlarmMethod& alarmMethod) {
  switch (alarmMethod) {
    case LossState::AlarmMethod::EarlyRetransmitOrReordering:
      os << "EarlyRetransmitOrReordering";
      break;
    case LossState::AlarmMethod::PTO:
      os << "PTO";
      break;
  }
  return os;
}

template <class ClockType = Clock>
std::pair<std::chrono::milliseconds, LossState::AlarmMethod>
calculateAlarmDuration(const QuicConnectionStateBase& conn) {
  std::chrono::microseconds alarmDuration;
  folly::Optional<LossState::AlarmMethod> alarmMethod;
  TimePoint lastSentPacketTime =
      conn.lossState.lastRetransmittablePacketSentTime;
  auto lossTimeAndSpace = earliestLossTimer(conn);
  if (lossTimeAndSpace.first) {
    if (*lossTimeAndSpace.first > lastSentPacketTime) {
      // We do this so that lastSentPacketTime + alarmDuration = lossTime
      alarmDuration = std::chrono::duration_cast<std::chrono::microseconds>(
          *lossTimeAndSpace.first - lastSentPacketTime);
    } else {
      // This should trigger an immediate alarm.
      alarmDuration = 0us;
    }
    alarmMethod = LossState::AlarmMethod::EarlyRetransmitOrReordering;
  } else {
    auto ptoTimeout = calculatePTO(conn);
    ptoTimeout *= 1ULL << std::min(conn.lossState.ptoCount, (uint32_t)31);
    alarmDuration = ptoTimeout;
    alarmMethod = LossState::AlarmMethod::PTO;
  }
  TimePoint now = ClockType::now();
  std::chrono::milliseconds adjustedAlarmDuration{0};
  // The alarm duration is calculated based on the last packet that was sent
  // rather than the current time.
  if (lastSentPacketTime + alarmDuration > now) {
    adjustedAlarmDuration = folly::chrono::ceil<std::chrono::milliseconds>(
        lastSentPacketTime + alarmDuration - now);
  } else {
    VLOG(10) << __func__ << " alarm already due method=" << *alarmMethod
             << " lastSentPacketTime="
             << lastSentPacketTime.time_since_epoch().count()
             << " now=" << now.time_since_epoch().count()
             << " alarm=" << alarmDuration.count() << "us"
             << " deadline="
             << (lastSentPacketTime + alarmDuration).time_since_epoch().count()
             << " " << conn;
  }
  DCHECK(alarmMethod.hasValue()) << "Alarm method must have a value";
  return std::make_pair(adjustedAlarmDuration, *alarmMethod);
}

/*
 * This function should be invoked after some event that is possible to change
 * the loss detection timer, for example, write happened, timeout happened or
 * packets are acked.
 */
template <class Timeout, class ClockType = Clock>
void setLossDetectionAlarm(QuicConnectionStateBase& conn, Timeout& timeout) {
  /*
   * We might have new data or lost data to send even if we don't have any
   * outstanding packets. When we get a PTO event, it is possible that only
   * cloned packets might be outstanding. Since cwnd might be set to min cwnd,
   * we might not be able to send data. However we might still have data sitting
   * in the buffers which is unsent or known to be lost. We should set a timer
   * in this case to be able to send this data on the next PTO.
   */
  bool hasDataToWrite = hasAckDataToWrite(conn) ||
      (hasNonAckDataToWrite(conn) != WriteDataReason::NO_WRITE);
  auto totalPacketsOutstanding = conn.outstandings.numOutstanding();
  /*
   * We have this condition to disambiguate the case where we have.
   * (1) All outstanding packets that are clones that are processed and there is
   * no data to write. (2) All outstanding are clones that are processed and
   * there is data to write. If there are only clones with no data, then we
   * don't need to set the timer. This will free up the evb. However after a PTO
   * verified event, clones take up space in cwnd. If we have data left to
   * write, we would not be able to write them since we could be blocked by
   * cwnd. So we must set the loss timer so that we can write this data with the
   * slack packet space for the clones.
   */
  if (!hasDataToWrite && conn.outstandings.packetEvents.empty() &&
      totalPacketsOutstanding == conn.outstandings.numClonedPackets()) {
    VLOG(10) << __func__ << " unset alarm pure ack or processed packets only"
             << " outstanding=" << totalPacketsOutstanding
             << " handshakePackets="
             << conn.outstandings.packetCount[PacketNumberSpace::Handshake]
             << " " << conn;
    conn.pendingEvents.setLossDetectionAlarm = false;
    timeout.cancelLossTimeout();
    return;
  }
  /**
   * Either previous timer or an Ack can clear the lossTime without setting a
   * new one, for example, if such timer or ack marks everything as loss, or
   * every as acked. In that case, if an early retransmit timer is already set,
   * we should clear it.
   */
  if (conn.lossState.currentAlarmMethod ==
          LossState::AlarmMethod::EarlyRetransmitOrReordering &&
      !earliestLossTimer(conn).first) {
    VLOG(10) << __func__
             << " unset alarm due to invalidated early retran timer";
    timeout.cancelLossTimeout();
  }
  if (!conn.pendingEvents.setLossDetectionAlarm) {
    VLOG_IF(10, !timeout.isLossTimeoutScheduled())
        << __func__ << " alarm not scheduled"
        << " outstanding=" << totalPacketsOutstanding << " initialPackets="
        << conn.outstandings.packetCount[PacketNumberSpace::Initial]
        << " handshakePackets="
        << conn.outstandings.packetCount[PacketNumberSpace::Handshake] << " "
        << nodeToString(conn.nodeType) << " " << conn;
    return;
  }
  timeout.cancelLossTimeout();
  auto alarmDuration = calculateAlarmDuration<ClockType>(conn);
  conn.lossState.currentAlarmMethod = alarmDuration.second;
  VLOG(10) << __func__ << " setting transmission"
           << " alarm=" << alarmDuration.first.count() << "ms"
           << " method=" << conn.lossState.currentAlarmMethod
           << " haDataToWrite=" << hasDataToWrite
           << " outstanding=" << totalPacketsOutstanding
           << " outstanding clone=" << conn.outstandings.numClonedPackets()
           << " packetEvents=" << conn.outstandings.packetEvents.size()
           << " initialPackets="
           << conn.outstandings.packetCount[PacketNumberSpace::Initial]
           << " handshakePackets="
           << conn.outstandings.packetCount[PacketNumberSpace::Handshake] << " "
           << nodeToString(conn.nodeType) << " " << conn;
  timeout.scheduleLossTimeout(alarmDuration.first);
  conn.pendingEvents.setLossDetectionAlarm = false;
}

template <class LossVisitor>
void processOutstandingsForLoss(
    QuicConnectionStateBase& conn,
    const folly::Optional<PacketNum>& largestAcked,
    const PacketNumberSpace& pnSpace,
    const InlineMap<StreamId, PacketNum, 20>& largestDsrAcked,
    const folly::Optional<PacketNum>& largestNonDsrAcked,
    const TimePoint& lossTime,
    const std::chrono::microseconds& rttSample,
    const LossVisitor& lossVisitor,
    std::chrono::microseconds& delayUntilLost,
    bool& shouldSetTimer,
    CongestionController::LossEvent& lossEvent,
    folly::Optional<SocketObserverInterface::LossEvent>& observerLossEvent) {
  auto iter = getFirstOutstandingPacket(conn, pnSpace);
  while (iter != conn.outstandings.packets.end()) {
    auto& pkt = *iter;
    auto currentPacketNum = pkt.packet.header.getPacketSequenceNum();
    if (!largestAcked.has_value() || currentPacketNum >= *largestAcked) {
      break;
    }
    auto currentPacketNumberSpace = pkt.packet.header.getPacketNumberSpace();
    if (currentPacketNumberSpace != pnSpace) {
      iter++;
      continue;
    }

    // We now have to determine the largest ACKed packet number we should use
    // for the reordering threshold loss determination.
    auto maybeStreamFrame = pkt.packet.frames.empty()
        ? nullptr
        : pkt.packet.frames.front().asWriteStreamFrame();
    // The max here is taken to ensure that if the largestAcked ends up smaller
    // than the current packet number, we don't consider it lost.
    auto largestAckedForComparison = std::max(
        [&]() -> PacketNum {
          if (maybeStreamFrame && maybeStreamFrame->fromBufMeta) {
            // If the packet being considered is a DSR packet, we use the
            // largest ACKed for that stream. The default value here covers the
            // case where no DSR packets were ACKed, in which case we should
            // not declare reorder loss.
            CHECK(pkt.isDSRPacket);
            return folly::get_default(
                largestDsrAcked, maybeStreamFrame->streamId, currentPacketNum);
          } else {
            // If the packet being considered is a non-DSR packet, the
            // straightforward case is to use the largest non-DSR ACKed.
            // If DSR packets have been ACKed, we need to use the largest
            // non-DSR ACKed. If there were no non-DSR ACKed, we shouldn't
            // declare reorder loss.
            if (largestDsrAcked.empty()) {
              CHECK(largestNonDsrAcked == largestAcked);
              return largestNonDsrAcked.value();
            } else {
              return largestNonDsrAcked.value_or(currentPacketNum);
            }
          }
        }(),
        currentPacketNum);

    bool lostByTimeout = (lossTime - pkt.metadata.time) > delayUntilLost;
    const auto reorderDistance = largestAckedForComparison - currentPacketNum;
    bool lostByReorder = reorderDistance > conn.lossState.reorderingThreshold;

    if (!(lostByTimeout || lostByReorder)) {
      // We can exit early here because if packet N doesn't meet the
      // threshold, then packet N + 1 will not either.
      shouldSetTimer = true;
      break;
    }
    lossEvent.addLostPacket(pkt);
    if (observerLossEvent) {
      observerLossEvent->addLostPacket(lostByTimeout, lostByReorder, pkt);
    }

    if (pkt.isDSRPacket) {
      CHECK_GT(conn.outstandings.dsrCount, 0);
      --conn.outstandings.dsrCount;
    }
    if (pkt.associatedEvent) {
      CHECK(conn.outstandings.clonedPacketCount[pnSpace]);
      --conn.outstandings.clonedPacketCount[pnSpace];
    }
    // Invoke LossVisitor if the packet doesn't have a associated PacketEvent;
    // or if the PacketEvent is present in conn.outstandings.packetEvents.
    bool processed = pkt.associatedEvent &&
        !conn.outstandings.packetEvents.count(*pkt.associatedEvent);
    lossVisitor(conn, pkt.packet, processed);
    // Remove the PacketEvent from the outstandings.packetEvents set
    if (pkt.associatedEvent) {
      conn.outstandings.packetEvents.erase(*pkt.associatedEvent);
    }
    if (!processed) {
      CHECK(conn.outstandings.packetCount[currentPacketNumberSpace]);
      --conn.outstandings.packetCount[currentPacketNumberSpace];
    }
    VLOG(10) << __func__ << " lost packetNum=" << currentPacketNum
             << " handshake=" << pkt.metadata.isHandshake << " " << conn;
    // Rather than erasing here, instead mark the packet as lost so we can
    // determine if this was spurious later.
    conn.lossState.totalPacketsMarkedLost++;
    if (lostByTimeout && rttSample.count() > 0) {
      conn.lossState.totalPacketsMarkedLostByPto++;
      pkt.lossTimeoutDividend = (lossTime - pkt.metadata.time) *
          conn.transportSettings.timeReorderingThreshDivisor / rttSample;
    }
    if (lostByReorder) {
      conn.lossState.totalPacketsMarkedLostByReorderingThreshold++;
      iter->lossReorderDistance = reorderDistance;
    }
    conn.outstandings.declaredLostCount++;
    iter->declaredLost = true;
    iter++;
  }
}

/*
 * This function should be invoked after some event that is possible to
 * trigger loss detection, for example: packets are acked
 */
template <class LossVisitor>
folly::Optional<CongestionController::LossEvent> detectLossPackets(
    QuicConnectionStateBase& conn,
    const folly::Optional<PacketNum> largestAcked,
    const LossVisitor& lossVisitor,
    const TimePoint lossTime,
    const PacketNumberSpace pnSpace,
    const CongestionController::AckEvent* ackEvent = nullptr) {
  getLossTime(conn, pnSpace).reset();
  std::chrono::microseconds rttSample =
      std::max(conn.lossState.srtt, conn.lossState.lrtt);
  std::chrono::microseconds delayUntilLost = rttSample *
      conn.transportSettings.timeReorderingThreshDividend /
      conn.transportSettings.timeReorderingThreshDivisor;
  VLOG(10) << __func__ << " outstanding=" << conn.outstandings.numOutstanding()
           << " largestAcked=" << largestAcked.value_or(0)
           << " delayUntilLost=" << delayUntilLost.count() << "us"
           << " " << conn;
  CongestionController::LossEvent lossEvent(lossTime);
  folly::Optional<SocketObserverInterface::LossEvent> observerLossEvent;
  {
    const auto socketObserverContainer = conn.getSocketObserverContainer();
    if (socketObserverContainer &&
        socketObserverContainer->hasObserversForEvent<
            SocketObserverInterface::Events::lossEvents>()) {
      observerLossEvent.emplace(lossTime);
    }
  }
  // Note that time based loss detection is also within the same PNSpace.
  bool shouldSetTimer = false;

  // Loop over all ACKed packets and collect the largest ACKed packet per DSR
  // stream. This facilitates only considering the reordering threshold per DSR
  // sender, which avoids the problem of "natural" reordering caused by
  // multiple DSR senders. Similarly track the largest non-DSR ACKed, for the
  // reason but when DSR packets are reordered "before" non-DSR packets.
  InlineMap<StreamId, PacketNum, 20> largestDsrAcked;
  folly::Optional<PacketNum> largestNonDsrAcked;
  if (ackEvent) {
    for (const auto& ackPacket : ackEvent->ackedPackets) {
      for (auto& [stream, details] : ackPacket.detailsPerStream) {
        if (details.dsrStream) {
          largestDsrAcked[stream] = std::max(
              folly::get_default(largestDsrAcked, stream, ackPacket.packetNum),
              ackPacket.packetNum);
        } else {
          largestNonDsrAcked =
              std::max(largestNonDsrAcked.value_or(0), ackPacket.packetNum);
        }
      }
      // If there are no streams, then it's not a DSR packet.
      if (ackPacket.detailsPerStream.empty()) {
        largestNonDsrAcked =
            std::max(largestNonDsrAcked.value_or(0), ackPacket.packetNum);
      }
    }
  }
  // This covers the case where there's no ackedPackets.
  if (largestDsrAcked.empty()) {
    largestNonDsrAcked = largestAcked;
  }

  processOutstandingsForLoss(
      conn,
      largestAcked,
      pnSpace,
      largestDsrAcked,
      largestNonDsrAcked,
      lossTime,
      rttSample,
      lossVisitor,
      delayUntilLost,
      shouldSetTimer,
      lossEvent,
      observerLossEvent);

  // notify observers
  {
    const auto socketObserverContainer = conn.getSocketObserverContainer();
    if (observerLossEvent && observerLossEvent->hasPackets() &&
        socketObserverContainer &&
        socketObserverContainer->hasObserversForEvent<
            SocketObserverInterface::Events::lossEvents>()) {
      socketObserverContainer
          ->invokeInterfaceMethod<SocketObserverInterface::Events::lossEvents>(
              [observerLossEvent](auto observer, auto observed) {
                observer->packetLossDetected(observed, *observerLossEvent);
              });
    }
  }

  auto earliest = getFirstOutstandingPacket(conn, pnSpace);
  for (; earliest != conn.outstandings.packets.end();
       earliest = getNextOutstandingPacket(conn, pnSpace, earliest + 1)) {
    if (!earliest->associatedEvent ||
        conn.outstandings.packetEvents.count(*earliest->associatedEvent)) {
      break;
    }
  }
  if (shouldSetTimer && earliest != conn.outstandings.packets.end()) {
    // We are eligible to set a loss timer and there are a few packets which
    // are unacked, so we can set the early retransmit timer for them.
    VLOG(10) << __func__ << " early retransmit timer outstanding="
             << conn.outstandings.packets.empty() << " delayUntilLost"
             << delayUntilLost.count() << "us"
             << " " << conn;
    getLossTime(conn, pnSpace) = delayUntilLost + earliest->metadata.time;
  }
  if (lossEvent.largestLostPacketNum.hasValue()) {
    DCHECK(lossEvent.largestLostSentTime && lossEvent.smallestLostSentTime);
    if (conn.qLogger) {
      conn.qLogger->addPacketsLost(
          lossEvent.largestLostPacketNum.value(),
          lossEvent.lostBytes,
          lossEvent.lostPackets);
    }

    conn.lossState.rtxCount += lossEvent.lostPackets;
    if (conn.congestionController) {
      return lossEvent;
    }
  }
  return folly::none;
}

void onPTOAlarm(QuicConnectionStateBase& conn);

/*
 * Function invoked when loss detection timer fires
 */
template <class LossVisitor, class ClockType = Clock>
void onLossDetectionAlarm(
    QuicConnectionStateBase& conn,
    const LossVisitor& lossVisitor) {
  auto now = ClockType::now();
  if (conn.outstandings.packets.empty()) {
    VLOG(10) << "Transmission alarm fired with no outstanding packets " << conn;
    return;
  }
  if (conn.lossState.currentAlarmMethod ==
      LossState::AlarmMethod::EarlyRetransmitOrReordering) {
    auto lossTimeAndSpace = earliestLossTimer(conn);
    CHECK(lossTimeAndSpace.first);
    auto lossEvent = detectLossPackets<LossVisitor>(
        conn,
        getAckState(conn, lossTimeAndSpace.second).largestAckedByPeer,
        lossVisitor,
        now,
        lossTimeAndSpace.second);
    if (conn.congestionController && lossEvent) {
      DCHECK(lossEvent->largestLostSentTime && lossEvent->smallestLostSentTime);
      conn.congestionController->onPacketAckOrLoss(
          nullptr, lossEvent.get_pointer());
    }
  } else {
    onPTOAlarm(conn);
  }
  conn.pendingEvents.setLossDetectionAlarm =
      conn.outstandings.numOutstanding() > 0;
  VLOG(10) << __func__ << " setLossDetectionAlarm="
           << conn.pendingEvents.setLossDetectionAlarm
           << " outstanding=" << conn.outstandings.numOutstanding()
           << " initialPackets="
           << conn.outstandings.packetCount[PacketNumberSpace::Initial]
           << " handshakePackets="
           << conn.outstandings.packetCount[PacketNumberSpace::Handshake] << " "
           << conn;
}

/*
 * Process streams in a RegularQuicWritePacket for loss
 *
 * processed: whether this packet is a already processed clone
 */
void markPacketLoss(
    QuicConnectionStateBase& conn,
    RegularQuicWritePacket& packet,
    bool processed);

template <class LossVisitor>
folly::Optional<CongestionController::LossEvent> handleAckForLoss(
    QuicConnectionStateBase& conn,
    const LossVisitor& lossVisitor,
    CongestionController::AckEvent& ack,
    PacketNumberSpace pnSpace) {
  auto& largestAcked = getAckState(conn, pnSpace).largestAckedByPeer;
  if (ack.largestNewlyAckedPacket.has_value()) {
    conn.lossState.ptoCount = 0;
    largestAcked = std::max<PacketNum>(
        largestAcked.value_or(*ack.largestNewlyAckedPacket),
        *ack.largestNewlyAckedPacket);
  }
  auto lossEvent = detectLossPackets(
      conn,
      getAckState(conn, pnSpace).largestAckedByPeer,
      lossVisitor,
      ack.ackTime,
      pnSpace,
      &ack);
  conn.pendingEvents.setLossDetectionAlarm =
      conn.outstandings.numOutstanding() > 0;
  VLOG(10) << __func__ << " largestAckedInPacket="
           << ack.largestNewlyAckedPacket.value_or(0)
           << " setLossDetectionAlarm="
           << conn.pendingEvents.setLossDetectionAlarm
           << " outstanding=" << conn.outstandings.numOutstanding()
           << " initialPackets="
           << conn.outstandings.packetCount[PacketNumberSpace::Initial]
           << " handshakePackets="
           << conn.outstandings.packetCount[PacketNumberSpace::Handshake] << " "
           << conn;
  return lossEvent;
}

/**
 * We force mark zero rtt packets as lost during zero rtt rejection.
 */
template <class LossVisitor, class ClockType = Clock>
void markZeroRttPacketsLost(
    QuicConnectionStateBase& conn,
    const LossVisitor& lossVisitor) {
  CongestionController::LossEvent lossEvent(ClockType::now());
  auto iter = getFirstOutstandingPacket(conn, PacketNumberSpace::AppData);
  while (iter != conn.outstandings.packets.end()) {
    DCHECK_EQ(
        iter->packet.header.getPacketNumberSpace(), PacketNumberSpace::AppData);
    auto isZeroRttPacket =
        iter->packet.header.getProtectionType() == ProtectionType::ZeroRtt;
    if (isZeroRttPacket) {
      auto& pkt = *iter;
      DCHECK(!pkt.metadata.isHandshake);
      bool processed = pkt.associatedEvent &&
          !conn.outstandings.packetEvents.count(*pkt.associatedEvent);
      lossVisitor(conn, pkt.packet, processed);
      // Remove the PacketEvent from the outstandings.packetEvents set
      if (pkt.associatedEvent) {
        conn.outstandings.packetEvents.erase(*pkt.associatedEvent);
        CHECK(conn.outstandings.clonedPacketCount[PacketNumberSpace::AppData]);
        --conn.outstandings.clonedPacketCount[PacketNumberSpace::AppData];
      }
      lossEvent.addLostPacket(pkt);
      if (!processed) {
        CHECK(conn.outstandings.packetCount[PacketNumberSpace::AppData]);
        --conn.outstandings.packetCount[PacketNumberSpace::AppData];
      }
      iter = conn.outstandings.packets.erase(iter);
      iter = getNextOutstandingPacket(conn, PacketNumberSpace::AppData, iter);
    } else {
      iter =
          getNextOutstandingPacket(conn, PacketNumberSpace::AppData, iter + 1);
    }
  }
  conn.lossState.rtxCount += lossEvent.lostPackets;
  if (conn.congestionController && lossEvent.largestLostPacketNum.hasValue()) {
    conn.congestionController->onRemoveBytesFromInflight(lossEvent.lostBytes);
  }
  VLOG(10) << __func__ << " marked=" << lossEvent.lostPackets;
}
} // namespace quic
