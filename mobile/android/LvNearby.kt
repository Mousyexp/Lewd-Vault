package __PACKAGE__

// Lewd Vault Nearby: phones close by, over Bluetooth LE, like AirDrop (ui/chat.js draws the sheet).
// tools/android-patch.mjs copies this file next to MainActivity.kt and hands it the web view.
//
// Every phone does both sides while the Nearby sheet is open: it advertises the Nearby service (a GATT
// server with "hello", the id and nickname to read, and "data" to write to) and scans for the same
// service, connects to what it finds, reads its hello and, when you send, writes to it. What travels is
// already sealed for the one person by the chat core (src-tauri/src/chat.rs): this only moves text.
// A message is its length (4 bytes, big-endian), then its UTF-8 bytes, cut to the link's size.
//
// The page talks to it through window.LvBle (start(helloJson), stop(), send(peer, text)) and hears back
// through window.__lvNearby({ type: found | lost | data | state | sent, ... }).
import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.webkit.JavascriptInterface
import android.webkit.WebView
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.util.ArrayDeque
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap

@SuppressLint("MissingPermission")
class LvNearby(private val activity: WryActivity, private val webView: WebView) {
  companion object {
    // (our own: nothing else answers to these)
    val SERVICE: UUID = UUID.fromString("4c560001-6c76-4e65-6172-627930303031")
    val HELLO: UUID = UUID.fromString("4c560002-6c76-4e65-6172-627930303031")
    val DATA: UUID = UUID.fromString("4c560003-6c76-4e65-6172-627930303031")
    const val MAX_MESSAGE = 6_000_000
  }

  private val main = Handler(Looper.getMainLooper())
  private var hello = ByteArray(0)
  private var running = false
  private var server: BluetoothGattServer? = null
  private var advertiser: android.bluetooth.le.BluetoothLeAdvertiser? = null
  private var scanner: android.bluetooth.le.BluetoothLeScanner? = null
  private val peers = ConcurrentHashMap<String, Peer>()
  private val incoming = ConcurrentHashMap<String, ByteArrayOutputStream>()
  private val incomingLen = ConcurrentHashMap<String, Int>()

  inner class Peer(val device: BluetoothDevice) {
    var gatt: BluetoothGatt? = null
    var mtu = 23
    var ready = false
    var writing = false
    val queue = ArrayDeque<ByteArray>()
  }

  inner class Bridge {
    @JavascriptInterface fun start(helloJson: String) { main.post { begin(helloJson) } }
    @JavascriptInterface fun stop() { main.post { end() } }
    @JavascriptInterface fun send(peer: String, data: String) { main.post { sendTo(peer, data) } }
  }
  val bridge = Bridge()

  private fun manager(): BluetoothManager? = activity.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager?

  private fun emit(o: JSONObject) {
    val js = "window.__lvNearby && window.__lvNearby(" + o.toString() + ")"
    main.post { try { webView.evaluateJavascript(js, null) } catch (e: Exception) {} }
  }
  private fun state(on: Boolean, error: String? = null) {
    val o = JSONObject().put("type", "state").put("on", on)
    if (error != null) o.put("error", error)
    emit(o)
  }

