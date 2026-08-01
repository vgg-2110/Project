import socket
import struct
import threading
import time
import queue
import collections
import numpy as np
import cv2

UDP_IP          = "0.0.0.0"
UDP_PORT        = 5000
RECV_BUFFER     = 65536        
SOCKET_RCVBUF   = 4 * 1024 * 1024  

HEADER_FORMAT   = "<IbbhH"  
HEADER_SIZE     = struct.calcsize(HEADER_FORMAT) 

FRAME_TIMEOUT   = 2.0         
STATS_INTERVAL  = 1.0           
MAX_QUEUE_SIZE  = 60           

WINDOW_NAME     = "ESP32-CAM  |  UDP Stream"
FONT            = cv2.FONT_HERSHEY_SIMPLEX

packet_queue: queue.Queue = queue.Queue(maxsize=MAX_QUEUE_SIZE)
frame_queue: queue.Queue = queue.Queue(maxsize=10)

stats_lock = threading.Lock()

stats = {
    "bytes_received"    : 0,       
    "throughput_kbps"   : 0.0,      
    "total_bytes"       : 0,      

    # ── Packet Loss ─────────────────────────
    "expected_seq"      : None,     
    "lost_packets"      : 0,       
    "received_packets"  : 0,        
    "loss_pct"          : 0.0,      

    # ── Frame stats ─────────────────────────
    "frames_decoded"    : 0,
    "frames_dropped"    : 0,       
    "fps"               : 0.0,

    # ── RSSI ────────────────────────────────
    "rssi"              : 0,        

    # ── Misc ────────────────────────────────
    "uptime_s"          : 0,

    # ── Information Theory ──────────────────
    "comp_ratio"        : 0.0,      
    "entropy"           : 0.0,    
    "frame_delay_ms"    : 0.0,  
}


history_len = 100
hist_tp = collections.deque([0.0] * history_len, maxlen=history_len)
hist_loss = collections.deque([0.0] * history_len, maxlen=history_len)
hist_entropy = collections.deque([0.0] * history_len, maxlen=history_len)

# Bộ đếm frame trong interval cho FPS
_frame_counter_interval = 0
_start_time             = time.time()


def receiver_thread():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, SOCKET_RCVBUF)
    sock.bind((UDP_IP, UDP_PORT))
    sock.settimeout(1.0)

    actual_buf = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
    print(f"[Receiver] Lắng nghe tại {UDP_IP}:{UDP_PORT}")
    print(f"[Receiver] Socket recv buffer: {actual_buf // 1024} KB")

    while not stop_event.is_set():
        try:
            data, addr = sock.recvfrom(RECV_BUFFER)
        except socket.timeout:
            continue
        except OSError:
            break

        # Thống kê throughput (tính theo raw bytes nhận được)
        with stats_lock:
            stats["bytes_received"] += len(data)
            stats["total_bytes"]    += len(data)

        # Đẩy vào queue cho Assembler
        try:
            packet_queue.put_nowait(data)
        except queue.Full:
            pass  

    sock.close()
    print("[Receiver] Dừng.")


