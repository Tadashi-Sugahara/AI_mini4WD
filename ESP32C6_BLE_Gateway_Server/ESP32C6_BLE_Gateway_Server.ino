#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <esp_bt.h> // ★この1行を追加

// --- WiFi(アクセスポイント)設定 ---
const char* ssid = "ESP32_Controller"; 
const char* password = "password123";  

WebServer server(80);

// --- BLE(UART)設定 ---
static BLEUUID serviceUUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
static BLEUUID charTX_UUID("6e400002-b5a3-f393-e0a9-e50e24dcca9e");
static BLEUUID charRX_UUID("6e400003-b5a3-f393-e0a9-e50e24dcca9e");

static boolean doConnect = false;
static boolean connected = false;
static boolean doScan = false;
static BLERemoteCharacteristic* pRemoteCharTX;
static BLERemoteCharacteristic* pRemoteCharRX;
static BLEAdvertisedDevice* myDevice;

// 受信用バッファ
String bleBuffer = "";

// --- SFC風 Web UI (1.1倍サイズ) ---
const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>BLE Controller</title>
  <style>
    body { 
      background-color: #d0d0d0; 
      display: flex; 
      justify-content: center; 
      align-items: center; 
      height: 100vh; 
      margin: 0; 
      font-family: sans-serif; 
      user-select: none;
      -webkit-user-select: none;
      touch-action: none; 
      overflow: hidden;
    }
    .gamepad { 
      background: #e0e0e0; 
      border-radius: 66px; 
      padding: 44px; 
      box-shadow: inset 0 -5px 15px rgba(0,0,0,0.2), 0 10px 20px rgba(0,0,0,0.3); 
      display: flex; 
      gap: 88px; 
      align-items: center; 
    }
    
    .dpad { position: relative; width: 132px; height: 132px; }
    .dpad-btn { position: absolute; background: #444; width: 44px; height: 44px; border-radius: 6px; box-shadow: inset 0 2px 6px rgba(255,255,255,0.2), 0 2px 6px rgba(0,0,0,0.5); border: none; cursor: pointer; }
    .dpad-btn:active { background: #222; }
    .dpad-up { top: 0; left: 44px; }
    .dpad-down { bottom: 0; left: 44px; }
    .dpad-left { top: 44px; left: 0; }
    .dpad-right { top: 44px; right: 0; }
    .dpad-center { position: absolute; top: 44px; left: 44px; width: 44px; height: 44px; background: #444; }

    .action-buttons { position: relative; width: 176px; height: 176px; }
    .btn { position: absolute; width: 55px; height: 55px; border-radius: 50%; border: none; font-size: 22px; font-weight: bold; color: rgba(255,255,255,0.9); box-shadow: 2px 2px 6px rgba(0,0,0,0.3); cursor: pointer; display: flex; justify-content: center; align-items: center; }
    .btn:active { transform: scale(0.95); box-shadow: 1px 1px 2px rgba(0,0,0,0.5); }
    
    .btn-X { top: 0; left: 61px; background: #0044cc; } 
    .btn-B { top: 121px; left: 61px; background: #ffcc00; color: rgba(0,0,0,0.5); } 
    .btn-Y { top: 61px; left: 0; background: #009933; } 
    .btn-A { top: 61px; left: 121px; background: #cc0000; } 
  </style>
  <script>
    function sendCmd(c) { fetch('/cmd?c=' + c); }
  </script>
</head>
<body>
  <div class="gamepad">
    <div class="dpad">
      <div class="dpad-center"></div>
      <button class="dpad-btn dpad-up" onmousedown="sendCmd('f')" ontouchstart="sendCmd('f')"></button>
      <button class="dpad-btn dpad-down" onmousedown="sendCmd('b')" ontouchstart="sendCmd('b')"></button>
      <button class="dpad-btn dpad-left"></button>
      <button class="dpad-btn dpad-right"></button>
    </div>
    <div class="action-buttons">
      <button class="btn btn-X" onmousedown="sendCmd('s')" ontouchstart="sendCmd('s')">X</button>
      <button class="btn btn-Y" onmousedown="sendCmd('u')" ontouchstart="sendCmd('u')">Y</button>
      <button class="btn btn-B" onmousedown="sendCmd('a')" ontouchstart="sendCmd('a')">B</button>
      <button class="btn btn-A" onmousedown="sendCmd('d')" ontouchstart="sendCmd('d')">A</button>
    </div>
  </div>
</body>
</html>
)rawliteral";

// --- BLE通知受信コールバック (厳格なデータフィルター付き) ---
static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  for (int i = 0; i < length; i++) {
    char c = (char)pData[i];
    
    // 不要なキャリッジリターンは無視
    if (c == '\r') continue;
    
    if (c == '\n') {
      bleBuffer.trim(); 
      
      // ★追加: カンマの数を数えて、正常な6軸データかチェックする
      int commaCount = 0;
      for (int j = 0; j < bleBuffer.length(); j++) {
        if (bleBuffer.charAt(j) == ',') {
          commaCount++;
        }
      }
      
      // カンマが正確に5個（データが6ブロック）の時だけシリアル出力する
      if (commaCount == 5) {
        Serial.println(bleBuffer);
      }
      
      // バッファをリセット
      bleBuffer = ""; 
    } else {
      bleBuffer += c;
      
      // フェイルセーフ: 万が一改行が来ずにバッファが肥大化したら強制リセット (80文字)
      if (bleBuffer.length() > 80) {
        bleBuffer = "";
      }
    }
  }
}

// --- BLEクライアント接続処理 ---
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) { connected = true; }
  void onDisconnect(BLEClient* pclient) { connected = false; }
};

bool connectToServer() {
  BLEClient* pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  pClient->setMTU(512); 
  
  pClient->connect(myDevice);

  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    pClient->disconnect();
    return false;
  }

  pRemoteCharTX = pRemoteService->getCharacteristic(charTX_UUID);
  pRemoteCharRX = pRemoteService->getCharacteristic(charRX_UUID);

  if (pRemoteCharTX == nullptr || pRemoteCharRX == nullptr) {
    pClient->disconnect();
    return false;
  }

  if (pRemoteCharRX->canNotify()) {
    pRemoteCharRX->registerForNotify(notifyCallback);
    
    BLERemoteDescriptor* pDesc = pRemoteCharRX->getDescriptor(BLEUUID((uint16_t)0x2902));
    if(pDesc != nullptr) {
      uint8_t notificationOn[] = {0x1, 0x0};
      pDesc->writeValue(notificationOn, 2, true);
    }
  }
  return true;
}

// --- BLEスキャン処理 ---
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getName() == "nRF52_MotorCtrl") {
      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      doScan = true;
    }
  }
};

