# Robbery Bob 2: Double Trouble — port PortMaster (muOS / dArkOS)

Port bản Android `com.chillingo.robberybob2` (1.8.0, engine C++ riêng của
Level Eight, OpenGL ES 1.1, arm64-v8a) cho máy cầm tay Linux AArch64.

Game này **không phải Unity**, nên không dùng được loader Unity của NextOS.
Port có loader riêng `rb2loader` (mã nguồn trong `src/`), đóng gói theo cùng
cấu trúc PortMaster như các port NextOS:

- nạp `libc++_shared.so` và `librobberybob.so` gốc thẳng từ APK (tự làm
  relocation, shim bionic → glibc, giả lập stack guard TLS của bionic);
- giả lập JVM/JNI và phần Java của game (GameActivity, leSoundManager,
  leGameSpecificData, Firebase remote config mặc định...);
- hình: SDL2 + EGL, context GLES 1.1, hoặc GLES 2.0 + lớp giả lập GLES1 (`src/gles1emu.c`) khi máy không có GLES 1.1 (chỉnh bằng `RB2_GLES` trong `rb2.conf`); tiếng: SDL2_mixer (đọc ogg trong APK);
- save: `robberybob2/savedata/` (file `userdefaults.gsd` của game).

Quảng cáo, mua hàng, Google Play Games, YouTube đều bị tắt (trả "không có").

## Cài đặt

1. Chép `robberybob2.zip` vào thư mục `autoinstall` của PortMaster
   (muOS: `/mnt/mmc/MUOS/PortMaster/autoinstall/`), mở PortMaster để cài;
   hoặc giải nén tay để có:

   ```
   ports/Robbery Bob 2.sh
   ports/robberybob2/
   ```
2. Chép APK vào `ports/robberybob2/gamedata/` (không cần giải nén).

## Điều khiển

| Nút | Chức năng |
|---|---|
| Cần trái / D-pad | Joystick ảo (chạm giữ tại `RB2_JOY` rồi kéo) |
| Cần phải | Di chuyển con trỏ |
| A | Chạm tại con trỏ (giữ để kéo) |
| X | Nút giày (chạy nhanh, giữ để chạy) |
| Y | Dấu "+" trên nút giày (dùng item) |
| L1 | Nút dụng cụ (hình cờ lê) |
| R1 | Nút tua nhanh |
| B / Start | Nút Back của Android (tạm dừng / quay lại) |
| Select (nhấn nhả) | Đổi D-pad giữa joystick và con trỏ (máy không có cần analog) |
| Select + L1 | Chụp màn hình vào `robberybob2/screenshots/` |
| Select + Start | Thoát |

Vị trí chạm của X, Y, L1, R1 (và L2, R2 nếu muốn) chỉnh trong `robberybob2/rb2.conf`,
theo tỉ lệ màn hình: `0,0` là góc trên trái, `1,1` là góc dưới phải.

## Nhật ký

`robberybob2/log.txt`. `RB2_VERBOSE=1` trong `rb2.conf` ghi thêm file mở,
lời gọi JNI và chạm. Khi crash, log có dòng `[rb2] fatal signal` kèm
`pc librobberybob.so+0x...`.

## Build loader

Cần Python với `pip install ziglang`:

```
sh src/build.sh
```

Kết quả: `robberybob2/rb2loader` (aarch64, glibc ≥ 2.17; libc++ của game cần
glibc ≥ 2.18 lúc chạy).
