// Lewd Vault Nearby on iOS: phones close by, over Bluetooth LE, like AirDrop (ui/chat.js draws the
// sheet). tools/ios-patch.mjs compiles this into the app; LvNearbyLoad.m starts it when the app loads,
// and it finds the app's WKWebView by itself (no link to the Rust side, so nothing there can break).
//
// The same protocol as Android's (mobile/android/LvNearby.kt), so an iPhone and an Android phone see
// each other: every phone advertises the Nearby service (a GATT service with "hello", the id and
// nickname to read, and "data" to write to) and scans for it, connects, reads the hello and writes.
// What travels is already sealed for one person by the chat core (src-tauri/src/chat.rs).
// A message is its length (4 bytes, big-endian), then its UTF-8 bytes, cut to the link's size.
//
// The page sends { op: start | stop | send } to webkit.messageHandlers.lvBle and hears back through
// window.__lvNearby({ type: found | lost | data | state | sent, ... }).
import CoreBluetooth
import Foundation
import UIKit
import WebKit

private let SERVICE = CBUUID(string: "4C560001-6C76-4E65-6172-627930303031")
private let HELLO = CBUUID(string: "4C560002-6C76-4E65-6172-627930303031")
private let DATA = CBUUID(string: "4C560003-6C76-4E65-6172-627930303031")
private let MAX_MESSAGE = 6_000_000

final class LvNearby: NSObject, WKScriptMessageHandler, CBCentralManagerDelegate, CBPeripheralDelegate, CBPeripheralManagerDelegate {
  static var shared: LvNearby?
  private weak var webView: WKWebView?
  private var central: CBCentralManager?
  private var periph: CBPeripheralManager?
  private var hello = Data()
  private var running = false
  private var serviceAdded = false

  private final class Peer {
    let p: CBPeripheral
    var data: CBCharacteristic?
    var ready = false
    var queue: [Data] = []
    var writing = false
    init(_ p: CBPeripheral) { self.p = p }
  }
  private var peers: [UUID: Peer] = [:]
  private var incoming: [UUID: Data] = [:]
  private var incomingLen: [UUID: Int] = [:]

  init(webView: WKWebView) {
    self.webView = webView
    super.init()
  }

