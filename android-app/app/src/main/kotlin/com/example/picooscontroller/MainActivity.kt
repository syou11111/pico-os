package com.example.picooscontroller

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.picooscontroller.ui.theme.PicoOSControllerTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import java.net.HttpURLConnection
import java.net.URL

class MainActivity : ComponentActivity() {
  private var connectivityManager: ConnectivityManager? = null
  private var wifiCallback: ConnectivityManager.NetworkCallback? = null

  @Volatile
  private var wifiNetwork: Network? = null

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    bindToWifiNetwork()
    enableEdgeToEdge()
    setContent {
      PicoOSControllerTheme {
        Surface(
          modifier = Modifier.fillMaxSize(),
          color = Color(0xFF0D1117)
        ) {
          PicoControllerScreen(wifiNetworkProvider = { wifiNetwork })
        }
      }
    }
  }

  private fun bindToWifiNetwork() {
    val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    connectivityManager = cm
    val req = NetworkRequest.Builder()
      .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
      .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
      .build()
    val cb = object : ConnectivityManager.NetworkCallback() {
      override fun onAvailable(network: Network) {
        wifiNetwork = network
        try {
          cm.bindProcessToNetwork(network)
        } catch (_: Throwable) {}
      }

      override fun onLost(network: Network) {
        if (wifiNetwork == network) {
          wifiNetwork = null
          try {
            cm.bindProcessToNetwork(null)
          } catch (_: Throwable) {}
        }
      }
    }
    wifiCallback = cb
    try {
      cm.requestNetwork(req, cb)
    } catch (_: Throwable) {
      try {
        cm.registerNetworkCallback(req, cb)
      } catch (_: Throwable) {}
    }
  }

  override fun onDestroy() {
    wifiCallback?.let {
      try {
        connectivityManager?.unregisterNetworkCallback(it)
      } catch (_: Throwable) {}
    }
    try {
      connectivityManager?.bindProcessToNetwork(null)
    } catch (_: Throwable) {}
    super.onDestroy()
  }
}

private val ANSI_REGEX = Regex("\u001B\\[[0-9;?]*[A-Za-z]")

