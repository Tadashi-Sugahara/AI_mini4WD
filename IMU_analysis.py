import serial
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from collections import deque

# --- 設定 ---
PORT = 'COM5'      # ★ご自身の環境に合わせてCOMポートを変更してください（例: 'COM5', '/dev/ttyUSB0'）
BAUD_RATE = 115200
MAX_POINTS = 100   # グラフのX軸に表示する最大データ数（約10秒分）

# データ格納用のキュー（指定した最大数を超えると古いデータから自動で消えます）
ax_data = deque(maxlen=MAX_POINTS)
ay_data = deque(maxlen=MAX_POINTS)
az_data = deque(maxlen=MAX_POINTS)
gx_data = deque(maxlen=MAX_POINTS)
gy_data = deque(maxlen=MAX_POINTS)
gz_data = deque(maxlen=MAX_POINTS)

# シリアル通信の開始
try:
    ser = serial.Serial(PORT, BAUD_RATE, timeout=0.1)
    print(f"[{PORT}] に接続しました。グラフを描画します...")
except Exception as e:
    print(f"シリアルポートのオープンに失敗しました: {e}")
    print("Arduino IDEのシリアルモニタを開いたままにしていませんか？閉じてから再実行してください。")
    exit()

# グラフの初期化
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(8, 6.4))
fig.canvas.manager.set_window_title('IMU Real-time Telemetry')

# [上段] 加速度グラフの設定
ax1.set_title("Accelerometer (Ax, Ay, Az)")
ax1.set_ylabel("Accel [g]")
ax1.set_ylim(-3, 3) # 値の振れ幅に合わせて調整してください
line_ax, = ax1.plot([], [], label='Ax', color='red')
line_ay, = ax1.plot([], [], label='Ay', color='green')
line_az, = ax1.plot([], [], label='Az', color='blue')
ax1.legend(loc='upper right')
ax1.grid(True, linestyle=':', color='gray')

# [下段] ジャイログラフの設定
ax2.set_title("Gyroscope (Gx, Gy, Gz)")
ax2.set_ylabel("Gyro [dps]")
ax2.set_ylim(-250, 250) # 値の振れ幅に合わせて調整してください
line_gx, = ax2.plot([], [], label='Gx', color='red')
line_gy, = ax2.plot([], [], label='Gy', color='green')
line_gz, = ax2.plot([], [], label='Gz', color='blue')
ax2.legend(loc='upper right')
ax2.grid(True, linestyle=':', color='gray')

# アニメーション更新用の関数
def update(frame):
    # バッファに溜まっているすべてのシリアルデータを読み込む
    while ser.in_waiting > 0:
        try:
            line = ser.readline().decode('utf-8').strip()
            if not line:
                continue
            
            # カンマ区切りで分割
            data = line.split(',')
            
            # ESP32C6の厳格なフィルターのおかげで基本は6個揃っていますが、念のためチェック
            if len(data) == 6:
                val_ax, val_ay, val_az, val_gx, val_gy, val_gz = map(float, data)
                
                # データをキューに追加
                ax_data.append(val_ax)
                ay_data.append(val_ay)
                az_data.append(val_az)
                gx_data.append(val_gx)
                gy_data.append(val_gy)
                gz_data.append(val_gz)
                
        except Exception as e:
            # パース失敗時などは無視して続行
            pass

    # グラフの描画更新
    if len(ax_data) > 0:
        x = list(range(len(ax_data)))
        
        # 加速度グラフ更新
        line_ax.set_data(x, ax_data)
        line_ay.set_data(x, ay_data)
        line_az.set_data(x, az_data)
        ax1.set_xlim(0, MAX_POINTS)
        
        # ジャイログラフ更新
        line_gx.set_data(x, gx_data)
        line_gy.set_data(x, gy_data)
        line_gz.set_data(x, gz_data)
        ax2.set_xlim(0, MAX_POINTS)

    return line_ax, line_ay, line_az, line_gx, line_gy, line_gz

# アニメーション開始 (50ミリ秒ごとに画面更新)
ani = animation.FuncAnimation(fig, update, interval=50, blit=False, cache_frame_data=False)

plt.tight_layout()
plt.show()

# ウィンドウを閉じたらシリアルポートを開放
ser.close()
print("切断しました。")