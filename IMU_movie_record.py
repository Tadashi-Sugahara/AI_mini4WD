import cv2
import serial
import serial.tools.list_ports
import threading
import time
from datetime import datetime
import tkinter as tk
from PIL import Image, ImageTk

BAUD_RATE = 115200

class SyncRecorderApp:
    def __init__(self, root, port, cam_index):
        self.root = root
        self.root.title("GP-Chip Data Recorder")
        self.root.configure(bg="#222222")
        
        self.is_recording = False
        self.video_out = None
        self.txt_file = None
        self.running = True
        self.latest_imu_data = "Waiting for IMU sensor data..."
        self.current_frame = None # スレッド間で共有する最新の映像フレーム

        self.port = port
        self.cam_index = cam_index

        # --- シリアルポートの初期化 ---
        try:
            self.ser = serial.Serial(self.port, BAUD_RATE, timeout=0.1)
            print(f"[{self.port}] に接続しました。")
        except Exception as e:
            print(f"シリアルポートエラー: {e}")
            self.ser = None

        # --- Webカメラの初期化 ---
        self.cap = cv2.VideoCapture(self.cam_index, cv2.CAP_DSHOW)
        if not self.cap.isOpened():
            print(f"エラー: カメラ番号 {self.cam_index} にアクセスできません。")
        else:
            # カメラに明示的に30fpsを要求
            self.cap.set(cv2.CAP_PROP_FPS, 30.0)
            self.width = int(self.cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            self.height = int(self.cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
            
            # 実際のFPSを取得（異常値の場合は30に固定）
            self.fps = self.cap.get(cv2.CAP_PROP_FPS)
            if self.fps <= 0 or self.fps > 120:
                self.fps = 30.0

        # --- GUIレイアウトの構築 ---
        self.video_label = tk.Label(root, bg="#000000")
        self.video_label.pack(pady=5, padx=5)

        btn_frame = tk.Frame(root, bg="#222222")
        btn_frame.pack(fill=tk.X, pady=5, padx=20)

        self.btn_start = tk.Button(btn_frame, text="● 録画開始", bg="lightgreen", font=("Arial", 12, "bold"), command=self.start_recording)
        self.btn_start.pack(side=tk.LEFT, expand=True, fill=tk.X, padx=10, ipady=5)

        self.btn_stop = tk.Button(btn_frame, text="■ 録画停止", bg="salmon", font=("Arial", 12, "bold"), state=tk.DISABLED, command=self.stop_recording)
        self.btn_stop.pack(side=tk.RIGHT, expand=True, fill=tk.X, padx=10, ipady=5)

        self.imu_label = tk.Label(root, text=self.latest_imu_data, 
                                  font=("Consolas", 14, "bold"), bg="#000000", fg="#00FF00", 
                                  relief=tk.SUNKEN, bd=3, pady=10)
        self.imu_label.pack(fill=tk.X, padx=20, pady=10)

        # --- スレッドの開始 ---
        # 1. IMU受信スレッド
        self.serial_thread = threading.Thread(target=self.read_serial, daemon=True)
        self.serial_thread.start()
        
        # 2. カメラ録画専用スレッド (GUIの遅延から切り離す)
        self.camera_thread = threading.Thread(target=self.camera_loop, daemon=True)
        self.camera_thread.start()

        # 3. GUI描画ループの開始
        self.update_gui()

    def camera_loop(self):
        """別スレッドでカメラ映像を全力で取得し、録画する"""
        while self.running:
            if self.cap.isOpened():
                ret, frame = self.cap.read()
                if ret:
                    # 映像へのタイムスタンプ焼き付け
                    now_str = datetime.now().strftime('%H:%M:%S.%f')[:-3]
                    cv2.putText(frame, now_str, (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)

                    # 録画中ならフレームを保存（画面描画の遅さに影響されない）
                    if self.is_recording and self.video_out is not None:
                        self.video_out.write(frame)

                    # GUI表示用に最新フレームを共有変数にコピー
                    self.current_frame = frame.copy()
            else:
                time.sleep(0.01)

    def update_gui(self):
        """Tkinterの画面更新ループ（描画が遅れても録画には影響しない）"""
        if self.current_frame is not None:
            # OpenCV BGR -> RGB
            cv2_img = cv2.cvtColor(self.current_frame, cv2.COLOR_BGR2RGB)
            img = Image.fromarray(cv2_img)
            imgtk = ImageTk.PhotoImage(image=img)
            self.video_label.imgtk = imgtk
            self.video_label.configure(image=imgtk)

        self.imu_label.config(text=self.latest_imu_data)

        if self.running:
            # GUIは適当なペース(約30fps)で更新を試みる
            self.root.after(30, self.update_gui)

    def start_recording(self):
        if not self.is_recording and self.cap.isOpened():
            timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
            video_filename = f'record_{timestamp}.mp4'
            txt_filename = f'record_{timestamp}.txt'

            fourcc = cv2.VideoWriter_fourcc(*'mp4v')
            # 取得した正確なFPSで動画ファイルを作成
            self.video_out = cv2.VideoWriter(video_filename, fourcc, self.fps, (self.width, self.height))
            
            self.txt_file = open(txt_filename, 'w', encoding='utf-8')
            
            self.is_recording = True
            self.btn_start.config(state=tk.DISABLED)
            self.btn_stop.config(state=tk.NORMAL)
            print(f"録画開始: {video_filename} (FPS: {self.fps}) / {txt_filename}")

    def stop_recording(self):
        if self.is_recording:
            self.is_recording = False
            
            if self.video_out:
                self.video_out.release()
                self.video_out = None
            if self.txt_file:
                self.txt_file.close()
                self.txt_file = None

            self.btn_start.config(state=tk.NORMAL)
            self.btn_stop.config(state=tk.DISABLED)
            print("録画を停止し、ファイルを保存しました。")

    def read_serial(self):
        """別スレッドでシリアルポートを常時監視"""
        while self.running:
            if self.ser and self.ser.in_waiting > 0:
                try:
                    line = self.ser.readline().decode('utf-8').strip()
                    if line.count(',') == 5:
                        v = line.split(',')
                        formatted_str = f"Accel(G): X={v[0]:>5} Y={v[1]:>5} Z={v[2]:>5} | Gyro: X={v[3]:>6} Y={v[4]:>6} Z={v[5]:>6}"
                        self.latest_imu_data = formatted_str

                        if self.is_recording and self.txt_file and not self.txt_file.closed:
                            now_str = datetime.now().strftime('%H:%M:%S.%f')[:-3]
                            self.txt_file.write(f"[{now_str}] {line}\n")
                            self.txt_file.flush() 
                except Exception:
                    pass
            time.sleep(0.001)

    def on_close(self):
        self.running = False
        self.stop_recording()
        if self.ser:
            self.ser.close()
        if self.cap.isOpened():
            self.cap.release()
        self.root.destroy()

if __name__ == "__main__":
    print("========================================")
    print(" GP-Chip Data Recorder - 起動セットアップ")
    print("========================================\n")

    ports = serial.tools.list_ports.comports()
    if ports:
        print("【現在認識されているCOMポート】")
        for p in ports:
            print(f" - {p.device} : {p.description}")
    else:
        print("【警告】利用可能なCOMポートが見つかりません。")
    print("")

    port_input = input("使用するCOMポートを入力してください (例: COM5) [デフォルト: COM5]: ").strip()
    if not port_input:
        port_input = "COM5"

    cam_input = input("使用するカメラ番号を入力してください (例: 0, 1) [デフォルト: 0]: ").strip()
    try:
        cam_index = int(cam_input) if cam_input else 0
    except ValueError:
        cam_index = 0

    print(f"\n>> {port_input} と カメラ {cam_index} でシステムを起動します...\n")

    root = tk.Tk()
    app = SyncRecorderApp(root, port_input, cam_index)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()