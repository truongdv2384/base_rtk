#include "ota_selftest.h"

#include <Arduino.h>
#include "sdkconfig.h"
#include "esp_ota_ops.h"

// -----------------------------------------------------------------------------
//  Override weak hook cua Arduino core (cores/esp32/esp32-hal-misc.c):
//  tra ve true => HOAN viec verify luc boot (initArduino se bo qua khoi verify).
//  Ta tu quyet dinh mark-valid / rollback SAU khi chay self-test ket noi trong
//  poll().
//
//  BAT BUOC nam cung translation unit voi begin() (duoc goi tu setup()) — vi
//  begin() la symbol duoc tham chieu, linker se keo .o nay vao, kEO theo
//  strong-def verifyRollbackLater() de ghi de weak-def trong core. Neu tach ra
//  file rieng khong duoc tham chieu, linker se KHONG keo vao -> override im lang
//  that bai.
// -----------------------------------------------------------------------------
extern "C" bool verifyRollbackLater() { return true; }

namespace {
bool     s_pending   = false;  // anh dang chay o trang thai THU NGHIEM (pending-verify)
bool     s_settled   = false;  // da chot (mark valid) hoac da rollback
uint32_t s_startMs   = 0;
uint32_t s_timeoutMs = 60000;
}  // namespace

namespace OtaSelfTest {

void begin(uint32_t timeoutMs) {
  s_timeoutMs = timeoutMs;
  s_startMs   = millis();
  s_settled   = false;
  s_pending   = false;

#ifdef CONFIG_APP_ROLLBACK_ENABLE
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
      st == ESP_OTA_IMG_PENDING_VERIFY) {
    s_pending = true;
    Serial.printf("[OTA SELFTEST] Anh MOI dang THU NGHIEM (pending-verify). "
                  "Self-test toi da %u ms: dat thi CHOT, khong thi ROLLBACK.\n",
                  (unsigned)timeoutMs);
  }
#endif

  if (!s_pending) {
    s_settled = true;  // boot binh thuong (khong sau OTA) -> khong co gi de lam
  }
}

bool isPendingVerify() { return s_pending && !s_settled; }

void poll(bool healthy) {
  if (s_settled) return;

#ifdef CONFIG_APP_ROLLBACK_ENABLE
  if (healthy) {
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    Serial.printf("[OTA SELFTEST] Self-test DAT -> CHOT anh moi (mark valid), err=%d\n",
                  (int)e);
    s_settled = true;
    return;
  }
  if (millis() - s_startMs > s_timeoutMs) {
    Serial.println("[OTA SELFTEST] Self-test THAT BAI (het gio) -> ROLLBACK ve anh cu, reboot...");
    delay(100);
    esp_ota_mark_app_invalid_rollback_and_reboot();  // reboot, khong tro ve
    s_settled = true;  // phong truong hop khong co slot cu de rollback
  }
#else
  (void)healthy;
  s_settled = true;
#endif
}

}  // namespace OtaSelfTest
