
#define TINY_GSM_MODEM_BG96

#include <Arduino.h>
#include <PubSubClient.h>
#include <serial_log.h>
#include <mqtt_cmd.h>
#include <ota_rtk.h>
#include <ota_selftest.h>
#include "net_mode.h" // NET_MODE_GSM / NET_MODE_WIFI / NET_MODE_AUTO

#if NET_HAS_GSM
#include <TinyGsmClient.h>
#endif
#if NET_HAS_WIFI
#include <WiFi.h>
#include "secrets.h"
#endif

#define FW_NAME "base_rtk"
#define VERSION_FW "v1.0.0"
// Dau thoi gian bien dich: hai ban OTA cung so version van phan biet duoc.
#define FW_BUILD __DATE__ " " __TIME__

#define GPS_RX 4
#define GPS_TX 5
#define GPS_BAUD 115200

#define MODEM_RX 8
#define MODEM_TX 18

#define SURVEY_PIN 10
#define LED_SURVEY_IN 11
#define LED_PING 7

#define SURVEY_SEC 20
#define SURVEY_ACC 4000 // svinAccLimit, đơn vị 0.1mm (4000 = 40cm)

// ---------- Transport ----------
// Moi duong mang co HAI socket: mot cho MQTT (giu ket noi lien tuc), mot RIENG
// cho tai firmware. KHONG dung chung — dung chung la treo (bai hoc tu ban
// IoT-mesh-node).
#if NET_HAS_GSM
HardwareSerial modem(1);
TinyGsm modemGsm(modem);
TinyGsmClient gsmNetClient(modemGsm, 0);
TinyGsmClient gsmOtaClient(modemGsm, 1); // mux 1: socket thu hai cua BG96
bool gsmReady = false;                   // da restart + gprsConnect thanh cong
#endif

#if NET_HAS_WIFI
WiFiClient wifiNetClient;
WiFiClient wifiOtaClient;
#endif

// Duong mang dang thuc su dung. O NET_MODE_GSM/WIFI gia tri nay co dinh sau
// khi ket noi; chi NET_MODE_AUTO moi doi qua lai luc chay.
ConnMode activeConnMode = ConnMode::None;

// AUTO khoi tao bang WiFi roi doi luc chay qua setClient(). PHAI co client ngay
// tu dau: PubSubClient::disconnect()/connect() KHONG kiem tra _client == NULL.
#if NET_MODE == NET_MODE_GSM
PubSubClient mqtt(gsmNetClient);
#else
PubSubClient mqtt(wifiNetClient);
#endif

HardwareSerial F9P(2);
unsigned long lastModemReset = 0;
unsigned long lastF9pMs = 0; // lan cuoi thay du lieu tu F9P (dung cho self-test)

const char *MQTT_HOST = "103.82.22.78";
const int MQTT_PORT = 1883;
const char *MQTT_CLIENT_ID = "cacao_2_rtk_base";
const char *MQTT_TOPIC = "rtk/rtcm3-2";
const char *MQTT_CMD_TOPIC = "rtk/cacao_2_rtk_base/cmd";
const char *MQTT_LOG_TOPIC = "rtk/cacao_2_rtk_base/log";
const char *MQTT_OTA_STATUS_TOPIC = "rtk/cacao_2_rtk_base/ota";

void switchToFixed();
void switchToSurveyIn();

// Lenh OTA: "OTA {"url":"http://...","size":1234,"sha256":"..."}"
// Chi NAP yeu cau — viec tai/flash chay o loop() qua OtaRtk::tick(), khong bao
// gio chay trong callback MQTT.
static void otaCommand(const char *arg, size_t len)
{
    OtaRtk::handleCommand(arg, len);
}

const MqttCmd::Entry cmdEntries[] = {
    {"FIXED", switchToFixed, nullptr},
    {"SURVEY", switchToSurveyIn, nullptr},
    {"RESTART", []()
     { esp_restart(); },
     nullptr},
    {"OTA", nullptr, otaCommand},
};

enum ParseState
{
    WAIT_SYNC,
    RTCM_COLLECT,
    UBX_COLLECT,
    WAIT_UBX_SYNC2,
};
ParseState parseState = WAIT_SYNC;
uint16_t expectedLen = 0;
unsigned long lastFlush = 0;

uint8_t ubxBuf[512];
uint16_t ubxIdx = 0;