  // ---------- the page ----------
  func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
    guard let body = message.body as? [String: Any], let op = body["op"] as? String else { return }
    switch op {
    case "start": begin(hello: body["hello"] as? String ?? "{}")
    case "stop": end()
    case "send": send(peer: body["peer"] as? String ?? "", text: body["data"] as? String ?? "")
    default: break
    }
  }

  private func emit(_ o: [String: Any]) {
    guard let json = try? JSONSerialization.data(withJSONObject: o), var text = String(data: json, encoding: .utf8) else { return }
    // (two characters JSON allows and JavaScript source doesn't)
    text = text.replacingOccurrences(of: "\u{2028}", with: "\\u2028").replacingOccurrences(of: "\u{2029}", with: "\\u2029")
    DispatchQueue.main.async { self.webView?.evaluateJavaScript("window.__lvNearby && window.__lvNearby(\(text))", completionHandler: nil) }
  }
  private func state(_ on: Bool, _ error: String? = nil) {
    var o: [String: Any] = ["type": "state", "on": on]
    if let e = error { o["error"] = e }
    emit(o)
  }

  // ---------- on and off ----------
  private func begin(hello h: String) {
    hello = Data(h.utf8)
    if running { end() }
    running = true
    // (asking for Bluetooth the first time: iOS shows its permission prompt here)
    central = CBCentralManager(delegate: self, queue: nil)
    periph = CBPeripheralManager(delegate: self, queue: nil)
  }

  private func end() {
    running = false
    central?.stopScan()
    for peer in peers.values { central?.cancelPeripheralConnection(peer.p) }
    peers.removeAll()
    periph?.stopAdvertising()
    periph?.removeAllServices()
    serviceAdded = false
    central = nil
    periph = nil
    incoming.removeAll()
    incomingLen.removeAll()
  }

  private func unavailable(_ s: CBManagerState) {
    switch s {
    case .unauthorized: state(false, "Lewd Vault needs Bluetooth: Settings → Lewd Vault → Bluetooth.")
    case .poweredOff: state(false)
    case .unsupported: state(false, "This device has no Bluetooth LE.")
    default: break
    }
  }

  // ---------- being found: the peripheral side ----------
  func peripheralManagerDidUpdateState(_ pm: CBPeripheralManager) {
    guard running else { return }
    guard pm.state == .poweredOn else { unavailable(pm.state); return }
    if serviceAdded { return }
    let svc = CBMutableService(type: SERVICE, primary: true)
    svc.characteristics = [
      CBMutableCharacteristic(type: HELLO, properties: [.read], value: nil, permissions: [.readable]),
      CBMutableCharacteristic(type: DATA, properties: [.write], value: nil, permissions: [.writeable]),
    ]
    pm.add(svc)
    serviceAdded = true
  }

  func peripheralManager(_ pm: CBPeripheralManager, didAdd service: CBService, error: Error?) {
    if let e = error { state(true, "Others can't see this phone right now (\(e.localizedDescription))."); return }
    pm.startAdvertising([CBAdvertisementDataServiceUUIDsKey: [SERVICE]])
    state(true)
  }

  func peripheralManager(_ pm: CBPeripheralManager, didReceiveRead request: CBATTRequest) {
    guard request.characteristic.uuid == HELLO else { pm.respond(to: request, withResult: .requestNotSupported); return }
    if request.offset > hello.count { pm.respond(to: request, withResult: .invalidOffset); return }
    request.value = hello.subdata(in: request.offset..<hello.count)
    pm.respond(to: request, withResult: .success)
  }

  func peripheralManager(_ pm: CBPeripheralManager, didReceiveWrite requests: [CBATTRequest]) {
    for r in requests where r.characteristic.uuid == DATA {
      if let v = r.value { take(from: r.central.identifier, chunk: v) }
    }
    if let first = requests.first { pm.respond(to: first, withResult: .success) }
  }

  private func take(from: UUID, chunk: Data) {
    var bytes = chunk
    if incoming[from] == nil {
      guard chunk.count >= 4 else { return }
      let b = [UInt8](chunk.prefix(4))
      let len = (Int(b[0]) << 24) | (Int(b[1]) << 16) | (Int(b[2]) << 8) | Int(b[3])
      guard len > 0 && len <= MAX_MESSAGE else { return }
      incoming[from] = Data()
      incomingLen[from] = len
      bytes = chunk.subdata(in: chunk.startIndex + 4..<chunk.endIndex)
    }
    incoming[from]!.append(bytes)
    guard let len = incomingLen[from], let buf = incoming[from], buf.count >= len else { return }
    incoming[from] = nil
    incomingLen[from] = nil
    if let text = String(data: buf.prefix(len), encoding: .utf8) {
      emit(["type": "data", "peer": from.uuidString, "data": text])
    }
  }

  // ---------- finding others: the central side ----------
  func centralManagerDidUpdateState(_ cm: CBCentralManager) {
    guard running else { return }
    guard cm.state == .poweredOn else { unavailable(cm.state); return }
    cm.scanForPeripherals(withServices: [SERVICE], options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
  }

  func centralManager(_ cm: CBCentralManager, didDiscover p: CBPeripheral, advertisementData: [String: Any], rssi: NSNumber) {
    guard running, peers[p.identifier] == nil else { return }
    peers[p.identifier] = Peer(p)
    p.delegate = self
    cm.connect(p, options: nil)
  }

  func centralManager(_ cm: CBCentralManager, didConnect p: CBPeripheral) {
    p.discoverServices([SERVICE])
  }

  func centralManager(_ cm: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
    peers[p.identifier] = nil
  }

  func centralManager(_ cm: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
    let was = peers[p.identifier]
    peers[p.identifier] = nil
    if was?.ready == true { emit(["type": "lost", "peer": p.identifier.uuidString]) }
  }

  func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
    guard let svc = p.services?.first(where: { $0.uuid == SERVICE }) else { central?.cancelPeripheralConnection(p); return }
    p.discoverCharacteristics([HELLO, DATA], for: svc)
  }

  func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
    guard let peer = peers[p.identifier] else { return }
    for ch in service.characteristics ?? [] {
      if ch.uuid == DATA { peer.data = ch }
      if ch.uuid == HELLO { p.readValue(for: ch) }
    }
  }

  func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
    guard ch.uuid == HELLO, error == nil, let peer = peers[p.identifier], !peer.ready, let v = ch.value else { return }
    peer.ready = true
    emit(["type": "found", "peer": p.identifier.uuidString, "hello": String(data: v, encoding: .utf8) ?? "{}"])
  }

  // ---------- sending ----------
  private func send(peer id: String, text: String) {
    guard let uuid = UUID(uuidString: id), let peer = peers[uuid], peer.ready, peer.data != nil else { state(running, "They're out of reach now."); return }
    let bytes = Data(text.utf8)
    var framed = Data([UInt8((bytes.count >> 24) & 0xff), UInt8((bytes.count >> 16) & 0xff), UInt8((bytes.count >> 8) & 0xff), UInt8(bytes.count & 0xff)])
    framed.append(bytes)
    let size = max(20, peer.p.maximumWriteValueLength(for: .withResponse))
    var i = 0
    while i < framed.count {
      let j = min(framed.count, i + size)
      peer.queue.append(framed.subdata(in: i..<j))
      i = j
    }
    pump(peer)
  }

  private func pump(_ peer: Peer) {
    guard !peer.writing, let ch = peer.data else { return }
    guard !peer.queue.isEmpty else { emit(["type": "sent", "peer": peer.p.identifier.uuidString]); return }
    peer.writing = true
    peer.p.writeValue(peer.queue.removeFirst(), for: ch, type: .withResponse)
  }

  func peripheral(_ p: CBPeripheral, didWriteValueFor ch: CBCharacteristic, error: Error?) {
    guard let peer = peers[p.identifier] else { return }
    peer.writing = false
    if error != nil { peer.queue.removeAll(); state(true, "Sending didn't finish: try again closer."); return }
    pump(peer)
  }
}

