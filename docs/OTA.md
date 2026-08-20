# OTA cho tram RTK base

Cập nhật firmware từ xa qua MQTT + HTTP, có SHA-256 và **rollback tự động** nếu ảnh mới hỏng.

Thiết kế rút gọn từ dự án `IoT-mesh-node-`: giữ lại nhánh "self-update" (download → verify → flash → rollback self-test), **bỏ** toàn bộ phần relay UART / mesh broadcast / LZSS vì ở đây chỉ có một thiết bị độc lập.

---

## Luồng

```
[PC]  python ota.py
        │  MQTT rtk/cacao_2_rtk_base/cmd
        │  payload: OTA {"url":"http://...","size":N,"sha256":"..."}
        ▼
   [ESP32-S3 base]  MqttCmd -> OtaRtk::handleCommand()   (chỉ nạp yêu cầu)
        │
        │  loop() -> OtaRtk::tick()
        ▼
   HTTP GET qua socket RIÊNG (TinyGsmClient mux 1, hoặc WiFiClient)
        │  đọc từng block 1KB:  Update.write() + sha256.update()
        ▼
   so size với manifest -> so SHA-256 -> Update.end() -> ESP.restart()
        │
        ▼
   boot ảnh mới ở trạng thái PENDING_VERIFY
        ├─ MQTT nối lại + F9P còn ra dữ liệu  -> mark valid (chốt)
        └─ quá 180s chưa đạt / crash / treo    -> tự rollback về ảnh cũ
```

**Không dùng PSRAM** — stream thẳng vào flash, tốn ~1KB RAM.

---

## File

| File | Vai trò |
|---|---|
| [lib/OtaCommon/](../lib/OtaCommon/) | `OtaSelfTest` (rollback) + `OtaSha256`. Copy nguyên từ `IoT-mesh-node-`, đã chạy thật ở đó. |
| [lib/OtaRtk/](../lib/OtaRtk/) | HTTP GET thủ công + stream vào `Update` + parse manifest JSON. Viết riêng cho dự án này. |
| [lib/mqtt_cmd/](../lib/mqtt_cmd/) | Đã mở rộng: `Entry` có thêm `DataHandler` để lệnh mang tham số (`OTA {json}`). |
| [ota.py](../ota.py) | Tính sha256 + size từ `firmware.bin`, publish lệnh. |

---

## Chế độ mạng (`NET_MODE`)

**Một firmware, một env build.** Đổi chế độ bằng cách sửa đúng một dòng trong [include/net_mode.h](../include/net_mode.h):

```c
#define NET_MODE NET_MODE_WIFI     // hoặc NET_MODE_GSM / NET_MODE_AUTO
```

| Giá trị | Hành vi |
|---|---|
| `NET_MODE_GSM` | Chỉ 4G/BG96. WiFi **không được biên dịch vào** (0 symbol WiFi trong ELF). Flash 32%. |
| `NET_MODE_WIFI` | Chỉ WiFi. GSM **tắt hoàn toàn**: không mở UART modem, không gọi hàm TinyGSM nào. Flash 70%. |
| `NET_MODE_AUTO` | WiFi ưu tiên, mất WiFi thì tự chuyển 4G, WiFi về thì quay lại. Flash 71%. |

Sửa xong chỉ cần `pio run` — PlatformIO tự nhận header đổi và build lại (~15s), **không cần clean, không cần đổi env**.

Mỗi đường mạng có **hai socket riêng**: `*NetClient` cho MQTT, `*OtaClient` cho tải firmware. `useConnMode()` đổi cả hai qua `PubSubClient::setClient()` + `OtaRtk::setClient()`.

`NET_MODE_WIFI`/`NET_MODE_AUTO` cần SSID+PASS trong `include/secrets.h`. **ESP32-S3 chỉ bắt được 2.4GHz.**

## Cách chạy

### Test qua WiFi (không tốn cước 4G)

```bash
cp include/secrets.h.example include/secrets.h   # rồi điền SSID/PASS
# đặt NET_MODE = NET_MODE_WIFI trong include/net_mode.h
pio run -t upload && pio device monitor
```

### Đẩy firmware

```bash
# 1. build ảnh muốn nạp
pio run

# 2. phục vụ file (chạy tại gốc dự án)
python -m http.server 8080

# 3. sửa BASE_URL trong ota.py cho khớp IP máy này, rồi:
python ota.py
```

Theo dõi:
```bash
mosquitto_sub -h 103.82.22.78 -t 'rtk/cacao_2_rtk_base/#' -v
```

Trạng thái trên topic `rtk/cacao_2_rtk_base/ota`:
`queued` → `start` → `downloading` (mỗi 10%) → `ok` → reboot.
Lỗi: `http_failed` / `size_mismatch` / `bad_size` / `begin_failed` / `failed` / `end_failed`.

### Qua 4G (thật)

Giống hệt, chỉ đổi `NET_MODE` và **`BASE_URL` phải là địa chỉ public** — thiết bị nằm sau NAT của nhà mạng, không với tới `192.168.x.x` được.

```bash
# đặt NET_MODE = NET_MODE_GSM trong include/net_mode.h
pio run && python ota.py
```

---

## Ràng buộc

- **HTTP thuần, không TLS.** `HTTPClient` của esp32 chỉ nhận `NetworkClient` nên không dùng được với `TinyGsmClient` → tự viết GET. Đổi lại một đường code chạy cả 4G lẫn WiFi.
- **Bảng phân vùng `default.csv`** (2 slot app 1.31MB). Đã pin trong `platformio.ini`. Không được đổi sang `huge_app.csv` — mất OTA.
  Hiện dùng: 4G **420KB (32%)**, WiFi **922KB (70%)**, auto **932KB (71%)**.
- **RTCM3 đứt trong lúc OTA.** `loop()` tạm ngừng bơm RTCM khi `OtaRtk::busy()`; rover mất hiệu chỉnh vài chục giây (WiFi) đến vài phút (4G).
- **Socket riêng cho OTA** (`mux 1` của BG96). Dùng chung socket với MQTT sẽ treo.

## Điều kiện self-test (quyết định chốt hay rollback)

```
mqtt.connected() && (chưa từng thấy F9P || F9P ra dữ liệu trong 30s gần nhất)
```

Timeout 180s (`OTA_SELFTEST_TIMEOUT_MS`) — đặt dài vì BG96 đăng ký mạng chậm. Ảnh crash/treo thì bootloader revert ngay, không chờ mốc này.

## Còn thiếu / cần lưu ý

- **Đã test HW qua WiFi (2026-08-20):** tải 973504B → SHA-256 OK → flash → boot ảnh mới → `Self-test DAT -> CHOT anh moi`. Chu trình trọn vẹn.
- **Chưa test:** nhánh **rollback-fail** (nạp ảnh cố tình không nối được MQTT, xem có tự revert sau 180s không) và nhánh **tải qua 4G** — nhánh GSM bên `IoT-mesh-node-` cũng chưa từng được test thật.
- **Kênh lệnh không có xác thực.** `mqtt.connect(MQTT_CLIENT_ID)` không user/pass, topic `cmd` ai publish cũng được, firmware phục vụ qua HTTP không ký số. Trước khi dùng ngoài thực địa nên bật user/pass cho broker — nếu không, ai cũng flash được thiết bị.
- **Chưa có idempotency:** gửi lại cùng một firmware sẽ nạp lại từ đầu, không skip.
