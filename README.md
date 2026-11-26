# ESP32-S3 Web Radio (E-Ink & TFT Dual-Display)

Internet Radio MP3 (HTTP/HTTPS) chạy trên **ESP32-S3**, **ESP-IDF v6.1**, FreeRTOS, giải mã MP3 fixed-point **libhelix-mp3**, xuất âm thanh qua DAC **PCM5102** (I2S + XSMT chống pop), điều khiển bằng núm xoay **KY-040**, LED RGB **WS2812**, giao diện **Audio Card** trên màn hình **Waveshare 1.54" E-Ink (SSD1681 200×200)** hoặc **TFT ST7789 (240×240)**, cấu hình Wi-Fi qua **Captive Portal**, và cập nhật firmware **OTA** qua trình duyệt.

---

## 1. Tính năng nổi bật

- **Phát Internet Radio MP3 (HTTP & HTTPS)**:
  - Hỗ trợ 19 kênh MP3 đã kiểm chứng (`station_list.c`), tự động xử lý HTTP redirect (`301/302/303/307/308`), tách metadata `StreamTitle` (ICY metadata), và tự động reconnect khi rớt mạng.
  - Bộ giải mã **libhelix-mp3** (fixed-point) tối ưu hiệu năng, tự động chuyển đổi sample rate (`8–48 kHz`), hỗ trợ **Mono downmix `(L+R)/2`** cho loa đơn (lưu NVS).
  - **Chống pop/click (Zero-pop)**: Mute phần cứng qua chân `XSMT` của PCM5102 + ramp âm lượng mượt + preload silence vào I2S DMA.
- **Hiển thị đa màn hình dùng chung chân SPI (`ui_display.c`)**:
  - **Chọn loại màn hình trực tiếp trên Web Dashboard** (lưu NVS, tự khởi động lại để nạp driver):
    - `epd_154`: **Waveshare 1.54" e-Paper (200×200, SSD1681, 1-bit)** — sử dụng **Partial Refresh** (~0.3 s) khi cập nhật trạng thái/quay đĩa và **Full Refresh** định kỳ mỗi 20 lần để chống bóng mờ.
    - `st7789_240`: **TFT 1.54" ST7789 (240×240, RGB565, `esp_lcd`)** — 12.5 FPS mượt mà, dùng chung hoàn toàn các chân SPI2.
  - **Xoay màn hình tức thì (`0° / 90° / 180° / 270°`)**: Áp dụng ngay lập tức không cần reboot, lưu NVS.
  - **Giao diện Audio Card hiện đại**:
    - Thanh trạng thái trên cùng: chấm trạng thái Wi-Fi + IP/AP, biểu tượng pin + `%` giả lập.
    - Phần trên (2 cột): ảnh bìa bo góc kèm **biểu tượng đĩa than quay** khi đang `PLAYING`, bên phải là thể loại, số thứ tự đài (`#n/N`) và tên đài cỡ lớn.
    - Thẻ Player bo góc: trạng thái phát, sample rate/bitrate, tên bài hát, vòng tròn chế độ núm (`VOL`/`STA`), nút `Prev / Play-Pause / Next`, biểu tượng loa, thanh âm lượng và cường độ Wi-Fi (`dBm`).
- **Biểu tượng thời lượng pin giả lập**:
  - Giả lập chu kỳ pin (`92%` → giảm `1%` mỗi 30 giây xuống `15%` rồi nạp lại `100%`), hiển thị đồng bộ trên màn hình và Web Dashboard (`/api/status.battery`).
- **Điều khiển phần cứng (KY-040 + LED WS2812)**:
  - **Xoay**: Tăng/giảm âm lượng (`±5%`) ở chế độ `VOL`, hoặc chuyển đài (coalesce 300 ms) ở chế độ `STA`.
  - **Nhấn ngắn**: Đổi chế độ núm `VOL ↔ STA`.
  - **Nhấn giữ ≥ 3 s**: Cảnh báo LED đỏ từ 1 s, xoá Wi-Fi đã lưu và khởi động lại vào SoftAP cấu hình.
  - **LED RGB trạng thái**: Tím nháy = SoftAP, vàng nháy = đang kết nối Wi-Fi, cyan = chế độ `VOL`, cam = chế độ `STA`.
