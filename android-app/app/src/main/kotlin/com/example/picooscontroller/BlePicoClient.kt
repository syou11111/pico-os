package com.example.picooscontroller

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import java.util.UUID

@SuppressLint("MissingPermission")
class BlePicoClient(
  private val context: Context,
  private val onStatusChanged: (connected: Boolean, label: String) -> Unit,
  private val onDataReceived: (String) -> Unit
) {
  companion object {
    private const val TAG = "PicoOS-BLE"
    private const val TARGET_NAME = "PicoOS-BLE"
    private val NUS_SERVICE_UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    private val NUS_RX_UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
    private val NUS_TX_UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")
    private val CCCD_UUID = UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")
  }

  private val mainHandler = Handler(Looper.getMainLooper())
  private val txLock = Any()
  private val txQueue = ArrayDeque<ByteArray>()

  @Volatile private var active = false
  @Volatile private var scanning = false
  @Volatile private var connecting = false
  @Volatile private var connected = false
  @Volatile private var writing = false
  @Volatile private var servicesRequested = false
  @Volatile private var mtuPayload = 20

  private var scanner: BluetoothLeScanner? = null
  private var gatt: BluetoothGatt? = null
  private var rxChar: BluetoothGattCharacteristic? = null
  private var txChar: BluetoothGattCharacteristic? = null

  private val retryScanRunnable = Runnable {
    if (active && !connected && !connecting) {
      beginScan()
    }
  }

  private val discoverFallbackRunnable = Runnable {
    val g = gatt
    if (active && g != null && !servicesRequested) {
      servicesRequested = true
      g.discoverServices()
    }
  }

  fun start() {
    if (active) return
    active = true
    beginScan()
  }

  fun restart() {
    stop()
    active = true
    beginScan()
  }

  fun stop() {
    active = false
    mainHandler.removeCallbacks(retryScanRunnable)
    mainHandler.removeCallbacks(discoverFallbackRunnable)
    stopScanInternal()
    closeGattInternal()
    synchronized(txLock) {
      txQueue.clear()
      writing = false
    }
  }

  fun sendBytes(bytes: ByteArray) {
    if (bytes.isEmpty()) return
    val chunkLimit = mtuPayload.coerceIn(20, 180)
    synchronized(txLock) {
      var offset = 0
      while (offset < bytes.size) {
        val end = minOf(offset + chunkLimit, bytes.size)
        txQueue.addLast(bytes.copyOfRange(offset, end))
        offset = end
      }
      if (txQueue.size > 256) {
        while (txQueue.size > 256) txQueue.removeFirst()
      }
    }
    pumpNextWrite()
  }

  private fun hasBlePermissions(): Boolean {
    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
      context.checkSelfPermission(android.Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED &&
        context.checkSelfPermission(android.Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
    } else {
      context.checkSelfPermission(android.Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
    }
  }

  private fun bluetoothAdapter(): BluetoothAdapter? {
    val bm = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    return bm?.adapter
  }

  private fun beginScan() {
    if (!active) return
    if (!hasBlePermissions()) {
      postStatus(false, "Bluetooth権限待ち")
      return
    }
    val adapter = bluetoothAdapter()
    if (adapter == null || !adapter.isEnabled) {
      postStatus(false, "BluetoothをONにしてください")
      mainHandler.postDelayed(retryScanRunnable, 2000L)
      return
    }
    val leScanner = adapter.bluetoothLeScanner
    if (leScanner == null) {
      postStatus(false, "BLE利用不可")
      return
    }
    scanner = leScanner
    stopScanInternal()
    try {
      val settings = ScanSettings.Builder()
        .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
        .build()
      scanning = true
      postStatus(false, "BLEスキャン中...")
      leScanner.startScan(null, settings, scanCallback)
    } catch (t: Throwable) {
      Log.w(TAG, "startScan failed: ${t.message}")
      scanning = false
      postStatus(false, "BLEスキャン失敗")
      mainHandler.postDelayed(retryScanRunnable, 2000L)
    }
  }

  private fun stopScanInternal() {
    if (!scanning) return
    scanning = false
    try {
      scanner?.stopScan(scanCallback)
    } catch (_: Throwable) {}
  }

  private fun closeGattInternal() {
    connected = false
    connecting = false
    servicesRequested = false
    rxChar = null
    txChar = null
    mtuPayload = 20
    try {
      gatt?.disconnect()
    } catch (_: Throwable) {}
    try {
      gatt?.close()
    } catch (_: Throwable) {}
    gatt = null
  }

  private fun scheduleRetryScan() {
    mainHandler.removeCallbacks(retryScanRunnable)
    if (active) {
      postStatus(false, "BLE再接続待機中...")
      mainHandler.postDelayed(retryScanRunnable, 1500L)
    }
  }

  private val scanCallback = object : ScanCallback() {
    override fun onScanResult(callbackType: Int, result: ScanResult) {
      if (!active || connecting || connected) return
      val device = result.device
      val devName = try {
        device.name ?: result.scanRecord?.deviceName
      } catch (_: SecurityException) {
        result.scanRecord?.deviceName
      }
      val hasNusUuid = result.scanRecord?.serviceUuids?.contains(ParcelUuid(NUS_SERVICE_UUID)) == true
      if (devName == TARGET_NAME || hasNusUuid) {
        Log.i(TAG, "Found $devName (${device.address})")
        stopScanInternal()
        connectToDevice(device)
      }
    }

    override fun onScanFailed(errorCode: Int) {
      Log.w(TAG, "onScanFailed: $errorCode")
      scanning = false
      scheduleRetryScan()
    }
  }

  private fun connectToDevice(device: BluetoothDevice) {
    if (!active || connecting) return
    connecting = true
    servicesRequested = false
    postStatus(false, "BLE接続処理中...")
    try {
      gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    } catch (t: Throwable) {
      Log.w(TAG, "connectGatt failed: ${t.message}")
      closeGattInternal()
      scheduleRetryScan()
    }
  }

  private val gattCallback = object : BluetoothGattCallback() {
    override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
      if (status != BluetoothGatt.GATT_SUCCESS) {
        Log.w(TAG, "Connection error status=$status newState=$newState")
        mainHandler.removeCallbacks(discoverFallbackRunnable)
        closeGattInternal()
        scheduleRetryScan()
        return
      }
      if (newState == BluetoothProfile.STATE_CONNECTED) {
        connecting = false
        servicesRequested = false
        try {
          g.requestMtu(247)
        } catch (_: Throwable) {}
        mainHandler.removeCallbacks(discoverFallbackRunnable)
        mainHandler.postDelayed(discoverFallbackRunnable, 600L)
      } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
        mainHandler.removeCallbacks(discoverFallbackRunnable)
        closeGattInternal()
        scheduleRetryScan()
      }
    }

    override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
      if (status == BluetoothGatt.GATT_SUCCESS && mtu > 23) {
        mtuPayload = (mtu - 3).coerceIn(20, 180)
      }
      mainHandler.removeCallbacks(discoverFallbackRunnable)
      if (!servicesRequested) {
        servicesRequested = true
        g.discoverServices()
      }
    }

    override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
      if (status != BluetoothGatt.GATT_SUCCESS) {
        closeGattInternal()
        scheduleRetryScan()
        return
      }
      val service = g.getService(NUS_SERVICE_UUID)
      val rx = service?.getCharacteristic(NUS_RX_UUID)
      val tx = service?.getCharacteristic(NUS_TX_UUID)
      if (service == null || rx == null || tx == null) {
        Log.w(TAG, "NUS service/characteristics not found")
        closeGattInternal()
        scheduleRetryScan()
        return
      }
      rxChar = rx
      txChar = tx
      g.setCharacteristicNotification(tx, true)
      val cccd = tx.getDescriptor(CCCD_UUID)
      if (cccd != null) {
        val ok = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
          g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) == BluetoothGatt.GATT_SUCCESS || true
        } else {
          @Suppress("DEPRECATION")
          run {
            cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            g.writeDescriptor(cccd)
          }
        }
        if (!ok) {
          Log.w(TAG, "writeDescriptor returned false")
        }
      } else {
        connected = true
        postStatus(true, "BLE接続中 (PicoOS-BLE)")
        pumpNextWrite()
      }
    }

    override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
      if (descriptor.uuid == CCCD_UUID) {
        connected = (status == BluetoothGatt.GATT_SUCCESS)
        if (connected) {
          Log.i(TAG, "BLE Connected & Notifications enabled")
          postStatus(true, "BLE接続中 (PicoOS-BLE)")
          pumpNextWrite()
        } else {
          closeGattInternal()
          scheduleRetryScan()
        }
      }
    }

    override fun onCharacteristicChanged(
      g: BluetoothGatt,
      characteristic: BluetoothGattCharacteristic,
      value: ByteArray
    ) {
      if (characteristic.uuid == NUS_TX_UUID && value.isNotEmpty()) {
        val text = String(value, Charsets.UTF_8)
        mainHandler.post { onDataReceived(text) }
      }
    }

    @Deprecated("Deprecated in Java")
    @Suppress("DEPRECATION")
    override fun onCharacteristicChanged(
      g: BluetoothGatt,
      characteristic: BluetoothGattCharacteristic
    ) {
      if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU && characteristic.uuid == NUS_TX_UUID) {
        val value = characteristic.value ?: return
        if (value.isNotEmpty()) {
          val text = String(value, Charsets.UTF_8)
          mainHandler.post { onDataReceived(text) }
        }
      }
    }

    override fun onCharacteristicWrite(
      g: BluetoothGatt,
      characteristic: BluetoothGattCharacteristic,
      status: Int
    ) {
      synchronized(txLock) {
        writing = false
      }
      pumpNextWrite()
    }
  }

  private fun pumpNextWrite() {
    val g = gatt ?: return
    val rx = rxChar ?: return
    if (!connected) return

    val chunk: ByteArray = synchronized(txLock) {
      if (writing || txQueue.isEmpty()) return
      writing = true
      txQueue.removeFirst()
    }

    val initiated = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      g.writeCharacteristic(
        rx,
        chunk,
        BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
      ) == BluetoothGatt.GATT_SUCCESS
    } else {
      @Suppress("DEPRECATION")
      run {
        rx.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        rx.value = chunk
        g.writeCharacteristic(rx)
      }
    }

    if (!initiated) {
      synchronized(txLock) {
        writing = false
      }
    }
  }

  private fun postStatus(ok: Boolean, label: String) {
    mainHandler.post { onStatusChanged(ok, label) }
  }
}
