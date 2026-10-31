// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#include "ble_midi.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <atomic>
#include <cstring>

namespace {

// The BLE-MIDI service and its one characteristic (Apple / MMA spec).
constexpr const char* kService = "03B80E5A-EDE8-4B33-A751-6CE34EC4C700";
constexpr const char* kCharacteristic = "7772E5DB-3868-4112-A1A9-F2669D106BF3";
constexpr const char* kName = "PocketDynamo";

// NimBLE host task (core 0) -> control task: packets with their arrival.
constexpr uint32_t kRing = 16;
pd::blemidi::Packet g_ring[kRing];
std::atomic<uint32_t> g_head{0}, g_tail{0};
std::atomic<bool> g_connected{false};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    g_connected = true;
    // 7.5..15 ms: the shortest interval hosts usually grant, for latency.
    server->updateConnParams(info.getConnHandle(), 6, 12, 0, 200);
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    g_connected = false;
    NimBLEDevice::startAdvertising();
  }
};

class MidiCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    const NimBLEAttValue v = c->getValue();
    const uint32_t now = micros();
    if (v.length() == 0 || v.length() > pd::blemidi::kMaxPacket) return;   // big SysEx: nothing we use
    const uint32_t h = g_head.load(std::memory_order_relaxed);
    if (h - g_tail.load(std::memory_order_acquire) >= kRing) return;       // full: drop
    auto& p = g_ring[h % kRing];
    memcpy(p.data, v.data(), v.length());
    p.len = (uint8_t)v.length();
    p.at = now;
    g_head.store(h + 1, std::memory_order_release);
  }
};

ServerCallbacks g_serverCallbacks;
MidiCallbacks g_midiCallbacks;

}  // namespace

namespace pd {
namespace blemidi {

bool begin() {
  if (!NimBLEDevice::init(kName)) return false;
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(&g_serverCallbacks, false);
  NimBLEService* svc = server->createService(kService);
  NimBLECharacteristic* ch = svc->createCharacteristic(
      kCharacteristic, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
  ch->setCallbacks(&g_midiCallbacks);
  svc->start();
  // The 128-bit UUID fills the advertisement; the name rides in the scan
  // response.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData ad, sr;
  ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  ad.addServiceUUID(NimBLEUUID(kService));
  sr.setName(kName);
  adv->setAdvertisementData(ad);
  adv->setScanResponseData(sr);
  return adv->start();
}

bool connected() { return g_connected; }

bool poll(Packet* out) {
  const uint32_t t = g_tail.load(std::memory_order_relaxed);
  if (t == g_head.load(std::memory_order_acquire)) return false;
  *out = g_ring[t % kRing];
  g_tail.store(t + 1, std::memory_order_release);
  return true;
}

}  // namespace blemidi
}  // namespace pd