/// Started by LvNearbyLoad.m: waits for the app's web view to exist, then puts "lvBle" on it.
@objc(LvNearbyInstaller) final class LvNearbyInstaller: NSObject {
  @objc static func begin() { tick(0) }
  private static var authHandler: LvAuth?

  private static func tick(_ n: Int) {
    if LvNearby.shared != nil || n > 240 { return }
    if let wv = findWebView() {
      let nearby = LvNearby(webView: wv)
      LvNearby.shared = nearby
      wv.configuration.userContentController.add(nearby, name: "lvBle")
      // (and Google Drive's sign-in sheet, LvAuth.swift)
      let auth = LvAuth(webView: wv)
      authHandler = auth
      wv.configuration.userContentController.add(auth, name: "lvAuth")
      // (the page shows Drive's sign-in once it knows the sheet is there)
      wv.evaluateJavaScript("document.documentElement.classList.add('lv-auth')", completionHandler: nil)
      return
    }
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { tick(n + 1) }
  }

  private static func findWebView() -> WKWebView? {
    for scene in UIApplication.shared.connectedScenes {
      guard let ws = scene as? UIWindowScene else { continue }
      for w in ws.windows { if let v = search(w) { return v } }
    }
    return nil
  }

  private static func search(_ v: UIView) -> WKWebView? {
    if let w = v as? WKWebView { return w }
    for s in v.subviews { if let f = search(s) { return f } }
    return nil
  }
}
