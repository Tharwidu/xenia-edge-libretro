/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "xenia/net/xnet_association.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::net::secure::test {
namespace {

using KeyStore = std::map<KeyId, Key>;

Address Identity(uint8_t machine) {
  Address address{0x7F, 0x00, 0x00, 0x01};
  const Mac mac{0x02, 0x58, 0x00, 0x00, 0x00, machine};
  std::copy(mac.begin(), mac.end(), address.begin() + 10);
  return address;
}

struct Pair {
  KeyStore a, b;
  KeyId id{1, 2, 3, 4, 5, 6, 7, 8};
  Key key{9, 10, 11, 12, 13, 14, 15, 16};
  ProtectionKeys broadcast;
  std::unique_ptr<Associations> left, right;
  Address aa = Identity(1), ba = Identity(2);

  Pair() {
    a[id] = key;
    b[id] = key;
    auto derived = DeriveBroadcast(key, [&](auto bytes, auto& digest) {
      digest = HmacSha1(key, {bytes});
      return true;
    });
    REQUIRE(derived);
    broadcast = *derived;
    left = Make(a, aa);
    right = Make(b, ba);
  }

  std::unique_ptr<Associations> Make(
      KeyStore& keys, Address address, std::array<uint8_t, 4> reason = {},
      bool sequence_zero = false,
      std::optional<CrossPlatform> cross_platform = std::nullopt) {
    AssociationConfig config;
    config.identity = address;
    config.broadcast = broadcast;
    config.teardown_reason = reason;
    config.sequence_zero = sequence_zero;
    config.cross_platform = cross_platform;
    config.freshness_time = [] { return uint64_t(100); };
    config.metadata = MakeMetadata(0x10203040, 0x01020304, 2, 0, 1234, 1);
    config.credential = [&keys](const KeyId& id) -> std::optional<Key> {
      auto found = keys.find(id);
      return found != keys.end() ? std::optional(found->second) : std::nullopt;
    };
    return std::make_unique<Associations>(std::move(config));
  }

  void Transfer(Associations& from, Associations& to, uint64_t now = 100) {
    for (auto& frame : from.TakeOutbound()) {
      auto wire = EncodeFrame(frame);
      REQUIRE(wire);
      auto decoded = DecodeFrame(*wire);
      REQUIRE(decoded);
      to.Receive(*decoded, now);
    }
  }

  void Pump(uint64_t now = 100) {
    for (int i = 0; i < 4; ++i) {
      Transfer(*left, *right, now);
      Transfer(*right, *left, now);
    }
  }
};

Packet Data() {
  Packet packet;
  packet.source_port = 500;
  packet.destination_port = 60000;
  packet.encrypted = {0, 2, 4, 6, 8, 10, 12, 14, 16};
  return packet;
}

}  // namespace

TEST_CASE("Secure association zero counter policy is explicit per generation",
          "[net][secure][association]") {
  for (bool accept_zero : {false, true}) {
    Pair pair;
    pair.left = pair.Make(pair.a, pair.aa, {}, true);
    pair.right = pair.Make(pair.b, pair.ba, {}, accept_zero);
    for (int generation = 0; generation < 2; ++generation) {
      REQUIRE(pair.left->Connect(pair.ba, pair.id, 100));
      pair.Transfer(*pair.left, *pair.right);
      pair.Transfer(*pair.right, *pair.left);
      auto first = pair.left->TakeOutbound();
      REQUIRE(first.size() == 1);
      REQUIRE(first[0].payload.size() >= 12);
      CHECK(first[0].payload[first[0].payload.size() - 12] == 0);
      CHECK(first[0].payload[first[0].payload.size() - 11] == 0);
      pair.right->Receive(first[0], 100);
      CHECK(pair.right->Status(pair.aa, pair.id) ==
            (accept_zero ? AssociationStatus::Established
                         : AssociationStatus::Pending));
      pair.right->TakeEvents();
      pair.right->Receive(first[0], 100);
      CHECK(pair.right->TakeEvents().empty());
      REQUIRE(pair.left->Send(pair.ba, pair.id, Data(), 101));
      auto next = pair.left->TakeOutbound();
      REQUIRE(next.size() == 1);
      CHECK(next[0].payload[next[0].payload.size() - 12] == 1);
      pair.right->Receive(next[0], 101);
      auto events = pair.right->TakeEvents();
      REQUIRE(events.size() == (accept_zero ? 1 : 2));
      CHECK(events.back().packet.encrypted == Data().encrypted);
      pair.left->Close(pair.ba, pair.id, 102);
      pair.Transfer(*pair.left, *pair.right, 102);
      pair.left->TakeEvents();
      pair.right->TakeEvents();
    }
  }
}