- **Wi-Fi Captive Portal & Web Dashboard + OTA**:
  - Khi chưa có Wi-Fi (hoặc mất kết nối sau 5 lần thử), tự phát SoftAP **`ESP32-S3-Radio`** (`192.168.4.1`, pass `12345678`) kèm Captive DNS tự động mở trang cấu hình.
  - Dashboard web tối giản: điều khiển phát/dừng/chuyển đài, âm lượng, Mono/Stereo, chọn loại màn hình & góc xoay, quét Wi-Fi, và **Upload OTA** (2 slot `ota_0`/`ota_1` + tự động rollback nếu firmware mới lỗi).

---

## 2. Sơ đồ chân kết nối (ESP32-S3)

Các chân được chọn để **tránh hoàn toàn** chân strapping (`GPIO 0, 3, 45, 46`), USB-Serial-JTAG (`GPIO 19, 20`), UART0 (`GPIO 43, 44`) và Flash/Octal PSRAM (`GPIO 26–37`).

### 2.1 Màn hình (dùng chung SPI2 cho E-Ink & ST7789)

| Chân trên module | Chức năng | Chân ESP32-S3 | Ghi chú |
|---|---|---|---|
| **VCC** | Nguồn | **3V3** | Dùng 3.3V để đồng bộ mức logic với ESP32-S3 |
| **GND** | Mass | **GND** | |
| **DIN / SDA** | SPI MOSI | **GPIO 11** | `APP_EPD_MOSI_GPIO` (SPI2 MOSI) |
| **CLK / SCL** | SPI SCLK | **GPIO 12** | `APP_EPD_CLK_GPIO` (SPI2 CLK) |
| **CS** | Chip Select | **GPIO 10** | `APP_EPD_CS_GPIO` (SPI2 CS0, active LOW) |
| **DC** | Data / Command | **GPIO 9** | `APP_EPD_DC_GPIO` |
| **RST / RES** | Reset | **GPIO 8** | `APP_EPD_RST_GPIO` |
| **BUSY / BLK** | E-Ink BUSY / TFT BLK | **GPIO 16** | Input khi dùng E-Ink; tự động xuất HIGH (đèn nền) khi chọn ST7789 |

### 2.2 DAC PCM5102, Núm xoay KY-040 & LED RGB

| Khối | Chân tín hiệu | Chân ESP32-S3 | Ghi chú |
|---|---|---|---|
| **PCM5102** | BCK / LRCK / DIN | **GPIO 4 / 5 / 6** | I2S0 std, 16-bit stereo, SCK nối GND |
| **PCM5102** | XSMT | **GPIO 7** | LOW = mute, HIGH = unmute |
| **KY-040** | CLK / DT / SW | **GPIO 1 / 2 / 15** | Pull-up nội |
| **WS2812** | LED RGB on-board | **GPIO 48** | Đổi sang `GPIO_NUM_38` trong `main/app_config.h` nếu dùng DevKitC-1 v1.1 |

---

## 3. REST API chính

| Method | Endpoint | Mô tả |
|---|---|---|
| `GET` | `/api/status` | Trạng thái phát, đài, tiêu đề bài, âm lượng, mono, Wi-Fi, `display`, `battery` |
| `GET` | `/api/diag` | Thông số kỹ thuật: sample rate, bitrate, heap, RSSI, underrun, reconnect |
| `GET` | `/api/stations` | Danh sách 19 đài radio mặc định |
| `POST` | `/api/play`, `/api/stop`, `/api/resume`, `/api/next`, `/api/previous` | Điều khiển phát nhạc |
| `POST` | `/api/volume` | Đặt âm lượng `{"volume": 0..100}` |
| `POST` | `/api/mono` | Bật/tắt mono downmix `{"mono": true|false}` |
| `POST` | `/api/display` | Đổi loại màn hình / xoay: `{"type": "epd_154"\|"st7789_240", "rotation": 0\|90\|180\|270}` |
| `GET/POST` | `/api/wifi`, `GET /api/scan` | Quét mạng và cấu hình Wi-Fi |
| `GET/POST` | `/api/ota` | Thông tin phân vùng OTA và nạp file `.bin` |

---

## 4. Build & Nạp firmware (ESP-IDF v6.1)

```powershell
# Kích hoạt môi trường ESP-IDF v6.1
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1

idf.py set-target esp32s3
idf.py build
idf.py -p COM11 flash monitor
```

> Nếu gặp lỗi xác minh MD5 khi nạp ở tốc độ cao hoặc sai kích thước flash mặc định, có thể nạp trực tiếp bằng `esptool`:
> ```powershell
> python -m esptool --chip esp32s3 -p COM11 -b 460800 --before=default-reset --after=hard-reset write-flash --flash-mode dio --flash-freq 40m --flash-size 16MB 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x20000 build/esp32s3_radio_eink.bin
> ```
