#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h> // Pastikan ini ada!
#include <ESPmDNS.h> // Tambahkan baris ini

#define NUM_CHANNELS 8
#define MAX_CONFIGS 24
const int channelPins[NUM_CHANNELS] = {15, 2, 4, 16, 23, 18, 19, 21};

// NTP Settings
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600); // GMT+7
String currentTime = "00:00:00";
String ntpServer = "pool.ntp.org";
String lastSyncTime = "Never";
// NEW: Variabel global untuk menyimpan time offset dalam detik
int timeOffset = 25200; // Default ke GMT+7 (7 jam * 3600 detik)

// Timer Configuration
struct TimerConfig {
  String onTime;
  String offTime;
  bool enable;
  // NEW: Tambahkan hari aktif (7 boolean untuk Senin-Minggu)
  // [0]=Senin, [1]=Selasa, [2]=Rabu, [3]=Kamis, [4]=Jumat, [5]=Sabtu, [6]=Minggu
  bool activeDays[7]; 
};

struct ChannelConfig {
  TimerConfig configs[MAX_CONFIGS];
  int count = 0;
  bool currentState = false; // Status ON/OFF saat ini
  bool manualOverride = false; // Flag untuk override manual via MQTT/Web
};
ChannelConfig channels[NUM_CHANNELS];

// NEW: Holiday Mode Flag
bool holidayModeActive = false; // Flag untuk Holiday Mode

// Web Server
AsyncWebServer server(80);
WiFiManager wifiManager;

// Variabel Global untuk nama host mDNS (seperti yang disarankan)
const char* mdns_hostname = "xtimer"; // <-- Nama host mDNS Anda

// --- MQTT Configuration ---
// Ganti dengan detail broker MQTT Anda dari HiveMQ Cloud atau EMQX Cloud
const char *mqtt_server = "af2be1d832ff4f46bf19c7297d4b7efd.s1.eu.hivemq.cloud"; // PASTI GANTI DENGAN PUNYA ANDA!
const int mqtt_port = 8883; // Port SSL/TLS untuk HiveMQ Cloud (biasanya 8883)
const char *mqtt_user = "esp32user"; // Ganti dengan username Anda
const char *mqtt_pass = "@Hivemq.cloud1"; // Ganti dengan password Anda

// ID unik untuk perangkat ESP32 ini. Pastikan ini unik jika Anda memiliki beberapa ESP32.
const char *mqtt_client_id = "ESP32_Timer_Device_Utama";

// Topik MQTT untuk komunikasi
// Anda bisa sesuaikan topik ini sesuai keinginan Anda
const char *mqtt_control_topic = "esp32/timer/control"; // Untuk ESP32 menerima perintah
const char *mqtt_status_topic = "esp32/timer/status";   // Untuk ESP32 mengirim status
const char *mqtt_config_topic = "esp32/timer/config";   // Untuk menerima seluruh config

// Deklarasikan sebagai WiFiClientSecure
WiFiClientSecure espClient; 
PubSubClient mqttClient(espClient);


// --- Deklarasi fungsi helper agar bisa dipanggil sebelum didefinisikan ---
String formatTime(int h, int m, int s);
bool isTimeInRange(String current, String start, String end);
int timeToSeconds(String timeStr);
void loadConfig();
void saveConfig();
bool updateNTPTime();
void publishStatus(const String &message); // Deklarasi fungsi MQTT
void initializeChannelDefaults(int channelIndex); // NEW
void initializeAllChannelsDefaults(); // NEW

// Nama-nama AP WiFi
const char* ap_config_ssid = "XTIMER_AP_Setup";      // AP untuk mode konfigurasi
const char* ap_fallback_ssid = "XTIMER_AP_FALLBACK"; // AP jika koneksi gagal

// --- Tambahkan ini di dekat deklarasi variabel global Anda ---

void applyTimezone() {
  // timeOffset sudah dalam detik, tapi configTzTime membutuhkan POSIX timezone string.
  // POSIX string time zone sedikit membingungkan karena tandanya berlawanan dengan GMT.
  // Contoh: GMT+7 (seperti WIB) di POSIX adalah "GMT-7".
  // Jadi, jika timeOffset kita positif, di POSIX akan negatif, dan sebaliknya.

  char tzString[20]; // Cukup besar untuk "GMT+/-HH:MM:SS"
  int offsetHours = timeOffset / 3600;
  int offsetMinutes = (abs(timeOffset) % 3600) / 60;
  int offsetSeconds = abs(timeOffset) % 60;

  // Format string timezone sesuai offset
  if (timeOffset == 0) {
      strcpy(tzString, "GMT0");
  } else {
      // Tentukan tanda untuk POSIX: invers dari tanda GMT (timeOffset kita).
      char sign = (offsetHours > 0) ? '-' : '+';
      if (timeOffset < 0) { // Jika timeOffset negatif, pastikan offsetHours jadi positif untuk format string
          offsetHours = abs(offsetHours);
      }

      // Format string: GMT+/-HH:MM:SS (jika ada menit/detik) atau GMT+/-HH
      if (offsetMinutes == 0 && offsetSeconds == 0) {
          sprintf(tzString, "GMT%c%d", sign, offsetHours);
      } else if (offsetSeconds == 0) {
          sprintf(tzString, "GMT%c%d:%02d", sign, offsetHours, offsetMinutes);
      } else {
          sprintf(tzString, "GMT%c%d:%02d:%02d", sign, offsetHours, offsetMinutes, offsetSeconds);
      }
  }

  // Terapkan timezone baru ke sistem waktu ESP32 dan NTP client
  configTzTime(tzString, ntpServer.c_str());
  Serial.printf("Timezone applied: %s using NTP Server: %s\n", tzString, ntpServer.c_str());

  // Perbarui NTPClient segera untuk mendapatkan waktu dengan offset baru
  timeClient.update();
  lastSyncTime = timeClient.getFormattedTime(); // Perbarui lastSyncTime di variabel global
  Serial.printf("NTP Client updated with new timezone. Current time: %s. Last Sync Time: %s\n", timeClient.getFormattedTime().c_str(), lastSyncTime.c_str()); // Tambah debug ini

  // Setelah timeOffset diperbarui dan NTP Client di-update,
  // picu pengiriman status baru ke MQTT untuk memperbarui UI (jika relevan).
  // publishStatus("Time offset updated and time re-synced."); // Bisa diaktifkan jika perlu notifikasi spesifik
}

// --- Fungsi untuk mengirim status melalui MQTT ---
void publishStatus(const String &message) {
  if (mqttClient.connected()) { // Pastikan terhubung sebelum publish
    StaticJsonDocument<256> doc; // Sesuaikan ukuran jika payload lebih besar
    doc["device_id"] = mqtt_client_id;
    doc["current_time"] = currentTime;
    doc["message"] = message;
    
    // Tambahkan status channel saat ini
    JsonArray channelStatus = doc.createNestedArray("channel_status");
    for (int i = 0; i < NUM_CHANNELS; i++) {
        channelStatus.add(channels[i].currentState);
    }

    String jsonString;
    serializeJson(doc, jsonString);
    mqttClient.publish(mqtt_status_topic, jsonString.c_str());
    Serial.print("MQTT Published: ");
    Serial.println(jsonString);
  } else {
    Serial.println("MQTT client not connected, cannot publish.");
  }
}