enum BaseState
{
    IDLE,
    SURVEY,
    FIXED
};

BaseState baseState = IDLE;

int32_t ecefX_cm, ecefY_cm, ecefZ_cm, fixedAcc;
bool surveyValid = false;
bool surveyActive = false;
float currentAccM = 0.0f;
uint32_t currentDur = 0;

// ---------- Network (WiFi / 4G) ----------

const char *connModeName(ConnMode m)
{
    switch (m)
    {
    case ConnMode::Wifi:
        return "WIFI";
    case ConnMode::Gsm:
        return "4G";
    default:
        return "NONE";
    }
}

#if NET_HAS_WIFI
// Khoi dong radio WiFi va bat dau ket noi. Khong cho o day — nguoi goi tu cho.
void wifiStart()
{
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); // TAT power-save: giam tre, tang toc tai OTA bulk
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

bool wifiUp() { return WiFi.status() == WL_CONNECTED; }
#endif

#if NET_HAS_GSM
// Bat modem BG96 va dang ky GPRS. Chi duoc goi o cac mode CO 4G — o
// NET_MODE_WIFI ham nay khong ton tai, UART modem khong bao gio duoc mo.
bool gsmStart()
{
    modem.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    delay(2000);
    modemGsm.restart();
    modemGsm.gprsConnect("v-internet", "", "");
    gsmReady = modemGsm.isNetworkConnected();
    SerialLog::log("gsm connected", gsmReady);
    return gsmReady;
}
#endif

// Gan duong mang cho MQTT + OTA. Ngat MQTT truoc khi doi socket.
void useConnMode(ConnMode m)
{
    if (m == activeConnMode)
        return;

    SerialLog::log("[NET] chuyen transport sang", connModeName(m));
    if (activeConnMode != ConnMode::None)
        mqtt.disconnect(); // dong socket cu truoc khi doi

    switch (m)
    {
#if NET_HAS_WIFI
    case ConnMode::Wifi:
        mqtt.setClient(wifiNetClient);
        OtaRtk::setClient(wifiOtaClient);
        break;
#endif
#if NET_HAS_GSM
    case ConnMode::Gsm:
        mqtt.setClient(gsmNetClient);
        OtaRtk::setClient(gsmOtaClient);
        break;
#endif
    default:
        break;
    }
    activeConnMode = m;
}

// Ket noi mang luc boot theo NET_MODE.
void netConnect()
{
#if NET_MODE == NET_MODE_WIFI
    SerialLog::log("wifi connecting...");
    wifiStart();

    const unsigned long deadline = millis() + 30000;
    while (!wifiUp() && (long)(millis() - deadline) < 0)
        delay(200);

    SerialLog::log("wifi connected", wifiUp(), WiFi.localIP().toString().c_str());
    useConnMode(ConnMode::Wifi);

#elif NET_MODE == NET_MODE_GSM
    SerialLog::log("gsm connecting...");
    gsmStart();
    useConnMode(ConnMode::Gsm);

#else // NET_MODE_AUTO — thu WiFi truoc, het gio thi xuong 4G
    SerialLog::log("auto: thu WiFi truoc...");
    wifiStart();

    const unsigned long deadline = millis() + NET_WIFI_BOOT_TIMEOUT_MS;
    while (!wifiUp() && (long)(millis() - deadline) < 0)
        delay(200);

    if (wifiUp())
    {
        SerialLog::log("wifi connected", true, WiFi.localIP().toString().c_str());
        useConnMode(ConnMode::Wifi);
    }
    else
    {
        SerialLog::log("khong co WiFi -> chuyen 4G");
        const bool ok = gsmStart();
        if (ok)
            useConnMode(ConnMode::Gsm);
    }
#endif
}

