# Halo CE cho iPhone (port native, không giả lập)

Bản port iOS của [OpenCE](https://github.com/OpenCommunityEdition/OpenCE) — bản decompile của Halo: Combat Evolved (Xbox, build 2342) chạy native trên Windows, Linux, Android và web.

**Không kèm dữ liệu game.** Bạn cần file đĩa Xbox (`.iso`/`.xiso`) Halo: Combat Evolved của chính bạn.

## Cài và chơi

1. Tải `HaloCE-unsigned.ipa` (Actions → artifact, hoặc nhánh `ci-logs`), ký và cài bằng Sideloadly/AltStore như các bản port trước.
2. Mở app một lần, rồi dùng app **Tệp → Trên iPhone → Halo CE**, chép file `.iso` vào đó.
3. Mở lại app: lần đầu app tự chép thư mục `maps/` ra khỏi ISO (khoảng 2 GB, mất một lúc). Xong có thể xoá ISO.
4. Điều khiển: cảm ứng (analog trái kéo-giữ, vuốt nửa phải để xoay camera, các nút) hoặc tay cầm Xbox qua Bluetooth. Overlay tự ẩn khi có tay cầm (`input.touch_controls` trong `config.toml`).

Log lần chạy gần nhất: `halo-log.txt` trong cùng thư mục.

## Cách hoạt động

iOS không cho app tự nạp mã thực thi lúc chạy (cần JIT) và không cho dùng vùng địa chỉ dưới 4 GB — hai việc bản Android làm. Game lại cần con trỏ 32-bit. Vì vậy:

1. **Guest**: đúng bộ mã guest của bản Android (game + lớp nền tảng + musl + runtime), biên dịch cho **wasm32** thay vì arm64_32 (`ios/tools/build_guest.py`, `ios/guest`).
2. **wasm2c** dịch module wasm đó thành C (WABT, có vá thêm `memory.atomic.wait/notify`), rồi Xcode biên dịch native cho arm64. Không JIT, không trình thông dịch.
3. **Host** (`ios/host`): bộ nhớ chia sẻ + mỗi luồng một instance, giả lập syscall Linux trên Darwin, SDL3 (UIKit) + OpenGL ES 3.0, overlay cảm ứng UIKit, giải nén ISO. Lớp bọc import sinh tự động từ chính các khai báo của bản Android (`ios/tools/gen_imports.py`).

Build: `scripts/build_ios.sh` trên macOS (GitHub Actions `macos-15`).

## Chưa có (bản đầu)

Chơi mạng (socket, DTLS, UPnP), Steam Workshop/MCC maps, rung.

## Giấy phép

AGPL-3.0, như OpenCE (một số tệp host là bản sửa từ `port/android/host` của OpenCE).