// --- Fungsi untuk memproses pesan MQTT yang masuk ---
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  Serial.print("MQTT message received [");
  Serial.print(topic);
  Serial.print("] ");

  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);

  // Jika pesan adalah perintah kontrol
  if (String(topic) == mqtt_control_topic) {
    StaticJsonDocument<512> doc; // Sesuaikan ukuran jika payload lebih besar
    DeserializationError error = deserializeJson(doc, message);
    if (error) {
      Serial.print(F("deserializeJson() failed: "));
      Serial.println(error.c_str());
      publishStatus("Error processing MQTT control message: " + String(error.c_str()));
      return;
    }

    // Perintah untuk mengubah status channel secara manual (Override Timer)
    if (doc.containsKey("channel") && doc.containsKey("state")) {
        int channel = doc["channel"].as<int>();
        bool state = doc["state"].as<bool>();
        if (channel >= 0 && channel < NUM_CHANNELS) {
            // Hanya izinkan perintah ON/OFF jika manual override aktif
            if (!channels[channel].manualOverride) {
                Serial.printf("MQTT Control: CH%d not in manual override. ON/OFF command ignored.\n", channel);
                publishStatus("Manual ON/OFF for CH" + String(channel+1) + " ignored: override not active.");
                return;
            }
            digitalWrite(channelPins[channel], state ? HIGH : LOW);
            channels[channel].currentState = state; // Update status internal
            Serial.printf("MQTT Control: CH%d set to %s (Manual Override)\n", channel, state ? "ON" : "OFF");
            publishStatus("Manual control: CH" + String(channel+1) + " set " + (state ? "ON" : "OFF"));
        }
    }
    
    // Perintah untuk mengaktifkan manual override
    if (doc.containsKey("channel") && doc.containsKey("override_on")) {
        int channel = doc["channel"].as<int>();
        if (channel >= 0 && channel < NUM_CHANNELS) {
            channels[channel].manualOverride = true; // Aktifkan override manual
            Serial.printf("MQTT Control: CH%d manual override activated.\n", channel);
            publishStatus("Manual override for CH" + String(channel+1) + " activated.");
        }
    }

    // Perintah untuk menonaktifkan manual override
    if (doc.containsKey("channel") && doc.containsKey("override_off")) {
        int channel = doc["channel"].as<int>();
        if (channel >= 0 && channel < NUM_CHANNELS) {
            channels[channel].manualOverride = false; // Matikan override manual
            Serial.printf("MQTT Control: CH%d manual override off. Timer will resume.\n", channel);
            publishStatus("Manual override for CH" + String(channel+1) + " turned OFF. Timer will resume.");
        }
    }

    // NEW: Perintah untuk mengaktifkan/menonaktifkan Holiday Mode via MQTT
    if (doc.containsKey("command") && doc["command"].as<String>() == "set_holiday_mode") {
        if (doc.containsKey("active")) {
            holidayModeActive = doc["active"].as<bool>();
            if (holidayModeActive) {
                Serial.println("MQTT Control: Holiday Mode Activated.");
                publishStatus("Global Holiday Mode activated via MQTT.");
                // Opsional: Langsung matikan semua relay saat diaktifkan
                for(int i = 0; i < NUM_CHANNELS; i++) {
                    if (channels[i].currentState) {
                        digitalWrite(channelPins[i], LOW);
                        channels[i].currentState = false;
                    }
                }
            } else {
                Serial.println("MQTT Control: Holiday Mode Deactivated.");
                publishStatus("Global Holiday Mode deactivated via MQTT.");
            }
            saveConfig(); // Simpan perubahan holidayMode
        }
    }

    // Perintah untuk mengubah NTP server atau sync time
    if (doc.containsKey("command")) {
        String command = doc["command"].as<String>();
        if (command == "sync_time") {
            if (doc.containsKey("ntp_server")) {
                ntpServer = doc["ntp_server"].as<String>();
            }
            if (updateNTPTime()) {
                publishStatus("Time synced via MQTT: " + currentTime);
            } else {
                publishStatus("Time sync failed via MQTT");
            }
        }
    }
  }
  // Jika pesan adalah update seluruh konfigurasi
  else if (String(topic) == mqtt_config_topic) {
    StaticJsonDocument<2048> doc; // Ukuran disesuaikan untuk config.json
    DeserializationError error = deserializeJson(doc, message);
    if (error) {
      Serial.print(F("deserializeJson() for config failed: "));
      Serial.println(error.c_str());
      publishStatus("Error processing MQTT config message: " + String(error.c_str()));
      return;
    }

    // Update ntpServer
    if (doc.containsKey("ntpServer")) {
        ntpServer = doc["ntpServer"].as<String>();
    }

    // NEW: Update holidayModeActive
    if (doc.containsKey("holidayModeActive")) {
        holidayModeActive = doc["holidayModeActive"].as<bool>();
    }
    
    // Update konfigurasi channels
    if (doc.containsKey("channels")) {
        JsonArray channelsJson = doc["channels"].as<JsonArray>();
        for (int i = 0; i < NUM_CHANNELS && i < channelsJson.size(); i++) {
            channels[i].count = 0;
            JsonArray configs = channelsJson[i];
            for (JsonObject cfg : configs) {
                if (channels[i].count < MAX_CONFIGS) {
                    channels[i].configs[channels[i].count] = {
                        cfg["onTime"].as<String>(),
                        cfg["offTime"].as<String>(),
                        cfg["enable"]
                    };
                    // NEW: Baca activeDays dari MQTT config
                    JsonArray activeDaysJson = cfg["activeDays"];
                    if (activeDaysJson && activeDaysJson.size() == 7) {
                        for (int k = 0; k < 7; k++) {
                            channels[i].configs[channels[i].count].activeDays[k] = activeDaysJson[k].as<bool>();
                        }
                    } else {
                        // Default: Semua hari aktif jika tidak ada
                        for (int k = 0; k < 7; k++) {
                            channels[i].configs[channels[i].count].activeDays[k] = true; 
                        }
                    }
                    channels[i].count++;
                }
            }
        }
        saveConfig(); // Simpan perubahan ke LittleFS
        publishStatus("Configuration updated via MQTT");
    }
  }
}

// --- Fungsi untuk menghubungkan kembali ke MQTT ---
void reconnectMqtt() {
  // Loop sampai terhubung kembali
  while (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Coba sambungkan dengan ID unik, username, dan password
    if (mqttClient.connect(mqtt_client_id, mqtt_user, mqtt_pass)) {
      Serial.println("connected");
      // Setelah terhubung, berlangganan ke topik kontrol dan topik konfigurasi
      mqttClient.subscribe(mqtt_control_topic);
      Serial.print("Subscribed to: ");
      Serial.println(mqtt_control_topic);
      mqttClient.subscribe(mqtt_config_topic);
      Serial.print("Subscribed to: ");
      Serial.println(mqtt_config_topic);
      // Kirim pesan status awal setelah koneksi berhasil
      publishStatus("ESP32 is online and connected to MQTT.");
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 5 seconds");
      // Tunggu 5 detik sebelum mencoba lagi
      delay(5000);
    }
  }
}

