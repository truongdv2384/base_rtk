#include "ota_rtk.h"

#include <Update.h>
#include <serial_log.h>

#include "ota_sha256.h"

namespace {

PubSubClient *s_mqtt = nullptr;
Client *s_client = nullptr;
const char *s_statusTopic = nullptr;

struct Request {
    bool pending = false;
    char url[192] = {};
    uint32_t size = 0;   // kich thuoc anh theo manifest (0 = khong khai bao)
    char sha256[65] = {}; // hex 64 ky tu (rong = bo qua verify)
};

Request s_req;

// ── Status ───────────────────────────────────────────────────────────────────

void status(const char *st, uint32_t done = 0, uint32_t total = 0)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"ota\":\"%s\",\"done\":%u,\"total\":%u}",
             st, (unsigned)done, (unsigned)total);
    SerialLog::log(msg);
    if (s_mqtt && s_statusTopic && s_mqtt->connected())
        s_mqtt->publish(s_statusTopic, msg);
}

// ── JSON toi gian ────────────────────────────────────────────────────────────
// Chi can 3 field co dinh -> khong keo them ArduinoJson vao build.

bool jsonString(const char *json, const char *key, char *out, size_t outSize)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return false;
    p = strchr(p + strlen(pat), ':');
    if (!p)
        return false;
    p = strchr(p, '"');
    if (!p)
        return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end)
        return false;
    size_t n = (size_t)(end - p);
    if (n >= outSize)
        return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

bool jsonUint(const char *json, const char *key, uint32_t &out)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return false;
    p = strchr(p + strlen(pat), ':');
    if (!p)
        return false;
    out = (uint32_t)strtoul(p + 1, nullptr, 10);
    return true;
}

// ── HTTP GET thu cong tren Client bat ky ─────────────────────────────────────
// HTTPClient cua esp32 chi nhan NetworkClient (WiFi/Ethernet), khong nhan
// TinyGsmClient -> tu viet de mot duong code chay ca 4G lan WiFi.

bool parseHttpUrl(const String &url, String &host, uint16_t &port, String &path)
{
    String rest = url;
    if (rest.startsWith("https://"))
    {
        SerialLog::log("[OTA] HTTPS khong duoc ho tro");
        return false;
    }
    if (rest.startsWith("http://"))
        rest = rest.substring(7);

    const int slashIdx = rest.indexOf('/');
    const String hostPort = (slashIdx >= 0) ? rest.substring(0, slashIdx) : rest;
    path = (slashIdx >= 0) ? rest.substring(slashIdx) : "/";

    const int colonIdx = hostPort.indexOf(':');
    if (colonIdx >= 0)
    {
        host = hostPort.substring(0, colonIdx);
        port = (uint16_t)hostPort.substring(colonIdx + 1).toInt();
    }
    else
    {
        host = hostPort;
        port = 80;
    }
    return host.length() > 0;
}

bool waitAvailable(Client &c, uint32_t idleTimeoutMs)
{
    const unsigned long deadline = millis() + idleTimeoutMs;
    while (c.connected() && !c.available())
    {
        if ((long)(millis() - deadline) > 0)
            return false;
        delay(10);
    }
    return c.available() > 0;
}

bool readLine(Client &c, char *buf, size_t bufSize)
{
    size_t len = 0;
    while (true)
    {
        if (!waitAvailable(c, OTA_HTTP_IDLE_TIMEOUT_MS))
            return false;
        const char ch = (char)c.read();
        if (ch == '\n')
            break;
        if (ch != '\r' && len < bufSize - 1)
            buf[len++] = ch;
    }
    buf[len] = '\0';
    return true;
}

// Gui GET + parse header. true => status 200, contentLength > 0, stream dinh vi
// ngay dau body.
bool httpGet(Client &c, const char *url, uint32_t &contentLength)
{
    String host, path;
    uint16_t port;
    if (!parseHttpUrl(String(url), host, port, path))
    {
        SerialLog::log("[OTA] URL khong hop le");
        return false;
    }

    SerialLog::log("[OTA] GET", host.c_str(), port, path.c_str());
    if (!c.connect(host.c_str(), port))
    {
        SerialLog::log("[OTA] khong ket noi duoc HTTP server");
        return false;
    }

    // HTTP/1.0 + Connection: close -> khong dinh chunked encoding, Content-Length
    // luon co, doc toi khi dong socket la het body.
    c.print(String("GET ") + path + " HTTP/1.0\r\nHost: " + host +
            "\r\nConnection: close\r\n\r\n");

    char line[192];
    if (!readLine(c, line, sizeof(line)) || strstr(line, " 200 ") == nullptr)
    {
        SerialLog::log("[OTA] HTTP status khong hop le", line);
        return false;
    }

    contentLength = 0;
    while (readLine(c, line, sizeof(line)))
    {
        if (line[0] == '\0')
            break; // het header
        if (strncasecmp(line, "Content-Length:", 15) == 0)
            contentLength = (uint32_t)strtoul(line + 15, nullptr, 10);
    }
    return contentLength > 0;
}

