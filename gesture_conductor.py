"""
gesture_conductor.py - Dual-Hand Zero-Latency Optical Gesture Conductor
Seamlessly isolates and tracks ONE moving hand or BOTH moving hands:
1. Right Hand: The Baton / Timekeeper (rhythmic strikes, pitch base, stereo pan)
2. Left Hand: Expressive Dynamics (modal gong accents, filter resonance, reverb)
3. Hand Span: Spreading hands apart opens stereo field and harmonic density
4. Dual Strike: Simultaneous downbeat triggers orchestral tutti sub-drop.
Uses OpenCV V4L2 background thread + motion contour clustering (zero static background).
"""

import sys
import os
import time
import socket
import struct
import threading
import argparse
import numpy as np
import pygame
import cv2

def make_osc_msg(address, *args):
    """Zero-dependency binary OSC encoder."""
    b_addr = address.encode('utf-8') + b'\0'
    while len(b_addr) % 4 != 0: b_addr += b'\0'
    
    tags = ','
    payload = b''
    for a in args:
        if isinstance(a, float) or isinstance(a, np.floating):
            tags += 'f'
            payload += struct.pack('>f', float(a))
        elif isinstance(a, int) or isinstance(a, np.integer):
            tags += 'i'
            payload += struct.pack('>i', int(a))
        elif isinstance(a, str):
            tags += 's'
            b_s = a.encode('utf-8') + b'\0'
            while len(b_s) % 4 != 0: b_s += b'\0'
            payload += b_s
            
    b_tags = tags.encode('utf-8') + b'\0'
    while len(b_tags) % 4 != 0: b_tags += b'\0'
    return b_addr + b_tags + payload

class ZeroLagCameraGrabber:
    """Native V4L2 capture with dedicated daemon thread to prevent pipe buffering."""
    def __init__(self, device_id=0, width=160, height=120):
        self.w = width
        self.h = height
        self.cap = cv2.VideoCapture(device_id, cv2.CAP_V4L2)
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        self.cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

        self.latest_frame = np.zeros((height, width), dtype=np.uint8)
        self.running = True
        self.lock = threading.Lock()
        
        self.thread = threading.Thread(target=self._worker, daemon=True)
        self.thread.start()
        print(f"[Camera] Zero-latency V4L2 capture active on /dev/video{device_id} ({width}x{height})")

    def _worker(self):
        while self.running:
            ret, frame = self.cap.read()
            if ret:
                gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
                # Flip horizontally for natural conducting mirror
                gray = cv2.flip(gray, 1)
                # Ensure fixed resolution
                if gray.shape[1] != self.w or gray.shape[0] != self.h:
                    gray = cv2.resize(gray, (self.w, self.h))
                with self.lock:
                    self.latest_frame = gray
            else:
                time.sleep(0.01)

    def get_frame(self):
        with self.lock:
            return self.latest_frame.copy()

    def close(self):
        self.running = False
        if self.cap.isOpened():
            self.cap.release()

class HandState:
    """Tracks kinematics and ictus strikes with adaptive 1-Euro smoothing."""
    def __init__(self, name="Hand"):
        self.name = name
        self.active = False
        self.active_frames = 0
        self.x = 0.5
        self.y = 0.5
        self.prev_x = 0.5
        self.prev_y = 0.5
        self.vx = 0.0
        self.vy = 0.0
        self.prev_vy = 0.0
        self.energy = 0.0
        self.ictus_cooldown = 0
        self.ictus_flash = 0.0

    def update(self, target_x, target_y, area):
        self.active = True
        self.active_frames = 12  # Hold active state across micro-pauses (~400ms)
        
        # Adaptive 1-Euro filtering: heavy smoothing when still, zero lag when moving fast
        dx = target_x - self.x
        dy = target_y - self.y
        dist = np.sqrt(dx * dx + dy * dy)
        alpha = float(np.clip(0.18 + dist * 9.0, 0.18, 0.72))
        
        self.x = alpha * target_x + (1.0 - alpha) * self.x
        self.y = alpha * target_y + (1.0 - alpha) * self.y
        
        self.vx = self.x - self.prev_x
        self.vy = self.y - self.prev_y
        
        speed = np.sqrt(self.vx * self.vx + self.vy * self.vy)
        self.energy = 0.4 * (speed * 28.0 + area / 250.0) + 0.6 * self.energy
        
        # Conducting Ictus Detector (Downbeat rebound: vy flips positive->negative OR high jerk)
        is_ictus = False
        intensity = 0.0
        jerk = abs(self.vy - self.prev_vy) * 50.0
        
        if self.ictus_cooldown <= 0:
            if (self.prev_vy > 0.006 and self.vy <= 0.002) or (jerk > 0.38) or (speed > 0.04 and jerk > 0.28):
                is_ictus = True
                intensity = float(np.clip(max(jerk, self.energy * 1.5), 0.25, 2.5))
                self.ictus_flash = 1.0
                self.ictus_cooldown = 6  # ~200ms lockout
                
        self.prev_x = self.x
        self.prev_y = self.y
        self.prev_vy = self.vy
        if self.ictus_cooldown > 0:
            self.ictus_cooldown -= 1
        self.ictus_flash *= 0.82
        
        return is_ictus, intensity

    def decay(self):
        if self.active_frames > 0:
            self.active_frames -= 1
        else:
            self.active = False
            
        self.energy *= 0.88
        self.vx *= 0.6
        self.vy *= 0.6
        self.ictus_flash *= 0.80
        if self.ictus_cooldown > 0:
            self.ictus_cooldown -= 1