// Bagian HTML Web UI (PROGMEM)
const char htmlPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Timer Controller</title>
  <style>
    body {
      font-family: Arial, sans-serif;
      margin: 0;
      padding: 0;
      background-color: #f5f5f5;
    }
    .container {
      max-width: 1000px;
      margin: 0 auto;
      background: white;
      padding: 0;
      border-radius: 8px;
      box-shadow: 0 2px 10px rgba(0,0,0,0.1);
      overflow: hidden;
    }
    .header {
      padding: 15px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      background: #4CAF50;
      color: white;
    }
    .time-display {
      display: flex;
      gap: 15px;
      align-items: center;
    }
    .time-box {
      background: rgba(0,0,0,0.2);
      padding: 5px 10px;
      border-radius: 4px;
      font-family: monospace;
      font-size: 1.2rem;
      font-weight: bold;
    }
    button {
      padding: 8px 12px;
      border: none;
      border-radius: 4px;
      cursor: pointer;
      background: #e0e0e0;
      transition: all 0.3s;
    }
    button:hover {
      opacity: 0.8;
    }
    #syncTimeBtn { background: #4CAF50; color: white; }
    .save-btn { background: #2196F3; color: white; }
    .wifi-btn { background: #FF9800; color: white; }
    #holidayModeBtn { /* NEW: Style for Holiday Mode button */
      background: #FF5722; /* Warna oranye untuk OFF */
      color: white;
      min-width: 110px; /* Tambahkan min-width agar teks "Holiday: OFF" pas */
	  padding: 8px 10px; /* Sesuaikan padding */
    }
    #holidayModeBtn.active { /* NEW: Style for active Holiday Mode */
      background: #8BC34A; /* Warna hijau untuk ON */
    }
    .control-row {
      display: flex;
      gap: 8px;
      align-items: center;
      padding: 5px 15px;
      padding-top: 20px;
      background: white;
      overflow-x: auto;
    }
    table {
      width: 100%;
      border-collapse: collapse;
    }
    th, td {
      padding: 10px;
      border: 1px solid #ddd;
      text-align: center;
    }
    th {
      background: #f1f1f1;
      position: sticky;
      top: 0;
      z-index: 2;
    }
    .tab-button {
      padding: 10px 15px;
      background: #f1f1f1;
      border: none;
      cursor: pointer;
      border-radius: 4px 4px 0 0;
      margin-right: 5px;
      min-width: 60px;
    }
    .tab-button.active {
      background: #4CAF50;
      color: white;
    }
    .tab-content {
      max-height: calc(100vh - 220px);
      overflow-y: auto;
    }
    .notification {
      position: fixed;
      bottom: 20px;
      right: 20px;
      background: #4CAF50;
      color: white;
      padding: 15px;
      border-radius: 5px;
      display: none;
      z-index: 100;
    }
    .error { background: #f44336; }
    .modal {
      display: none;
      position: fixed;
      z-index: 100;
      left: 0;
      top: 0;
      width: 100%;
      height: 100%;
      background-color: rgba(0,0,0,0.4);
    }
    .modal-content {
      background-color: #fefefe;
      margin: 15% auto;
      padding: 20px;
      border: 1px solid #888;
      width: 80%;
      max-width: 500px;
      border-radius: 5px;
    }
    .close {
      color: #aaa;
      float: right;
      font-size: 28px;
      font-weight: bold;
      cursor: pointer;
    }
    .add-btn {
      background: #4CAF50;
      color: white;
      padding: 5px 10px !important;
      border-radius: 4px;
    }
    [data-tooltip] {
      position: relative;
    }
    /* Tooltip Fix */
    [data-tooltip]::after {
      content: attr(data-tooltip);
      position: absolute;
      bottom: 100%;
      left: 50%;
      transform: translateX(-50%);
      background: #333;
      color: white;
      padding: 4px 8px;
      border-radius: 4px;
      font-size: 12px;
      white-space: nowrap;
      opacity: 0;
      transition: opacity 0.3s;
      pointer-events: none;
      z-index: 10; /* Tingkatkan z-index agar selalu di atas */
    }
    [data-tooltip]:hover::after {
      opacity: 1;
    }
    /* Time Input Fix */
    input.time-input {
      border: 1px solid #ddd;
      padding: 6px;
      border-radius: 4px;
      font-family: monospace;
      text-align: center;
      min-width: 85px;
    }
    /* Manual Control Buttons & Status */
    .manual-control-btn {
      padding: 6px 10px;
      margin: 0 3px; /* Kurangi margin untuk efisiensi ruang */
      border: none; /* Hapus border */
      border-radius: 4px;
      cursor: pointer;
      font-size: 0.9em;
      transition: all 0.2s ease-in-out;
      outline: none; /* Hapus outline saat focus */
    }
    .manual-control-btn.on-btn { background: #4CAF50; }
    .manual-control-btn.off-btn { background: #f44336; }
    .manual-control-btn.override-on-btn { background: #17a2b8; } /* Warna teal/info */
    .manual-control-btn.override-off-btn { background: #607D8B; }

    /* Untuk tombol yang disable */
    .manual-control-btn[disabled] {
      background-color: #cccccc !important;
      cursor: not-allowed;
      opacity: 0.7;
      box-shadow: none;
    }
    .manual-control-btn[disabled]:hover {
      opacity: 0.7; /* Jangan ada perubahan opacity saat hover disable */
    }


    .status-indicator {
      font-weight: bold;
      margin-right: 10px;
      color: #333;
    }
    .status-indicator.on { color: #4CAF50; }
    .status-indicator.off { color: #f44336; }
    .status-indicator.override { 
        color: #FF9800; /* Warna oranye untuk override aktif */
        text-shadow: 0 0 2px rgba(255, 152, 0, 0.5); /* Efek glow ringan */
    } 

    /* NEW: Styling for day checkboxes */
    .day-checkboxes {
      display: flex;
      flex-wrap: wrap; 
      justify-content: center; 
      gap: 5px; 
    }
    .day-checkboxes label {
      display: flex; 
      align-items: center;
      gap: 2px; 
      font-size: 0.85em; 
      cursor: pointer;
    }
    .day-checkboxes input[type="checkbox"] {
      margin: 0; 
    }

    /* Sesuaikan lebar kolom untuk mengakomodasi kolom hari */
    table th:nth-child(1), table td:nth-child(1), /* ON Time */
    table th:nth-child(2), table td:nth-child(2) { /* OFF Time */
        width: 15%; 
    }
    table th:nth-child(3), table td:nth-child(3) { /* Enable */
        width: 8%;
    }
    table th:nth-child(4), table td:nth-child(4) { /* Active Days */
        width: 45%; 
    }
    table th:nth-child(5), table td:nth-child(5) { /* Action */
        width: 12%;
    }

    /* Mobile Optimization */
    @media screen and (max-width: 600px) {
      .header {
        flex-direction: column;
        gap: 10px;
        text-align: center;
      }
      .control-row {
        position: sticky;
        bottom: 0;
        z-index: 10;
        border-top: 1px solid #ddd;
      }
      #channelTabs {
        position: sticky;
        top: 0;
        background: white;
        z-index: 10;
        padding: 5px 0;
        border-bottom: 1px solid #ddd;
        overflow-x: auto;
        white-space: nowrap;
      }
      .tab-button {
        display: inline-block;
        margin: 0 2px;
        padding: 8px 12px;
      }
      th, td {
        padding: 8px 5px;
        font-size: 14px;
      }
      input.time-input {
        min-width: 70px;
        font-size: 14px;
      }
      input.time-input::before {
        content: "hh:mm:ss";
        color: #999;
        position: absolute;
        pointer-events: none;
      }
      input.time-input:focus::before {
        display: none;
      }
      /* Mobile Optimization for new day checkboxes */
      #holidayModeBtn {
          min-width: unset; 
          padding: 8px 10px; 
      }
      .day-checkboxes {
          flex-direction: row; 
          justify-content: flex-start; 
          gap: 2px;
      }
      .day-checkboxes label {
          font-size: 0.75em; 
          white-space: nowrap; 
      }
      /* Sesuaikan lebar kolom di mobile agar tetap responsif */
      table th:nth-child(1), table td:nth-child(1), /* ON Time */
      table th:nth-child(2), table td:nth-child(2) { /* OFF Time */
          width: 20%; 
      }
      table th:nth-child(3), table td:nth-child(3) { /* Enable */
          width: 10%;
      }
      table th:nth-child(4), table td:nth-child(4) { /* Active Days */
          width: 45%; 
      }
      table th:nth-child(5), table td:nth-child(5) { /* Action */
          width: 10%;
      }
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <h1>Timer Controller</h1>
      <div class="time-display">
        <div>Current: <span id="currentTime" class="time-box">00:00:00</span></div>
      </div>
    </div>

    <div id="channelTabs"></div>
    <div id="channelContents"></div>

    <div class="control-row">
      <button id="syncTimeBtn" data-tooltip="Sync Time">🔄</button>
      <select id="ntpServerSelect">
        <option value="pool.ntp.org">pool.ntp.org</option>
        <option value="time.google.com">time.google.com</option>
      </select>
      <label for="timeOffsetInput" style="margin-left: 10px; white-space: nowrap;">Offset (H):</label>
      <input type="number" id="timeOffsetInput" value="7" min="-12" max="14" step="1" title="Time offset in hours (e.g., 7 for GMT+7)" style="width: 50px;">
      <button id="wifiConfigBtn" class="wifi-btn" data-tooltip="WiFi Config">📶</button>
      <button id="holidayModeBtn" data-tooltip="Activate/Deactivate Holiday Mode">Holiday: OFF</button> 
      <button id="saveBtn" class="save-btn" data-tooltip="Save All Configs">💾</button> 
      <span style="margin-left: auto;">Last Sync: <span id="lastSyncTime" class="time-box">Never</span></span>
    </div>

    <div id="notification" class="notification"></div>
  </div>

  <div id="wifiModal" class="modal">
    <div class="modal-content">
      <span class="close">&times;</span>
      <h2>WiFi Configuration</h2>
      <div id="wifiNetworks" class="wifi-list">
        <p>Scanning networks...</p>
      </div>
      <div style="margin-top: 15px;">
        <label for="wifiPassword">Password:</label>
        <input type="password" id="wifiPassword" placeholder="WiFi password" style="width: 100%; padding: 8px; margin-top: 5px;">
      </div>
      <button id="connectWifiBtn" class="save-btn" style="margin-top: 15px;">Connect</button>
    </div>
  </div>

  <script>
    // Time Input Formatter
    function formatTimeInputValue(input) {
      // Pastikan format hh:mm:ss
      let value = input.value;
      if (value && value.length === 5) { // Jika hanya hh:mm
        value = value + ':00'; // Tambahkan detik default
      }
      // Tambahkan validasi sederhana untuk memastikan format hh:mm:ss
      if (!/^\d{2}:\d{2}:\d{2}$/.test(value)) {
          // Fallback atau peringatan jika format tidak sesuai
          console.warn("Invalid time format, defaulting to 00:00:00");
          return '00:00:00';
      }
      return value;
    }

    // Inisialisasi tab
    function initTabs() {
      const tabsContainer = document.getElementById('channelTabs');
      const contentContainer = document.getElementById('channelContents');

      for (let i = 0; i < 8; i++) {
        const tabBtn = document.createElement('button');
        tabBtn.className = 'tab-button';
        tabBtn.textContent = `Ch.${i+1}`;
        tabBtn.onclick = () => showTab(i);
        tabsContainer.appendChild(tabBtn);

        const tabContent = document.createElement('div');
        tabContent.className = 'tab-content';
        tabContent.id = `tab-${i}`;
        tabContent.style.display = i === 0 ? 'block' : 'none';
        tabContent.innerHTML = `
          <table>
            <thead>
              <tr>
                <th>ON</th>
                <th>OFF</th>
                <th>Enable</th>
                <th>Active Days</th> <th>Action <button class="add-btn" data-channel="${i}" data-tooltip="Add Timer">➕</button></th>
              </tr>
            </thead>
            <tbody id="config-${i}"></tbody>
          </table>
          <div class="channel-manual-control" style="padding: 10px; text-align: center; background: #e9e9e9; margin-top: 10px; border-radius: 5px;">
            Status: <span id="status-ch${i}" class="status-indicator">OFF</span>
            <button class="manual-control-btn override-on-btn" data-channel="${i}" data-tooltip="Activate Manual Control">Override ON</button>
            <button class="manual-control-btn on-btn" data-channel="${i}" data-state="true" disabled data-tooltip="Turn ON Manually (requires Override ON)">ON</button>
            <button class="manual-control-btn off-btn" data-channel="${i}" data-state="false" disabled data-tooltip="Turn OFF Manually (requires Override ON)">OFF</button>
            <button class="manual-control-btn override-off-btn" data-channel="${i}" data-tooltip="Deactivate Manual Control">Override OFF</button>
          </div>
        `;
        contentContainer.appendChild(tabContent);
      }
      
      // Set tab pertama sebagai active
      if (tabsContainer.firstChild) {
        tabsContainer.firstChild.classList.add('active');
      }
    }

    function showTab(index) {
      // Update tab buttons
      document.querySelectorAll('.tab-button').forEach(btn => {
        btn.classList.remove('active');
      });
      document.querySelectorAll('.tab-button')[index].classList.add('active');
      
      // Update tab content
      document.querySelectorAll('.tab-content').forEach(tab => {
        tab.style.display = 'none';
      });
      document.getElementById(`tab-${index}`).style.display = 'block';
    }

    // NEW: Update addTimerRow to include day checkboxes
    function addTimerRow(channel, config = {onTime: '00:00:00', offTime: '00:00:00', enable: true, activeDays: Array(7).fill(true)}) { // NEW: Default activeDays ke semua true
      const tbody = document.getElementById(`config-${channel}`);
      const row = document.createElement('tr');
      const dayLabels = ['Sen', 'Sel', 'Rab', 'Kam', 'Jum', 'Sab', 'Min']; // Label hari untuk UI

      // Buat HTML untuk checkbox hari
      let dayCheckboxesHtml = '<div class="day-checkboxes">';
      for (let k = 0; k < 7; k++) {
          dayCheckboxesHtml += `
            <label>
              <input type="checkbox" ${config.activeDays[k] ? 'checked' : ''} data-day-index="${k}">
              ${dayLabels[k]}
            </label>
          `;
      }
      dayCheckboxesHtml += '</div>';

      row.innerHTML = `
				<td><input type="time" step="1" class="time-input" value="${config.onTime}" onchange="this.value = formatTimeInputValue(this)" onblur="validateTimeInput(this)"></td>
				<td><input type="time" step="1" class="time-input" value="${config.offTime}" onchange="this.value = formatTimeInputValue(this)" onblur="validateTimeInput(this)"></td>
				<td><input type="checkbox" ${config.enable ? 'checked' : ''}></td>
				<td>${dayCheckboxesHtml}</td>
				<td><button class="delete-btn" data-tooltip="Delete">❌</button></td>
      `;
      tbody.appendChild(row);
			// NEW: Tambahkan ini SETELAH `tbody.appendChild(row);`
			// Ini memastikan input sudah ada di DOM sebelum divalidasi
			const newTimeInputs = row.querySelectorAll('.time-input');
			newTimeInputs.forEach(input => {
					validateTimeInput(input); // Lakukan validasi awal saat baris dibuat/dimuat
			});			
    }

    function loadConfig() {
      // Mengembalikan Promise agar kita bisa menggunakan .then() setelah ini selesai
      return fetch('/getConfig') // Mengembalikan Promise dari fetch
        .then(r => {
            if (!r.ok) {
                throw new Error('Failed to fetch config: ' + r.statusText);
            }
            return r.json();
        })
        .then(data => {
          document.getElementById('ntpServerSelect').value = data.ntpServer;
          document.getElementById('lastSyncTime').textContent = data.lastSyncTime || 'Never';

				// NEW: Muat nilai timeOffset ke input
				const timeOffsetInput = document.getElementById('timeOffsetInput');
				if (timeOffsetInput) { // Pastikan elemen input ada di HTML
						// data.timeOffset datang dalam detik dari ESP32, konversi ke jam untuk UI
						timeOffsetInput.value = (data.timeOffset / 3600) || 7; // Default 7 jam jika tidak ada
				}

          // NEW: Muat status Holiday Mode
          updateHolidayModeUI(data.holidayModeActive); // Panggil dengan data langsung

          for (let i = 0; i < 8; i++) {
            const tbody = document.getElementById(`config-${i}`);
            tbody.innerHTML = '';
            // Pastikan data.channels[i] ada sebelum mencoba iterasi
            // Gunakan optional chaining (?.) untuk menghindari error jika channels[i] undefined
            data.channels[i]?.forEach(config => { 
              // Pastikan 'config' sekarang sudah memiliki properti activeDays dari C++
              // Jika config.activeDays undefined, addTimerRow akan menggunakan default [true, true, ...]
              addTimerRow(i, config); 
            });
          }
          console.log("Config loaded from ESP32.");
        })
        .catch(e => {
            console.error("Error loading config:", e);
            showNotification('Error loading config: ' + e.message, true);
        });
    }

    // WiFi Modal Functions
    function openWifiModal() {
      document.getElementById('wifiModal').style.display = 'block';
      scanNetworks();
    }

    function closeWifiModal() {
      document.getElementById('wifiModal').style.display = 'none';
    }

    function scanNetworks() {
      const container = document.getElementById('wifiNetworks');
      container.innerHTML = '<p>Scanning networks...</p>'; // Tampilkan pesan scanning
      
      fetch('/scanWifi')
        .then(r => r.json())
        .then(networks => {
          container.innerHTML = ''; // Hapus pesan scanning
          
          if (networks.length === 0) {
            container.innerHTML = '<p>No networks found</p>';
            return;
          }
          
          networks.forEach(network => {
            const div = document.createElement('div');
            div.style.padding = '8px';
            div.style.borderBottom = '1px solid #eee';
            div.style.cursor = 'pointer';
            div.innerHTML = `
              <strong>${network.ssid}</strong> 
              <span style="float: right;">${network.rssi} dBm</span>
            `;
            div.addEventListener('click', function() {
              document.querySelectorAll('#wifiNetworks > div').forEach(el => {
                el.style.backgroundColor = '';
              });
              this.style.backgroundColor = '#e0e0ff';
              document.getElementById('wifiPassword').focus();
            });
            container.appendChild(div);
          });
        })
        .catch(e => {
            container.innerHTML = '<p style="color: red;">Error scanning networks.</p>';
            console.error("Error scanning WiFi:", e);
        });
    }

    function connectToWifi() {
      const selected = document.querySelector('#wifiNetworks > div[style*="background-color"]');
      if (!selected) {
        showNotification('Please select a network first', true);
        return;
      }
      
      const ssid = selected.querySelector('strong').textContent;
      const password = document.getElementById('wifiPassword').value;
      
      fetch('/setWifi', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: `ssid=${encodeURIComponent(ssid)}&password=${encodeURIComponent(password)}`
      })
      .then(r => r.text())
      .then(msg => {
        showNotification(msg);
        closeWifiModal();
        setTimeout(() => {
          window.location.reload(); // Reload halaman setelah koneksi
        }, 2000);
      })
      .catch(e => showNotification('Connection failed: ' + e, true));
    }

    // Event handlers
    document.addEventListener('DOMContentLoaded', () => {
      initTabs();
      
      // Panggil loadConfig() terlebih dahulu
      loadConfig().then(() => { 
          updateClock();
          updateChannelStatusUI(); 
      }).catch(error => {
          console.error("Error during initial load:", error);
      });
      
      document.getElementById('saveBtn').addEventListener('click', saveConfig);
      document.getElementById('syncTimeBtn').addEventListener('click', syncTime);
      document.getElementById('wifiConfigBtn').addEventListener('click', openWifiModal);
      document.getElementById('holidayModeBtn').addEventListener('click', toggleHolidayMode); 
      document.querySelector('.close').addEventListener('click', closeWifiModal);
      document.getElementById('connectWifiBtn').addEventListener('click', connectToWifi);
      
      document.addEventListener('click', (e) => {
        if (e.target.classList.contains('add-btn')) {
          const channel = e.target.dataset.channel;
          addTimerRow(channel);
        }
        if (e.target.classList.contains('delete-btn')) {
          e.target.closest('tr').remove();
        }
        if (e.target == document.getElementById('wifiModal')) {
          closeWifiModal();
        }

        // Manual Control Buttons
        if (e.target.classList.contains('manual-control-btn')) {
          const channel = e.target.dataset.channel;
          console.log("Clicked manual control button for channel:", channel); 
          if (typeof channel === 'undefined' || channel === null) {
              console.error("Channel data-attribute is missing!");
              showNotification("Error: Channel data is missing for button.", true);
              return; 
          }
          let url = '/setManualControl?channel=' + channel; 
          
          if (e.target.classList.contains('on-btn')) {
            url += '&state=true';
          } else if (e.target.classList.contains('off-btn')) {
            url += '&state=false';
          } else if (e.target.classList.contains('override-on-btn')) { 
            url += '&override_on=true'; 
          } else if (e.target.classList.contains('override-off-btn')) {
            url += '&override_off=true';
          }
          
          fetch(url, { method: 'POST' }) 
            .then(r => r.text())
            .then(msg => {
              showNotification(msg);
            })
            .catch(e => showNotification('Error controlling channel: ' + e, true));
        }
      });
    });

    // NEW: Update saveConfig to include activeDays
    function saveConfig() {
		let allInputsAreValid = true;
	
		// NEW: Cek semua input waktu di semua channel
		document.querySelectorAll('.time-input').forEach(input => {
				if (!validateTimeInput(input)) {
						allInputsAreValid = false;
				}
		});
	
		if (!allInputsAreValid) {
				showNotification('Ada kesalahan dalam format waktu. Mohon perbaiki sebelum menyimpan!', true);
				return; // Hentikan proses save jika ada input tidak valid
		}

    // NEW: Tambahkan console.log() ini
    console.log("Value from timeOffsetInput:", document.getElementById('timeOffsetInput').value);
	
        const config = {
            ntpServer: document.getElementById('ntpServerSelect').value,
            lastSyncTime: document.getElementById('lastSyncTime').textContent,
            holidayModeActive: document.getElementById('holidayModeBtn').textContent.includes('ON'),
            // NEW: Tambahkan timeOffset ke objek config. Konversi jam ke detik.
            // Gunakan parseInt untuk memastikan nilainya angka, dan berikan default '7' jika input kosong.
            timeOffset: parseInt(document.getElementById('timeOffsetInput').value || '7') * 3600,
            channels: Array(8).fill().map((_, i) => {
                const rows = document.getElementById(`config-${i}`).rows;
                return Array.from(rows).map(row => {
                    const inputs = row.getElementsByTagName('input');
                    const activeDays = [];
                    row.querySelectorAll('.day-checkboxes input[type="checkbox"]').forEach(cb => {
                        activeDays.push(cb.checked);
                    });
                    return {
                        onTime: formatTimeInputValue(inputs[0]),
                        offTime: formatTimeInputValue(inputs[1]),
                        enable: inputs[2].checked,
                        activeDays: activeDays
                    };
                });
            })
        };

				// NEW: Tambahkan console.log() ini untuk melihat objek 'config' yang akan dikirim
				console.log("Config object to be sent:", config);
				console.log("timeOffset in config object (before encoding):", config.timeOffset);

        fetch('/saveConfig', {
            method: 'POST',
            headers: {'Content-Type': 'application/x-www-form-urlencoded'},
            body: `data=${encodeURIComponent(JSON.stringify(config))}`
        })
        .then(r => r.text())
        .then(msg => {
            showNotification(msg);
            // Panggil loadConfig() untuk memperbarui tampilan semua pengaturan,
            // termasuk lastSyncTime dan nilai timeOffset di input.
            // Kemudian panggil updateClock() untuk memastikan waktu terkini juga diperbarui.
            loadConfig().then(() => {
                updateClock();
                console.log("UI updated after config save with new timezone.");
            });
        })
        .catch(e => {
            console.error("Error saving config:", e);
            showNotification('Error saving config: ' + e.message, true);
        });
    } // Akhir dari saveConfig
		
    function syncTime() {
      const btn = document.getElementById('syncTimeBtn');
      btn.disabled = true;
      
      fetch('/syncTime?server=' + encodeURIComponent(document.getElementById('ntpServerSelect').value))
        .then(r => r.text())
        .then(msg => {
          showNotification(msg);
          loadConfig(); // Reload config to update lastSyncTime
        })
        .catch(e => showNotification('Error: ' + e, true))
        .finally(() => btn.disabled = false);
    }

    function showNotification(msg, isError = false) {
      const el = document.getElementById('notification');
      el.textContent = msg;
      el.className = isError ? 'notification error' : 'notification';
      el.style.display = 'block';
      setTimeout(() => el.style.display = 'none', 3000);
    }

		// Update clock every second
		function updateClock() {
			fetch('/getTime')
				.then(r => {
						if (!r.ok) {
								throw new Error('Network response from /getTime was not ok: ' + r.statusText);
						}
						return r.json();
				})
				.then(data => {
						// NEW: Tambahkan console.log() ini untuk melihat data yang diterima
						console.log("Data received from /getTime:", data);
						console.log("Current Time from data:", data.currentTime);
						console.log("Last Sync Time from data:", data.lastSyncTime);

						// NEW: Tambahkan pengecekan null untuk elemen HTML
						const currentTimeEl = document.getElementById('currentTime');
						const lastSyncTimeEl = document.getElementById('lastSyncTime');

						if (currentTimeEl && data.currentTime) {
								currentTimeEl.textContent = data.currentTime;
						} else {
								console.error("Error: 'currentTime' element or data.currentTime not found.", {currentTimeEl, data_currentTime: data.currentTime});
						}

						if (lastSyncTimeEl && data.lastSyncTime) {
								lastSyncTimeEl.textContent = data.lastSyncTime || 'Never';
						} else {
								console.error("Error: 'lastSyncTime' element or data.lastSyncTime not found.", {lastSyncTimeEl, data_lastSyncTime: data.lastSyncTime});
						}
						
						setTimeout(updateClock, 1000);
				})
				.catch(e => {
						console.error("Error fetching time or parsing JSON from /getTime:", e);
						setTimeout(updateClock, 5000);
				});
		}
    
    // Dapatkan dan tampilkan status channel saat ini (NEW)
    function updateChannelStatusUI() {
      fetch('/getChannelStatus') 
        .then(r => r.json())
        .then(data => {
          data.forEach((chStatus, index) => {
            const statusEl = document.getElementById(`status-ch${index}`);
            const onBtn = document.querySelector(`#tab-${index} .on-btn`);
            const offBtn = document.querySelector(`#tab-${index} .off-btn`);
            const overrideOnBtn = document.querySelector(`#tab-${index} .override-on-btn`);
            const overrideOffBtn = document.querySelector(`#tab-${index} .override-off-btn`);

            if (statusEl && onBtn && offBtn && overrideOnBtn && overrideOffBtn) {
              statusEl.textContent = chStatus.state ? 'ON' : 'OFF';
              statusEl.className = 'status-indicator ' + (chStatus.state ? 'on' : 'off');
              
              if (chStatus.manualOverride) {
                statusEl.classList.add('override');
                statusEl.setAttribute('data-tooltip', 'Manual Override Active');
                statusEl.textContent += ' (Manual)'; 
                onBtn.disabled = false; 
                offBtn.disabled = false; 
                overrideOnBtn.disabled = true; 
                overrideOffBtn.disabled = false; 
              } else {
                statusEl.classList.remove('override');
                statusEl.removeAttribute('data-tooltip');
                onBtn.disabled = true; 
                offBtn.disabled = true; 
                overrideOnBtn.disabled = false; 
                overrideOffBtn.disabled = true; 
              }
            }
          });
          setTimeout(updateChannelStatusUI, 2000); 
        })
        .catch(e => {
          console.error("Error fetching channel status:", e);
          setTimeout(updateChannelStatusUI, 5000); 
        });
    }

    // NEW: Fungsi untuk mengaktifkan/menonaktifkan Holiday Mode
    function toggleHolidayMode() {
      const btn = document.getElementById('holidayModeBtn');
      const isCurrentlyActive = btn.classList.contains('active'); 
      const newState = !isCurrentlyActive; 
      
      btn.disabled = true; 
      
      console.log("Attempting to set Holiday Mode to:", newState); 
      
      fetch('/setHolidayMode', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: `active=${newState ? 'true' : 'false'}` 
      })
      .then(response => {
        if (!response.ok) { 
            throw new Error('Network response was not ok: ' + response.statusText);
        }
        return response.text();
      })
      .then(msg => {
        showNotification(msg);
        updateHolidayModeUI(newState); 
        console.log("Holiday Mode response:", msg); 
      })
      .catch(e => {
        showNotification('Error setting Holiday Mode: ' + e.message, true); 
        console.error('Fetch error:', e); 
      })
      .finally(() => {
        btn.disabled = false; 
      });
    }

    // NEW: Fungsi untuk memperbarui tampilan tombol Holiday Mode berdasarkan status
    function updateHolidayModeUI(isActiveFromServer = null) { 
      const btn = document.getElementById('holidayModeBtn');

      if (isActiveFromServer !== null) { 
        if (isActiveFromServer) {
          btn.classList.add('active');
          btn.textContent = 'Holiday: ON'; // TEKS BARU
          btn.setAttribute('data-tooltip', 'Timers are disabled. Click to deactivate Holiday Mode.');
        } else {
          btn.classList.remove('active');
          btn.textContent = 'Holiday: OFF'; // TEKS BARU
          btn.setAttribute('data-tooltip', 'Timers are active. Click to activate Holiday Mode.');
        }
        console.log("Holiday Mode UI updated based on provided status:", isActiveFromServer); 
      } else { 
        fetch('/getHolidayMode')
          .then(r => r.json())
          .then(data => {
            if (data.active) {
              btn.classList.add('active');
              btn.textContent = 'Holiday: ON'; 
              btn.setAttribute('data-tooltip', 'Timers are disabled. Click to deactivate Holiday Mode.');
            } else {
              btn.classList.remove('active');
              btn.textContent = 'Holiday: OFF'; 
              btn.setAttribute('data-tooltip', 'Timers are active. Click to activate Holiday Mode.');
            }
            console.log("Holiday Mode UI updated by fetching from server. Status:", data.active); 
          })
          .catch(e => console.error("Error fetching Holiday Mode status for UI update:", e));
      }
    }

	// NEW: Fungsi untuk validasi input waktu dan memberikan feedback visual
	function validateTimeInput(inputElement) {
	    const value = inputElement.value;
	    // Regex untuk HH:MM:SS. Memastikan format 00-23, 00-59, 00-59
	    const timeRegex = /^(0[0-9]|1[0-9]|2[0-3]):([0-5][0-9]):([0-5][0-9])$/; 
	
	    let isValid = true;
	    let errorMessage = '';
	
	    if (!timeRegex.test(value)) {
	        isValid = false;
	        errorMessage = 'Format waktu harus HH:MM:SS (misal: 08:30:00).';
	    } else {
	        // Double check rentang numerik (regex sudah cukup ketat tapi ini lapisan pengamanan)
	        const parts = value.split(':').map(Number);
	        if (parts[0] > 23 || parts[1] > 59 || parts[2] > 59) {
	            isValid = false; // Ini seharusnya sudah ditangkap regex, tapi jaga-jaga
	            errorMessage = 'Waktu tidak valid (HH:0-23, MM:0-59, SS:0-59).';
	        }
	    }
	
	    if (!isValid) {
	        inputElement.style.borderColor = 'red'; // Border merah jika tidak valid
	        inputElement.title = errorMessage; // Tooltip pesan error
	    } else {
	        inputElement.style.borderColor = ''; // Kembalikan border normal
	        inputElement.title = ''; // Hapus tooltip
	    }
	    return isValid;
	}
	
  </script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- ESP32 Booting ---"); // Debug: Start of setup

  //1. Initialize GPIO
  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(channelPins[i], OUTPUT);
    digitalWrite(channelPins[i], LOW); // Pastikan semua channel OFF saat boot
  }
  Serial.println("GPIO Initialized."); // Debug

  //2. Initialize Filesystem
  if (!LittleFS.begin()) {
    Serial.println("LittleFS Mount Failed! Attempting to format..."); 
    if (LittleFS.format()) { 
      Serial.println("LittleFS formatted successfully! Retrying mount...");
      if (!LittleFS.begin()) { 
        Serial.println("LittleFS mount failed again after format! Halting.");
        while(true); 
      }
      Serial.println("LittleFS re-mounted after format.");
    } else {
      Serial.println("LittleFS format failed! Halting.");
      while(true); 
    }
  } else {
    Serial.println("LittleFS Mounted Successfully."); 
  }

  //3. Load Configuration dari LittleFS
  loadConfig(); 
  Serial.println("Configuration Loaded (or initialized to default if not found/corrupted)."); 
  applyTimezone(); // <<< PANGGIL FUNGSI INI DI SINI, setelah loadConfig() selesai

  // 4. WiFi Connection
  WiFi.mode(WIFI_STA);
  wifiManager.setConfigPortalTimeout(180); // 3 menit
  Serial.println("Attempting WiFi Connection...");

  if (!wifiManager.autoConnect(ap_config_ssid)) { // Gunakan variabel di sini
    Serial.println("Failed to connect and hit timeout. Starting Fallback AP.");
    WiFi.softAP(ap_fallback_ssid); // Gunakan variabel di sini
    Serial.println("AP Mode Started. IP: " + WiFi.softAPIP().toString());
  } else {
    Serial.println("Connected to WiFi."); 
    Serial.println("IP Address: " + WiFi.localIP().toString());
  }

  // Initialize NTP
  timeClient.begin();
	timeClient.setTimeOffset(timeOffset); // Gunakan variabel global timeOffset
	timeClient.update();
  timeClient.setPoolServerName(ntpServer.c_str()); // Set NTP server dari config yang dimuat
  Serial.println("NTP Client Initialized. Attempting time sync..."); 
  updateNTPTime();
  Serial.println("NTP Time Sync Attempted."); 

  // --- Inisialisasi MQTT ---
  espClient.setInsecure(); // Biarkan ini untuk debugging SSL
  Serial.println("MQTT Client: setInsecure() called."); 

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  Serial.println("MQTT Client Initialized with Server and Callback."); 

  // Web Server Endpoints
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", htmlPage); // Gunakan send_P untuk PROGMEM
  });

  server.on("/getConfig", HTTP_GET, [](AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(2048);
    doc["ntpServer"] = ntpServer;
    doc["lastSyncTime"] = lastSyncTime;
    doc["holidayModeActive"] = holidayModeActive; 
  // NEW: Tambahkan timeOffset ke JSON response
  doc["timeOffset"] = timeOffset; // Kirim nilai global timeOffset ke browser
    
    JsonArray channelsArray = doc.createNestedArray("channels");
    for (int i = 0; i < NUM_CHANNELS; i++) {
      JsonArray configs = channelsArray.createNestedArray();
      for (int j = 0; j < channels[i].count; j++) {
        JsonObject cfg = configs.createNestedObject();
        cfg["onTime"] = channels[i].configs[j].onTime;
        cfg["offTime"] = channels[i].configs[j].offTime;
        cfg["enable"] = channels[i].configs[j].enable;
        // NEW: Tambahkan activeDays saat mengirim config ke UI
        JsonArray activeDaysJson = cfg.createNestedArray("activeDays");
        for (int k = 0; k < 7; k++) {
            activeDaysJson.add(channels[i].configs[j].activeDays[k]);
        }
      }
    }
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
    Serial.printf("DEBUG: JSON sent to browser for /getConfig (timeOffset): %d seconds\n", timeOffset); // <<< TAMBAHKAN BARIS INI
    Serial.println("GET /getConfig served."); 
  });

  server.on("/saveConfig", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("data", true)) {
      String jsonData = request->getParam("data", true)->value();
      Serial.println("Received /saveConfig with data: " + jsonData); 
      DynamicJsonDocument doc(2048); 
      DeserializationError error = deserializeJson(doc, jsonData);
      
      if (!error) {
        // Perbarui pengaturan global dari data yang diterima
        ntpServer = doc["ntpServer"].as<String>();
        lastSyncTime = doc["lastSyncTime"].as<String>();
        holidayModeActive = doc["holidayModeActive"].as<bool>();

        // NEW: Perbarui variabel global timeOffset dari data yang diterima
        if (doc.containsKey("timeOffset")) {
            timeOffset = doc["timeOffset"].as<int>();
            Serial.printf("Time Offset updated from Web UI input: %d seconds\n", timeOffset); // Debug print
        }

      for (int i = 0; i < NUM_CHANNELS; i++) {
        channels[i].count = 0; 
        JsonArray configs = doc["channels"][i];
        if (configs) { 
            for (JsonObject cfg : configs) {
                if (channels[i].count < MAX_CONFIGS) {
                    channels[i].configs[channels[i].count] = {
                        cfg["onTime"].as<String>(),
                        cfg["offTime"].as<String>(),
                        cfg["enable"]
                    };
                    // NEW: Baca activeDays dari JSON yang diterima dari UI
                    JsonArray activeDaysJson = cfg["activeDays"];
                    if (activeDaysJson && activeDaysJson.size() == 7) {
                        for (int k = 0; k < 7; k++) {
                            channels[i].configs[channels[i].count].activeDays[k] = activeDaysJson[k].as<bool>();
                        }
                    } else {
                         // Fallback: Jika data hari tidak ada dari UI, set ke semua true (ini seharusnya tidak terjadi jika UI benar)
                        Serial.printf("activeDays not found or invalid from UI for CH%d config %d, setting all true.\n", i, channels[i].count);
                        for (int k = 0; k < 7; k++) {
                            channels[i].configs[channels[i].count].activeDays[k] = true; 
                        }
                    }
                    channels[i].count++;
                }
            }
        }
      }
      
        saveConfig(); // Panggil saveConfig untuk menulis ke LittleFS

        // NEW: Panggil applyTimezone() setelah variabel global timeOffset diperbarui
        applyTimezone(); // <<< PANGGIL FUNGSI INI DI SINI
        request->send(200, "text/plain", "Configuration saved!");
        Serial.println("Config saved to LittleFS.");
        // Ini akan memicu pengiriman status MQTT, yang juga akan memperbarui waktu di UI.
        publishStatus("Configuration saved via Web UI.");
      } else {
        request->send(400, "text/plain", "Failed to parse config data!");
        Serial.print("Failed to parse config data: ");
        Serial.println(error.c_str());
      }
    } else {
      request->send(400, "text/plain", "No config data received!");
      Serial.println("No config data received for /saveConfig.");
    }
  });

  server.on("/syncTime", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("Received /syncTime request."); 
    if (request->hasParam("server")) {
      ntpServer = request->getParam("server")->value();
      timeClient.setPoolServerName(ntpServer.c_str()); 
      Serial.println("NTP Server updated to: " + ntpServer); 
    }
    
    if (updateNTPTime()) {
      request->send(200, "text/plain", "Time synced: " + currentTime);
      publishStatus("Time synced via Web UI: " + currentTime); 
    } else {
      request->send(500, "text/plain", "Sync failed");
      publishStatus("Time sync failed via Web UI."); 
    }
  });

	server.on("/getTime", HTTP_GET, [](AsyncWebServerRequest *request){
		DynamicJsonDocument doc(256); // Ukuran kecil sudah cukup
		struct tm timeinfo;
		if(!getLocalTime(&timeinfo)){
			Serial.println("Failed to obtain time");
			request->send(200, "text/plain", "Time not set"); // Fallback jika waktu belum diset
			return;
		}
		char timeChar[9]; // HH:MM:SS
		strftime(timeChar, 9, "%H:%M:%S", &timeinfo);

		doc["currentTime"] = timeChar;
		doc["lastSyncTime"] = lastSyncTime; // Menggunakan variabel global lastSyncTime

		String output;
		serializeJson(doc, output);
		request->send(200, "application/json", output);
	});

  // Endpoint untuk kontrol manual dari Web UI
  server.on("/setManualControl", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println("Received /setManualControl request.");
    if (request->hasParam("channel")) { 
      int channel = request->getParam("channel")->value().toInt(); 
      Serial.printf("Processing channel %d\n", channel); 
      
      if (channel >= 0 && channel < NUM_CHANNELS) {
        if (request->hasParam("state")) { 
          Serial.println("  - Command: State Change"); 
          if (!channels[channel].manualOverride) {
              request->send(403, "text/plain", "Override not active for CH" + String(channel+1));
              Serial.printf("Web UI Control: CH%d not in manual override. ON/OFF command ignored.\n", channel);
              return; 
          }
          bool state = request->getParam("state")->value().equals("true");
          digitalWrite(channelPins[channel], state ? HIGH : LOW);
          channels[channel].currentState = state;
          Serial.printf("  - CH%d set to %s (Manual Override)\n", channel, state ? "ON" : "OFF");
          publishStatus("Manual control from Web UI: CH" + String(channel+1) + " set " + (state ? "ON" : "OFF"));
          request->send(200, "text/plain", "OK");
        } else if (request->hasParam("override_on")) { 
          Serial.println("  - Command: Override ON"); 
          channels[channel].manualOverride = true; 
          Serial.printf("  - CH%d manual override activated.\n", channel);
          publishStatus("Manual override for CH" + String(channel+1) + " activated from Web UI.");
          request->send(200, "text/plain", "Override ON Success");
        } else if (request->hasParam("override_off")) { 
          Serial.println("  - Command: Override OFF"); 
          channels[channel].manualOverride = false; 
          // Ketika override dimatikan, status pin akan segera dikendalikan oleh timer
          // Jadi kita panggil logika timer untuk channel ini agar segera update
          bool shouldBeOn = false;
          if (!holidayModeActive) { 
            for (int j = 0; j < channels[channel].count; j++) {
              // NEW: Sertakan pemeriksaan hari dalam logika timer setelah override dimatikan
              int currentDay = timeClient.getDay(); // 0=Minggu, 1=Senin, ..., 6=Sabtu
              int ourDayIndex = (currentDay == 0) ? 6 : (currentDay - 1); // 0=Senin, ..., 6=Minggu (sesuai array kita)

              if (channels[channel].configs[j].enable && 
                  channels[channel].configs[j].activeDays[ourDayIndex] && // Periksa hari aktif!
                  isTimeInRange(currentTime, channels[channel].configs[j].onTime, channels[channel].configs[j].offTime)) {
                shouldBeOn = true;
                break;
              }
            }
          }
          if (shouldBeOn != channels[channel].currentState) {
            digitalWrite(channelPins[channel], shouldBeOn ? HIGH : LOW);
            channels[channel].currentState = shouldBeOn;
          }
          Serial.printf("  - CH%d manual override off. Timer will resume.\n", channel);
          publishStatus("Manual override for CH" + String(channel+1) + " turned OFF from Web UI. Timer will resume.");
          request->send(200, "text/plain", "Override OFF Success");
        } else {
          Serial.println("  - Error: Missing state or override command"); 
          request->send(400, "text/plain", "Bad request: Missing state or override command");
        }
      } else {
        Serial.println("Error: Invalid channel parameter."); 
        request->send(400, "text/plain", "Invalid channel");
      }
    } else {
      Serial.println("Error: Missing channel parameter in request."); 
      request->send(400, "text/plain", "Bad request: Missing channel parameter");
    }
  });

  // Endpoint untuk mendapatkan status channel saat ini untuk UI
  server.on("/getChannelStatus", HTTP_GET, [](AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(512); 
    JsonArray channelStatusArray = doc.to<JsonArray>();
    
    for (int i = 0; i < NUM_CHANNELS; i++) {
      JsonObject channelObj = channelStatusArray.createNestedObject();
      channelObj["state"] = channels[i].currentState;
      channelObj["manualOverride"] = channels[i].manualOverride;
    }
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
//    Serial.println("GET /getChannelStatus served."); 
  });

  // NEW: Endpoint untuk Holiday Mode
  server.on("/setHolidayMode", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("active", true)) { 
      String activeParam = request->getParam("active", true)->value(); 
      holidayModeActive = activeParam.equals("true"); 
      
      Serial.printf("Received setHolidayMode: active=%s, set to %s\n", activeParam.c_str(), holidayModeActive ? "true" : "false"); 

      if (holidayModeActive) {
        Serial.println("Holiday Mode Activated.");
        publishStatus("Global Holiday Mode activated.");
        for(int i = 0; i < NUM_CHANNELS; i++) {
            if (channels[i].currentState) {
                digitalWrite(channelPins[i], LOW);
                channels[i].currentState = false;
                Serial.printf("CH%d forced OFF due to Holiday Mode activation.\n", i); 
            }
        }
        request->send(200, "text/plain", "Holiday Mode ON"); 
      } else {
        Serial.println("Holiday Mode Deactivated. Timers will resume.");
        publishStatus("Global Holiday Mode deactivated. Timers will resume.");
        request->send(200, "text/plain", "Holiday Mode OFF"); 
      }
      saveConfig(); 
    } else {
      Serial.println("Bad request to /setHolidayMode: Missing 'active' parameter."); 
      request->send(400, "text/plain", "Bad request: Missing 'active' parameter.");
    }
  });

  server.on("/getHolidayMode", HTTP_GET, [](AsyncWebServerRequest *request) {
    String response = holidayModeActive ? "{\"active\":true}" : "{\"active\":false}";
    request->send(200, "application/json", response);
    Serial.println("GET /getHolidayMode served. Status: " + response); 
  });

  // WiFi Management Endpoints
  server.on("/scanWifi", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("Received /scanWifi request."); 
    int n = WiFi.scanNetworks();
    DynamicJsonDocument doc(1024);
    JsonArray networks = doc.to<JsonArray>();
    
    for (int i = 0; i < n; ++i) {
      JsonObject network = networks.createNestedObject();
      network["ssid"] = WiFi.SSID(i);
      network["rssi"] = WiFi.RSSI(i);
    }
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
    Serial.println("GET /scanWifi served."); 
  });

  server.on("/setWifi", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println("Received /setWifi request."); 
    if (request->hasParam("ssid", true) && request->hasParam("password", true)) {
      String ssid = request->getParam("ssid", true)->value();
      String password = request->getParam("password", true)->value();
      
      Serial.printf("Attempting to connect to WiFi: %s\n", ssid.c_str()); 
      WiFi.begin(ssid.c_str(), password.c_str());
      int attempts = 0;
      
      while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        attempts++;
        Serial.print("."); 
      }
      Serial.println(); 

      if (WiFi.status() == WL_CONNECTED) {
        request->send(200, "text/plain", "Connected to " + ssid + ". IP: " + WiFi.localIP().toString());
        Serial.println("Connected to " + ssid + ". IP: " + WiFi.localIP().toString());
        // Ini akan menyebabkan ESP32 untuk reboot agar kredensial tersimpan oleh WiFiManager
        delay(1000);
        ESP.restart(); 
      } else {
        request->send(200, "text/plain", "Failed to connect to " + ssid);
        Serial.println("Failed to connect to " + ssid);
      }
    } else {
      Serial.println("Bad request to /setWifi: Missing ssid or password."); 
      request->send(400, "text/plain", "Bad request");
    }
  });

  // --- Inisialisasi mDNS ---
  // Gunakan variabel global mdns_hostname
  if (MDNS.begin(mdns_hostname)) { // <--- Gunakan variabel di sini
    Serial.printf("mDNS responder started. Access at %s.local\n", mdns_hostname);
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("Error starting mDNS");
  }
  // --- Akhir Inisialisasi mDNS ---

  server.begin();
  Serial.println("Web Server Started."); 
}

