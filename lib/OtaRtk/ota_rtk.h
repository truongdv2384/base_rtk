#pragma once
// =============================================================================
//  OtaRtk — OTA cho tram RTK base (mot thiet bi doc lap, khong relay/mesh).
//
//  Luong:  MQTT cmd "OTA {json}" -> HTTP GET (4G hoac WiFi) -> STREAM thang vao
//          Update.write() + SHA-256 luy ke -> so sanh sha/size -> reboot
//          -> OtaSelfTest chot anh moi hoac tu rollback.
//
//  KHAC ban IoT-mesh-node: KHONG buffer ca anh vao PSRAM (board nay khong co
//  PSRAM) va KHONG nen LZSS (khong co chang relay nao can tiet kiem bang thong).
//
//  Download di qua interface `Client` chung nen dung duoc ca TinyGsmClient (4G)
//  lan WiFiClient — chi ho tro HTTP thuan, KHONG TLS.
//
//  Cach dung:
//    setup():  OtaRtk::begin(mqtt, otaClient, MQTT_STATUS_TOPIC);
//              OtaSelfTest::begin(OTA_SELFTEST_TIMEOUT_MS);
//    cmd cb:   OtaRtk::handleCommand(payloadSauChu"OTA", len);
//    loop():   OtaRtk::tick();   // thuc thi o loop, KHONG trong callback MQTT
// =============================================================================

#include <Arduino.h>
#include <Client.h>
#include <PubSubClient.h>

// ── Tham so ──────────────────────────────────────────────────────────────────

// Timeout self-test sau OTA. Dat DAI vi BG96 dang ky mang 4G cham; anh treo/crash
// van duoc bootloader revert ngay, khong phu thuoc moc nay.
constexpr uint32_t OTA_SELFTEST_TIMEOUT_MS = 180000;

// Chan tren kich thuoc anh chap nhan tai (chan Content-Length bat thuong).
constexpr uint32_t OTA_MAX_IMAGE_BYTES = 4UL * 1024 * 1024;

// Het gio cho du lieu ke tiep tu socket (4G hay ngat quang hon WiFi).
constexpr uint32_t OTA_HTTP_IDLE_TIMEOUT_MS = 20000;

// Block doc/ghi. Nho de khong ngon RAM va de nuoi watchdog.
constexpr size_t OTA_IO_BLOCK = 1024;

namespace OtaRtk {

// mqtt/statusTopic dung de bao tien do; otaClient PHAI la socket RIENG, khong
// dung chung voi client cua PubSubClient (dung chung se treo nhu ban cu).
void begin(PubSubClient &mqtt, Client &otaClient, const char *statusTopic);

// Doi socket tai firmware khi duong mang doi (NET_MODE_AUTO: WiFi <-> 4G).
void setClient(Client &otaClient);

// Parse lenh JSON {"url":"...","size":N,"sha256":"..."} va NAP yeu cau.
// Goi tu callback MQTT — chi luu lai, khong tai o day.
// true = lenh hop le, da nhan.
bool handleCommand(const char *payload, size_t len);

// Goi trong loop(). No-op neu khong co yeu cau. Khi co, ham nay CHAY DONG BO
// toan bo qua trinh tai + flash (vai chuc giay ~ vai phut) roi reboot.
void tick();

// true khi dang co yeu cau OTA cho xu ly / dang chay (de loop() tam ngung
// bom RTCM3).
bool busy();

} // namespace OtaRtk
