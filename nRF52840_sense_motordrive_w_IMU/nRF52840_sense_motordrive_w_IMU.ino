#include <bluefruit.h>
#include <Wire.h>
#include "LSM6DS3.h"

// IMUのインスタンス作成 (I2C通信, アドレス0x6A)
LSM6DS3 myIMU(I2C_MODE, 0x6A);

// BLE UARTサービス
BLEUart bleuart;

// XIAO nRF52840 のRGB LEDピン定義
const int LEDR = 11;
const int LEDG = 12;
const int LEDB = 13;

// ピン割り当て (IN3はD6)
const int PIN_IN1 = 2;  
const int PIN_IN2 = 10; 
const int PIN_IN3 = 6;  
const int PIN_IN4 = 8;  

enum MotorState {
  STATE_COAST,
  STATE_FORWARD,
  STATE_BACKWARD,
  STATE_BRAKE
};

MotorState currentState = STATE_COAST;
int currentDuty = 0; 

const int DUTY_MIN_RUN = 204; // 80%
const int DUTY_MAX_RUN = 255; // 100%

unsigned long previousMillis = 0;
const long blinkInterval = 500;
bool blueLedState = false;

// IMU自動送信用の変数
unsigned long previousImuMillis = 0;
const long imuInterval = 100; // 100msec更新

void setup() {
  Serial.begin(115200);
  
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  pinMode(PIN_IN3, OUTPUT);
  pinMode(PIN_IN4, OUTPUT);

  pinMode(LEDR, OUTPUT);
  pinMode(LEDG, OUTPUT);
  pinMode(LEDB, OUTPUT);
  
  digitalWrite(LEDR, HIGH);
  digitalWrite(LEDG, HIGH);
  digitalWrite(LEDB, HIGH);

  motorCoast();

  if (myIMU.begin() != 0) {
    Serial.println("IMU Initialization Error!");
  } else {
    Serial.println("IMU Initialization OK!");
  }

  // ★修正: 存在しない configMtu は削除し、帯域拡張のみを begin() の「前」に実行
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  
  Bluefruit.begin();
  Bluefruit.setTxPower(8);
  Bluefruit.setName("nRF52_MotorCtrl");
  bleuart.begin();
  startAdv();

  Serial.println("--- BLE Motor Control Ready ---");
}

void startAdv(void) {
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0); 
}

void loop() {
  updateLED();

  // IMUデータの自動送信 (BLE接続中のみ)
  if (Bluefruit.connected()) {
    unsigned long currentMillis = millis();
    if (currentMillis - previousImuMillis >= imuInterval) {
      previousImuMillis = currentMillis;
      sendIMUData();
    }
  }

  if (bleuart.available() > 0) {
    String input = bleuart.readStringUntil('\n');
    input.trim();
    if (input.length() == 0) return;

    Serial.print("BLE Received: ");
    Serial.println(input);

    if (input == "f") {
      changeState(STATE_FORWARD);
    } 
    else if (input == "b") {
      changeState(STATE_BACKWARD);
    } 
    else if (input == "s") {
      changeState(STATE_BRAKE);
    } 
    else if (input == "a") {
      changeState(STATE_COAST);
    } 
    else if (input == "u") {
      if (currentState == STATE_FORWARD || currentState == STATE_BACKWARD) {
        if (currentDuty == 0) {
          currentDuty = DUTY_MIN_RUN; 
        } else {
          currentDuty += 5;
        }
        if (currentDuty > DUTY_MAX_RUN) currentDuty = DUTY_MAX_RUN;
        applySpeedChange();
      } else {
        Serial.println("Warn: Set f or b first");
      }
    }
    else if (input == "d") {
      if (currentState == STATE_FORWARD || currentState == STATE_BACKWARD) {
        if (currentDuty > 0) {
          currentDuty -= 5;
          if (currentDuty < DUTY_MIN_RUN) currentDuty = DUTY_MIN_RUN;
        }
        applySpeedChange();
      } else {
        Serial.println("Warn: Set f or b first");
      }
    }
  }
}