void loop() {
  // --- MQTT Loop and Reconnect ---
  if (WiFi.status() == WL_CONNECTED) { 
    if (!mqttClient.connected()) {
      reconnectMqtt();
    }
    mqttClient.loop(); 
  }

  timeClient.update();
  currentTime = formatTime(timeClient.getHours(), timeClient.getMinutes(), timeClient.getSeconds());

  for (int i = 0; i < NUM_CHANNELS; i++) {
    // Jika Holiday Mode aktif, paksa semua relay ke OFF dan lewati logika timer
    if (holidayModeActive) {
        if (channels[i].currentState) { 
            digitalWrite(channelPins[i], LOW); 
            channels[i].currentState = false;
            Serial.printf("CH%d forced OFF due to Holiday Mode.\n", i);
            publishStatus("Channel " + String(i+1) + " forced OFF by Holiday Mode.");
        }
        continue; 
    }

    // Hanya jalankan logika timer jika TIDAK dalam manual override (dan Holiday Mode sudah di handle di atas)
    if (!channels[i].manualOverride) {
      bool shouldBeOn = false;
      
      // Dapatkan hari saat ini dari NTPClient
      // getDay() mengembalikan: 0=Minggu, 1=Senin, ..., 6=Sabtu
      int currentDay = timeClient.getDay(); 
      // Konversi ke indeks array kita: 0=Senin, 1=Selasa, ..., 6=Minggu
      // Jika currentDay adalah 0 (Minggu), maka dayIndex kita adalah 6.
      // Selain itu, dayIndex adalah currentDay - 1.
      int ourDayIndex = (currentDay == 0) ? 6 : (currentDay - 1); 

      for (int j = 0; j < channels[i].count; j++) {
        if (channels[i].configs[j].enable && 
            channels[i].configs[j].activeDays[ourDayIndex] && // NEW: Periksa apakah hari ini adalah hari aktif!
            isTimeInRange(currentTime, channels[i].configs[j].onTime, channels[i].configs[j].offTime)) {
          shouldBeOn = true;
          break;
        }
      }
      
      // Periksa apakah status output berubah
      if (shouldBeOn != channels[i].currentState) {
        digitalWrite(channelPins[i], shouldBeOn ? HIGH : LOW);
        channels[i].currentState = shouldBeOn;
        Serial.printf("CH%d %s @ %s (Timer) on Day: %d\n", i, shouldBeOn ? "ON" : "OFF", currentTime.c_str(), currentDay);
        publishStatus("Channel " + String(i+1) + " turned " + (shouldBeOn ? "ON" : "OFF") + " by timer.");
      }
    }
  }
  
  delay(50); 
}

