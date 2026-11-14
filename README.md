# ĐỀ 1 LAB3
Trương Hải Dương 
 Mạch Giám sát và cảnh báo người chạy (runner)
https://wokwi.com/projects/476779027964731393
## 1. Tính năng chính

- **Đo và hiển thị:** đọc nhiệt độ, áp suất mỗi giây, tính độ cao từ áp suất, in ra Serial Monitor.
- **Gửi dữ liệu thời gian thực:** ESP32 gửi gói JSON lên broker; trang web hiển thị nhiệt độ, áp suất, độ cao dạng số và hai biểu đồ nhiệt độ, độ cao theo thời gian.
- **Cảnh báo tự động:**
  - Nhiệt độ ngoài khoảng 22 đến 30 °C: **LED sáng**.
  - Độ cao thay đổi quá 8 m trong 10 giây (dấu hiệu bất thường khi di chuyển): **còi kêu 10 giây**.
- **Điều khiển thủ công:** bật/tắt LED và còi bằng nút trên web hoặc 2 nút nhấn vật lý trên mạch; chuyển qua lại giữa chế độ tự động và thủ công.
- **Tắt cảnh báo thông minh:** ở chế độ tự động, bấm tắt chỉ tắt cảnh báo hiện tại; LED tự sáng lại khi nhiệt độ vượt ngưỡng lần mới, đổi phía (nóng sang lạnh hoặc ngược lại) hoặc xấu thêm từ 2 °C; còi tự kêu lại khi có lần thay đổi độ cao đột ngột mới.

## 2. Sơ đồ đấu nối phần cứng

| Linh kiện | Chân trên linh kiện | Chân ESP32 |
|---|---|---|
| BMP180 | VCC, GND | 3V3, GND |
| BMP180 | SDA, SCL (I2C) | GPIO21, GPIO22 |
| LED cảnh báo | Chân dương | GPIO19 |
| Còi (buzzer) | Chân dương | GPIO18 |
| Nút nhấn LED | Một chân (có điện trở kéo xuống, nhấn = HIGH) | GPIO25 |
| Nút nhấn còi | Một chân (có điện trở kéo xuống, nhấn = HIGH) | GPIO26 |

## 3. Kiến trúc FreeRTOS

### 3.1. Bảng các task

| Task | Lõi | Ưu tiên | Nhiệm vụ |
|---|---|---|---|
| Task truyền data | Core 0 | 1 | Kết nối WiFi; khi thành công thì kết nối MQTT, đọc `dataQueue` và gửi lên broker, nhận lệnh từ web |
| Task đọc data | Core 1 | 2 | Đọc BMP180 mỗi 1 giây, in Serial, gửi dữ liệu đầy đủ vào `dataQueue` và dữ liệu cảnh báo (nhiệt độ, độ cao) vào `alertQueue` |
| Task cảnh báo | Core 1 | 3 | Đọc `alertQueue` và lệnh điều khiển, quyết định trạng thái LED và còi |
| Task nút nhấn | Core 1 | 2 | Đọc 2 nút vật lý mỗi 10 ms, chống dội 50 ms, gửi lệnh vào hàng đợi lệnh |

### 3.2. Các đối tượng giao tiếp giữa task

| Đối tượng | Loại | Từ task | Tới task | Nội dung |
|---|---|---|---|---|
| `dataQueue` | Queue 5 ô | Task đọc | Task truyền | Nhiệt độ, áp suất, độ cao ("data Queue" của đề) |
| `alertQueue` | Queue 5 ô | Task đọc | Task cảnh báo | Nhiệt độ, độ cao ("data Alert" của đề) |
| `cmdQueue` | Queue 8 ô | Task truyền, Task nút nhấn | Task cảnh báo | Lệnh bật/tắt, đổi chế độ |
| `statusQueue` | Queue 1 ô | Task cảnh báo | Task truyền | Trạng thái LED, còi, chế độ hiện tại |
| `semWiFi` | Binary semaphore | Sự kiện WiFi | Task truyền | Báo đã kết nối WiFi thành công |
| `serialMutex` | Mutex | Các task | Các task | Khóa Serial dùng chung giữa hai lõi |