def assembler_thread():
    global _frame_counter_interval

    buffer = {}
    last_frame_time = time.time()
    def parse(data: bytes):
        """Giải mã header, trả về (header_dict, payload_bytes) hoặc (None, None)."""
        if len(data) < HEADER_SIZE:
            return None, None
        seq, chunk_idx, total_chunks, rssi, chunk_sz = struct.unpack_from(
            HEADER_FORMAT, data
        )
        payload = data[HEADER_SIZE: HEADER_SIZE + chunk_sz]
        return {
            "seq"         : seq,
            "chunk_idx"   : chunk_idx,
            "total_chunks": total_chunks,
            "rssi"        : rssi,
            "chunk_sz"    : chunk_sz,
        }, payload

    def update_loss(seq: int):
        """Cập nhật Packet Loss khi nhận một gói UDP mới."""
        with stats_lock:
            stats["received_packets"] += 1
            expected = stats["expected_seq"]

            if expected is None:
                stats["expected_seq"] = seq + 1
                return

            if seq > expected:
                gap = seq - expected
                stats["lost_packets"]  += gap
                stats["expected_seq"]   = seq + 1
            elif seq == expected:
                stats["expected_seq"]  += 1

            total_expected = stats["received_packets"] + stats["lost_packets"]
            stats["loss_pct"] = (
                stats["lost_packets"] / total_expected * 100.0
                if total_expected > 0 else 0.0
            )
            stats["rssi"] = stats.get("rssi_latest", stats["rssi"])

    def try_assemble(seq: int):
        """Ghép JPEG nếu đã đủ chunk."""
        entry = buffer[seq]
        if len(entry["chunks"]) < entry["total"]:
            return None
        jpeg = b"".join(entry["chunks"][i] for i in range(entry["total"]))
        return jpeg

    def cleanup():
        """Loại frame timeout."""
        now = time.time()
        expired = [s for s, e in buffer.items()
                   if now - e["ts"] > FRAME_TIMEOUT]
        for s in expired:
            with stats_lock:
                stats["frames_dropped"] += 1
            del buffer[s]

    print("[Assembler] Sẵn sàng.")

    while not stop_event.is_set():
        try:
            data = packet_queue.get(timeout=0.5)
        except queue.Empty:
            cleanup()
            continue

        hdr, payload = parse(data)
        if hdr is None:
            continue

        seq   = hdr["seq"]
        cidx  = hdr["chunk_idx"]
        total = hdr["total_chunks"]
        rssi  = hdr["rssi"]

        update_loss(seq)

        # Lưu RSSI mới nhất
        with stats_lock:
            stats["rssi_latest"] = rssi

        # Tạo entry nếu chưa có
        if seq not in buffer:
            buffer[seq] = {
                "chunks": {},
                "total" : total,
                "rssi"  : rssi,
                "ts"    : time.time(),
            }

        buffer[seq]["chunks"][cidx] = payload

        # Thử ghép
        jpeg = try_assemble(seq)
        if jpeg:
            try:
                frame_queue.put_nowait(jpeg)
            except queue.Full:
                pass 
            
            # ---TÍNH DELAY (JITTER) ---
            now_ts = time.time()
            delay_ms = (now_ts - last_frame_time) * 1000.0
            last_frame_time = now_ts  
            with stats_lock:
                stats["frames_decoded"] += 1
                stats["frame_delay_ms"] = delay_ms
            _frame_counter_interval += 1
            del buffer[seq]

        if len(buffer) > 50:
            cleanup()

    print("[Assembler] Dừng.")


def draw_chart(canvas, data, x, y, w, h, color, title, is_percent=False):
    """Hàm vẽ biểu đồ Line Chart siêu tốc bằng OpenCV"""
    # Vẽ phông nền và viền cho biểu đồ
    cv2.rectangle(canvas, (x, y), (x+w, y+h), (30, 30, 30), -1)
    cv2.rectangle(canvas, (x, y), (x+w, y+h), (100, 100, 100), 1)
    cv2.putText(canvas, title, (x+5, y+15), FONT, 0.45, (220, 220, 220), 1, cv2.LINE_AA)

    if len(data) < 2: return

    # Tìm giá trị lớn nhất để scale trục Y
    max_val = max(data)
    if is_percent: 
        max_val = max(10.0, max_val) # Tối thiểu thang đo 10% cho Loss
    elif max_val == 0: 
        max_val = 1.0

    # In giá trị Max lên góc phải
    unit = "%" if is_percent else ""
    cv2.putText(canvas, f"Max: {max_val:.1f}{unit}", (x+w-80, y+15), FONT, 0.35, (150, 150, 150), 1)

    # Tính toán tọa độ các điểm (Points)
    pts = []
    for i, val in enumerate(data):
        px = x + int((i / (len(data) - 1)) * w)
        py = y + h - 5 - int((val / max_val) * (h - 30))
        pts.append((px, py))
        
    # Nối các điểm thành đường Line Chart
    cv2.polylines(canvas, [np.array(pts, dtype=np.int32)], False, color, 2, cv2.LINE_AA)