// Helper Functions
String formatTime(int h, int m, int s) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
  return String(buf);
}

bool isTimeInRange(String current, String start, String end) {
  int currentSec = timeToSeconds(current);
  int startSec = timeToSeconds(start);
  int endSec = timeToSeconds(end);

  // Handle midnight rollover (e.g., start 23:00, end 01:00)
  if (endSec <= startSec) {
    endSec += 86400; // Add 24 hours in seconds
    if (currentSec < startSec) { // If current time is past midnight but before start time
      currentSec += 86400;
    }
  }
  return (currentSec >= startSec && currentSec < endSec);
}

int timeToSeconds(String timeStr) {
  int h = timeStr.substring(0, 2).toInt();
  int m = timeStr.substring(3, 5).toInt();
  int s = timeStr.substring(6, 8).toInt();
  return h * 3600 + m * 60 + s;
}

void loadConfig() {
  bool configExists = LittleFS.exists("/config.json");
  Serial.printf("config.json exists: %s\n", configExists ? "true" : "false"); // Debug

  // Deklarasikan 'error' di scope fungsi loadConfig() agar bisa diakses di seluruh fungsi
  DeserializationError error = DeserializationError::Ok; // Inisialisasi dengan default 'no error'

  if (configExists) {
    File file = LittleFS.open("/config.json", "r");
    if (file) {
      DynamicJsonDocument doc(2048); // Sesuaikan ukuran buffer jika konfigurasi semakin besar
      error = deserializeJson(doc, file); // Assign ke variabel 'error' yang sudah dideklarasikan
      file.close();

      if (!error) { // Jika tidak ada error deserialisasi
        Serial.println("Successfully deserialized config.json.");

        // Memuat Pengaturan Global
        if (doc.containsKey("ntpServer")) {
          ntpServer = doc["ntpServer"].as<String>();
        } else {
          ntpServer = "pool.ntp.org";
          Serial.println("ntpServer not found in config.json, using default.");
        }

        if (doc.containsKey("lastSyncTime")) {
          lastSyncTime = doc["lastSyncTime"].as<String>();
        } else {
          lastSyncTime = "Never";
          Serial.println("lastSyncTime not found in config.json, using default.");
        }

        if (doc.containsKey("holidayModeActive")) {
          holidayModeActive = doc["holidayModeActive"].as<bool>();
          Serial.printf("Holiday Mode loaded as: %s\n", holidayModeActive ? "true" : "false");
        } else {
          holidayModeActive = false;
          Serial.println("holidayModeActive not found in config.json, using default.");
        }

        // NEW: Baca timeOffset dari config.json
        if (doc.containsKey("timeOffset")) {
          timeOffset = doc["timeOffset"].as<int>();
          Serial.printf("Time Offset loaded as: %d seconds\n", timeOffset);
          Serial.printf("DEBUG: Global timeOffset after loadConfig (C++): %d seconds\n", timeOffset); // <<< INI YANG SANGAT PENTING
        } else {
          timeOffset = 25200; // Default ke GMT+7 (7 jam * 3600 detik) jika tidak ada di file
          Serial.println("timeOffset not found in config.json, using default GMT+7.");
          Serial.printf("DEBUG: Global timeOffset set to default (C++): %d seconds\n", timeOffset); // <<< INI JUGA PENTING
        }

        // Memuat Pengaturan Channel
        JsonArray channelsJson = doc["channels"];
        if (channelsJson) {
            bool allChannelsLoadedSuccessfully = true;
            for (int i = 0; i < NUM_CHANNELS; i++) { // Pastikan melingkupi semua channel
                channels[i].count = 0; // Reset count untuk channel ini
                JsonArray configs = channelsJson[i]; // Ambil array konfigurasi untuk channel 'i'

                if (configs) { // Jika ada array konfigurasi untuk channel ini
                    for (JsonObject cfg : configs) {
                        if (channels[i].count < MAX_CONFIGS) {
                            channels[i].configs[channels[i].count] = {
                                cfg["onTime"].as<String>(),
                                cfg["offTime"].as<String>(),
                                cfg["enable"]
                            };

                            JsonArray activeDaysJson = cfg["activeDays"];
                            if (activeDaysJson && activeDaysJson.size() == 7) {
                                for (int k = 0; k < 7; k++) {
                                    channels[i].configs[channels[i].count].activeDays[k] = activeDaysJson[k].as<bool>();
                                }
                            } else {
                                Serial.printf("activeDays not found or invalid for CH%d config %d, setting all true.\n", i, channels[i].count);
                                // Default all days to true if missing or invalid
                                for (int k = 0; k < 7; k++) {
                                    channels[i].configs[channels[i].count].activeDays[k] = true;
                                }
                            }
                            channels[i].count++;
                        } else {
                             Serial.printf("Max configs reached for channel %d. Skipping further configs.\n", i);
                        }
                    }
                } else {
                    // Jika tidak ada array konfigurasi untuk channel ini, atau array kosong
                    Serial.printf("No configs array found for channel %d in config.json. Initializing default for this channel.\n", i);
                    // Set flag to indicate that we need to re-initialize defaults for channels
                    allChannelsLoadedSuccessfully = false; // Karena ada channel yang tidak valid
                    break; // Keluar dari loop channel karena ada masalah, akan inisialisasi ulang semua channel
                }
            } // End of for (int i = 0; i < NUM_CHANNELS; i++)

            if (!allChannelsLoadedSuccessfully) { // Jika ada masalah dengan channel manapun
                Serial.println("One or more channels failed to load. Re-initializing all channels to default.");
                error = DeserializationError::EmptyInput; // Set error agar inisialisasi default berjalan
            }

        } else { // Jika tidak ada array 'channels' sama sekali di doc
            Serial.println("No 'channels' array found in config.json. Initializing all channels to default.");
            error = DeserializationError::EmptyInput; // Set error agar inisialisasi default berjalan
        }
      } else { // Deserialization error (file corrupt/malformed)
        Serial.print("Failed to deserialize config.json: ");
        Serial.println(error.c_str());
        Serial.println("Attempting to delete corrupted config.json and initialize defaults.");
        LittleFS.remove("/config.json");
        // Error sudah di set, jadi tidak perlu goto. Logic inisialisasi default akan berjalan di bawah.
      }
    } else { // Gagal membuka file (mungkin tidak ada atau rusak)
      Serial.println("Failed to open config.json for reading (might be corrupted or not exist). Attempting to delete and initialize defaults.");
      LittleFS.remove("/config.json"); // Coba hapus jika ada file rusak
      error = DeserializationError::EmptyInput; // Set error agar inisialisasi default berjalan
    }
  }

  // --- REFACTORED DEFAULT INITIALIZATION LOGIC ---
  // Inisialisasi default hanya jika config.json tidak ada ATAU ada error deserialisasi/masalah channel
  if (!configExists || error != DeserializationError::Ok) {
    if (error != DeserializationError::Ok) {
        Serial.println("Config error detected during load or channel data invalid. Initializing ALL config to default.");
    } else if (!configExists) {
        Serial.println("Config file not found. Initializing ALL config to default.");
    }

    // Inisialisasi pengaturan global ke default
    ntpServer = "pool.ntp.org";
    lastSyncTime = "Never";
    holidayModeActive = false;
    timeOffset = 25200; // Default time offset ke GMT+7 (7 jam * 3600 detik)

    // Inisialisasi semua channel ke default
    for (int i = 0; i < NUM_CHANNELS; i++) {
        channels[i].count = 1; // Satu konfigurasi default
        channels[i].configs[0] = {"00:00:00", "00:00:00", false}; // Off by default
        for (int k = 0; k < 7; k++) {
            channels[i].configs[0].activeDays[k] = true; // Aktif setiap hari secara default
        }
    }
    saveConfig(); // Simpan konfigurasi default yang baru
    Serial.println("Default configuration initialized and saved.");
  }
}