## 4. Giao tiếp MQTT

- Broker: `broker.emqx.io`, cổng `1883` (ESP32), `8084` WebSocket bảo mật (trang web).
- Topic dữ liệu (ESP32 gửi lên): `runner_truongduong_2026/data`
- Topic lệnh (web gửi xuống): `runner_truongduong_2026/cmd`

Ví dụ gói dữ liệu:

```json
{"t":26.12,"p":1012.35,"h":12.30,"led":1,"buzzer":0,"mode":"auto","alert":"TEMP_WARNING","alarmLed":1,"alarmBuzzer":0,"muteLed":0,"muteBuzzer":0}
```

Các lệnh điều khiển: `led_on`, `led_off`, `buzzer_on`, `buzzer_off`, `auto`, `manual`.

## 5. Hướng dẫn cài đặt

### 5.1. Chạy trên Wokwi

Mở link mô phỏng ở đầu trang, bấm nút chạy. Thư viện cần có trong Library Manager của Wokwi: **Adafruit BMP085 Library**, **PubSubClient**.

### 5.2. Biên dịch bằng Arduino IDE

1. Cài Arduino IDE, vào **File → Preferences**, thêm đường dẫn board ESP32:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. Vào **Boards Manager**, cài **esp32 by Espressif Systems**, chọn board **ESP32 Dev Module**.
3. Vào **Library Manager**, cài **Adafruit BMP085 Library** (chọn Install All) và **PubSubClient** (Nick O'Leary).
4. Mở `new.ino`. Nếu chạy trên mạch thật, sửa `WIFI_SSID` và `WIFI_PASS` thành WiFi của bạn.
5. Nạp vào ESP32, hoặc dùng **Sketch → Export Compiled Binary** để lấy file `.bin` nạp vào Wokwi.

## 6. Cách vận hành

1. Chạy mạch, mở Serial Monitor tốc độ **115200**, chờ ESP32 kết nối WiFi và MQTT.
2. Mở file `runner_monitor_dashboard.html` bằng trình duyệt (Chrome, Edge). Khi trang báo đã kết nối broker, số đo và biểu đồ cập nhật mỗi giây.
3. Thử cảnh báo trên Wokwi: bấm vào BMP180, kéo nhiệt độ ra ngoài 22 đến 30 °C để LED sáng; kéo áp suất thay đổi nhanh để còi kêu.
4. Điều khiển: dùng nút LED, Còi trên web hoặc nút nhấn trên mạch; chuyển chế độ Tự động / Thủ công.
5. Có thể kiểm tra bằng MQTTX: kết nối `broker.emqx.io:1883`, đăng ký topic `runner_truongduong_2026/#`.

## 7. Video demo

- [Video 1](2026-11-14%2022-45-34.mp4)
- [Video 2](2026-11-14%2022-49-19.mp4)
- [Video 3](2026-11-14%2022-50-48.mp4)

## 8. Cấu trúc dự án

```
├── new.ino                          # Chương trình ESP32 (FreeRTOS + BMP180 + MQTT)
├── runner_monitor_dashboard.html    # Giao diện web giám sát và điều khiển
├── LAB3_của_Dương_da_sua.docx       # Báo cáo chi tiết
├── 2026-11-14 22-45-34.mp4          # Video demo
├── 2026-11-14 22-49-19.mp4          # Video demo
├── 2026-11-14 22-50-48.mp4          # Video demo
└── README.md
```

## 9. Lưu ý

- Topic trong `new.ino` và trong file web phải giống hệt nhau (phân biệt chữ hoa, chữ thường).
- BMP180 đo nhiệt độ **môi trường**, không phải nhiệt độ cơ thể người chạy.
- Hệ thống dùng broker công cộng nên kết nối có thể chập chờn; khi triển khai thực tế nên dùng broker riêng có tài khoản và mã hóa TLS.
- Nếu Serial báo không tìm thấy BMP180, kiểm tra lại dây SDA, SCL.

## 10. Giấy phép

Dự án thực hiện phục vụ mục đích học tập, nghiên cứu, phi thương mại.
