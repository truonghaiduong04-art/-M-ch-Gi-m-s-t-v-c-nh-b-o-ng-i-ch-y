// =====================================================================
// RUNNER MONITOR - ESP32 + BMP180 + MQTT + FreeRTOS
// ---------------------------------------------------------------------
// Task truyen data  : Core 0 - WiFi, MQTT, doc "data Queue" -> gui broker
// Task doc data     : Core 1 - doc BMP180, in Serial, gui "data Queue" + "data Alert"
// Task canh bao     : Core 1 - doc "data Alert", dieu khien LED / coi
// Task nut nhan     : Core 1 - doc 2 nut vat ly, gui lenh vao hang doi lenh
//
// Canh bao (che do AUTO):
//   Nhiet do ngoai 22..30 C                         -> LED sang
//   Do cao (tinh tu ap suat) doi > 8 m trong 10 s   -> coi keu 10 s
//   Nut LED / web tat LED : tat LED cua canh bao nhiet do hien tai
//   Nut COI / web tat coi : tat coi cua canh bao do cao hien tai
//   LED tu sang lai khi nhiet do: ve binh thuong roi vuot nguong lai,
//     hoac doi phia (dang nong -> lanh, lanh -> nong),
//     hoac xau them >= 2 C so voi luc bam tat
//   Coi tu keu lai khi co lan thay doi do cao dot ngot moi
// Che do MANUAL: nut vat ly va nut web bat/tat truc tiep LED va coi.
//
// MQTT (khop voi runner_monitor_dashboard.html):
//   TOPIC_DATA (ESP32 -> web), JSON, retained:
//     {"t":26.12,"p":1012.35,"h":12.30,"led":1,"buzzer":0,"mode":"auto",
//      "alert":"TEMP_WARNING","alarmLed":1,"alarmBuzzer":0,"muteLed":0,"muteBuzzer":0}
//   TOPIC_CMD (web -> ESP32): led_on | led_off | buzzer_on | buzzer_off | auto | manual
// =====================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <PubSubClient.h>
#include <math.h>

// =======================
// 1. CAU HINH CHAN
// =======================
#define LED_PIN       19
#define BUZZER_PIN    18
#define BUTTON_LED    25      // dien tro keo xuong ngoai: nhan = HIGH
#define BUTTON_BUZZER 26
#define BMP_SDA       21
#define BMP_SCL       22
#define BUZZER_FREQ   2000

// =======================
// 2. WIFI + MQTT
// =======================
const char* WIFI_SSID   = "Wokwi-GUEST";
const char* WIFI_PASS   = "";
const char* MQTT_SERVER = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;

// Phai trung voi TOPIC_DATA / TOPIC_CMD trong file web
const char* TOPIC_DATA = "runner_truongduong_2026/data";
const char* TOPIC_CMD  = "runner_truongduong_2026/cmd";

// =======================
// 3. NGUONG CANH BAO
// =======================
// BMP180 do nhiet do MOI TRUONG, khong phai nhiet do co the runner.
const float TEMP_MIN = 22.0f;            // duoi nguong nay -> LED sang
const float TEMP_MAX = 30.0f;            // tren nguong nay -> LED sang
const float TEMP_REALERT_DELTA = 2.0f;   // da tat LED ma nhiet do xau them >= 2 C -> LED sang lai

const float    ALT_CHANGE_LIMIT = 8.0f;  // m
const uint8_t  ALT_WINDOW       = 10;    // so mau trong cua so (1 mau/giay = 10 s)
const uint32_t ALT_HOLD_MS      = 10000; // giu canh bao do cao 10 s

const float    SEA_LEVEL_PRESSURE_PA = 101325.0f;
const uint32_t SENSOR_PERIOD_MS = 1000;
const uint32_t BUTTON_PERIOD_MS = 10;
const uint32_t DEBOUNCE_MS      = 50;

// =======================
// 4. KIEU DU LIEU
// =======================
typedef struct { float t; float p; float h; } SensorData;   // -> data Queue
typedef struct { float t; float h; } AlertData;             // -> data Alert

typedef enum {
  CMD_LED_ON, CMD_LED_OFF, CMD_BUZ_ON, CMD_BUZ_OFF,          // tu web
  CMD_AUTO, CMD_MANUAL,                                      // tu web
  CMD_BTN_LED, CMD_BTN_BUZ                                   // tu nut vat ly
} Command;