def draw_dual_chart(canvas, data1, data2, x, y, w, h, color1, color2, title1, title2):
    """Hàm vẽ biểu đồ 2 trục tung (Dual Y-Axis) cho Throughput và Entropy"""
    # Vẽ phông nền và viền
    cv2.rectangle(canvas, (x, y), (x+w, y+h), (30, 30, 30), -1)
    cv2.rectangle(canvas, (x, y), (x+w, y+h), (100, 100, 100), 1)

    if len(data1) < 2 or len(data2) < 2: return

    # Xử lý chia 0 nếu dữ liệu trống
    max1 = max(data1) if max(data1) > 0 else 1.0
    max2 = max(data2) if max(data2) > 0 else 8.0 # Giới hạn vật lý của Entropy thường là 8

    # Hiển thị Tiêu đề và Max Value cho Trục trái (Throughput - Màu xanh)
    cv2.putText(canvas, f"{title1} (Left)", (x+5, y+15), FONT, 0.45, color1, 1, cv2.LINE_AA)
    cv2.putText(canvas, f"Max: {max1:.1f}", (x+5, y+30), FONT, 0.35, color1, 1)

    # Hiển thị Tiêu đề và Max Value cho Trục phải (Entropy - Màu hồng)
    # Căn lề phải cho text
    cv2.putText(canvas, f"(Right) {title2}", (x+w-160, y+15), FONT, 0.45, color2, 1, cv2.LINE_AA)
    cv2.putText(canvas, f"Max: {max2:.2f}", (x+w-80, y+30), FONT, 0.35, color2, 1)

    # Tính toán tọa độ cho Line 1 (Throughput)
    pts1 = []
    for i, val in enumerate(data1):
        px = x + int((i / (len(data1) - 1)) * w)
        py = y + h - 5 - int((val / max1) * (h - 40)) # Trừ 40 để nhường chỗ cho text ở trên
        pts1.append((px, py))
        
    # Tính toán tọa độ cho Line 2 (Entropy)
    pts2 = []
    for i, val in enumerate(data2):
        px = x + int((i / (len(data2) - 1)) * w)
        py = y + h - 5 - int((val / max2) * (h - 40))
        pts2.append((px, py))

    # Vẽ 2 đường polylines đè lên nhau
    cv2.polylines(canvas, [np.array(pts1, dtype=np.int32)], False, color1, 2, cv2.LINE_AA)
    cv2.polylines(canvas, [np.array(pts2, dtype=np.int32)], False, color2, 2, cv2.LINE_AA)


