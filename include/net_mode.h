#pragma once
// =============================================================================
//  net_mode.h — Chon duong mang cho MQTT va OTA.
//
//  CHI CO MOT FIRMWARE, MOT ENV BUILD. Doi che do bang cach sua DUNG MOT DONG
//  ben duoi roi build lai — khong can doi env, khong can clean.
// =============================================================================

#define NET_MODE_GSM 0  // chi 4G/BG96. WiFi khong duoc bien dich vao.
#define NET_MODE_WIFI 1 // chi WiFi. GSM TAT HOAN TOAN: khong mo UART modem,
                        // khong goi bat cu ham TinyGSM nao.  <- de TEST OTA
#define NET_MODE_AUTO 2 // WiFi uu tien, mat WiFi thi tu chuyen sang 4G va
                        // nguoc lai.

// ─────────────────────────────────────────────────────────────────────────────
//                        ↓↓↓  DOI CHE DO TAI DAY  ↓↓↓
// ─────────────────────────────────────────────────────────────────────────────
#ifndef NET_MODE // van co the ghi de bang -DNET_MODE=... neu can
#define NET_MODE NET_MODE_WIFI
#endif
// ─────────────────────────────────────────────────────────────────────────────

// NET_MODE_WIFI va NET_MODE_AUTO can SSID+PASS trong include/secrets.h.
// LUU Y: ESP32-S3 chi bat duoc WiFi 2.4GHz.

// Co bien dich — dung cho #if de loai han code cua duong mang khong dung.
#define NET_HAS_WIFI (NET_MODE == NET_MODE_WIFI || NET_MODE == NET_MODE_AUTO)
#define NET_HAS_GSM (NET_MODE == NET_MODE_GSM || NET_MODE == NET_MODE_AUTO)

// Duong mang dang thuc su duoc dung (chi doi runtime o NET_MODE_AUTO).
enum class ConnMode
{
    None,
    Wifi,
    Gsm
};

// ── Tham so cho NET_MODE_AUTO ────────────────────────────────────────────────
// Cho WiFi len bao lau truoc khi chap nhan roi xuong 4G (luc boot).
constexpr uint32_t NET_WIFI_BOOT_TIMEOUT_MS = 15000;
// Rate-limit cho lan thu ket noi 4G LAI (lan dau thu ngay, khong cho).
constexpr uint32_t NET_GSM_RETRY_INTERVAL_MS = 30000;
// Bao lau kiem tra link GPRS mot lan. KHONG kiem tra moi vong lap: spam
// AT+CGATT? se tranh kenh AT voi PubSubClient va gay false positive "mat link".
constexpr uint32_t NET_GSM_LINK_CHECK_MS = 20000;
// Bao lau quet lai xem WiFi da ve chua khi dang chay bang 4G.
constexpr uint32_t NET_WIFI_RECHECK_MS = 30000;
