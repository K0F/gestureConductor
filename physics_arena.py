"""
physics_arena.py - Ground-Truth Kinematic Optical Environment
Supports:
1. Two-Body Sandbox with Opaque Occluder (Blind Tunnel)
2. Active Reinforcement Learning Arena with Controllable Paddle (David Silver / MuZero style)
3. Direct pixel generation (64x64) with rewards and collision feedback.
"""

import numpy as np

class ActivePaddleArena:
    """Active RL Environment: The agent must control a paddle to keep bouncing bodies in play."""
    def __init__(self, width=64, height=64):
        self.w = width
        self.h = height
        
        # Paddle state: [x_center, width, height, speed]
        self.paddle_w = 16.0
        self.paddle_h = 3.0
        self.paddle_y = float(self.h - 5)
        self.paddle_x = float(self.w / 2.0)
        self.paddle_speed = 2.2
        
        # Ball state
        self.ball_r = 3.0
        self.ball_x = 0.0
        self.ball_y = 0.0
        self.ball_vx = 0.0
        self.ball_vy = 0.0
        
        # Performance metrics
        self.score = 0
        self.consecutive_hits = 0
        self.episodes = 0
        
        self.reset()

    def reset(self):
        self.paddle_x = float(self.w / 2.0)
        self.ball_x = float(np.random.uniform(16, self.w - 16))
        self.ball_y = float(np.random.uniform(12, 28))
        angle = np.random.uniform(0.3 * np.pi, 0.7 * np.pi)  # Downward launch
        speed = np.random.uniform(1.2, 1.8)
        self.ball_vx = float(np.cos(angle) * speed)
        self.ball_vy = float(np.sin(angle) * speed)
        self.consecutive_hits = 0
        self.episodes += 1

    def step(self, action):
        """
        Actions: 0 = Left (-1), 1 = Stay (0), 2 = Right (+1)
        Returns: (reward, done)
        """
        # 1. Update Paddle
        if action == 0:
            self.paddle_x -= self.paddle_speed
        elif action == 2:
            self.paddle_x += self.paddle_speed
        
        # Paddle boundary clamps
        half_w = self.paddle_w / 2.0
        self.paddle_x = np.clip(self.paddle_x, half_w + 1, self.w - half_w - 2)

        # 2. Update Ball
        self.ball_vy += 0.015  # Slight gravity
        self.ball_x += self.ball_vx
        self.ball_y += self.ball_vy

        reward = 0.02  # Survival incentive
        done = False

        # Left / Right Wall Reflections
        if self.ball_x - self.ball_r <= 1:
            self.ball_x = 1 + self.ball_r
            self.ball_vx = -self.ball_vx
        elif self.ball_x + self.ball_r >= self.w - 2:
            self.ball_x = self.w - 2 - self.ball_r
            self.ball_vx = -self.ball_vx

        # Ceiling Bounce
        if self.ball_y - self.ball_r <= 1:
            self.ball_y = 1 + self.ball_r
            self.ball_vy = -self.ball_vy

        # Paddle Collision Detection
        p_x1 = self.paddle_x - half_w
        p_x2 = self.paddle_x + half_w
        p_y1 = self.paddle_y - self.paddle_h / 2.0
        p_y2 = self.paddle_y + self.paddle_h / 2.0

        if (self.ball_y + self.ball_r >= p_y1 and self.ball_y - self.ball_r <= p_y2):
            if self.ball_x >= p_x1 - 1 and self.ball_x <= p_x2 + 1:
                # Successful deflection!
                self.ball_y = p_y1 - self.ball_r
                # Influence reflection angle by paddle strike offset
                offset = (self.ball_x - self.paddle_x) / half_w
                self.ball_vx = offset * 2.0 + np.random.uniform(-0.2, 0.2)
                self.ball_vy = -abs(self.ball_vy) * 1.02  # Slight acceleration
                reward = 1.0
                self.consecutive_hits += 1
                self.score += 10

        # Bottom Loss Detection
        if self.ball_y - self.ball_r >= self.h - 1:
            reward = -1.0
            done = True
            self.reset()

        return float(reward), done

    def render_frame(self):
        """Renders the active RL arena to raw pixel array [64, 64] float in [0.0, 1.0]."""
        frame = np.zeros((self.h, self.w), dtype=np.float32)

        # 1. Borders
        frame[0, :] = 0.25
        frame[:, 0] = 0.25
        frame[:, -1] = 0.25

        # 2. Render Paddle
        p_x1 = int(np.clip(self.paddle_x - self.paddle_w / 2.0, 0, self.w - 1))
        p_x2 = int(np.clip(self.paddle_x + self.paddle_w / 2.0, 0, self.w - 1))
        p_y1 = int(np.clip(self.paddle_y - self.paddle_h / 2.0, 0, self.h - 1))
        p_y2 = int(np.clip(self.paddle_y + self.paddle_h / 2.0, 0, self.h - 1))
        frame[p_y1:p_y2+1, p_x1:p_x2+1] = 0.95

        # 3. Render Ball (Anti-aliased)
        y_grid, x_grid = np.ogrid[:self.h, :self.w]
        d2 = (x_grid - self.ball_x)**2 + (y_grid - self.ball_y)**2
        r2 = self.ball_r**2
        mask = np.clip(1.0 - (d2 - r2*0.6) / (r2*0.6 + 1e-5), 0.0, 1.0)
        frame = np.maximum(frame, mask * 1.0)

        return frame