def make_hud(frame: np.ndarray, s: dict) -> np.ndarray:
    """Vẽ thông số lên góc trái của frame."""
    overlay = frame.copy()
    h, w = frame.shape[:2]

    # Nền mờ cho HUD (Mở rộng chiều cao)
    panel_w, panel_h = 300, 260
    cv2.rectangle(overlay, (0, 0), (panel_w, panel_h), (0, 0, 0), -1)
    cv2.addWeighted(overlay, 0.55, frame, 0.45, 0, frame)

    def put(text, row, color=(200, 255, 200), scale=0.52, bold=False):
        thickness = 2 if bold else 1
        cv2.putText(frame, text, (10, 22 + row * 26),
                    FONT, scale, (0, 0, 0), thickness + 2, cv2.LINE_AA)
        cv2.putText(frame, text, (10, 22 + row * 26),
                    FONT, scale, color, thickness, cv2.LINE_AA)

    # ── Throughput ──────────────────────────────────────
    tp = s["throughput_kbps"]
    if tp >= 1000:
        tp_str = f"{tp/1000:.2f} Mbps"
        tp_col = (100, 255, 100)
    elif tp >= 100:
        tp_str = f"{tp:.1f} Kbps"
        tp_col = (200, 255, 100)
    else:
        tp_str = f"{tp:.1f} Kbps"
        tp_col = (100, 200, 255)

    # ── Packet Loss color ───────────────────────────────
    lp = s["loss_pct"]
    if lp < 1.0:
        lp_col = (100, 255, 100)    
    elif lp < 5.0:
        lp_col = (0, 200, 255)      
    else:
        lp_col = (60, 60, 255)      

    # ── RSSI color ──────────────────────────────────────
    rssi = s["rssi"]
    if rssi >= -60:
        rssi_col = (100, 255, 100)
    elif rssi >= -75:
        rssi_col = (0, 200, 255)
    else:
        rssi_col = (60, 60, 255)

    put("--- ESP32-CAM MONITOR ---",  0, (180, 220, 255), scale=0.50, bold=True)
    put(f"Throughput : {tp_str}",      1, tp_col)
    put(f"Packet Loss: {lp:.2f}%  "
        f"(lost {s['lost_packets']})",  2, lp_col)
    put(f"FPS        : {s['fps']:.1f}", 3, (200, 255, 200))
    put(f"RSSI       : {rssi} dBm",    4, rssi_col)
    put(f"Frames     : {s['frames_decoded']}", 5, (180, 180, 255))
    put(f"Dropped    : {s['frames_dropped']}", 6, (120, 120, 200))
    put(f"Uptime     : {s['uptime_s']}s",      7, (160, 160, 160), scale=0.45)
    
    # ── Information Theory Stats ─────────────────────────
    put(f"Comp Ratio : {s.get('comp_ratio', 0):.1f}x", 8, (255, 200, 100))
    put(f"Entropy    : {s.get('entropy', 0):.2f} bits/B", 9, (255, 150, 255))
    
    delay_val = s.get('frame_delay_ms', 0)
    delay_col = (100, 255, 100) if delay_val < 50 else (0, 200, 255) if delay_val < 150 else (60, 60, 255)
    put(f"Delay (Jit): {delay_val:.1f} ms", 10, delay_col)
    
    # Đường viền HUD
    cv2.rectangle(frame, (0, 0), (panel_w, panel_h), (80, 120, 80), 1)

    return frame


def display_thread():
    cv2.namedWindow(WINDOW_NAME, cv2.WINDOW_AUTOSIZE)
    print("[Display] Cửa sổ OpenCV Dashboard mở.")

    while not stop_event.is_set():
        key = cv2.waitKey(1) & 0xFF
        if key == ord("q") or key == 27:   
            stop_event.set()
            break

        try:
            jpeg_bytes = frame_queue.get(timeout=0.05)
        except queue.Empty:
            continue

        # Decode JPEG → numpy array
        nparr = np.frombuffer(jpeg_bytes, dtype=np.uint8)
        
        # ==============================================================
        # TÍNH TOÁN LÝ THUYẾT THÔNG TIN
        raw_size = 640 * 480 * 3  
        jpeg_size = len(jpeg_bytes)
        c_ratio = raw_size / jpeg_size if jpeg_size > 0 else 0

        _, counts = np.unique(nparr, return_counts=True)
        probs = counts / len(nparr)
        entropy = -np.sum(probs * np.log2(probs))

        with stats_lock:
            stats["comp_ratio"] = c_ratio
            stats["entropy"] = entropy
            s_snap = stats.copy()
        # ==============================================================

        frame = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
        if frame is None:
            continue    
        
        if frame.shape[:2] != (480, 640):
            frame = cv2.resize(frame, (640, 480))

        hist_tp.append(s_snap["throughput_kbps"])
        hist_loss.append(s_snap["loss_pct"])
        hist_entropy.append(s_snap["entropy"])

        # Vẽ bảng HUD
        frame = make_hud(frame, s_snap)

        # ========================================================
        # XÂY DỰNG KHU VỰC DASHBOARD BIỂU ĐỒ (BÊN PHẢI)
        # ========================================================
        panel_w = 460 # Mở rộng bề ngang từ 360 lên 460
        panel_h = 480
        dashboard_panel = np.zeros((panel_h, panel_w, 3), dtype=np.uint8)

        cv2.putText(dashboard_panel, "REAL-TIME TELEMETRY (DUAL-AXIS)", (70, 30), FONT, 0.6, (255, 255, 255), 2, cv2.LINE_AA)
        cv2.line(dashboard_panel, (20, 45), (panel_w-20, 45), (100, 100, 100), 1)

        # 1. Đồ thị ghép (Throughput & Entropy) - Nới chiều cao lên 200px
        draw_dual_chart(dashboard_panel, list(hist_tp), list(hist_entropy), 
                        15,  60, panel_w-30, 200, 
                        (100, 255, 100), (255, 150, 255), 
                        "Throughput", "Entropy")

        # 2. Đồ thị Packet Loss - Cao 180px
        draw_chart(dashboard_panel, list(hist_loss),    
                   15, 280, panel_w-30, 180, 
                   (60, 60, 255), "Packet Loss (%)", is_percent=True)
                   
        final_ui = np.hstack((frame, dashboard_panel))

        cv2.imshow(WINDOW_NAME, final_ui)

    cv2.destroyAllWindows()
    print("[Display] Cửa sổ đóng.")


