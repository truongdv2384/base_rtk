"""Day lenh OTA cho tram RTK base qua MQTT.

Cach dung:
    1) Build firmware:      pio run
    2) Phuc vu file bin:    python -m http.server 8080     (chay tai goc du an)
    3) Sua BASE_URL ben duoi cho khop IP may nay (thiet bi phai voi toi duoc)
    4) Gui lenh:            python ota.py

Yeu cau: pip install paho-mqtt
"""

import hashlib
import os
import sys

import paho.mqtt.publish as mqtt

HOST = "103.82.22.78"
PORT = 1883
USER = None       # broker hien khong bat auth; dien vao neu sau nay bat
PASSWORD = None

DEVICE = "cacao_2_rtk_base"
CMD_TOPIC = f"rtk/{DEVICE}/cmd"

# IP cua may dang chay HTTP server — thiet bi phai voi toi duoc.
# Qua 4G thi day phai la IP/domain PUBLIC, khong phai 192.168.x.x.
#
# CHI dat toi THU MUC .pio/build — script se tu noi "<env>/firmware.bin".
# Them "/firmware.bin" vao day se thanh duong dan doi -> HTTP 404.
BASE_URL = "http://192.168.1.102:8080/.pio/build"
BUILD_DIR = ".pio/build"

# Mot env duy nhat. Che do WiFi/4G doi trong include/net_mode.h, khong phai o day.
ENV = "esp32-s3-devkitm-1"


def send(env):
    fw_path = os.path.join(BUILD_DIR, env, "firmware.bin")
    if not os.path.exists(fw_path):
        print(f"Khong thay {fw_path} — chay 'pio run' truoc da.")
        sys.exit(1)

    with open(fw_path, "rb") as f:
        data = f.read()

    sha256 = hashlib.sha256(data).hexdigest()
    size = len(data)
    url = f"{BASE_URL}/{env}/firmware.bin"

    payload = f'OTA {{"url":"{url}","size":{size},"sha256":"{sha256}"}}'

    auth = {"username": USER, "password": PASSWORD} if USER else None
    mqtt.single(CMD_TOPIC, payload=payload, hostname=HOST, port=PORT, auth=auth)

    print(f"Sent OTA command ({size} bytes, sha256={sha256[:12]}...)")
    print(f"  topic: {CMD_TOPIC}")
    print(f"  {payload}")
    print(f"Theo doi:  mosquitto_sub -h {HOST} -t 'rtk/{DEVICE}/#' -v")


if __name__ == "__main__":
    # Tham so tuy chon: ten env khac (vd "release").
    send(sys.argv[1] if len(sys.argv) > 1 else ENV)