typedef enum { AL_NORMAL, AL_TEMP_WARNING, AL_ALTITUDE, AL_TEMP_WARNING_ALT } AlertCode;
const char* ALERT_TEXT[] = { "NORMAL", "TEMP_WARNING", "ALTITUDE_CHANGE", "TEMP_WARNING_ALTITUDE" };

typedef struct {
  bool led; bool buzzer; bool autoMode;
  bool muteLed; bool muteBuz;          // AUTO: nguoi dung da tat LED / coi cua canh bao hien tai
  uint8_t alert;
  bool alarmLed; bool alarmBuz;        // dang co canh bao nhiet do / do cao
} Status;

// =======================
// 5. DOI TUONG FreeRTOS
// =======================
QueueHandle_t dataQueue;     // "data Queue"  : Task doc  -> Task truyen
QueueHandle_t alertQueue;    // "data Alert"  : Task doc  -> Task canh bao
QueueHandle_t cmdQueue;      // lenh          : web + nut -> Task canh bao
QueueHandle_t statusQueue;   // hop thu 1 o   : Task canh bao -> Task truyen, Task doc
SemaphoreHandle_t semWiFi;   // co nhi phan   : WiFi da co IP
SemaphoreHandle_t serialMutex;

Adafruit_BMP085 bmp;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
volatile bool mqttOnline = false;

const Status STATUS_INIT = { false, false, true, false, false, AL_NORMAL, false, false };

// =======================
// 6. SERIAL DUNG CHUNG (mutex)
// =======================
void logf(const char* fmt, ...) {
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    Serial.println(buf);
    xSemaphoreGive(serialMutex);
  }
}

static bool khacNhau(const Status& a, const Status& b) {
  return a.led != b.led || a.buzzer != b.buzzer || a.autoMode != b.autoMode ||
         a.muteLed != b.muteLed || a.muteBuz != b.muteBuz ||
         a.alert != b.alert || a.alarmLed != b.alarmLed || a.alarmBuz != b.alarmBuz;
}