TEST_CASE(
    "Secure association delivers discovery and unchanged UDP/VDP in both "
    "directions",
    "[net][secure][association]") {
  for (bool reverse : {false, true}) {
    Pair pair;
    auto& sender = reverse ? *pair.right : *pair.left;
    auto& receiver = reverse ? *pair.left : *pair.right;
    const auto& remote = reverse ? pair.aa : pair.ba;
    const auto& local = reverse ? pair.ba : pair.aa;
    REQUIRE(sender.Discover(Data()));
    pair.Transfer(sender, receiver);
    auto events = receiver.TakeEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].packet.kind == Kind::Discovery);
    CHECK(events[0].packet.encrypted == Data().encrypted);
    CHECK(receiver.Status(local, pair.id) == AssociationStatus::Missing);
    REQUIRE(sender.Send(remote, pair.id, Data(), 100));
    CHECK(sender.Status(remote, pair.id) == AssociationStatus::Pending);
    pair.Pump();
    CHECK(sender.Status(remote, pair.id) == AssociationStatus::Established);
    CHECK(receiver.Status(local, pair.id) == AssociationStatus::Established);
    events = receiver.TakeEvents();
    REQUIRE(events.size() == 2);
    CHECK(events[0].type == AssociationEvent::Type::Connected);
    CHECK(events[1].packet.encrypted == Data().encrypted);
    CHECK(events[1].packet.destination_port == 60000);
    sender.TakeEvents();
    auto vdp = Data();
    vdp.kind = Kind::Vdp;
    vdp.clear = {11, 22, 33, 44, 55};
    REQUIRE(receiver.Send(local, pair.id, vdp, 101));
    pair.Transfer(receiver, sender, 101);
    events = sender.TakeEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].packet.kind == Kind::Vdp);
    CHECK(events[0].packet.encrypted == vdp.encrypted);
    CHECK(events[0].packet.clear == vdp.clear);
  }
}