// ----------------------------------------------------
// IMUデータ送信関数 (文字列結合を廃止し、安全なバッファ生成に変更)
// ----------------------------------------------------
void sendIMUData() {
  float ax = myIMU.readFloatAccelX();
  float ay = myIMU.readFloatAccelY();
  float az = myIMU.readFloatAccelZ();
  float gx = myIMU.readFloatGyroX();
  float gy = myIMU.readFloatGyroY();
  float gz = myIMU.readFloatGyroZ();

  // snprintfで一撃でフォーマットする (1行のCSV形式)
  char buf[64];
  snprintf(buf, sizeof(buf), "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f", ax, ay, az, gx, gy, gz);
  
  bleuart.println(buf);
}

// ----------------------------------------------------
// LED制御関数
// ----------------------------------------------------
void updateLED() {
  if (!Bluefruit.connected()) {
    digitalWrite(LEDR, HIGH); 
    digitalWrite(LEDG, HIGH); 
    
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= blinkInterval) {
      previousMillis = currentMillis;
      blueLedState = !blueLedState;
      digitalWrite(LEDB, blueLedState ? LOW : HIGH);
    }
  } 
  else {
    digitalWrite(LEDB, HIGH);
    if (currentState == STATE_FORWARD) {
      digitalWrite(LEDG, LOW);  
      digitalWrite(LEDR, HIGH); 
    } 
    else if (currentState == STATE_BACKWARD) {
      digitalWrite(LEDR, LOW);  
      digitalWrite(LEDG, HIGH); 
    } 
    else {
      digitalWrite(LEDR, HIGH);
      digitalWrite(LEDG, HIGH);
    }
  }
}

// ----------------------------------------------------
// 状態遷移・モーター制御関数群
// ----------------------------------------------------
void applySpeedChange() {
  // BLEへの送信を廃止。PCシリアルでのみ確認する
  Serial.println("Duty: " + String(currentDuty) + "/255");

  if (currentState == STATE_FORWARD) {
    motorForward(currentDuty);
  } else if (currentState == STATE_BACKWARD) {
    motorReverse(currentDuty);
  }
}

void changeState(MotorState nextState) {
  if (currentState == nextState) return;

  if (nextState == STATE_FORWARD || nextState == STATE_BACKWARD) {
    if (currentState != STATE_BRAKE && currentState != STATE_COAST) {
      Serial.println("Warn: Stop first");
      return;
    }
  }

  if (nextState == STATE_BRAKE || nextState == STATE_COAST) {
    currentDuty = 0;
  }

  motorCoast();
  delay(20); 

  currentState = nextState;
  
  // BLEへのステータス送信を廃止し、IMUデータのみを流す
  if (currentState == STATE_FORWARD) Serial.println("State: Forward (Duty 0)");
  else if (currentState == STATE_BACKWARD) Serial.println("State: Backward (Duty 0)");
  else if (currentState == STATE_BRAKE) { motorBrake(); Serial.println("State: Brake"); }
  else if (currentState == STATE_COAST) Serial.println("State: Coast");
}

void motorCoast() {
  analogWrite(PIN_IN2, 0);
  analogWrite(PIN_IN4, 0);
  digitalWrite(PIN_IN2, LOW);
  digitalWrite(PIN_IN4, LOW);
  delayMicroseconds(50);
  digitalWrite(PIN_IN1, HIGH);
  digitalWrite(PIN_IN3, HIGH);
}

void motorForward(int duty) {
  digitalWrite(PIN_IN3, HIGH); 
  analogWrite(PIN_IN2, 0);     
  digitalWrite(PIN_IN2, LOW);  

  digitalWrite(PIN_IN1, LOW); 
  analogWrite(PIN_IN4, duty); 
}

void motorReverse(int duty) {
  digitalWrite(PIN_IN1, HIGH); 
  analogWrite(PIN_IN4, 0);     
  digitalWrite(PIN_IN4, LOW);  

  digitalWrite(PIN_IN3, LOW); 
  analogWrite(PIN_IN2, duty); 
}

void motorBrake() {
  analogWrite(PIN_IN2, 0);
  analogWrite(PIN_IN4, 0);
  digitalWrite(PIN_IN1, HIGH);
  digitalWrite(PIN_IN3, HIGH);
  delayMicroseconds(50);
  digitalWrite(PIN_IN2, HIGH);
  digitalWrite(PIN_IN4, HIGH);
}