@Composable
fun PicoControllerScreen(wifiNetworkProvider: () -> Network? = { null }) {
  var baseUrl by remember { mutableStateOf("http://192.168.4.1") }
  var isConnected by remember { mutableStateOf(false) }
  var terminalText by remember { mutableStateOf("PicoOS-AP (pass: picoos123) にWi-Fi接続してください...\n") }
  var liveInput by remember { mutableStateOf("") }
  var cmdInput by remember { mutableStateOf("") }

  val txLock = remember { Any() }
  val txQueue = remember { StringBuilder() }
  var seq by remember { mutableLongStateOf(0L) }
  val scrollState = rememberScrollState()

  fun queueHex(hex: String) {
    synchronized(txLock) {
      txQueue.append(hex)
    }
  }

  fun queueText(text: String) {
    val bytes = text.toByteArray(Charsets.UTF_8)
    val sb = StringBuilder(bytes.size * 2)
    for (b in bytes) {
      sb.append(String.format("%02X", b.toInt() and 0xFF))
    }
    queueHex(sb.toString())
  }

  LaunchedEffect(terminalText) {
    scrollState.animateScrollTo(scrollState.maxValue)
  }

  LaunchedEffect(baseUrl) {
    withContext(Dispatchers.IO) {
      while (isActive) {
        val d = synchronized(txLock) {
          val s = txQueue.toString()
          txQueue.setLength(0)
          s
        }
        try {
          val url = URL("${baseUrl.trimEnd('/')}/k?s=$seq&d=$d")
          val net = wifiNetworkProvider()
          val conn = ((net?.openConnection(url) ?: url.openConnection()) as HttpURLConnection).apply {
            connectTimeout = 1500
            readTimeout = 1500
            requestMethod = "GET"
          }
          val bodyText = conn.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
          conn.disconnect()

          val nl = bodyText.indexOf('\n')
          if (nl >= 0) {
            bodyText.substring(0, nl).trim().toLongOrNull()?.let { seq = it }
            val clean = bodyText.substring(nl + 1)
              .replace(ANSI_REGEX, "")
              .replace("\r", "")
            if (clean.isNotEmpty()) {
              withContext(Dispatchers.Main) {
                val updated = terminalText + clean
                terminalText = if (updated.length > 4000) updated.takeLast(3500) else updated
              }
            }
          }
          if (!isConnected) {
            withContext(Dispatchers.Main) { isConnected = true }
          }
        } catch (_: Exception) {
          if (isConnected) {
            withContext(Dispatchers.Main) { isConnected = false }
          }
        }
        delay(if (d.isEmpty()) 180L else 40L)
      }
    }
  }

  Column(
    modifier = Modifier
      .fillMaxSize()
      .windowInsetsPadding(WindowInsets.safeDrawing)
      .padding(10.dp),
    verticalArrangement = Arrangement.spacedBy(8.dp)
  ) {
    // Header
    Row(
      modifier = Modifier.fillMaxWidth(),
      verticalAlignment = Alignment.CenterVertically,
      horizontalArrangement = Arrangement.SpaceBetween
    ) {
      Text(
        text = "PicoOS Controller",
        color = Color(0xFF38BDF8),
        fontSize = 18.sp,
        fontWeight = FontWeight.Bold
      )
      Box(
        modifier = Modifier
          .background(
            if (isConnected) Color(0xFF14532D) else Color(0xFF451A1A),
            RoundedCornerShape(12.dp)
          )
          .padding(horizontal = 10.dp, vertical = 4.dp)
      ) {
        Text(
          text = if (isConnected) "接続中 (192.168.4.1)" else "未接続",
          color = if (isConnected) Color(0xFF86EFAC) else Color(0xFFFCA5A5),
          fontSize = 12.sp,
          fontWeight = FontWeight.SemiBold
        )
      }
    }

    // Terminal output box
    Box(
      modifier = Modifier
        .fillMaxWidth()
        .weight(1f)
        .background(Color(0xFF05080F), RoundedCornerShape(8.dp))
        .border(1.dp, Color(0xFF30363D), RoundedCornerShape(8.dp))
        .padding(8.dp)
        .verticalScroll(scrollState)
    ) {
      Text(
        text = terminalText,
        color = Color(0xFFE2E8F0),
        fontFamily = FontFamily.Monospace,
        fontSize = 12.sp,
        lineHeight = 16.sp
      )
    }

    // Live 1-char input + Command input
    Row(
      modifier = Modifier.fillMaxWidth(),
      horizontalArrangement = Arrangement.spacedBy(6.dp),
      verticalAlignment = Alignment.CenterVertically
    ) {
      OutlinedTextField(
        value = liveInput,
        onValueChange = { v ->
          if (v.isNotEmpty()) {
            queueText(v)
            liveInput = ""
          }
        },
        placeholder = { Text("ライブ入力 (即送信)", fontSize = 12.sp) },
        singleLine = true,
        modifier = Modifier.weight(1f),
        colors = OutlinedTextFieldDefaults.colors(
          focusedTextColor = Color.White,
          unfocusedTextColor = Color.White,
          focusedBorderColor = Color(0xFF38BDF8),
          unfocusedBorderColor = Color(0xFF30363D)
        )
      )
    }

    Row(
      modifier = Modifier.fillMaxWidth(),
      horizontalArrangement = Arrangement.spacedBy(6.dp),
      verticalAlignment = Alignment.CenterVertically
    ) {
      OutlinedTextField(
        value = cmdInput,
        onValueChange = { cmdInput = it },
        placeholder = { Text("コマンド送信 (例: cc -r game.c)", fontSize = 12.sp) },
        singleLine = true,
        modifier = Modifier.weight(1f),
        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
        keyboardActions = KeyboardActions(onSend = {
          queueText(cmdInput + "\r")
          cmdInput = ""
        }),
        colors = OutlinedTextFieldDefaults.colors(
          focusedTextColor = Color.White,
          unfocusedTextColor = Color.White,
          focusedBorderColor = Color(0xFF38BDF8),
          unfocusedBorderColor = Color(0xFF30363D)
        )
      )
      Button(
        onClick = {
          queueText(cmdInput + "\r")
          cmdInput = ""
        },
        colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF0284C7))
      ) {
        Text("送信")
      }
    }

    // Hotkeys
    Row(
      modifier = Modifier
        .fillMaxWidth()
        .horizontalScroll(rememberScrollState()),
      horizontalArrangement = Arrangement.spacedBy(6.dp)
    ) {
      CtrlBtn("ESC", Color(0xFF21262D)) { queueHex("1B") }
      CtrlBtn("TAB", Color(0xFF21262D)) { queueHex("09") }
      CtrlBtn("BS", Color(0xFF21262D)) { queueHex("7F") }
      CtrlBtn("ENTER", Color(0xFF0284C7)) { queueHex("0D") }
      CtrlBtn("Ctrl+C", Color(0xFF991B1B)) { queueHex("03") }
      CtrlBtn(":w", Color(0xFF21262D)) { queueText(":w\r") }
      CtrlBtn(":wq", Color(0xFF21262D)) { queueText(":wq\r") }
      CtrlBtn(":q!", Color(0xFF21262D)) { queueText(":q!\r") }
    }

    // D-Pad + Quick Commands
    Row(
      modifier = Modifier.fillMaxWidth(),
      verticalAlignment = Alignment.CenterVertically,
      horizontalArrangement = Arrangement.SpaceBetween
    ) {
      // D-Pad
      Column(horizontalAlignment = Alignment.CenterHorizontally) {
        CtrlBtn("▲", Color(0xFF21262D)) { queueHex("1B5B41") }
        Spacer(Modifier.height(4.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
          CtrlBtn("◀", Color(0xFF21262D)) { queueHex("1B5B44") }
          CtrlBtn("▼", Color(0xFF21262D)) { queueHex("1B5B42") }
          CtrlBtn("▶", Color(0xFF21262D)) { queueHex("1B5B43") }
        }
      }

      // Quick command grid
      Column(
        modifier = Modifier.weight(1f).padding(start = 10.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp)
      ) {
        Row(
          modifier = Modifier.horizontalScroll(rememberScrollState()),
          horizontalArrangement = Arrangement.spacedBy(4.dp)
        ) {
          CtrlBtn("ps", Color(0xFF21262D)) { queueText("ps\r") }
          CtrlBtn("ls", Color(0xFF21262D)) { queueText("ls\r") }
          CtrlBtn("pinout", Color(0xFF21262D)) { queueText("pinout\r") }
          CtrlBtn("clear", Color(0xFF21262D)) { queueText("clear\r") }
        }
        Row(
          modifier = Modifier.horizontalScroll(rememberScrollState()),
          horizontalArrangement = Arrangement.spacedBy(4.dp)
        ) {
          CtrlBtn("sensor.c", Color(0xFF15803D)) { queueText("cc -r sensor.c\r") }
          CtrlBtn("game.c", Color(0xFF15803D)) { queueText("cc -r game.c\r") }
        }
      }
    }
  }
}

@Composable
private fun CtrlBtn(label: String, bg: Color, onClick: () -> Unit) {
  Button(
    onClick = onClick,
    colors = ButtonDefaults.buttonColors(containerColor = bg, contentColor = Color.White),
    contentPadding = PaddingValues(horizontal = 10.dp, vertical = 6.dp),
    shape = RoundedCornerShape(6.dp),
    modifier = Modifier.defaultMinSize(minWidth = 44.dp, minHeight = 36.dp)
  ) {
    Text(label, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
  }
}
