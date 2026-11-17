# ESP32 Passkey

ทำ passkey (FIDO2 / WebAuthn security key) ราคาถูกจาก ESP32 โอเพนซอร์สและฟรีทั้งหมด (AGPL-3.0)
ใช้เฟิร์มแวร์ [Pico FIDO](https://github.com/polhenarejos/pico-fido) เป็นแกนหลัก
โปรเจกต์นี้เพิ่มเครื่องมือ build ด้วย Docker, เว็บแฟลช, แอปจัดการ `passkey-cli` ที่ใช้ได้ฟรี และ transport แบบ serial สำหรับชิปที่ไม่มี USB ในตัว

> Cheap, open-source FIDO2 passkeys on ESP32. Firmware core: Pico FIDO (AGPL-3.0).
> This repo adds Docker builds, a web flasher, a free management CLI, and a serial
> transport for ESP32 chips without native USB.

## เลือกบอร์ด

| ชิป | ใช้เป็น passkey แบบไหน | Windows / macOS / Linux / iOS (USB-C) |
|---|---|---|
| **ESP32-S3** (แนะนำ), ESP32-S2 | USB security key (เสียบแล้วใช้ได้เลย เหมือน YubiKey) | ใช้ได้ทุกระบบ ไม่ต้องลงไดรเวอร์ |
| ESP32 (รุ่นแรก), ESP32-C3, ESP32-C6 | ผ่าน serial (ชิป CP210x/CH340 บนบอร์ด) | `passkey-cli` ใช้ได้ทุกระบบ ส่วนเบราว์เซอร์ใช้ได้เฉพาะ Linux ผ่าน `uhid-bridge` |

ชิปที่ไม่มี USB ในตัวไม่สามารถเป็นอุปกรณ์ USB HID ได้เอง ถ้าต้องการใช้ล็อกอินเว็บบน Windows/macOS ให้ใช้ ESP32-S3

## ติดตั้งเฟิร์มแวร์

### วิธีง่าย: เว็บแฟลช
เปิดหน้า `web-flasher/` (GitHub Pages ของ repo นี้หลัง release) ด้วย Chrome หรือ Edge แล้วกด "ติดตั้งเฟิร์มแวร์"

### Build เอง (ต้องมีแค่ Docker)
```powershell
git clone --recursive <repo-url> esp32-passkey
cd esp32-passkey
powershell -File firmware/build.ps1 -Target esp32s3   # หรือ esp32s2, esp32, esp32c3, esp32c6
```
ได้ไฟล์ `dist/passkey_<target>.bin` แล้วแฟลชด้วย
```bash
python -m esptool --chip esp32s3 --port COM5 write-flash 0x0 dist/passkey_esp32s3.bin
```
ถ้าขึ้น `Wrong boot mode detected` ให้กดปุ่ม **BOOT** ค้างไว้ตอนที่ขึ้น `Connecting...`

อัปเดตเฟิร์มแวร์ทับของเดิมได้เลย PIN และ passkey ยังอยู่ ส่วน `erase-flash` จะล้างทุกอย่าง (กลายเป็นอุปกรณ์ใหม่)

### ยืนยันตัวตนด้วยปุ่ม BOOT
ทุกครั้งที่สร้าง passkey หรือล็อกอิน ต้องกดปุ่ม **BOOT** แล้วปล่อยภายใน 30 วินาที ถ้าไม่กด คำขอจะถูกปฏิเสธ
ถ้ากดค้างเกิน 15 วินาทีก็ถูกปฏิเสธเช่นกัน (กันกรณีปุ่มค้างหรือขาลัดวงจรแล้วอนุมัติเอง)
(Pico FIDO ต้นฉบับปิดการเช็กปุ่มไว้ ทำให้โปรแกรมใดก็ได้บนคอมใช้ key ได้โดยไม่รู้ตัว เราจึงเปิดไว้เสมอ)

**ESP32 รุ่นแรก: แตะขา GPIO4 แทนปุ่มได้** บอร์ดราคาถูกบางรุ่นปุ่ม BOOT ใช้ไม่ได้ตอนเครื่องทำงาน
ให้แตะขา **GPIO4** (บนบอร์ดมักเขียนว่า D4 หรือ IO4, เป็น touch pad T0) ด้วยปลายนิ้วประมาณ 1 วินาทีแล้วปล่อย
ต้องโดนโลหะเปล่าของขา (เล็บหรือพลาสติกไม่นับ) ถ้าบอร์ดเสียบอยู่ใน breadboard ให้เสียบสายจัมเปอร์ที่ D4 แล้วจับปลายโลหะของสาย
หรือบัดกรีสายไฟ/แผ่นทองแดงต่อจากขานี้ไว้แตะก็ได้
- **อย่าแตะขานี้ตอนเสียบสาย/รีเซ็ต** เฟิร์มแวร์วัดค่าตอนไม่มีคนแตะเป็นค่าอ้างอิงตอนบูต
- ใช้ได้เฉพาะ ESP32 รุ่นแรก (C3/C6 ไม่มี touch sensor ส่วน S2/S3 ใช้ปุ่ม BOOT)
- อย่าต่อวงจรอื่นไว้ที่ GPIO4

## จัดการ passkey ด้วย passkey-cli (ฟรี)

```bash
pip install ./tools/passkey-cli
passkey-cli list                      # ดูอุปกรณ์
passkey-cli info                      # ข้อมูล authenticator
passkey-cli pin set                   # ตั้ง PIN (ควรทำทันที)
passkey-cli selftest                  # ลงทะเบียน + ล็อกอินทดสอบ (กดปุ่ม BOOT เพื่อยืนยัน)
passkey-cli creds list                # ดู passkey ที่เก็บไว้
passkey-cli creds delete <id>         # ลบ passkey
passkey-cli reset                     # ล้างทุกอย่าง
```
บอร์ดที่ใช้ serial ให้เติม `--device serial:COM3` (Windows) หรือ `--device serial:/dev/ttyUSB0` (Linux)

## ใช้กับเบราว์เซอร์บน Linux สำหรับบอร์ดที่ใช้ serial
```bash
sudo python3 tools/uhid-bridge/uhid_bridge.py serial:/dev/ttyUSB0
```
จะสร้าง security key เสมือนให้ Chrome/Firefox มองเห็น

## โครงสร้าง
```
firmware/pico-fido/     upstream Pico FIDO (git submodule, ไม่แก้ไขโดยตรง)
firmware/build.sh|ps1   build ใน Docker image espressif/idf:v5.5.4
firmware/serial-hid/    transport แบบ UART + overlay สำหรับ ESP32/C3/C6
tools/passkey-cli/      แอปจัดการ (Python, python-fido2)
tools/uhid-bridge/      Linux bridge: serial -> virtual USB FIDO key
emulator/               Pico FIDO emulator (Docker) สำหรับทดสอบโดยไม่ต้องมีบอร์ด
web-flasher/            หน้าเว็บแฟลช (ESP Web Tools)
```

## ทดสอบ
```bash
pytest tools/passkey-cli/tests                  # unit tests

# เฟิร์มแวร์ Pico FIDO ที่ build เป็นโปรแกรม Linux (TCP)
docker build -f emulator/Dockerfile -t esp32-passkey-emulator .
docker run -d --rm --name emu -p 35962:35962 esp32-passkey-emulator
PASSKEY_DEVICE=tcp:127.0.0.1:35962 pytest tools/passkey-cli/tests/test_e2e_emulator.py

# เฟิร์มแวร์ ESP32/ESP32-C3 แบบ serial ตัวจริง รันใน QEMU ของ Espressif
# QEMU ไม่มีปุ่ม BOOT จึงต้องใช้ build สำหรับทดสอบที่ไม่เช็กปุ่ม (ห้ามใช้งานจริง)
powershell -File firmware/build.ps1 -Target esp32 -NoButton    # ได้ dist/passkey_esp32_nobutton.bin
docker run -d --name qemu -p 5555:5555 -v "$PWD:/src" espressif/idf:v5.5.4 \
  bash -c "tr -d '\r' < /src/emulator/qemu-esp32.sh > /tmp/q.sh && QEMU_TARGET=esp32 FIRMWARE=/src/dist/passkey_esp32_nobutton.bin bash /tmp/q.sh"
PASSKEY_DEVICE=socket://127.0.0.1:5555 pytest tools/passkey-cli/tests/test_e2e_emulator.py

# บอร์ดจริง: กดแล้วปล่อย BOOT (หรือแตะ GPIO4 บน ESP32) เมื่อขึ้น ">>> Tap BOOT ... <<<" หรือ "Tap the button..."
PASSKEY_DEVICE=serial:COM3 pytest -v tools/passkey-cli/tests/test_e2e_emulator.py
```

### หาสาเหตุปัญหาบนบอร์ดแบบ serial
```powershell
powershell -File firmware/build.ps1 -Target esp32 -SerialDebug   # ได้ dist/passkey_esp32_debug.bin
$env:PASSKEY_TRACE = '1'; passkey-cli --device serial:COM3 selftest
```
build แบบ debug จะส่ง log ของเฟิร์มแวร์ (รวมสถานะขา BOOT) ออกมาทาง UART เดียวกัน และ `PASSKEY_TRACE=1` จะแสดง log นั้นใน terminal

## ข้อควรรู้ก่อนเผยแพร่
- **VID/PID:** README ของ Pico FIDO ห้ามแจกไบนารีที่ใช้ VID/PID ที่ไม่ใช่ของเรา ก่อน release ต้องขอ PID ฟรีจาก [pid.codes](https://pid.codes) แล้วตั้งค่าตัวแปร `USB_VID`/`USB_PID` ใน GitHub
- **ความปลอดภัย:** ดู [docs/SECURITY.md](docs/SECURITY.md)

## License
AGPL-3.0 (เหมือน Pico FIDO) ดู [LICENSE](LICENSE)