def run_stats_loop():
    global _frame_counter_interval

    print("[Stats] Bộ đếm thống kê khởi động.")
    prev_time = time.time()

    while not stop_event.is_set():
        time.sleep(STATS_INTERVAL)

        now    = time.time()
        delta  = now - prev_time
        prev_time = now

        with stats_lock:
            # ── Throughput ──────────────────────────────
            bytes_in_interval       = stats["bytes_received"]
            stats["bytes_received"] = 0   

            kbps = (bytes_in_interval * 8) / 1000.0 / delta
            stats["throughput_kbps"] = kbps

            # ── FPS ─────────────────────────────────────
            frames_in_interval      = _frame_counter_interval
            _frame_counter_interval = 0
            stats["fps"] = frames_in_interval / delta

            # ── Uptime ──────────────────────────────────
            stats["uptime_s"] = int(now - _start_time)

            s = stats.copy()

        loss_bar = "▓" * int(s["loss_pct"] / 5) + "░" * (20 - int(s["loss_pct"] / 5))
        loss_bar = loss_bar[:20]
        print(
            f"[{s['uptime_s']:5d}s] "
            f"Throughput: {s['throughput_kbps']:8.1f} Kbps  │  "
            f"Loss: {s['loss_pct']:5.2f}%  {loss_bar}  │  "
            f"FPS: {s['fps']:5.1f}  │  "
            f"RSSI: {s['rssi']:4d} dBm  │  "
            f"Frames: {s['frames_decoded']}  Drop: {s['frames_dropped']}"
        )

    print("[Stats] Dừng.")

if __name__ == "__main__":
    stop_event = threading.Event()

    print("=" * 65)
    print("   ESP32-CAM UDP Receiver  |  nhấn Q hoặc ESC để thoát")
    print(f"   Lắng nghe cổng UDP :{UDP_PORT}")
    print("=" * 65)

    t_recv = threading.Thread(target=receiver_thread,  daemon=True, name="Receiver")
    t_asm  = threading.Thread(target=assembler_thread, daemon=True, name="Assembler")
    t_disp = threading.Thread(target=display_thread,   daemon=True, name="Display")

    t_recv.start()
    t_asm.start()
    t_disp.start()

    try:
        run_stats_loop()   
    except KeyboardInterrupt:
        print("\n[Main] Ctrl+C nhận được, đang dừng...")
        stop_event.set()

    t_recv.join(timeout=2)
    t_asm.join(timeout=2)
    t_disp.join(timeout=2)

    print("\n[Main] Thống kê phiên làm việc:")
    with stats_lock:
        s = stats.copy()
    total_expected = s["received_packets"] + s["lost_packets"]
    print(f"  Tổng bytes nhận    : {s['total_bytes'] / 1024:.1f} KB")
    print(f"  Tổng frame decode  : {s['frames_decoded']}")
    print(f"  Tổng frame dropped : {s['frames_dropped']}")
    print(f"  Packet Loss cuối   : {s['loss_pct']:.2f}%  "
          f"({s['lost_packets']} / {total_expected} gói)")
    print(f"  Thời gian chạy     : {s['uptime_s']} giây")
    print("Thoát.")