class PhysicsArena:
    """Two-body kinetic sandbox with opaque occluder tunnel (Object Permanence)."""
    def __init__(self, width=64, height=64, num_bodies=2):
        self.w = width
        self.h = height
        self.num_bodies = num_bodies
        self.bodies = []
        self.reset()
        
        self.occluder_active = True
        self.occluder_x1 = 26
        self.occluder_x2 = 38
        self.external_impulse = np.array([0.0, 0.0], dtype=np.float32)

    def reset(self):
        self.bodies = []
        for i in range(self.num_bodies):
            radius = 3.5 if i == 0 else 2.8
            brightness = 1.0 if i == 0 else 0.8
            x = np.random.uniform(10, self.w - 10)
            y = np.random.uniform(10, self.h - 10)
            angle = np.random.uniform(0, 2 * np.pi)
            speed = np.random.uniform(1.2, 2.2)
            vx = np.cos(angle) * speed
            vy = np.sin(angle) * speed
            self.bodies.append({
                'x': float(x), 'y': float(y),
                'vx': float(vx), 'vy': float(vy),
                'r': float(radius), 'b': float(brightness)
            })

    def apply_action(self, fx, fy):
        self.external_impulse[0] += fx
        self.external_impulse[1] += fy

    def step(self):
        if np.linalg.norm(self.external_impulse) > 1e-4:
            self.bodies[0]['vx'] += self.external_impulse[0]
            self.bodies[0]['vy'] += self.external_impulse[1]
            self.external_impulse *= 0.2

        for b in self.bodies:
            b['vy'] += 0.02
            b['x'] += b['vx']
            b['y'] += b['vy']
            
            if b['x'] - b['r'] <= 1:
                b['x'] = 1 + b['r']
                b['vx'] = -b['vx'] * 0.95
            elif b['x'] + b['r'] >= self.w - 2:
                b['x'] = self.w - 2 - b['r']
                b['vx'] = -b['vx'] * 0.95

            if b['y'] - b['r'] <= 1:
                b['y'] = 1 + b['r']
                b['vy'] = -b['vy'] * 0.95
            elif b['y'] + b['r'] >= self.h - 2:
                b['y'] = self.h - 2 - b['r']
                b['vy'] = -b['vy'] * 0.95

        if len(self.bodies) >= 2:
            b1, b2 = self.bodies[0], self.bodies[1]
            dx = b2['x'] - b1['x']
            dy = b2['y'] - b1['y']
            dist = np.sqrt(dx*dx + dy*dy)
            min_dist = b1['r'] + b2['r']
            if dist < min_dist and dist > 1e-5:
                nx = dx / dist
                ny = dy / dist
                kx = b1['vx'] - b2['vx']
                ky = b1['vy'] - b2['vy']
                p = 2 * (nx * kx + ny * ky) / 2.0
                b1['vx'] -= p * nx
                b1['vy'] -= p * ny
                b2['vx'] += p * nx
                b2['vy'] += p * ny
                overlap = 0.5 * (min_dist - dist)
                b1['x'] -= overlap * nx
                b1['y'] -= overlap * ny
                b2['x'] += overlap * nx
                b2['y'] += overlap * ny

    def render_frame(self):
        frame = np.zeros((self.h, self.w), dtype=np.float32)
        frame[0, :] = 0.25
        frame[-1, :] = 0.25
        frame[:, 0] = 0.25
        frame[:, -1] = 0.25

        y_grid, x_grid = np.ogrid[:self.h, :self.w]
        for b in self.bodies:
            d2 = (x_grid - b['x'])**2 + (y_grid - b['y'])**2
            r2 = b['r']**2
            mask = np.clip(1.0 - (d2 - r2*0.6) / (r2*0.6 + 1e-5), 0.0, 1.0)
            frame = np.maximum(frame, mask * b['b'])

        if self.occluder_active:
            frame[:, self.occluder_x1:self.occluder_x2] = 0.03
            frame[:, self.occluder_x1] = 0.15
            frame[:, self.occluder_x2 - 1] = 0.15

        return frame