// =====================================================================
// TASK DOC DATA - Core 1, uu tien 2
// =====================================================================
void TaskDocData(void* pv) {
  bool ready = bmp.begin();
  TickType_t last = xTaskGetTickCount();

  for (;;) {
    if (!ready) {
      logf("[BMP180] Khong tim thay cam bien, thu lai sau 2 s");
      vTaskDelay(pdMS_TO_TICKS(2000));
      ready = bmp.begin();
      last = xTaskGetTickCount();
      continue;
    }

    SensorData d;
    d.t = bmp.readTemperature();
    int32_t pa = bmp.readPressure();
    if (!isfinite(d.t) || pa <= 0) {
      logf("[BMP180] Du lieu khong hop le!");
      vTaskDelayUntil(&last, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
      continue;
    }
    d.p = pa / 100.0f;                                                     // hPa
    d.h = 44330.0f * (1.0f - powf(pa / SEA_LEVEL_PRESSURE_PA, 0.1903f));  // m

    // Du lieu canh bao (nhiet do + do cao) -> "data Alert"
    AlertData a = { d.t, d.h };
    xQueueSend(alertQueue, &a, 0);

    // Du lieu day du -> "data Queue"; day (mat mang) thi bo goi cu nhat
    if (xQueueSend(dataQueue, &d, 0) != pdTRUE) {
      SensorData old;
      xQueueReceive(dataQueue, &old, 0);
      xQueueSend(dataQueue, &d, 0);
    }

    // Hien thi Serial
    Status st = STATUS_INIT;
    xQueuePeek(statusQueue, &st, 0);
    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      Serial.printf("\n========== RUNNER MONITOR ==========\n");
      Serial.printf("Nhiet do : %.2f C\n", d.t);
      Serial.printf("Ap suat  : %.2f hPa\n", d.p);
      Serial.printf("Do cao   : %.2f m\n", d.h);
      Serial.printf("Che do   : %s | LED: %s | Coi: %s\n",
                    st.autoMode ? "AUTO" : "MANUAL", st.led ? "ON" : "OFF", st.buzzer ? "ON" : "OFF");
      Serial.printf("Canh bao : %s\n", ALERT_TEXT[st.alert]);
      Serial.printf("WiFi: %s | MQTT: %s\n",
                    WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED",
                    mqttOnline ? "CONNECTED" : "DISCONNECTED");
      Serial.printf("====================================\n");
      xSemaphoreGive(serialMutex);
    }

    vTaskDelayUntil(&last, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
  }
}

// =====================================================================
// TASK NUT NHAN - Core 1, uu tien 2 (chong doi phim, gui lenh vao cmdQueue)
// =====================================================================
void TaskNutNhan(void* pv) {
  const uint8_t pins[2] = { BUTTON_LED, BUTTON_BUZZER };
  const Command cmds[2] = { CMD_BTN_LED, CMD_BTN_BUZ };
  bool stable[2], lastRead[2];
  TickType_t changed[2];
  for (int i = 0; i < 2; i++) {
    stable[i] = lastRead[i] = digitalRead(pins[i]);
    changed[i] = xTaskGetTickCount();
  }

  for (;;) {
    TickType_t now = xTaskGetTickCount();
    for (int i = 0; i < 2; i++) {
      bool r = digitalRead(pins[i]);
      if (r != lastRead[i]) {
        lastRead[i] = r;
        changed[i] = now;
      } else if (r != stable[i] && (now - changed[i]) >= pdMS_TO_TICKS(DEBOUNCE_MS)) {
        stable[i] = r;
        if (r == HIGH) {                                     // canh nhan
          xQueueSend(cmdQueue, &cmds[i], 0);
          logf("[NUT] Nhan nut %s", i == 0 ? "LED" : "COI");
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(BUTTON_PERIOD_MS));
  }
}

// =====================================================================
// TASK CANH BAO - Core 1, uu tien 3 (chu duy nhat cua LED va coi)
// =====================================================================
static void ganDauRa(bool led, bool buz) {
  static bool curLed = false, curBuz = false;
  if (led != curLed) { digitalWrite(LED_PIN, led ? HIGH : LOW); curLed = led; }
  if (buz != curBuz) { if (buz) tone(BUZZER_PIN, BUZZER_FREQ); else noTone(BUZZER_PIN); curBuz = buz; }
}

void TaskCanhBao(void* pv) {
  float hist[ALT_WINDOW];
  uint8_t nHist = 0, head = 0;
  float lastT = NAN;
  bool altActive = false;
  TickType_t altUntil = 0;

  bool autoMode = true;
  bool muteLed = false, muteBuz = false;   // AUTO: da tat LED/coi cua canh bao hien tai
  bool manLed = false, manBuz = false;     // MANUAL: trang thai do nguoi dung dat
  bool prevAlt = false;
  float muteT = NAN;        // nhiet do luc bam tat LED
  int8_t muteSide = 0;      // phia luc bam tat: +1 qua nong, -1 qua lanh

  Status st = STATUS_INIT;
  xQueueOverwrite(statusQueue, &st);

  for (;;) {
    // 1. Du lieu tu "data Alert" (cho toi da 50 ms de con xu ly lenh nhanh)
    AlertData a;
    bool suKienDoCao = false;
    if (xQueueReceive(alertQueue, &a, pdMS_TO_TICKS(50)) == pdTRUE) {
      lastT = a.t;
      // Do cao: so voi moi mau trong 10 s gan nhat
      float maxDiff = 0;
      for (uint8_t i = 0; i < nHist; i++) maxDiff = fmaxf(maxDiff, fabsf(a.h - hist[i]));
      if (nHist > 0 && maxDiff > ALT_CHANGE_LIMIT) {
        altActive = true;
        suKienDoCao = true;
        altUntil = xTaskGetTickCount() + pdMS_TO_TICKS(ALT_HOLD_MS);
        logf("[CANH BAO] Do cao thay doi %.1f m trong 10 s", maxDiff);
        nHist = 0; head = 0;                               // bat dau cua so moi tu muc hien tai
      }
      if (nHist < ALT_WINDOW) hist[nHist++] = a.h;
      else { hist[head] = a.h; head = (head + 1) % ALT_WINDOW; }
    }

    // 2. Xac dinh canh bao: nhiet do -> LED, do cao (ap suat) -> coi
    TickType_t now = xTaskGetTickCount();
    if (altActive && (int32_t)(now - altUntil) >= 0) altActive = false;
    bool tempAlarm = isfinite(lastT) && (lastT < TEMP_MIN || lastT > TEMP_MAX);
    int8_t side = !tempAlarm ? 0 : (lastT > TEMP_MAX ? 1 : -1);   // +1 nong, -1 lanh, 0 binh thuong
    bool altAlarm  = altActive;
    AlertCode code = (tempAlarm && altAlarm) ? AL_TEMP_WARNING_ALT
                   : altAlarm  ? AL_ALTITUDE
                   : tempAlarm ? AL_TEMP_WARNING : AL_NORMAL;

    // LED da bi tat: sang lai neu la mot truong hop nhiet do MOI
    if (muteLed) {
      bool doiPhia = (side != muteSide);                     // ve binh thuong, hoac nong <-> lanh
      bool xauHon  = (side == 1  && lastT >= muteT + TEMP_REALERT_DELTA) ||
                     (side == -1 && lastT <= muteT - TEMP_REALERT_DELTA);
      if (doiPhia || xauHon) {
        muteLed = false;
        if (tempAlarm) logf("[CANH BAO] Nhiet do %.1f C: truong hop moi -> LED sang lai", lastT);
      }
    }
    // Coi da bi tat: keu lai khi co lan doi do cao dot ngot moi (hoac canh bao vua het)
    if (altAlarm != prevAlt || suKienDoCao) muteBuz = false;
    prevAlt = altAlarm;

    // 3. Lenh tu web va nut vat ly
    // Ghi lai nhiet do va phia luc tat LED de biet khi nao la truong hop moi
    auto tatLed = [&]() { muteLed = true; muteT = lastT; muteSide = side; };

    Command c;
    while (xQueueReceive(cmdQueue, &c, 0) == pdTRUE) {
      switch (c) {
        case CMD_AUTO:    autoMode = true; muteLed = muteBuz = false; break;
        case CMD_MANUAL:  if (autoMode) { manLed = st.led; manBuz = st.buzzer; } autoMode = false; break;
        case CMD_LED_ON:  if (autoMode) muteLed = false; else manLed = true;  break;
        case CMD_LED_OFF: if (autoMode) tatLed();        else manLed = false; break;
        case CMD_BUZ_ON:  if (autoMode) muteBuz = false; else manBuz = true;  break;
        case CMD_BUZ_OFF: if (autoMode) muteBuz = true;  else manBuz = false; break;
        case CMD_BTN_LED: if (autoMode) { if (muteLed) muteLed = false; else tatLed(); } else manLed = !manLed; break;
        case CMD_BTN_BUZ: if (autoMode) muteBuz = !muteBuz; else manBuz = !manBuz; break;
      }
    }
    // Khong co canh bao thi khong co gi de "tat"
    if (!tempAlarm) muteLed = false;
    if (!altAlarm) muteBuz = false;

    // 4. Quyet dinh dau ra
    bool led, buz;
    if (autoMode) {
      led = tempAlarm && !muteLed;
      buz = altAlarm && !muteBuz;
    } else {
      led = manLed;
      buz = manBuz;
    }
    ganDauRa(led, buz);

    // 5. Bao trang thai moi vao hop thu cho Task truyen va Task doc
    Status moi = { led, buz, autoMode, autoMode && muteLed, autoMode && muteBuz, (uint8_t)code, tempAlarm, altAlarm };
    if (khacNhau(moi, st)) {
      st = moi;
      xQueueOverwrite(statusQueue, &st);
      logf("[CANH BAO] %s | %s | LED %s | COI %s", ALERT_TEXT[code],
           autoMode ? "AUTO" : "MANUAL", led ? "ON" : "OFF", buz ? "ON" : "OFF");
    }
  }
}

// =====================================================================
// TASK TRUYEN DATA - Core 0, uu tien 1
// =====================================================================
void onWiFiEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    logf("[WiFi] Da ket noi, IP = %s", WiFi.localIP().toString().c_str());
    xSemaphoreGive(semWiFi);                                // gio co: WiFi san sang
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    logf("[WiFi] Mat ket noi");
  }
}

// Chay ben trong mqtt.loop() cua Task truyen: chi chuyen lenh, khong cham LED/coi
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_CMD) != 0) return;

  char msg[16];
  unsigned int n = 0;
  for (unsigned int i = 0; i < length && n < sizeof(msg) - 1; i++) {
    char ch = (char)payload[i];
    if (ch != ' ' && ch != '\r' && ch != '\n') msg[n++] = tolower(ch);
  }
  msg[n] = '\0';

  Command c;
  if      (strcmp(msg, "led_on") == 0)     c = CMD_LED_ON;
  else if (strcmp(msg, "led_off") == 0)    c = CMD_LED_OFF;
  else if (strcmp(msg, "buzzer_on") == 0)  c = CMD_BUZ_ON;
  else if (strcmp(msg, "buzzer_off") == 0) c = CMD_BUZ_OFF;
  else if (strcmp(msg, "auto") == 0)       c = CMD_AUTO;
  else if (strcmp(msg, "manual") == 0)     c = CMD_MANUAL;
  else { logf("[MQTT RX] Lenh khong hop le: %s", msg); return; }

  xQueueSend(cmdQueue, &c, 0);
  logf("[MQTT RX] Lenh: %s", msg);
}

