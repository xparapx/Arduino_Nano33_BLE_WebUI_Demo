// ════════════════════════════════════════════════════
// Arduino Nano 33 BLE Sense (Rev1) — 대시보드 펌웨어 v2.1
// USB Serial + BLE 동시 지원 / nRF52840 DEVICEID 기반 식별
// ════════════════════════════════════════════════════
// [고유 ID 방식]
//   nRF52840 칩 내장 FICR DEVICEID 레지스터 사용
//   공장 출하 시 각인된 64bit 값 → 변경 불가, 전 세계 유일
//   뒤 4자리(16진수)를 장치명으로 사용
//   예) DEVICEID = ...A3F7 → 장치명: BLE-A3F7
//
// [라이브러리]
//   ArduinoBLE            ← BLE
//   Arduino_LSM9DS1       ← IMU
//   Arduino_HTS221        ← 온습도
//   Arduino_LPS22HB       ← 기압
//   Arduino_APDS9960      ← 조도/근접
//   PDM                   ← 마이크 (자동 포함)
// ════════════════════════════════════════════════════
#include <ArduinoBLE.h>
#include <Arduino_LSM9DS1.h>
#include <Arduino_HTS221.h>
#include <Arduino_LPS22HB.h>
#include <Arduino_APDS9960.h>
#include <PDM.h>

// ── 고유 디바이스 ID 추출 ─────────────────────────────
// nRF52840 FICR 레지스터에서 64bit ID의 하위 16bit(4자리 hex) 추출
String getDeviceID() {
  uint32_t id = NRF_FICR->DEVICEID[0];  // 하위 32bit
  // 하위 16bit → 4자리 16진수 문자열
  String hex = String(id & 0xFFFF, HEX);
  hex.toUpperCase();
  // 4자리 미만이면 앞에 0 패딩
  while (hex.length() < 4) hex = "0" + hex;
  return hex;
}

// ── 전역 변수 ─────────────────────────────────────────
String deviceID   = "";   // 예: "A3F7"
String deviceName = "";   // 예: "BLE-A3F7"

float         sampleRate     = 1.0;
unsigned long sampleInterval = 1000;
bool active[7] = {true,true,true,true,true,true,true};
int  htsFailCount = 0;
bool bleConnected = false;

// ── BLE 서비스 / 특성 ─────────────────────────────────
BLEService sensorService("19B10000-E8F2-537E-4F6C-D104768A1214");
BLEStringCharacteristic chImuAcc ("19B10001-E8F2-537E-4F6C-D104768A1214", BLENotify|BLERead, 20);
BLEStringCharacteristic chImuGyr ("19B10002-E8F2-537E-4F6C-D104768A1214", BLENotify|BLERead, 20);
BLEStringCharacteristic chImuMag ("19B10003-E8F2-537E-4F6C-D104768A1214", BLENotify|BLERead, 20);
BLEStringCharacteristic chTempHum("19B10004-E8F2-537E-4F6C-D104768A1214", BLENotify|BLERead, 20);
BLEStringCharacteristic chPress  ("19B10005-E8F2-537E-4F6C-D104768A1214", BLENotify|BLERead, 20);
BLEStringCharacteristic chLight  ("19B10006-E8F2-537E-4F6C-D104768A1214", BLENotify|BLERead, 20);
BLEStringCharacteristic chMic    ("19B10007-E8F2-537E-4F6C-D104768A1214", BLENotify|BLERead, 20);
// 디바이스 정보 Characteristic (Read 전용 — 대시보드가 ID 확인용)
BLEStringCharacteristic chDeviceInfo("19B10008-E8F2-537E-4F6C-D104768A1214", BLERead, 20);
// 커맨드 수신 (Write)
BLEStringCharacteristic chCmd    ("19B10010-E8F2-537E-4F6C-D104768A1214", BLEWrite, 20);

// ── PDM 마이크 ───────────────────────────────────────
short micBuf[256];
float micLevel = 0;