void saveConfig() {
  DynamicJsonDocument doc(2048); 
  doc["ntpServer"] = ntpServer;
  doc["lastSyncTime"] = lastSyncTime;
  doc["holidayModeActive"] = holidayModeActive; 
	// NEW: Simpan timeOffset ke config.json
	doc["timeOffset"] = timeOffset;
  
  JsonArray channelsArray = doc.createNestedArray("channels");
  for (int i = 0; i < NUM_CHANNELS; i++) {
    JsonArray configs = channelsArray.createNestedArray();
    for (int j = 0; j < channels[i].count; j++) {
      JsonObject cfg = configs.createNestedObject();
      cfg["onTime"] = channels[i].configs[j].onTime;
      cfg["offTime"] = channels[i].configs[j].offTime;
      cfg["enable"] = channels[i].configs[j].enable;
      
      // NEW: Simpan activeDays ke JSON
      JsonArray activeDaysJson = cfg.createNestedArray("activeDays");
      for (int k = 0; k < 7; k++) {
          activeDaysJson.add(channels[i].configs[j].activeDays[k]);
      }
    }
  }
  
  File file = LittleFS.open("/config.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
    Serial.println("Config saved to LittleFS.");
  } else {
    Serial.println("Failed to open config.json for writing.");
  }
}