bool ketNoiMQTT() {
  char clientId[40];
  snprintf(clientId, sizeof(clientId), "TruongDuongESP32-%08X", (uint32_t)ESP.getEfuseMac());
  logf("[MQTT] Dang ket noi EMQX...");
  if (mqtt.connect(clientId)) {
    mqtt.subscribe(TOPIC_CMD);
    logf("[MQTT] Connected!");
    return true;
  }
  logf("[MQTT] Loi, rc = %d, thu lai sau 5 s", mqtt.state());
  return false;
}

// Gop so do + trang thai thanh 1 ban tin JSON (retained: web mo sau van thay ngay)
void guiJSON(const SensorData& d, const Status& s) {
  char json[256];
  snprintf(json, sizeof(json),
           "{\"t\":%.2f,\"p\":%.2f,\"h\":%.2f,\"led\":%d,\"buzzer\":%d,\"mode\":\"%s\","
           "\"alert\":\"%s\",\"alarmLed\":%d,\"alarmBuzzer\":%d,\"muteLed\":%d,\"muteBuzzer\":%d}",
           d.t, d.p, d.h, s.led, s.buzzer, s.autoMode ? "auto" : "manual",
           ALERT_TEXT[s.alert], s.alarmLed, s.alarmBuz, s.muteLed, s.muteBuz);
  if (mqtt.publish(TOPIC_DATA, json, true)) logf("[MQTT TX] %s", json);
  else logf("[MQTT TX] Gui that bai");
}