TEST_CASE(
    "Secure exchange retries preserve logical identity and reject forged "
    "binding",
    "[net][secure][association]") {
  Pair pair;
  REQUIRE(pair.left->Connect(pair.ba, pair.id, 100));
  auto request = pair.left->TakeOutbound().at(0);
  auto registration =
      DeriveRegistration(pair.broadcast.authentication, pair.id, pair.key);
  auto original = DecodeExchange(registration, pair.id, request.payload);
  REQUIRE(original);
  auto mutation = request;
  mutation.payload.back() ^= 1;
  pair.right->Receive(mutation, 100);
  CHECK(pair.right->TakeOutbound().empty());
  mutation = request;
  mutation.source[5] ^= 1;
  pair.right->Receive(mutation, 100);
  CHECK(pair.right->TakeOutbound().empty());
  pair.right->Receive(request, 100);
  auto response = pair.right->TakeOutbound().at(0);
  auto first = DecodeExchange(registration, pair.id, response.payload);
  REQUIRE(first);
  CHECK(pair.right->Status(pair.aa, pair.id) == AssociationStatus::Pending);
  pair.left->Tick(1100);
  request = pair.left->TakeOutbound().at(0);
  auto retry = DecodeExchange(registration, pair.id, request.payload);
  REQUIRE(retry);
  CHECK(retry->initiator_spi == original->initiator_spi);
  CHECK(retry->initiator_challenge == original->initiator_challenge);
  CHECK(retry->public_value == original->public_value);
  CHECK(retry->freshness > original->freshness);
  CHECK(original->freshness == 100);
  CHECK(retry->freshness == 101);
  CHECK(retry->address_iv != original->address_iv);
  pair.right->Receive(request, 1100);
  auto second_response = pair.right->TakeOutbound().at(0);
  auto second = DecodeExchange(registration, pair.id, second_response.payload);
  REQUIRE(second);
  CHECK(second->responder_spi == first->responder_spi);
  CHECK(second->responder_challenge == first->responder_challenge);
  CHECK(second->public_value == first->public_value);
  CHECK(second->freshness > first->freshness);
  // A repeat with the same freshness changes nothing.
  pair.right->Receive(request, 1101);
  CHECK(pair.right->TakeOutbound().empty());
  mutation = second_response;
  mutation.source[5] ^= 1;
  pair.left->Receive(mutation, 1101);
  CHECK(pair.left->Status(pair.ba, pair.id) == AssociationStatus::Pending);
  auto bad = *second;
  bad.initiator_challenge[0] ^= 1;
  mutation = second_response;
  auto payload = EncodeExchange(registration, bad);
  REQUIRE(payload);
  mutation.payload.assign(payload->begin(), payload->end());
  pair.left->Receive(mutation, 1101);
  CHECK(pair.left->Status(pair.ba, pair.id) == AssociationStatus::Pending);
  pair.left->Receive(second_response, 1101);
  pair.Pump(1101);
  CHECK(pair.right->Status(pair.aa, pair.id) == AssociationStatus::Established);
  REQUIRE(pair.left->Send(pair.ba, pair.id, Data(), 1102));
  auto traffic = pair.left->TakeOutbound().at(0);
  pair.right->TakeEvents();
  mutation = traffic;
  mutation.payload.back() ^= 1;
  pair.right->Receive(mutation, 1102);
  CHECK(pair.right->TakeEvents().empty());
  pair.right->Receive(traffic, 1102);
  CHECK(pair.right->TakeEvents().size() == 1);
  pair.right->Receive(traffic, 1102);
  CHECK(pair.right->TakeEvents().empty());
}

TEST_CASE(
    "Secure teardown retires a generation once and rejoin negotiates fresh "
    "keys",
    "[net][secure][association]") {
  Pair pair;
  REQUIRE(pair.left->Connect(pair.ba, pair.id, 100));
  auto initial = pair.left->TakeOutbound().at(0);
  pair.right->Receive(initial, 100);
  pair.Pump();
  pair.left->TakeEvents();
  pair.right->TakeEvents();
  REQUIRE(pair.left->Send(pair.ba, pair.id, Data(), 101));
  auto old = pair.left->TakeOutbound().at(0);
  pair.left->Close(pair.ba, pair.id, 102);
  pair.Transfer(*pair.left, *pair.right, 102);
  auto events = pair.right->TakeEvents();
  REQUIRE(events.size() == 1);
  CHECK(events[0].type == AssociationEvent::Type::Retired);
  CHECK(events[0].packet.encrypted == std::vector<uint8_t>(4, 0));
  CHECK(pair.right->Status(pair.aa, pair.id) == AssociationStatus::Missing);
  pair.right->Receive(old, 103);
  CHECK(pair.right->TakeEvents().empty());
  REQUIRE(pair.left->Connect(pair.ba, pair.id, 103));
  auto rejoin = pair.left->TakeOutbound().at(0);
  auto registration =
      DeriveRegistration(pair.broadcast.authentication, pair.id, pair.key);
  auto first = DecodeExchange(registration, pair.id, initial.payload);
  auto fresh = DecodeExchange(registration, pair.id, rejoin.payload);
  REQUIRE(first);
  REQUIRE(fresh);
  CHECK(first->public_value != fresh->public_value);
  CHECK(first->initiator_challenge != fresh->initiator_challenge);
  pair.right->Receive(rejoin, 103);
  pair.Pump(103);
  pair.right->TakeEvents();
  pair.right->Receive(old, 104);
  CHECK(pair.right->TakeEvents().empty());
  REQUIRE(pair.left->Send(pair.ba, pair.id, Data(), 104));
  pair.Transfer(*pair.left, *pair.right, 104);
  CHECK(pair.right->TakeEvents().size() == 1);
  pair.left->Unregister(pair.id, 105);
  REQUIRE(pair.a.erase(pair.id) == 1);
  pair.Transfer(*pair.left, *pair.right, 105);
  CHECK_FALSE(pair.left->Connect(pair.ba, pair.id, 106));
  CHECK(pair.right->Status(pair.aa, pair.id) == AssociationStatus::Missing);
}