// --- Webサーバーのルーティング設定 ---
void handleRoot() {
  server.send(200, "text/html", htmlPage);
}

void handleCmd() {
  if (server.hasArg("c")) {
    String cmd = server.arg("c");
    if (connected && pRemoteCharTX != nullptr) {
      cmd += "\n";
      pRemoteCharTX->writeValue(cmd.c_str(), cmd.length());
    }
  }
  server.send(200, "text/plain", "OK");
}

void setup() {
  // ★追加: XIAO ESP32-C6のRFスイッチを「内蔵アンテナ」に明示的に接続する
  pinMode(3, OUTPUT);
  digitalWrite(3, LOW);  // RFスイッチ機能自体を有効化
  delay(100);
  pinMode(14, OUTPUT);
  digitalWrite(14, LOW); // LOW = 内蔵セラミックアンテナを使用 (HIGHだと外部端子)


  Serial.begin(115200);

  Serial.println("\nStarting Access Point...");
  WiFi.softAP(ssid, password);
  IPAddress myIP = WiFi.softAPIP();
  
  Serial.print("AP Started! SSID: ");
  Serial.println(ssid);
  Serial.print("Access IP: ");
  Serial.println(myIP); 

  server.on("/", handleRoot);
  server.on("/cmd", handleCmd);
  server.begin();

  BLEDevice::init("");

  // ★追加: ESP32C6側のBLEの電波強度を最大レベルに固定する
  // (接続中の通信パワーと、スキャン時の探査パワーを最大化)
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, ESP_PWR_LVL_P9);
  
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(5, false);
}

void loop() {
  server.handleClient();

  if (doConnect == true) {
    if (connectToServer()) {
      Serial.println("Ready to control and receive IMU data!");
    } else {
      Serial.println("Connection failed, retrying...");
    }
    doConnect = false;
  }

  if (!connected) {
    if(doScan){
      BLEDevice::getScan()->start(0);  
    }
  }
  
  delay(2); 
}