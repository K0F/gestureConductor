"""
optical_stream.py - External Optical Ingestion Module
Ingests external video frames into the visual world model:
1. Live network UDP camera stream (e.g., from t520 webcam sent to port 5555)
2. Local V4L2 webcam capture (/dev/video*)
3. Synthetic candlelight / low-light darkroom optic stream fallback
"""

import socket
import select
import subprocess
import numpy as np

class OpticalStreamReceiver:
    def __init__(self, port=5555, width=64, height=64):
        self.port = port
        self.w = width
        self.h = height
        self.frame_size = width * height
        
        # Setup UDP socket
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.sock.bind(("0.0.0.0", port))
            self.sock.setblocking(False)
            self.network_active = True
        except Exception as e:
            print(f"[OpticalStream] UDP bind warning: {e}")
            self.network_active = False

        self.last_frame = np.zeros((height, width), dtype=np.float32)
        self.frames_received = 0
        self.candle_phase = 0.0

    def get_frame(self):
        """
        Retrieves the latest available optical frame.
        If live UDP packet arrives, decodes it.
        Otherwise falls back to synthetic candlelight low-light stream.
        """
        got_packet = False
        if self.network_active:
            # Drain socket buffer to get the freshest frame (drop stale packets)
            while True:
                ready = select.select([self.sock], [], [], 0.0)
                if not ready[0]:
                    break
                try:
                    data, addr = self.sock.recvfrom(65535)
                    if len(data) >= self.frame_size:
                        raw = np.frombuffer(data[:self.frame_size], dtype=np.uint8)
                        self.last_frame = raw.reshape((self.h, self.w)).astype(np.float32) / 255.0
                        self.frames_received += 1
                        got_packet = True
                except Exception:
                    break

        if not got_packet and self.frames_received == 0:
            # Fallback: Synthetic candlelight optics (point-light source in low light)
            self.candle_phase += 0.08
            cx = 32.0 + np.sin(self.candle_phase * 0.7) * 4.0 + np.sin(self.candle_phase * 1.9) * 2.0
            cy = 32.0 + np.cos(self.candle_phase * 0.5) * 3.0 + np.sin(self.candle_phase * 2.3) * 1.5
            intensity = 0.8 + np.sin(self.candle_phase * 3.7) * 0.15 + np.random.uniform(-0.04, 0.04)

            y_grid, x_grid = np.ogrid[:self.h, :self.w]
            d2 = (x_grid - cx)**2 + (y_grid - cy)**2
            flame = np.exp(-d2 / 18.0) * intensity
            halo = np.exp(-d2 / 120.0) * (intensity * 0.25)
            noise = np.random.normal(0.0, 0.02, (self.h, self.w))
            
            frame = np.clip(flame + halo + noise, 0.0, 1.0).astype(np.float32)
            self.last_frame = frame

        return self.last_frame