TEST_CASE(
    "Secure association notes broadcasts of other builds that fail to "
    "authenticate",
    "[net][secure][association]") {
  Pair pair;
  // The receiver runs version 0x01020304 of title 0x10203040 on kernel 1234.
  const uint32_t title = 0x10203040, version = 0x01020304;
  auto broadcast = [&](const Metadata& metadata, bool same_keys,
                       const Packet& packet = Data()) {
    AssociationConfig config;
    config.identity = Identity(3);
    config.broadcast = pair.broadcast;
    if (!same_keys) {
      config.broadcast.authentication[0] ^= 1;
    }
    config.metadata = metadata;
    Associations sender(std::move(config));
    REQUIRE(sender.Discover(packet));
    pair.Transfer(sender, *pair.right);
  };
  auto heard = [&](const Metadata& metadata, bool same_keys,
                   const Packet& packet = Data()) {
    broadcast(metadata, same_keys, packet);
    return pair.right->BroadcastVersionStatus(true);
  };
  const auto older = MakeMetadata(title, version - 1, 2, 0, 1234, 1);
  const auto newer_kernel = MakeMetadata(title, version, 2, 0, 1235, 1);
  CHECK(heard(older, false) == kBroadcastVersionOlder);
  CHECK(heard(newer_kernel, false) == kBroadcastVersionNewer);
  CHECK(heard(MakeMetadata(title, version + 1, 2, 0, 1233, 1), false) ==
        (kBroadcastVersionOlder | kBroadcastVersionNewer));
  CHECK(heard(MakeMetadata(title, version, 2, 0, 1234, 1), false) == 0);
  CHECK(heard(MakeMetadata(0x50607080, version - 1, 2, 0, 1234, 1), false) ==
        0);
  // A broadcast that authenticates is delivered whatever its version.
  CHECK(heard(older, true) == 0);
  CHECK(pair.right->TakeEvents().size() == 1);
  // Ports that don't fit in the padding go in the clear before the metadata.
  auto unpadded = Data();
  unpadded.encrypted.resize(8);
  CHECK(heard(older, false, unpadded) == kBroadcastVersionOlder);
  auto short_ports = Data();
  short_ports.source_port = short_ports.destination_port = 1001;
  short_ports.encrypted.resize(7);
  CHECK(heard(older, false, short_ports) == kBroadcastVersionOlder);

  // The bits add up until a reset.
  broadcast(older, false);
  broadcast(newer_kernel, false);
  const uint32_t both = kBroadcastVersionOlder | kBroadcastVersionNewer;
  CHECK(pair.right->BroadcastVersionStatus(false) == both);
  CHECK(pair.right->BroadcastVersionStatus(true) == both);
  CHECK(pair.right->BroadcastVersionStatus(false) == 0);
  broadcast(older, false);
  pair.right->Reset();
  CHECK(pair.right->BroadcastVersionStatus(false) == 0);
}

