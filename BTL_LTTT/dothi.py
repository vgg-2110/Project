import re
import matplotlib.pyplot as plt

# ==========================================
# 1. DÁN ĐOẠN LOG TERMINAL CỦA BẠN VÀO ĐÂY
# ==========================================
raw_logs = """
[ 132s] Throughput:   2140.8 Kbps  |  Loss:  0.00%  |  FPS: 21.0  |  RSSI:  -48 dBm  |  Frames: 2836  Drop: 48
[ 133s] Throughput:   2387.1 Kbps  |  Loss:  0.00%  |  FPS: 20.0  |  RSSI:  -48 dBm  |  Frames: 2856  Drop: 48
[ 134s] Throughput:   2469.0 Kbps  |  Loss:  0.00%  |  FPS: 22.0  |  RSSI:  -45 dBm  |  Frames: 2878  Drop: 48
[ 135s] Throughput:   2443.2 Kbps  |  Loss:  0.00%  |  FPS: 20.0  |  RSSI:  -47 dBm  |  Frames: 2898  Drop: 48
[ 136s] Throughput:   2536.3 Kbps  |  Loss:  0.00%  |  FPS: 22.0  |  RSSI:  -46 dBm  |  Frames: 2920  Drop: 48
[ 137s] Throughput:   2662.8 Kbps  |  Loss:  0.00%  |  FPS: 18.0  |  RSSI:  -55 dBm  |  Frames: 2938  Drop: 48
[ 138s] Throughput:   2352.4 Kbps  |  Loss:  0.00%  |  FPS: 20.0  |  RSSI:  -57 dBm  |  Frames: 2958  Drop: 48
[ 139s] Throughput:   2590.8 Kbps  |  Loss:  0.00%  |  FPS: 22.0  |  RSSI:  -51 dBm  |  Frames: 2980  Drop: 48
[ 140s] Throughput:   2738.2 Kbps  |  Loss:  0.00%  |  FPS: 23.0  |  RSSI:  -50 dBm  |  Frames: 3003  Drop: 48
[ 141s] Throughput:   2217.8 Kbps  |  Loss:  0.00%  |  FPS: 18.0  |  RSSI:  -59 dBm  |  Frames: 3021  Drop: 48
[ 142s] Throughput:   2462.3 Kbps  |  Loss:  0.00%  |  FPS: 16.0  |  RSSI:  -57 dBm  |  Frames: 3037  Drop: 48
[ 143s] Throughput:   2532.0 Kbps  |  Loss:  0.00%  |  FPS: 24.0  |  RSSI:  -56 dBm  |  Frames: 3061  Drop: 48
"""

# ==========================================
# 2. HÀM PHÂN TÍCH DỮ LIỆU (PARSING)
# ==========================================
times, throughput, loss, fps, rssi = [], [], [], [], []

for line in raw_logs.strip().split('\n'):
    # Dùng Regex để tự động nhặt các con số ra khỏi chuỗi text
    match = re.search(r'\[\s*(\d+)s\].*?Throughput:\s*([\d.]+).*?Loss:\s*([\d.]+).*?FPS:\s*([\d.]+).*?RSSI:\s*(-?\d+)', line)
    if match:
        times.append(int(match.group(1)))
        throughput.append(float(match.group(2)))
        loss.append(float(match.group(3)))
        fps.append(float(match.group(4)))
        rssi.append(int(match.group(5)))

# ==========================================
# 3. VẼ ĐỒ THỊ CHUẨN HỌC THUẬT (BÁO CÁO)
# ==========================================
# Thiết lập font và style chung
plt.style.use('seaborn-v0_8-whitegrid')
fig, axs = plt.subplots(4, 1, figsize=(10, 12), sharex=True)
fig.suptitle('Đánh giá chất lượng kênh truyền UDP Video', fontsize=16, fontweight='bold', y=0.95)

# Trục X: Chỉnh lại thời gian bắt đầu từ 0s thay vì số giây Uptime gốc
times_normalized = [t - times[0] for t in times]

# 1. Đồ thị Throughput
axs[0].plot(times_normalized, throughput, color='green', linewidth=2, marker='o', markersize=4)
axs[0].set_ylabel('Băng thông\n(Kbps)', fontweight='bold')
axs[0].set_title('Biến thiên Thông lượng mạng (Throughput)')
axs[0].grid(True, linestyle='--', alpha=0.7)

# 2. Đồ thị Packet Loss
axs[1].plot(times_normalized, loss, color='red', linewidth=2, marker='s', markersize=4)
axs[1].set_ylabel('Rớt gói\n(%)', fontweight='bold')
axs[1].set_title('Tỷ lệ rớt gói tin (Packet Loss)')
axs[1].set_ylim(bottom=-0.5, top=max(10, max(loss) * 1.2)) # Ép khung Y đẹp hơn
axs[1].grid(True, linestyle='--', alpha=0.7)

# 3. Đồ thị FPS
axs[2].plot(times_normalized, fps, color='blue', linewidth=2, marker='^', markersize=4)
axs[2].set_ylabel('Tốc độ\n(FPS)', fontweight='bold')
axs[2].set_title('Tốc độ khung hình (Frame Per Second)')
axs[2].set_ylim(bottom=0, top=30)
axs[2].grid(True, linestyle='--', alpha=0.7)

# 4. Đồ thị RSSI
axs[3].plot(times_normalized, rssi, color='purple', linewidth=2, marker='D', markersize=4)
axs[3].set_ylabel('Cường độ sóng\n(dBm)', fontweight='bold')
axs[3].set_title('Chỉ số cường độ tín hiệu thu (RSSI)')
axs[3].set_xlabel('Thời gian thử nghiệm (giây)', fontweight='bold')
axs[3].grid(True, linestyle='--', alpha=0.7)

# Căn chỉnh lại bố cục cho đẹp và không bị đè chữ
plt.tight_layout(rect=[0, 0, 1, 0.96])

# Lưu ra file ảnh chất lượng cao để chèn vào Word/LaTeX
plt.savefig('KetQuaThucNghiem_UDP.png', dpi=300)
print("Đã lưu đồ thị thành công vào file: KetQuaThucNghiem_UDP.png")

# Hiển thị lên màn hình
plt.show()