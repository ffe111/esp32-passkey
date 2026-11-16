# ความปลอดภัย / Security notes

ESP32 ไม่มี secure element จึงปลอดภัยน้อยกว่า YubiKey หรือ Titan Key
เหมาะกับการใช้งานส่วนตัว การเรียนรู้ และเป็นคีย์สำรอง ส่วนบัญชีสำคัญมากควรใช้คีย์ที่ผ่านการรับรอง

## สิ่งที่ควรทำ
1. **ตั้ง PIN เสมอ** (`passkey-cli pin set`) ถ้าไม่มี PIN ใครได้บอร์ดไปก็ใช้คีย์ได้ทันที
2. **เปิด flash encryption** สำหรับการใช้งานจริง ถ้าไม่เปิด คนที่ถือบอร์ดสามารถอ่าน flash ออกมาได้
3. ลงทะเบียนคีย์อย่างน้อย 2 ตัวกับบัญชีสำคัญ เผื่อคีย์หาย

## ความต่างตามรุ่นชิป
| ชิป | ที่เก็บคีย์หลักของอุปกรณ์ (OTP) | หมายเหตุ |
|---|---|---|
| ESP32-S2/S3, C3, C6 | eFuse (โค้ด upstream ของ Pico FIDO) | เฟิร์มแวร์ upstream **เขียน eFuse ตอนบูตครั้งแรก** ซึ่งย้อนกลับไม่ได้ |
| ESP32 (รุ่นแรก) | NVS ใน flash (`firmware/serial-hid/otp_esp32_nvs.c`) | ไม่เผา eFuse ย้อนกลับได้ด้วยการ erase flash แต่ปลอดภัยเท่าที่ flash ปลอดภัย จึงควรเปิด flash encryption |

## Transport แบบ serial
- ใครที่ต่อสาย USB ได้ก็ส่งคำสั่ง CTAP ได้ แบบเดียวกับ USB key ทั่วไป การกดปุ่ม BOOT ยังเป็นการยืนยันตัวตน (user presence) และ PIN ยังป้องกันอยู่
- โหมดนี้ปิด log ทาง UART0 แล้ว แต่ข้อความบูตจาก ROM ยังออกมาตอน reset ซึ่ง host จะข้ามให้อัตโนมัติ
- ปิดแอป OATH/OTP ไว้ เพราะต้องใช้ CCID หรือคีย์บอร์ด USB

## รายงานช่องโหว่
ถ้าเป็นปัญหาในตัวแกน FIDO ให้รายงานที่ upstream: https://github.com/polhenarejos/pico-fido/security
ถ้าเป็นปัญหาในส่วนที่ repo นี้เพิ่มเอง (serial transport, CLI, bridge) ให้เปิด issue แบบ private ใน repo นี้