TEST_CASE(
    "Secure association ignores the version of malformed or unicast discovery "
    "packets",
    "[net][secure][association]") {
  Pair pair;
  AssociationConfig config;
  config.identity = Identity(3);
  config.broadcast = pair.broadcast;
  config.broadcast.authentication[0] ^= 1;
  config.metadata = MakeMetadata(0x10203040, 0x01020303, 2, 0, 1234, 1);
  Associations older(std::move(config));
  REQUIRE(older.Discover(Data()));
  auto frames = older.TakeOutbound();
  REQUIRE(frames.size() == 1);
  const Frame search = frames[0];
  auto heard = [&](const Frame& frame) {
    pair.right->Receive(frame, 100);
    return pair.right->BroadcastVersionStatus(true);
  };
  REQUIRE(heard(search) == kBroadcastVersionOlder);
  auto unicast = search;
  unicast.broadcast = false;
  unicast.destination = {0x02, 0x58, 0x00, 0x00, 0x00, 0x02};
  CHECK(heard(unicast) == 0);
  auto altered = [&](const std::function<void(std::vector<uint8_t>&)>& change) {
    auto frame = search;
    change(frame.payload);
    return heard(frame);
  };
  // A nonzero SPI, another kind, block counts and sizes that disagree.
  CHECK(altered([](auto& wire) { wire[3] = 1; }) == 0);
  CHECK(altered([](auto& wire) { wire[0] = (wire[0] & 0xF0) | 4; }) == 0);
  CHECK(altered([](auto& wire) { ++wire[wire.size() - 13]; }) == 0);
  CHECK(altered([](auto& wire) { wire.insert(wire.end() - 25, 0); }) == 0);
  CHECK(altered([](auto& wire) { wire.erase(wire.begin() + 4); }) == 0);
  CHECK(altered([](auto& wire) { wire.resize(28); }) == 0);
}

TEST_CASE(
    "Secure pending retries and authenticated receive inactivity are bounded "
    "local policy",
    "[net][secure][association]") {
  Pair pair;
  REQUIRE(pair.left->Connect(pair.ba, pair.id, 0));
  for (uint64_t now = 1000; now <= 5000; now += 1000) {
    pair.left->TakeOutbound();
    pair.left->Tick(now);
  }
  CHECK(pair.left->Status(pair.ba, pair.id) == AssociationStatus::Missing);
  REQUIRE(pair.left->TakeEvents().size() == 1);
  pair.left->Tick(6000);
  CHECK(pair.left->TakeEvents().empty());
  REQUIRE(pair.left->Connect(pair.ba, pair.id, 10000));
  pair.Pump(10000);
  pair.left->TakeEvents();
  pair.right->TakeEvents();
  pair.left->Tick(20000);
  pair.Transfer(*pair.left, *pair.right, 20000);
  pair.right->Tick(79000);
  CHECK(pair.right->Status(pair.aa, pair.id) == AssociationStatus::Established);
  pair.right->Tick(80000);
  CHECK(pair.right->Status(pair.aa, pair.id) == AssociationStatus::Missing);
  CHECK(pair.right->TakeEvents().size() == 1);
}

TEST_CASE(
    "A lost first keepalive is retried without resetting established traffic "
    "keys",
    "[net][secure][association]") {
  Pair pair;
  REQUIRE(pair.left->Connect(pair.ba, pair.id, 100));
  pair.Transfer(*pair.left, *pair.right, 100);
  pair.Transfer(*pair.right, *pair.left, 100);
  pair.left->TakeOutbound();  // Drop the first protected keepalive.
  CHECK(pair.left->Status(pair.ba, pair.id) == AssociationStatus::Established);
  CHECK(pair.right->Status(pair.aa, pair.id) == AssociationStatus::Pending);
  pair.right->Tick(1100);
  pair.Transfer(*pair.right, *pair.left, 1100);
  pair.Transfer(*pair.left, *pair.right, 1100);
  CHECK(pair.right->Status(pair.aa, pair.id) == AssociationStatus::Established);
  REQUIRE(pair.left->Send(pair.ba, pair.id, Data(), 1101));
  pair.right->TakeEvents();
  pair.Transfer(*pair.left, *pair.right, 1101);
  CHECK(pair.right->TakeEvents().size() == 1);
}