// ── Thuc thi OTA ─────────────────────────────────────────────────────────────

void runUpdate()
{
    status("start");

    uint32_t contentLength = 0;
    if (!httpGet(*s_client, s_req.url, contentLength))
    {
        s_client->stop();
        status("http_failed");
        return;
    }

    if (contentLength > OTA_MAX_IMAGE_BYTES)
    {
        SerialLog::log("[OTA] kich thuoc bat thuong", contentLength);
        s_client->stop();
        status("bad_size");
        return;
    }

    // Manifest la nguon chan ly: server noi bao nhieu byte thi phai dung bay nhieu.
    if (s_req.size && contentLength != s_req.size)
    {
        SerialLog::log("[OTA] size lech manifest", contentLength, s_req.size);
        s_client->stop();
        status("size_mismatch");
        return;
    }

    if (!Update.begin(contentLength))
    {
        SerialLog::log("[OTA] Update.begin that bai", Update.errorString());
        s_client->stop();
        status("begin_failed");
        return;
    }
    // TAT AES: setupCrypt(nullptr) KHONG set duoc mode (key null -> bail) nen
    // Updater giu AUTO va doi giai ma. Phai set tay.
    Update.setCryptMode(U_AES_DECRYPT_NONE);

    OtaSha256 sha;
    sha.begin();

    static uint8_t blk[OTA_IO_BLOCK];
    uint32_t received = 0;
    uint32_t lastReport = 0;
    bool ok = true;

    while (received < contentLength)
    {
        if (!waitAvailable(*s_client, OTA_HTTP_IDLE_TIMEOUT_MS))
        {
            SerialLog::log("[OTA] timeout cho du lieu tai", received);
            ok = false;
            break;
        }

        const size_t want = (size_t)min((uint32_t)sizeof(blk), contentLength - received);
        const int got = s_client->read(blk, want);
        if (got <= 0)
            continue;

        if (Update.write(blk, (size_t)got) != (size_t)got)
        {
            SerialLog::log("[OTA] Update.write that bai", received, Update.errorString());
            ok = false;
            break;
        }
        sha.update(blk, (size_t)got);
        received += (uint32_t)got;

        // Bao tien do moi ~10%. MQTT co the da rot trong luc tai (keepalive) —
        // status() tu bo qua khi mat ket noi, Serial van co.
        const uint32_t pct = (uint64_t)received * 10 / contentLength;
        if (pct != lastReport)
        {
            lastReport = pct;
            status("downloading", received, contentLength);
        }
        yield();
    }
    s_client->stop();

    if (ok && received != contentLength)
    {
        SerialLog::log("[OTA] tai thieu", received, contentLength);
        ok = false;
    }

    // Verify TOAN ANH truoc khi chot boot partition.
    if (ok && s_req.sha256[0])
    {
        uint8_t digest[32];
        sha.finish(digest);
        if (!OtaSha256::equalsHex(digest, s_req.sha256))
        {
            SerialLog::log("[OTA] SHA-256 MISMATCH");
            ok = false;
        }
        else
        {
            SerialLog::log("[OTA] SHA-256 OK");
        }
    }
    else if (ok)
    {
        SerialLog::log("[OTA] khong co sha256 trong lenh -> bo qua verify");
    }

    if (!ok)
    {
        Update.abort();
        status("failed", received, contentLength);
        return;
    }

    if (!Update.end())
    {
        SerialLog::log("[OTA] Update.end that bai", Update.errorString());
        Update.abort();
        status("end_failed");
        return;
    }

    status("ok", received, contentLength);
    SerialLog::log("[OTA] nap xong -> reboot de self-test");
    delay(500);
    ESP.restart();
}

} // namespace

namespace OtaRtk {

void begin(PubSubClient &mqtt, Client &otaClient, const char *statusTopic)
{
    s_mqtt = &mqtt;
    s_client = &otaClient;
    s_statusTopic = statusTopic;
}

void setClient(Client &otaClient) { s_client = &otaClient; }

bool handleCommand(const char *payload, size_t len)
{
    if (s_req.pending)
    {
        SerialLog::log("[OTA] dang co yeu cau khac -> bo qua");
        return false;
    }

    char json[320];
    const size_t n = len < sizeof(json) - 1 ? len : sizeof(json) - 1;
    memcpy(json, payload, n);
    json[n] = '\0';

    Request r;
    if (!jsonString(json, "url", r.url, sizeof(r.url)))
    {
        SerialLog::log("[OTA] lenh thieu \"url\"");
        status("bad_command");
        return false;
    }
    jsonUint(json, "size", r.size);
    jsonString(json, "sha256", r.sha256, sizeof(r.sha256));

    r.pending = true;
    s_req = r;
    SerialLog::log("[OTA] nhan lenh", r.url, r.size);
    status("queued");
    return true;
}

void tick()
{
    if (!s_req.pending || !s_client)
        return;
    runUpdate();
    // Chi toi day khi OTA that bai (thanh cong thi da reboot).
    s_req = Request{};
}

bool busy() { return s_req.pending; }

} // namespace OtaRtk