void onPDMdata() {
  int   bytes = PDM.available();
  PDM.read(micBuf, bytes);
  int   n = bytes / 2;
  float sum = 0;
  for (int i = 0; i < n; i++) sum += abs(micBuf[i]);
  float rms = sum / n;
  micLevel = (rms > 0) ? 20.0 * log10(rms / 32768.0) + 100.0 : 0;
}

// ── HTS221 초기화 (재시도) ───────────────────────────
bool initHTS() {
  for (int i = 0; i < 5; i++) {
    if (HTS.begin()) {
      delay(50);
      if (HTS.readTemperature() != 0.0 || HTS.readHumidity() != 0.0) {
        Serial.println("# HTS221 초기화 성공 (" + String(i+1) + "회)");
        return true;
      }
      HTS.end();
    }
    Serial.println("# HTS221 재시도 " + String(i+1));
    delay(200);
  }
  return false;
}

// ── 데이터 전송 (USB Serial + BLE 동시) ──────────────
void sendData(String line, BLEStringCharacteristic& ch) {
  Serial.println(line);
  if (bleConnected)
    ch.writeValue(line.substring(0, min((int)line.length(), 20)));
}

// ════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  while (!Serial);

  // ── 고유 ID 추출 ──────────────────────────────────
  deviceID   = getDeviceID();
  deviceName = "BLE-" + deviceID;

  Serial.println("# ═══════════════════════════════════");
  Serial.println("# Arduino Nano 33 BLE Sense Rev1 v2.1");
  Serial.println("# DEVICE ID : " + deviceID);
  Serial.println("# BLE 장치명: " + deviceName);
  Serial.println("# 대시보드 ID 입력란에 위 DEVICE ID를 입력하세요");
  Serial.println("# ═══════════════════════════════════");

  // ── BLE 초기화 ────────────────────────────────────
  if (!BLE.begin()) {
    Serial.println("# [ERROR] BLE 초기화 실패");
  } else {
    BLE.setLocalName(deviceName.c_str());
    BLE.setAdvertisedService(sensorService);

    sensorService.addCharacteristic(chImuAcc);
    sensorService.addCharacteristic(chImuGyr);
    sensorService.addCharacteristic(chImuMag);
    sensorService.addCharacteristic(chTempHum);
    sensorService.addCharacteristic(chPress);
    sensorService.addCharacteristic(chLight);
    sensorService.addCharacteristic(chMic);
    sensorService.addCharacteristic(chDeviceInfo);
    sensorService.addCharacteristic(chCmd);

    // 디바이스 정보 Characteristic에 ID 기록
    // → 대시보드가 BLE 연결 전 ID만으로 장치 특정 가능
    chDeviceInfo.writeValue(deviceID);

    BLE.addService(sensorService);
    BLE.advertise();
    Serial.println("# BLE 광고 시작: " + deviceName);
  }

  // ── 센서 초기화 ───────────────────────────────────
  if (!IMU.begin())  Serial.println("# [WARN] IMU(LSM9DS1) 초기화 실패");
  delay(500);
  if (!initHTS())    Serial.println("# [ERROR] HTS221 초기화 실패");
  if (!BARO.begin()) Serial.println("# [WARN] LPS22HB 초기화 실패");
  if (!APDS.begin()) Serial.println("# [WARN] APDS9960 초기화 실패");

  PDM.onReceive(onPDMdata);
  if (!PDM.begin(1, 16000)) Serial.println("# [WARN] PDM 마이크 초기화 실패");

  delay(300);
  Serial.println("# 초기화 완료 — 명령 대기 중");
}

// ── 커맨드 파서 ──────────────────────────────────────
String cmdBuf = "";