void TaskTruyenData(void* pv) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(2);

  SensorData lastData;
  bool coData = false;
  Status daGui = STATUS_INIT;

  for (;;) {
    // "Ket noi WiFi, neu thanh cong..." : chua co WiFi thi nam cho co semWiFi
    if (WiFi.status() != WL_CONNECTED) {
      mqttOnline = false;
      xSemaphoreTake(semWiFi, pdMS_TO_TICKS(2000));
      continue;
    }
    if (!mqtt.connected()) {
      mqttOnline = false;
      if (!ketNoiMQTT()) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
      mqttOnline = true;
    }

    mqtt.loop();                                          // giu ket noi + nhan lenh

    Status st = STATUS_INIT;
    xQueuePeek(statusQueue, &st, 0);

    // "...doc du lieu tu data Queue va gui len MQTT broker"
    SensorData d;
    if (xQueueReceive(dataQueue, &d, pdMS_TO_TICKS(50)) == pdTRUE) {
      lastData = d;
      coData = true;
      guiJSON(d, st);
      daGui = st;
    } else if (coData && khacNhau(st, daGui)) {
      // Trang thai doi (vd vua bam nut) ma chua toi chu ky doc: gui ngay de web phan hoi nhanh
      guiJSON(lastData, st);
      daGui = st;
    }
  }
}

// =====================================================================
// SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(BUTTON_LED, INPUT);        // dien tro keo xuong ngoai theo mach
  pinMode(BUTTON_BUZZER, INPUT);
  Wire.begin(BMP_SDA, BMP_SCL);

  // Tao doi tuong giao tiep TRUOC khi tao task
  dataQueue   = xQueueCreate(5, sizeof(SensorData));
  alertQueue  = xQueueCreate(5, sizeof(AlertData));
  cmdQueue    = xQueueCreate(8, sizeof(Command));
  statusQueue = xQueueCreate(1, sizeof(Status));
  semWiFi     = xSemaphoreCreateBinary();
  serialMutex = xSemaphoreCreateMutex();

  Serial.println("RUNNER MONITOR (FreeRTOS) STARTING...");

  //                      ham             ten          stack tham so uu tien handle core
  xTaskCreatePinnedToCore(TaskCanhBao,    "CanhBao",    4096, NULL,   3,      NULL,  1);
  xTaskCreatePinnedToCore(TaskDocData,    "DocData",    4096, NULL,   2,      NULL,  1);
  xTaskCreatePinnedToCore(TaskNutNhan,    "NutNhan",    4096, NULL,   2,      NULL,  1);
  xTaskCreatePinnedToCore(TaskTruyenData, "TruyenData", 8192, NULL,   1,      NULL,  0);
}

void loop() {
  vTaskDelete(NULL);   // khong dung loopTask
}