// NEW: Fungsi untuk menginisialisasi default untuk satu channel
void initializeChannelDefaults(int channelIndex) {
  channels[channelIndex].count = 1; 
  channels[channelIndex].configs[0] = {"00:00:00", "00:00:00", false}; 
  for (int k = 0; k < 7; k++) {
      channels[channelIndex].configs[0].activeDays[k] = true;
  }
}

// NEW: Fungsi untuk menginisialisasi default untuk semua channel
void initializeAllChannelsDefaults() {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    initializeChannelDefaults(i);
  }
  holidayModeActive = false; 
  saveConfig(); 
  Serial.println("Default configuration initialized and saved.");
}

// Helper function to initialize a specific channel with default timer configs
void initializeChannelWithDefaults(int channelIndex) {
    if (channelIndex >= 0 && channelIndex < NUM_CHANNELS) {
        channels[channelIndex].count = 1; 
        channels[channelIndex].configs[0] = {"00:00:00", "00:00:00", false}; 
        for (int k = 0; k < 7; k++) {
            channels[channelIndex].configs[0].activeDays[k] = true;
        }
    }
}

bool updateNTPTime() {
  timeClient.setPoolServerName(ntpServer.c_str()); 
  if (timeClient.forceUpdate()) {
    currentTime = formatTime(timeClient.getHours(), timeClient.getMinutes(), timeClient.getSeconds());
    lastSyncTime = currentTime;
    saveConfig(); 
    Serial.println("NTP Time synced: " + currentTime);
    return true;
  }
  Serial.println("NTP Time sync failed.");
  return false;
}