void parseCmd(String cmd) {
  cmd.trim();
  if (cmd.startsWith("RATE:")) {
    sampleRate = cmd.substring(5).toFloat();
    if (sampleRate <= 0) sampleRate = 1;
    sampleInterval = (unsigned long)(1000.0 / sampleRate);
    Serial.println("# RATE=" + String(sampleRate) + "Hz");
  }
  else if (cmd.startsWith("SENSORS:")) {
    String list = cmd.substring(8);
    for (int i = 0; i < 7; i++) active[i] = false;
    if (list.indexOf("imu_acc")  >= 0) active[0] = true;
    if (list.indexOf("imu_gyr")  >= 0) active[1] = true;
    if (list.indexOf("imu_mag")  >= 0) active[2] = true;
    if (list.indexOf("temp_hum") >= 0) active[3] = true;
    if (list.indexOf("pressure") >= 0) active[4] = true;
    if (list.indexOf("light")    >= 0) active[5] = true;
    if (list.indexOf("mic")      >= 0) active[6] = true;
    Serial.println("# SENSORS=" + list);
  }
  else if (cmd == "START")  Serial.println("# 수집 시작");
  else if (cmd == "STOP")   Serial.println("# 수집 종료");
  else if (cmd == "PING")   Serial.println("# PONG");
  // ID 조회 커맨드 — 시리얼로도 확인 가능
  else if (cmd == "ID?")    Serial.println("# ID: " + deviceID + " / " + deviceName);
}

unsigned long lastSample = 0;

void loop() {
  // ── BLE 연결 관리 ────────────────────────────────
  BLEDevice central = BLE.central();
  if (central) {
    if (!bleConnected) {
      bleConnected = true;
      Serial.println("# BLE 연결됨: " + central.address());
    }
    if (chCmd.written()) parseCmd(chCmd.value());
  } else {
    if (bleConnected) {
      bleConnected = false;
      Serial.println("# BLE 연결 해제됨");
    }
  }

  // ── USB Serial 커맨드 ────────────────────────────
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { parseCmd(cmdBuf); cmdBuf = ""; }
    else           { cmdBuf += c; }
  }

  // ── 샘플링 ───────────────────────────────────────
  unsigned long now = millis();
  if (now - lastSample < sampleInterval) return;
  lastSample = now;

  if (active[0] && IMU.accelerationAvailable()) {
    float ax,ay,az; IMU.readAcceleration(ax,ay,az);
    sendData("imu_acc,"+String(ax*9.81,3)+","+String(ay*9.81,3)+","+String(az*9.81,3)+","+String(now), chImuAcc);
  }
  if (active[1] && IMU.gyroscopeAvailable()) {
    float gx,gy,gz; IMU.readGyroscope(gx,gy,gz);
    sendData("imu_gyr,"+String(gx,2)+","+String(gy,2)+","+String(gz,2)+","+String(now), chImuGyr);
  }
  if (active[2] && IMU.magneticFieldAvailable()) {
    float mx,my,mz; IMU.readMagneticField(mx,my,mz);
    sendData("imu_mag,"+String(mx,2)+","+String(my,2)+","+String(mz,2)+","+String(now), chImuMag);
  }
  if (active[3]) {
    float temp = HTS.readTemperature();
    float hum  = HTS.readHumidity();
    if (temp == 0.0 && hum == 0.0) {
      htsFailCount++;
      if (htsFailCount >= 5) { htsFailCount=0; HTS.end(); delay(300); initHTS(); }
    } else {
      htsFailCount = 0;
      sendData("temp_hum,"+String(temp,1)+","+String(hum,1)+","+String(now), chTempHum);
    }
  }
  if (active[4]) {
    sendData("pressure,"+String(BARO.readPressure(),2)+","+String(now), chPress);
  }
  if (active[5]) {
    int prox=0,r=0,g=0,b=0;
    if (APDS.proximityAvailable()) prox=APDS.readProximity();
    if (APDS.colorAvailable())     APDS.readColor(r,g,b);
    sendData("light,"+String(prox)+","+String(r)+","+String(g)+","+String(b)+","+String(now), chLight);
  }
  if (active[6]) {
    sendData("mic,"+String(micLevel,1)+","+String(now), chMic);
  }
}
