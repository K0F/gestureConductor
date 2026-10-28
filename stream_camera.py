"""
stream_camera.py - Stream t520 Webcam to Bazina over UDP
Captures /dev/video0, scales to 64x64 grayscale, and transmits via UDP
to bazina (10.0.0.15:5555) with sub-millisecond latency.
"""

import sys
import time
import socket
import subprocess

def main():
    target_ip = sys.argv[1] if len(sys.argv) > 1 else "10.0.0.15"
    target_port = int(sys.argv[2]) if len(sys.argv) > 2 else 5555

    print(f"[CameraStreamer] Streaming /dev/video0 -> {target_ip}:{target_port} (64x64 @ 30 FPS)")
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    cmd = [
        "ffmpeg", "-f", "v4l2", "-video_size", "320x240", "-framerate", "30",
        "-i", "/dev/video0",
        "-vf", "scale=64:64,format=gray",
        "-f", "rawvideo", "-"
    ]
    
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    frame_bytes = 64 * 64
    count = 0
    t0 = time.time()

    try:
        while True:
            raw = proc.stdout.read(frame_bytes)
            if len(raw) < frame_bytes:
                break
            sock.sendto(raw, (target_ip, target_port))
            count += 1
            if count % 150 == 0:
                fps = count / (time.time() - t0)
                print(f"[CameraStreamer] Sent {count} frames (~{fps:.1f} FPS)")
    except KeyboardInterrupt:
        print("\n[CameraStreamer] Stopped.")
    finally:
        proc.terminate()
        sock.close()

if __name__ == "__main__":
    main()