  // ---------- permissions ----------
  private fun needed(): Array<String> =
    if (Build.VERSION.SDK_INT >= 31) arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_ADVERTISE, Manifest.permission.BLUETOOTH_CONNECT)
    else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
  private fun granted() = needed().all { activity.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

  // ---------- on and off ----------
  private fun begin(h: String) {
    hello = h.toByteArray(Charsets.UTF_8)
    if (!granted()) {
      activity.requestPermissions(needed()) { ok ->
        main.post { if (ok == true && granted()) begin(h) else state(false, "Lewd Vault needs the Nearby devices permission (Settings → Apps → Lewd Vault → Permissions).") }
      }
      return
    }
    val adapter = manager()?.adapter
    if (adapter == null || !adapter.isEnabled) { state(false); return }
    if (running) end()
    running = true
    try {
      openServer()
      advertise(adapter)
      scan(adapter)
      state(true)
    } catch (e: Exception) {
      state(false, "Bluetooth didn't start: " + (e.message ?: "unknown"))
    }
  }

  fun end() {
    if (!running) return
    running = false
    try { scanner?.stopScan(scanCallback) } catch (e: Exception) {}
    try { advertiser?.stopAdvertising(advertiseCallback) } catch (e: Exception) {}
    for (p in peers.values) try { p.gatt?.disconnect(); p.gatt?.close() } catch (e: Exception) {}
    peers.clear()
    try { server?.close() } catch (e: Exception) {}
    server = null
    incoming.clear()
    incomingLen.clear()
  }

  // ---------- being found: the server ----------
  private fun openServer() {
    val s = manager()?.openGattServer(activity, serverCallback) ?: return
    val svc = BluetoothGattService(SERVICE, BluetoothGattService.SERVICE_TYPE_PRIMARY)
    svc.addCharacteristic(BluetoothGattCharacteristic(HELLO, BluetoothGattCharacteristic.PROPERTY_READ, BluetoothGattCharacteristic.PERMISSION_READ))
    svc.addCharacteristic(BluetoothGattCharacteristic(DATA, BluetoothGattCharacteristic.PROPERTY_WRITE, BluetoothGattCharacteristic.PERMISSION_WRITE))
    s.addService(svc)
    server = s
  }

  private val serverCallback = object : BluetoothGattServerCallback() {
    override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
      if (newState == BluetoothProfile.STATE_DISCONNECTED) { incoming.remove(device.address); incomingLen.remove(device.address) }
    }
    override fun onCharacteristicReadRequest(device: BluetoothDevice, requestId: Int, offset: Int, characteristic: BluetoothGattCharacteristic) {
      val v = if (characteristic.uuid == HELLO) hello else ByteArray(0)
      val part = if (offset < v.size) v.copyOfRange(offset, v.size) else ByteArray(0)
      server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, part)
    }
    override fun onCharacteristicWriteRequest(device: BluetoothDevice, requestId: Int, characteristic: BluetoothGattCharacteristic, preparedWrite: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray?) {
      if (responseNeeded) server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, null)
      if (characteristic.uuid == DATA && value != null) take(device.address, value)
    }
  }

  private fun take(from: String, chunk: ByteArray) {
    var buf = incoming[from]
    var bytes = chunk
    if (buf == null) {
      if (chunk.size < 4) return
      val len = ((chunk[0].toInt() and 0xff) shl 24) or ((chunk[1].toInt() and 0xff) shl 16) or ((chunk[2].toInt() and 0xff) shl 8) or (chunk[3].toInt() and 0xff)
      if (len <= 0 || len > MAX_MESSAGE) return
      buf = ByteArrayOutputStream()
      incoming[from] = buf
      incomingLen[from] = len
      bytes = chunk.copyOfRange(4, chunk.size)
    }
    buf.write(bytes)
    val len = incomingLen[from] ?: return
    if (buf.size() >= len) {
      val text = String(buf.toByteArray(), 0, len, Charsets.UTF_8)
      incoming.remove(from)
      incomingLen.remove(from)
      emit(JSONObject().put("type", "data").put("peer", from).put("data", text))
    }
  }

  private val advertiseCallback = object : AdvertiseCallback() {
    override fun onStartFailure(errorCode: Int) { state(true, "Others can't see this phone right now (Bluetooth error $errorCode).") }
  }
  private fun advertise(adapter: BluetoothAdapter) {
    val a = adapter.bluetoothLeAdvertiser ?: return
    val settings = AdvertiseSettings.Builder()
      .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
      .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH)
      .setConnectable(true)
      .build()
    val data = AdvertiseData.Builder().addServiceUuid(ParcelUuid(SERVICE)).setIncludeDeviceName(false).build()
    a.startAdvertising(settings, data, advertiseCallback)
    advertiser = a
  }

  // ---------- finding others: the client ----------
  private val scanCallback = object : ScanCallback() {
    override fun onScanResult(callbackType: Int, result: ScanResult) { main.post { found(result.device) } }
    override fun onBatchScanResults(results: MutableList<ScanResult>) { main.post { results.forEach { found(it.device) } } }
    override fun onScanFailed(errorCode: Int) { state(true, "Looking for others failed (Bluetooth error $errorCode).") }
  }
  private fun scan(adapter: BluetoothAdapter) {
    val s = adapter.bluetoothLeScanner ?: return
    val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE)).build()
    val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
    s.startScan(listOf(filter), settings, scanCallback)
    scanner = s
  }

  private fun found(d: BluetoothDevice) {
    if (!running || peers.containsKey(d.address)) return
    val p = Peer(d)
    peers[d.address] = p
    p.gatt = d.connectGatt(activity, false, client(p), BluetoothDevice.TRANSPORT_LE)
  }

  private fun client(p: Peer) = object : BluetoothGattCallback() {
    override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
      if (newState == BluetoothProfile.STATE_CONNECTED) main.post { if (!gatt.requestMtu(517)) gatt.discoverServices() }
      else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
        try { gatt.close() } catch (e: Exception) {}
        if (peers[p.device.address] === p) peers.remove(p.device.address)
        if (p.ready) emit(JSONObject().put("type", "lost").put("peer", p.device.address))
      }
    }
    override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
      if (status == BluetoothGatt.GATT_SUCCESS) p.mtu = mtu
      main.post { gatt.discoverServices() }
    }
    override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
      val ch = gatt.getService(SERVICE)?.getCharacteristic(HELLO)
      if (ch == null) { gatt.disconnect(); return }
      main.post { gatt.readCharacteristic(ch) }
    }
    @Deprecated("for Android 12 and older")
    @Suppress("DEPRECATION")
    override fun onCharacteristicRead(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
      readDone(characteristic.value ?: ByteArray(0), status)
    }
    override fun onCharacteristicRead(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray, status: Int) {
      readDone(value, status)
    }
    private fun readDone(value: ByteArray, status: Int) {
      if (p.ready || status != BluetoothGatt.GATT_SUCCESS) return
      p.ready = true
      emit(JSONObject().put("type", "found").put("peer", p.device.address).put("hello", String(value, Charsets.UTF_8)))
    }
    override fun onCharacteristicWrite(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
      main.post {
        p.writing = false
        if (status != BluetoothGatt.GATT_SUCCESS) { p.queue.clear(); state(true, "Sending didn't finish: try again closer.") }
        pump(p)
      }
    }
  }

  // ---------- sending ----------
  private fun sendTo(peer: String, data: String) {
    val p = peers[peer]
    if (p == null || !p.ready) { state(running, "They're out of reach now."); return }
    val bytes = data.toByteArray(Charsets.UTF_8)
    val framed = ByteArray(4 + bytes.size)
    framed[0] = (bytes.size ushr 24).toByte(); framed[1] = (bytes.size ushr 16).toByte(); framed[2] = (bytes.size ushr 8).toByte(); framed[3] = bytes.size.toByte()
    System.arraycopy(bytes, 0, framed, 4, bytes.size)
    val size = maxOf(20, p.mtu - 3)
    var i = 0
    while (i < framed.size) { p.queue.add(framed.copyOfRange(i, minOf(framed.size, i + size))); i += size }
    pump(p)
  }

  private fun pump(p: Peer) {
    if (p.writing) return
    val chunk = p.queue.poll()
    if (chunk == null) { emit(JSONObject().put("type", "sent").put("peer", p.device.address)); return }
    val g = p.gatt ?: return
    val ch = g.getService(SERVICE)?.getCharacteristic(DATA) ?: return
    p.writing = true
    val ok = if (Build.VERSION.SDK_INT >= 33) {
      g.writeCharacteristic(ch, chunk, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == android.bluetooth.BluetoothStatusCodes.SUCCESS
    } else {
      ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
      legacyWrite(g, ch, chunk)
    }
    // (the radio was busy: the same piece again in a moment)
    if (!ok) { p.writing = false; p.queue.addFirst(chunk); main.postDelayed({ pump(p) }, 60) }
  }

  @Suppress("DEPRECATION")
  private fun legacyWrite(g: BluetoothGatt, ch: BluetoothGattCharacteristic, chunk: ByteArray): Boolean {
    ch.value = chunk
    return g.writeCharacteristic(ch)
  }
}
