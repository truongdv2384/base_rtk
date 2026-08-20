#pragma once
// =============================================================================
//  OtaSelfTest — self-test + rollback cho ảnh vừa nạp qua OTA.
//
//  Cơ chế (ESP-IDF app-rollback, đã bật sẵn trong bootloader precompiled:
//  CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y / CONFIG_APP_ROLLBACK_ENABLE=y):
//   - Sau OTA, ảnh mới boot ở trạng thái ESP_OTA_IMG_PENDING_VERIFY.
//   - .cpp override weak hook verifyRollbackLater() -> true để HOÃN việc core tự
//     quyết định lúc boot; ta tự chạy self-test trong loop() rồi mới chốt.
//   - poll(healthy): healthy==true -> mark valid (chốt ảnh mới); quá timeout mà
//     chưa healthy -> mark invalid + reboot (bootloader nạp lại ảnh cũ).
//   - Nếu ảnh mới crash/treo/boot-loop TRƯỚC khi kịp chốt -> bootloader tự revert.
//
//  Cách dùng trong mỗi firmware:
//    setup():  OtaSelfTest::begin(<timeout ms>);            // gọi sớm
//    loop():   if (OtaSelfTest::isPendingVerify())
//                OtaSelfTest::poll(<biểu thức "còn sống">); // vd hasRoute()/mqtt.connected()
//
//  Boot bình thường (không phải sau OTA): begin() tự nhận biết và poll() thành no-op.
// =============================================================================

#include <stdint.h>

namespace OtaSelfTest {

// Gọi 1 lần sớm trong setup(). Phát hiện ảnh đang chạy có ở trạng thái thử nghiệm
// (pending-verify) không, và khởi động đồng hồ đếm timeout.
void begin(uint32_t timeoutMs);

// true nếu đang trong giai đoạn thử nghiệm và CHƯA chốt/rollback. Dùng để tránh
// đánh giá biểu thức "còn sống" khi không cần (vd không gọi hasRoute() trước khi
// mesh khởi tạo ở các boot bình thường).
bool isPendingVerify();

// Gọi định kỳ trong loop() khi isPendingVerify()==true.
//   healthy==true              -> chốt ảnh mới (mark valid), dừng.
//   healthy==false + quá hạn   -> rollback về ảnh cũ + reboot.
void poll(bool healthy);

}  // namespace OtaSelfTest