TEST_CASE(
    "Simultaneous secure connect rejects invalid DH before retiring the losing "
    "pending state",
    "[net][secure][association]") {
  Pair pair;
  REQUIRE(pair.left->Connect(pair.ba, pair.id, 100));
  REQUIRE(pair.right->Connect(pair.aa, pair.id, 100));
  auto left_request = pair.left->TakeOutbound().at(0);
  auto right_request = pair.right->TakeOutbound().at(0);
  auto registration =
      DeriveRegistration(pair.broadcast.authentication, pair.id, pair.key);
  auto exchange = DecodeExchange(registration, pair.id, right_request.payload);
  REQUIRE(exchange);
  exchange->public_value.fill(0);
  auto encoded = EncodeExchange(registration, *exchange);
  REQUIRE(encoded);
  auto bad = right_request;
  bad.payload.assign(encoded->begin(), encoded->end());
  pair.left->Receive(bad, 101);
  CHECK(pair.left->Status(pair.ba, pair.id) == AssociationStatus::Pending);
  CHECK(pair.left->TakeEvents().empty());
  CHECK(pair.left->TakeOutbound().empty());
  pair.left->Receive(right_request, 101);
  pair.right->Receive(left_request, 101);
  pair.Pump(101);
  CHECK(pair.left->Status(pair.ba, pair.id) == AssociationStatus::Established);
  CHECK(pair.right->Status(pair.aa, pair.id) == AssociationStatus::Established);
}

TEST_CASE("Simultaneous secure connect keeps the larger MAC as the initiator",
          "[net][secure][association]") {
  Pair pair;
  REQUIRE(pair.left->Connect(pair.ba, pair.id, 100));
  REQUIRE(pair.right->Connect(pair.aa, pair.id, 100));
  auto left_request = pair.left->TakeOutbound().at(0);
  auto right_request = pair.right->TakeOutbound().at(0);
  // Only the smaller MAC answers a crossed request.
  pair.right->Receive(left_request, 101);
  CHECK(pair.right->TakeOutbound().empty());
  pair.left->Receive(right_request, 101);
  auto answer = pair.left->TakeOutbound();
  REQUIRE(answer.size() == 1);
  auto registration =
      DeriveRegistration(pair.broadcast.authentication, pair.id, pair.key);
  auto exchange = DecodeExchange(registration, pair.id, answer[0].payload);
  REQUIRE(exchange);
  CHECK(exchange->response);
  pair.right->Receive(answer[0], 101);
  CHECK(pair.right->Status(pair.aa, pair.id) == AssociationStatus::Established);
}

TEST_CASE(
    "A secure request is answered when the MAC of the XNADDR it targets is "
    "this console's",
    "[net][secure][association]") {
  Pair other;
  REQUIRE(other.left->Connect(Identity(3), other.id, 100));
  auto request = other.left->TakeOutbound().at(0);
  std::copy_n(other.ba.begin() + 10, 6, request.destination.begin());
  other.right->Receive(request, 100);
  CHECK(other.right->TakeOutbound().empty());

  Pair pair;
  // Splinter Cell: Conviction hosts put the first four characters of their
  // dotted IPs in place of the IPs.
  auto target = pair.ba;
  std::copy_n("127.0.0.", 8, target.begin());
  REQUIRE(pair.left->Connect(target, pair.id, 100));
  pair.Pump();
  CHECK(pair.left->Status(target, pair.id) == AssociationStatus::Established);
  CHECK(pair.right->Status(pair.aa, pair.id) == AssociationStatus::Established);
  REQUIRE(pair.left->Send(target, pair.id, Data(), 101));
  pair.right->TakeEvents();
  pair.Transfer(*pair.left, *pair.right, 101);
  auto events = pair.right->TakeEvents();
  REQUIRE(events.size() == 1);
  CHECK(events[0].peer == pair.aa);
  REQUIRE(pair.right->Send(pair.aa, pair.id, Data(), 102));
  pair.left->TakeEvents();
  pair.Transfer(*pair.right, *pair.left, 102);
  events = pair.left->TakeEvents();
  REQUIRE(events.size() == 1);
  CHECK(events[0].peer == target);
}