class DualHandConductor:
    """Isolates moving hands via contour clustering on temporal frame difference."""
    def __init__(self, diff_threshold=22, min_area=35):
        self.diff_thresh = diff_threshold
        self.min_area = min_area
        self.prev_gray = None
        
        self.left_hand = HandState("Left")
        self.right_hand = HandState("Right")
        self.span = 0.5

    def process(self, gray_frame):
        # 1. Subtle Gaussian blur to eliminate CMOS pixel sensor noise
        blurred = cv2.GaussianBlur(gray_frame, (5, 5), 0)
        
        if self.prev_gray is None or self.prev_gray.shape != gray_frame.shape:
            self.prev_gray = blurred.copy()
            return np.zeros_like(gray_frame), False, 0.0, False, 0.0, False

        # Temporal Motion Difference (Eliminates static background)
        diff = cv2.absdiff(blurred, self.prev_gray)
        self.prev_gray = blurred.copy()

        _, thresh = cv2.threshold(diff, self.diff_thresh, 255, cv2.THRESH_BINARY)
        # Morphological opening (removes salt-and-pepper dots) then dilation
        kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3))
        thresh = cv2.morphologyEx(thresh, cv2.MORPH_OPEN, kernel)
        thresh = cv2.dilate(thresh, kernel, iterations=1)

        contours, _ = cv2.findContours(thresh, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        
        valid_blobs = []
        h, w = gray_frame.shape
        for c in contours:
            area = cv2.contourArea(c)
            if area >= self.min_area:
                M = cv2.moments(c)
                if M['m00'] > 0:
                    cx = float(M['m10'] / M['m00']) / w
                    cy = float(M['m01'] / M['m00']) / h
                    valid_blobs.append((cx, cy, area))

        # Sort blobs by area (largest moving hands first)
        valid_blobs.sort(key=lambda b: b[2], reverse=True)

        left_ictus, left_int = False, 0.0
        right_ictus, right_int = False, 0.0

        if len(valid_blobs) == 0:
            # No hands moving
            self.left_hand.decay()
            self.right_hand.decay()
            
        elif len(valid_blobs) == 1:
            # Single hand moving: proximity-based assignment with hysteresis
            bx, by, b_area = valid_blobs[0]
            dist_left = abs(bx - self.left_hand.x) if self.left_hand.active else 999.0
            dist_right = abs(bx - self.right_hand.x) if self.right_hand.active else 999.0
            
            if dist_right < dist_left:
                right_ictus, right_int = self.right_hand.update(bx, by, b_area)
                self.left_hand.decay()
            elif dist_left < dist_right:
                left_ictus, left_int = self.left_hand.update(bx, by, b_area)
                self.right_hand.decay()
            else:
                if bx >= 0.5:
                    right_ictus, right_int = self.right_hand.update(bx, by, b_area)
                    self.left_hand.decay()
                else:
                    left_ictus, left_int = self.left_hand.update(bx, by, b_area)
                    self.right_hand.decay()
                
        else:
            # BOTH HANDS MOVING!
            # Take the top 2 largest moving blobs and sort from left to right
            top2 = sorted(valid_blobs[:2], key=lambda b: b[0])
            lx, ly, l_area = top2[0]
            rx, ry, r_area = top2[1]
            
            left_ictus, left_int = self.left_hand.update(lx, ly, l_area)
            right_ictus, right_int = self.right_hand.update(rx, ry, r_area)
            
            # Compute Hand Span (distance between hands)
            dx = self.right_hand.x - self.left_hand.x
            dy = self.right_hand.y - self.left_hand.y
            self.span = float(np.sqrt(dx*dx + dy*dy))

        # Check for Dual Tutti Strike (both hands striking together)
        is_tutti = False
        if left_ictus and right_ictus:
            is_tutti = True

        return thresh, left_ictus, left_int, right_ictus, right_int, is_tutti

        return thresh, left_ictus, left_int, right_ictus, right_int, is_tutti

def main():
    parser = argparse.ArgumentParser(description="Zero-Latency Dual-Hand Gesture Conductor")
    parser.add_argument("--sc-host", type=str, default="127.0.0.1", help="SuperCollider IP host")
    parser.add_argument("--sc-port", type=int, default=57130, help="SuperCollider UDP port (default 57130)")
    parser.add_argument("--device", type=int, default=0, help="V4L2 Camera ID")
    parser.add_argument("--thresh", type=int, default=22, help="Difference threshold (default 22)")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    targets = list({(args.sc_host, args.sc_port), (args.sc_host, 57120), (args.sc_host, 57130)})
    print(f"[Conductor] Dispatching Dual-Hand OSC to SuperCollider at targets: {targets}")

    def send_osc_all(pkt):
        for tgt in targets:
            try:
                sock.sendto(pkt, tgt)
            except Exception:
                pass

    camera = ZeroLagCameraGrabber(device_id=args.device, width=160, height=120)
    engine = DualHandConductor(diff_threshold=args.thresh, min_area=35)

    pygame.init()
    pygame.font.init()
    width, height = 1120, 640
    screen = pygame.display.set_mode((width, height))
    pygame.display.set_caption("PixelSense :: Dual-Hand Orchestral Conductor (kof)")
    clock = pygame.time.Clock()

    font = pygame.font.SysFont("monospace", 13)
    font_bold = pygame.font.SysFont("monospace", 15, bold=True)
    font_large = pygame.font.SysFont("monospace", 18, bold=True)

    left_trail = []
    right_trail = []
    running = True

    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT or (event.type == pygame.KEYDOWN and event.key in [pygame.K_ESCAPE, pygame.K_q]):
                running = False

        gray = camera.get_frame()
        motion_mask, l_hit, l_int, r_hit, r_int, is_tutti = engine.process(gray)

        # 1. Send Ictus Strikes (Accents)
        if is_tutti:
            # Tutti Strike (Dual downbeat)
            intensity = max(l_int, r_int)
            pkt = make_osc_msg("/conductor/ictus", float(intensity), 0.5, 0.2)
            send_osc_all(pkt)
        elif r_hit or l_hit:
            hit_x = float(engine.right_hand.x if r_hit else engine.left_hand.x)
            hit_y = float(1.0 - (engine.right_hand.y if r_hit else engine.left_hand.y))
            hit_int = float(r_int if r_hit else l_int)
            pkt = make_osc_msg("/conductor/ictus", hit_int, hit_x, hit_y)
            send_osc_all(pkt)

        # 2. Continuous Flow: Determine primary active hand coordinates
        # If right hand is active, it guides pitch & pan. If only left hand is active, it guides pitch & pan.
        if engine.right_hand.active and not engine.left_hand.active:
            prim_x = engine.right_hand.x
            prim_y = engine.right_hand.y
            span_val = 0.2
        elif engine.left_hand.active and not engine.right_hand.active:
            prim_x = engine.left_hand.x
            prim_y = engine.left_hand.y
            span_val = 0.2
        elif engine.right_hand.active and engine.left_hand.active:
            prim_x = engine.right_hand.x
            prim_y = engine.right_hand.y
            span_val = float(np.clip(engine.span, 0.1, 1.5))
        else:
            prim_x = 0.5
            prim_y = 0.5
            span_val = 0.2

        # Invert Y so hand raised high = high pitch & bright filter
        y_elev = float(np.clip(1.0 - prim_y, 0.0, 1.0))
        # Pan: -1.0 (left) to +1.0 (right)
        x_pan = float(np.clip((prim_x - 0.5) * 2.2, -1.0, 1.0))
        tot_energy = float(engine.left_hand.energy + engine.right_hand.energy)

        # Send Continuous Morph: [yElev (0..1), xPan (-1..1), energy (0..3), span (0..1.5)]
        pkt_morph = make_osc_msg("/conductor/morph", y_elev, x_pan, tot_energy, span_val)
        send_osc_all(pkt_morph)

        # 3. Tension / Kinetic Surge
        tot_energy = float(engine.left_hand.energy + engine.right_hand.energy)
        pkt_tension = make_osc_msg("/conductor/tension", tot_energy, float(is_tutti * 2.0))
        send_osc_all(pkt_tension)

        # Update visual trails
        if engine.left_hand.active:
            left_trail.append((engine.left_hand.x, engine.left_hand.y))
        if len(left_trail) > 60:
            left_trail.pop(0)

        if engine.right_hand.active:
            right_trail.append((engine.right_hand.x, engine.right_hand.y))
        if len(right_trail) > 60:
            right_trail.pop(0)

        # --- DRAWING PIPELINE ---
        screen.fill((8, 10, 14))

        p_w, p_h = 320, 240
        p1_x, p1_y = 40, 50
        p2_x = 390
        p3_x = 740

        # Viewport 1: Mirror Feed
        rgb_cam = cv2.cvtColor(gray, cv2.COLOR_GRAY2RGB)
        surf_cam = pygame.transform.scale(pygame.surfarray.make_surface(rgb_cam.swapaxes(0, 1)), (p_w, p_h))
        screen.blit(surf_cam, (p1_x, p1_y))
        pygame.draw.rect(screen, (35, 45, 55), (p1_x - 1, p1_y - 1, p_w + 2, p_h + 2), 1)
        screen.blit(font_bold.render("1. OPTICAL MIRROR (ZERO-LAG)", True, (200, 220, 240)), (p1_x, p1_y - 24))

        # Viewport 2: Motion Mask (Zero Background)
        rgb_mot = np.zeros((motion_mask.shape[0], motion_mask.shape[1], 3), dtype=np.uint8)
        rgb_mot[:, :, 1] = motion_mask
        rgb_mot[:, :, 2] = motion_mask
        surf_mot = pygame.transform.scale(pygame.surfarray.make_surface(rgb_mot.swapaxes(0, 1)), (p_w, p_h))
        screen.blit(surf_mot, (p2_x, p1_y))
        pygame.draw.rect(screen, (25, 60, 75), (p2_x - 1, p1_y - 1, p_w + 2, p_h + 2), 1)
        screen.blit(font_bold.render("2. DUAL HAND ISOLATION MASK", True, (60, 210, 255)), (p2_x, p1_y - 24))

        # Overlay Hand Markers on motion viewport
        if engine.left_hand.active:
            lx_px = int(p2_x + engine.left_hand.x * p_w)
            ly_px = int(p1_y + engine.left_hand.y * p_h)
            pygame.draw.circle(screen, (40, 240, 160), (lx_px, ly_px), 12, 2)
            screen.blit(font_bold.render("LEFT", True, (40, 240, 160)), (lx_px - 16, ly_px - 22))

        if engine.right_hand.active:
            rx_px = int(p2_x + engine.right_hand.x * p_w)
            ry_px = int(p1_y + engine.right_hand.y * p_h)
            pygame.draw.circle(screen, (255, 190, 50), (rx_px, ry_px), 12, 2)
            screen.blit(font_bold.render("RIGHT", True, (255, 190, 50)), (rx_px - 18, ry_px - 22))

        if engine.left_hand.active and engine.right_hand.active:
            # Span line
            pygame.draw.line(screen, (100, 200, 255), (lx_px, ly_px), (rx_px, ry_px), 1)

        # Viewport 3: Dual Ictus Strike Indicators
        pygame.draw.rect(screen, (14, 18, 24), (p3_x, p1_y, p_w, p_h))
        pygame.draw.rect(screen, (80, 50, 20), (p3_x, p1_y, p_w, p_h), 1)
        screen.blit(font_bold.render("3. CONDUCTING ICTUS STRIKES", True, (255, 180, 50)), (p3_x, p1_y - 24))

        # Left Strike indicator
        l_flash = engine.left_hand.ictus_flash
        l_r = int(18 + l_flash * 45)
        l_col = (int(40 + 200 * l_flash), int(220 + 35 * l_flash), int(140 * (1 - l_flash)))
        pygame.draw.circle(screen, l_col, (p3_x + 80, p1_y + 110), l_r, 2)
        screen.blit(font_bold.render("LEFT GONG", True, (60, 220, 160)), (p3_x + 45, p1_y + 175))

        # Right Strike indicator
        r_flash = engine.right_hand.ictus_flash
        r_r = int(18 + r_flash * 45)
        r_col = (int(255 * (0.6 + 0.4 * r_flash)), int(180 + 75 * r_flash), int(40 * (1 - r_flash)))
        pygame.draw.circle(screen, r_col, (p3_x + 240, p1_y + 110), r_r, 2)
        screen.blit(font_bold.render("RIGHT CHIME", True, (255, 190, 50)), (p3_x + 200, p1_y + 175))

        if is_tutti:
            screen.blit(font_large.render(">>> TUTTI DOWNBEAT! <<<", True, (255, 240, 120)), (p3_x + 40, p1_y + 35))

        # Bottom Left: Conducting Orbit Scope
        b_x, b_y, b_w, b_h = 40, 320, 670, 280
        pygame.draw.rect(screen, (12, 16, 22), (b_x, b_y, b_w, b_h))
        pygame.draw.rect(screen, (25, 45, 65), (b_x, b_y, b_w, b_h), 1)
        screen.blit(font_large.render("DUAL-HAND CONDUCTING ORBITS (SPACE & ELEVATION)", True, (180, 200, 220)), (b_x + 16, b_y + 14))

        # Grid lines
        pygame.draw.line(screen, (22, 30, 40), (b_x + b_w//2, b_y + 35), (b_x + b_w//2, b_y + b_h - 15), 1)
        pygame.draw.line(screen, (22, 30, 40), (b_x + 20, b_y + b_h//2 + 10), (b_x + b_w - 20, b_y + b_h//2 + 10), 1)

        # Left trail (Cyan/Green)
        if len(left_trail) > 1:
            for k in range(1, len(left_trail)):
                x1 = int(b_x + 20 + left_trail[k-1][0] * (b_w - 40))
                y1 = int(b_y + 35 + left_trail[k-1][1] * (b_h - 50))
                x2 = int(b_x + 20 + left_trail[k][0] * (b_w - 40))
                y2 = int(b_y + 35 + left_trail[k][1] * (b_h - 50))
                pygame.draw.line(screen, (40, 220, 160), (x1, y1), (x2, y2), 2)

        # Right trail (Amber/Gold)
        if len(right_trail) > 1:
            for k in range(1, len(right_trail)):
                x1 = int(b_x + 20 + right_trail[k-1][0] * (b_w - 40))
                y1 = int(b_y + 35 + right_trail[k-1][1] * (b_h - 50))
                x2 = int(b_x + 20 + right_trail[k][0] * (b_w - 40))
                y2 = int(b_y + 35 + right_trail[k][1] * (b_h - 50))
                pygame.draw.line(screen, (255, 190, 50), (x1, y1), (x2, y2), 2)

        # Bottom Right: Telemetry
        t_x, t_y, t_w, t_h = 740, 320, 320, 280
        pygame.draw.rect(screen, (14, 18, 24), (t_x, t_y, t_w, t_h))
        pygame.draw.rect(screen, (30, 40, 50), (t_x, t_y, t_w, t_h), 1)
        screen.blit(font_bold.render("ORCHESTRAL TELEMETRY", True, (240, 205, 50)), (t_x + 14, t_y + 14))

        telemetry = [
            f"Active Hands: {'BOTH' if (engine.left_hand.active and engine.right_hand.active) else ('RIGHT' if engine.right_hand.active else ('LEFT' if engine.left_hand.active else 'NONE'))}",
            f"Hand Span:    {engine.span:.2f} (Stereo Width)",
            "",
            f"Pitch / Elev: {y_elev:.2f} (4-Octave Sweep)",
            f"Stereo Pan:   {x_pan:+.2f} (L/R Balance)",
            f"Kinetic Speed:{tot_energy:.2f} (Saturation)",
            "",
            f"OSC Targets:  {', '.join(f'{t[0]}:{t[1]}' for t in targets)}",
            "",
            "CONDUCTING GESTURES:",
            "• Vertical Y:   Pitch & Moog Cutoff",
            "• Horizontal X: Stereo Panning",
            "• Downbeat Snap:ICTUS Resonant Strike",
            "• [Q] to quit"
        ]

        ty = t_y + 42
        for line in telemetry:
            if "Active Hands:" in line or "TUTTI" in line:
                col = (60, 240, 160)
            elif "RIGHT" in line:
                col = (255, 200, 60)
            elif "LEFT" in line:
                col = (100, 220, 240)
            elif line.startswith("•"):
                col = (140, 160, 180)
            else:
                col = (180, 200, 220)
            screen.blit(font.render(line, True, col), (t_x + 14, ty))
            ty += 18

        pygame.display.flip()
        clock.tick(30)

    camera.close()
    sock.close()
    pygame.quit()

if __name__ == "__main__":
    main()