#if NET_MODE == NET_MODE_AUTO
// Goi moi vong loop. WiFi duoc uu tien; mat WiFi thi bat 4G len, WiFi ve thi
// quay lai WiFi. (Rut gon tu maintainConnectivity() ben IoT-mesh-node-.)
void maintainConnectivity()
{
    if (wifiUp())
    {
        useConnMode(ConnMode::Wifi);
        return;
    }

    // Mat WiFi: cho radio thu lai ngam, dong thoi dua 4G len thay.
    static uint32_t lastWifiRecheck = 0;
    if (millis() - lastWifiRecheck >= NET_WIFI_RECHECK_MS)
    {
        lastWifiRecheck = millis();
        WiFi.reconnect();
    }

    if (!gsmReady)
    {
        // Lan dau thu ngay; cac lan sau moi rate-limit.
        static bool firstGsmAttempt = true;
        static uint32_t lastGsmAttempt = 0;
        if (!firstGsmAttempt && millis() - lastGsmAttempt < NET_GSM_RETRY_INTERVAL_MS)
            return;
        firstGsmAttempt = false;
        lastGsmAttempt = millis();
        if (!gsmStart())
            return;
    }

    useConnMode(ConnMode::Gsm);

    // Kiem tra link GPRS THUA THOT thoi. Goi isGprsConnected() moi vong lap se
    // spam AT+CGATT?, tranh kenh AT voi PubSubClient va gay false positive "mat
    // link" -> re-register 4G vo ich. Phai fail 2 lan LIEN TIEP moi tin.
    static uint32_t lastGprsCheck = 0;
    static int gprsFailStreak = 0;
    if (millis() - lastGprsCheck >= NET_GSM_LINK_CHECK_MS)
    {
        lastGprsCheck = millis();
        if (modemGsm.isGprsConnected())
        {
            gprsFailStreak = 0;
        }
        else if (++gprsFailStreak >= 2)
        {
            SerialLog::log("[GSM] mat link GPRS (2 lan lien tiep), se thu lai");
            gsmReady = false;
            gprsFailStreak = 0;
        }
    }
}
#endif