TEST_CASE(
    "A cross-platform secure request is answered when the IP and MAC of the "
    "XNADDR it targets are this console's",
    "[net][secure][association]") {
  Pair pair;
  auto id = pair.id;
  id[0] |= SystemLinkKeyType(true);
  pair.a[id] = pair.key;
  pair.b[id] = pair.key;
  constexpr uint32_t kLeftIp = 0xC0A8010A, kRightIp = 0xC0A80114,
                     kMask = 0xFFFFFF00;
  auto at = [](Address address, uint32_t ip) {
    for (size_t i = 0; i < 4; ++i) {
      address[i] = uint8_t(ip >> (24 - 8 * i));
    }
    return address;
  };
  auto aa = at(pair.aa, kLeftIp), ba = at(pair.ba, kRightIp);
  auto left = pair.Make(pair.a, aa, {}, false, CrossPlatform{kLeftIp, kMask});
  auto right = pair.Make(pair.b, ba, {}, false, CrossPlatform{kRightIp, kMask});
  REQUIRE(left->Connect(ba, id, 100));
  auto request = left->TakeOutbound().at(0);
  auto registration =
      DeriveRegistration(pair.broadcast.authentication, id, pair.key);
  auto exchange = DecodeExchange(registration, id, request.payload);
  REQUIRE(exchange);
  auto deliver = [&](const Address& target) {
    exchange->responder_address = target;
    auto payload = EncodeExchange(registration, *exchange);
    REQUIRE(payload);
    auto frame = request;
    frame.payload.assign(payload->begin(), payload->end());
    right->Receive(frame, 100);
    return right->TakeOutbound().size();
  };
  auto target = ba;
  target[3] = 30;
  CHECK(deliver(target) == 0);
  target = ba;
  target[10] ^= 1;
  CHECK(deliver(target) == 0);
  target = ba;
  target[4] = 1;
  CHECK(deliver(target) == 1);
}

TEST_CASE(
    "Secure pending datagrams honor exact physical MTU without blocking later "
    "traffic",
    "[net][secure][association]") {
  Pair pair;
  Packet packet;
  packet.encrypted.resize(1456, 1);
  REQUIRE(pair.left->Send(pair.ba, pair.id, packet, 100));
  auto oversized = packet;
  oversized.encrypted.push_back(2);
  CHECK_FALSE(pair.left->Send(pair.ba, pair.id, oversized, 100));
  pair.Pump();
  auto events = pair.right->TakeEvents();
  REQUIRE(events.size() == 2);
  CHECK(events.back().packet.encrypted == packet.encrypted);
  CHECK_FALSE(pair.left->Send(pair.ba, pair.id, oversized, 101));
  REQUIRE(pair.left->Send(pair.ba, pair.id, Data(), 102));
  pair.Transfer(*pair.left, *pair.right, 102);
  events = pair.right->TakeEvents();
  REQUIRE(events.size() == 1);
  CHECK(events.back().packet.encrypted == Data().encrypted);
}

TEST_CASE("Authenticated teardown preserves configurable opaque reason bytes",
          "[net][secure][association]") {
  Pair pair;
  pair.left = pair.Make(pair.a, pair.aa, {1, 2, 3, 4});
  REQUIRE(pair.left->Connect(pair.ba, pair.id, 100));
  pair.Pump();
  pair.right->TakeEvents();
  pair.left->Close(pair.ba, pair.id, 101);
  pair.Transfer(*pair.left, *pair.right, 101);
  auto events = pair.right->TakeEvents();
  REQUIRE(events.size() == 1);
  CHECK(events[0].type == AssociationEvent::Type::Retired);
  CHECK(events[0].packet.encrypted == std::vector<uint8_t>{1, 2, 3, 4});
}

}  // namespace xe::net::secure::test