void resetModem()
{
#if NET_HAS_GSM
    if (activeConnMode == ConnMode::Gsm)
    {
        modemGsm.restart();
        modemGsm.gprsConnect("v-internet", "", "");
    }
#endif
#if NET_HAS_WIFI
    if (activeConnMode == ConnMode::Wifi)
    {
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
#endif
    lastModemReset = millis();
}

// In ten firmware + phien ban dang chay. Goi luc boot (ra Serial) va ngay sau
// khi MQTT len (ra topic log) — luc boot MQTT chua noi nen log kia khong bay di
// dau ca.
static void logFirmwareVersion()
{
    SerialLog::log("FW", FW_NAME, VERSION_FW, FW_BUILD);
}

void mqttConnect()
{
    SerialLog::log("MQTT connecting via", connModeName(activeConnMode));

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(1024);
    MqttCmd::begin(mqtt, MQTT_CMD_TOPIC, cmdEntries,
                   sizeof(cmdEntries) / sizeof(cmdEntries[0]));
    SerialLog::setMqtt(mqtt, MQTT_LOG_TOPIC);
    while (!mqtt.connected())
    {
        mqtt.connect(MQTT_CLIENT_ID);
        delay(1000);
#if NET_MODE == NET_MODE_AUTO
        maintainConnectivity(); // ca 2 duong deu hong luc boot -> van tu do lai
#endif
    }
    MqttCmd::subscribe();

    SerialLog::log("MQTT connected");
    logFirmwareVersion();
}

void mqttReconnect()
{
#if NET_HAS_GSM
    if (activeConnMode == ConnMode::Gsm)
        SerialLog::log("GPRS connected:", modemGsm.isGprsConnected());
#endif
    SerialLog::log("Attempting MQTT connection via", connModeName(activeConnMode));
    int retry = 1;
    while (!mqtt.connected())
    {
        if (!mqtt.connect(MQTT_CLIENT_ID))
        {
            SerialLog::log("MQTT failed, state:", mqtt.state(), "retry:", retry);
            retry++;
            delay(1000);
        }
#if NET_MODE == NET_MODE_AUTO
        // Duong mang co the doi ngay giua luc dang ket lai — goi o day de
        // khong ket vo vong tren mot socket da chet.
        maintainConnectivity();
#endif
    }
    MqttCmd::subscribe();
}

// ---------- UBX ----------

void ubxChecksum(const uint8_t *data, uint16_t len, uint8_t &ckA, uint8_t &ckB)
{
    for (uint16_t i = 0; i < len; i++)
    {
        ckA += data[i];
        ckB += ckA;
    }
}

void sendUBX(uint8_t cls, uint8_t id, uint8_t *payload, uint16_t payloadLen)
{
    uint8_t header[6] = {
        0xB5, 0x62,
        cls, id,
        (uint8_t)(payloadLen & 0xFF),
        (uint8_t)(payloadLen >> 8)};

    uint8_t ckA = 0, ckB = 0;
    ubxChecksum(&header[2], 4, ckA, ckB);
    if (payloadLen > 0)
        ubxChecksum(payload, payloadLen, ckA, ckB);

    F9P.write(header, sizeof(header));
    if (payloadLen > 0)
        F9P.write(payload, payloadLen);
    F9P.write(ckA);
    F9P.write(ckB);
    F9P.flush();
}

void ubxUart1Enable(bool isOn)
{
    if (isOn)
    {
        uint8_t msg[] = {
            0xB5, 0x62, 0x06, 0x8A, 0x0E, 0x00,
            0x00, 0x01, 0x00, 0x00, 0x01, 0x00,
            0x74, 0x10, 0x01, 0x04, 0x00, 0x74,
            0x10, 0x00, 0xAD, 0x33};
        F9P.write(msg, sizeof(msg)); // fix: thiếu dòng này trong bản cũ
    }
    else
    {
        uint8_t msg[] = {
            0xB5, 0x62, 0x06, 0x8A, 0x0E, 0x00,
            0x00, 0x01, 0x00, 0x00, 0x01, 0x00,
            0x74, 0x10, 0x00, 0x04, 0x00, 0x74,
            0x10, 0x01, 0xAD, 0x2E};
        F9P.write(msg, sizeof(msg));
    }
}

void setBaseState(BaseState state)
{
    baseState = state;
    SerialLog::log("BASE STATE",
                   state == FIXED ? "FIXED" : (state == SURVEY ? "SURVEY" : "IDLE"));
    bool isSurvey = state == SURVEY;
    ubxUart1Enable(isSurvey);
    digitalWrite(LED_SURVEY_IN, isSurvey);
}

void switchToFixed()
{
    int32_t x = ecefX_cm;
    int32_t y = ecefY_cm;
    int32_t z = ecefZ_cm;

    uint8_t msg[128];
    uint8_t *p = msg;

    *p++ = 0x00;
    *p++ = 0x05;
    *p++ = 0x00;
    *p++ = 0x00;

    auto addU1 = [&](uint32_t key, uint8_t v)
    {
        memcpy(p, &key, 4);
        p += 4;
        *p++ = v;
    };

    auto addI4 = [&](uint32_t key, int32_t v)
    {
        memcpy(p, &key, 4);
        p += 4;
        memcpy(p, &v, 4);
        p += 4;
    };

    auto addU4 = [&](uint32_t key, uint32_t v)
    {
        memcpy(p, &key, 4);
        p += 4;
        memcpy(p, &v, 4);
        p += 4;
    };

    addU1(0x20030001, 2); // CFG-TMODE-MODE = FIXED
    addI4(0x40030003, x); // ECEF X
    addI4(0x40030004, y); // ECEF Y
    addI4(0x40030005, z); // ECEF Z
    addU4(0x4003000F, fixedAcc);
    addU4(0x40030010, 0);
    addU4(0x40030011, 0);

    uint16_t len = p - msg;
    sendUBX(0x06, 0x8A, msg, len);

    setBaseState(FIXED);
}

void switchToSurveyIn()
{
    uint8_t msg[128];
    uint8_t *p = msg;

    *p++ = 0x00;
    *p++ = 0x05;
    *p++ = 0x00;
    *p++ = 0x00;

    auto addU1 = [&](uint32_t key, uint8_t v)
    {
        memcpy(p, &key, 4);
        p += 4;
        *p++ = v;
    };

    auto addU4 = [&](uint32_t key, uint32_t v)
    {
        memcpy(p, &key, 4);
        p += 4;
        memcpy(p, &v, 4);
        p += 4;
    };

    addU1(0x20030001, 1); // CFG-TMODE-MODE = Survey-In
    addU4(0x40030003, 0);
    addU4(0x40030004, 0);
    addU4(0x40030005, 0);
    addU4(0x4003000F, 0);
    addU4(0x40030010, SURVEY_SEC); // svinMinDur
    addU4(0x40030011, SURVEY_ACC); // svinAccLimit

    uint16_t len = p - msg;
    sendUBX(0x06, 0x8A, msg, len);

    setBaseState(SURVEY);
}

void parseUBX(uint8_t *buf, uint16_t len)
{
    SerialLog::log("UBX", len);
    if (len < 8)
        return;

    if (buf[0] != 0xB5 || buf[1] != 0x62)
        return;

    uint8_t cls = buf[2];
    uint8_t id = buf[3];
    uint16_t payloadLen = buf[4] | (buf[5] << 8);
    const uint8_t *p = buf + 6;

    if (cls == 0x06 && id == 0x71 && payloadLen >= 40)
    {
        uint8_t tmodeMode = p[2];
        if (tmodeMode == 1)
            setBaseState(SURVEY);
        else if (tmodeMode == 2)
            setBaseState(FIXED);
        else
            switchToSurveyIn();
        return;
    }

    // UBX-NAV-SVIN
    if (cls == 0x01 && id == 0x3B && payloadLen >= 40)
    {
        uint32_t dur = *(uint32_t *)(p + 8);  // s
        int32_t meanX = *(int32_t *)(p + 12); // cm
        int32_t meanY = *(int32_t *)(p + 16); // cm
        int32_t meanZ = *(int32_t *)(p + 20); // cm
        uint32_t acc = *(uint32_t *)(p + 28); // 0.1 mm
        uint8_t valid = p[36];
        uint8_t active = p[37];

        surveyValid = valid;
        surveyActive = active;

        ecefX_cm = meanX;
        ecefY_cm = meanY;
        ecefZ_cm = meanZ;
        fixedAcc = acc;

        currentAccM = acc / 10000.0f;
        currentDur = dur;

        SerialLog::log("SVIN", ecefX_cm, ecefY_cm, ecefZ_cm, surveyValid, dur, acc);

        if (surveyValid && surveyActive == 0 && dur >= SURVEY_SEC)
        {
            switchToFixed();
        }
    }
}

// ---------- Data sync ----------

void rtcm3Sync(uint8_t *payload, unsigned int plength)
{
    digitalWrite(LED_PING, HIGH);
    mqtt.publish(MQTT_TOPIC, payload, plength);
    digitalWrite(LED_PING, LOW);
}

void ubxSync(uint8_t *payload, unsigned int plength)
{
    digitalWrite(LED_PING, HIGH);
    parseUBX(payload, plength);
    digitalWrite(LED_PING, LOW);
}

void f9pLoop()
{
    if (F9P.available())
        lastF9pMs = millis();

    if (baseState == FIXED)
    {
        int len = F9P.available();
        if (len == 0)
            return;
        uint8_t *frame = (uint8_t *)malloc(len);
        if (!frame)
            return;
        int n = F9P.readBytes(frame, len);
        rtcm3Sync(frame, n);
        free(frame);
        return;
    }

    while (F9P.available())
    {
        uint8_t b = F9P.read();

        switch (parseState)
        {
        case WAIT_SYNC:
            if (b == 0xB5)
            {
                ubxIdx = 0;
                ubxBuf[ubxIdx++] = b;
                parseState = WAIT_UBX_SYNC2;
            }
            break;

        case WAIT_UBX_SYNC2:
            if (b == 0x62)
            {
                ubxBuf[ubxIdx++] = b;
                parseState = UBX_COLLECT;
            }
            else
            {
                parseState = WAIT_SYNC;
                ubxIdx = 0;
            }
            break;

        case UBX_COLLECT:
            if (ubxIdx < sizeof(ubxBuf))
                ubxBuf[ubxIdx++] = b;

            if (ubxIdx == 6)
                expectedLen = ubxBuf[4] | (ubxBuf[5] << 8);

            if (ubxIdx >= 8 && ubxIdx == (uint16_t)(6 + expectedLen + 2))
            {
                ubxSync(ubxBuf, ubxIdx);
                parseState = WAIT_SYNC;
                ubxIdx = 0;
            }
            else if (ubxIdx >= sizeof(ubxBuf))
            {
                parseState = WAIT_SYNC;
                ubxIdx = 0;
            }
            break;

        default:
            parseState = WAIT_SYNC;
            ubxIdx = 0;
            break;
        }
    }
}

// ---------- Command task ----------

void _cmdTask(void *pvParameters)
{
    bool surveyPinState = HIGH;
    while (1)
    {
        bool currentState = digitalRead(SURVEY_PIN);

        if (surveyPinState == LOW && currentState == HIGH)
        {
            SerialLog::log("Start Survey-In");
            switchToSurveyIn();
        }

        surveyPinState = currentState;

        if (baseState == IDLE)
        {
            static unsigned long lastToggle = 0;
            static bool ledState = false;
            unsigned long now = millis();
            if (now - lastToggle >= 1000)
            {
                lastToggle = now;
                ledState = !ledState;
                digitalWrite(LED_SURVEY_IN, ledState);
                sendUBX(0x06, 0x71, nullptr, 0); // ping CFG-TMODE3
            }
        }

#ifdef DEBUG
        {
            auto cmd = Serial.readStringUntil('\n');
            if (cmd.startsWith("FIXED"))
                switchToFixed();
            else if (cmd.startsWith("SURVEY"))
                switchToSurveyIn();
        }
#if NET_HAS_GSM
        // Echo output AT cua modem ra Serial de debug. O NET_MODE_WIFI khong co
        // doi tuong `modem` — UART modem khong bao gio duoc mo.
        while (modem.available())
            Serial.write(modem.read());
#endif

        static bool lastState = HIGH;
        static unsigned long lastDebounceTime = 0;
        bool reading = digitalRead(SURVEY_PIN);
        if ((millis() - lastDebounceTime) > 50)
        {
            if (lastState == LOW && reading == HIGH)
                esp_restart();
        }
        if (reading != lastState)
            lastDebounceTime = millis();
        lastState = reading;
#endif
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// ---------- Setup / Loop ----------

// Dieu kien "anh moi con song" sau OTA. Chua dat trong OTA_SELFTEST_TIMEOUT_MS
// -> tu rollback ve anh cu.
//   - MQTT noi lai duoc  : chung to mang + broker OK.
//   - F9P con ra du lieu : chung to UART GNSS OK (nghia la tram van lam viec).
// Neu tu luc boot CHUA he thay byte nao tu F9P thi khong lay lam dieu kien,
// tranh rollback nham khi module GNSS chua duoc cau hinh phat.
static bool otaHealthy()
{
    if (!mqtt.connected())
        return false;
    if (lastF9pMs == 0)
        return true;
    return (millis() - lastF9pMs) < 30000;
}

void setup()
{
    Serial.begin(115200); // luôn bật để SerialLog hoạt động
    logFirmwareVersion();

    // Goi SOM: phat hien anh dang chay co phai ban vua OTA (pending-verify) hay
    // khong, va bat dong ho self-test.
    OtaSelfTest::begin(OTA_SELFTEST_TIMEOUT_MS);

    F9P.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);

    pinMode(LED_PING, OUTPUT);
    pinMode(LED_SURVEY_IN, OUTPUT);
    pinMode(SURVEY_PIN, INPUT_PULLUP);

    for (int i = 0; i < 5; i++)
    {
        digitalWrite(LED_PING, HIGH);
        delay(50);
        digitalWrite(LED_PING, LOW);
        delay(50);
    }

    xTaskCreatePinnedToCore(_cmdTask, "CMDTask", 8192, NULL, 1, NULL, PRO_CPU_NUM);

    // PHAI goi TRUOC netConnect(): useConnMode() ben trong netConnect() se goi
    // OtaRtk::setClient() de gan socket dung voi duong mang duoc chon.
#if NET_MODE == NET_MODE_GSM
    OtaRtk::begin(mqtt, gsmOtaClient, MQTT_OTA_STATUS_TOPIC);
#else
    OtaRtk::begin(mqtt, wifiOtaClient, MQTT_OTA_STATUS_TOPIC);
#endif

    netConnect();
    mqttConnect();
}

void loop()
{
#if NET_MODE == NET_MODE_AUTO
    maintainConnectivity(); // WiFi uu tien, mat WiFi thi tu chuyen 4G
#endif

    if (!mqtt.connected())
        mqttReconnect();

    mqtt.loop();

    if (OtaSelfTest::isPendingVerify())
        OtaSelfTest::poll(otaHealthy());

    // Co yeu cau OTA -> tam ngung bom RTCM3 (tai firmware chiem tron loop vai
    // chuc giay den vai phut; rover se mat hieu chinh trong khoang do).
    if (OtaRtk::busy())
    {
        OtaRtk::tick();
        return;
    }

    f9pLoop();

    if (millis() - lastModemReset > 6UL * 3600 * 1000)
        resetModem();

    if (ESP.getFreeHeap() < 20000)
        esp_restart();

    yield();
}
