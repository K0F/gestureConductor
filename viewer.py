"""
viewer.py - PixelSense: Complete Visual Sense-Making & Active RL Dashboard
Integrates:
1. Active Reinforcement Learning in Latent Imagination (David Silver / MuZero / Dreamer)
2. External Optical Ingestion (Live Webcam / Network UDP Stream)
3. Kinematic Physics & Object Permanence Tunnel (Ha & Schmidhuber / Hassabis)
"""

import sys
import os
import time
import argparse
import numpy as np
import pygame
import torch

from physics_arena import ActivePaddleArena, PhysicsArena
from optical_stream import OpticalStreamReceiver
from spatial_autoencoder import SpatialAutoencoder
from latent_dynamics import LatentWorldModel
from imagination_actor import DreamerLearner

def render_surface_from_numpy(arr, target_size=(256, 256), colormap="mono"):
    h, w = arr.shape
    arr_clipped = np.clip(arr, 0.0, 1.0)
    
    if colormap == "mono":
        rgb = np.zeros((h, w, 3), dtype=np.uint8)
        rgb[:, :, 0] = (arr_clipped * 210).astype(np.uint8)
        rgb[:, :, 1] = (arr_clipped * 240).astype(np.uint8)
        rgb[:, :, 2] = (arr_clipped * 220).astype(np.uint8)
    elif colormap == "dream":
        rgb = np.zeros((h, w, 3), dtype=np.uint8)
        rgb[:, :, 0] = (arr_clipped * 60).astype(np.uint8)
        rgb[:, :, 1] = (arr_clipped * 220).astype(np.uint8)
        rgb[:, :, 2] = (arr_clipped * 255).astype(np.uint8)
    elif colormap == "surprise":
        rgb = np.zeros((h, w, 3), dtype=np.uint8)
        rgb[:, :, 0] = (arr_clipped * 255).astype(np.uint8)
        rgb[:, :, 1] = (arr_clipped * 130).astype(np.uint8)
        rgb[:, :, 2] = (arr_clipped * 30).astype(np.uint8)
        
    surf = pygame.surfarray.make_surface(rgb.swapaxes(0, 1))
    return pygame.transform.scale(surf, target_size)

def main():
    parser = argparse.ArgumentParser(description="PixelSense: Visual World Model & Active RL")
    parser.add_argument("--mode", type=int, default=1, choices=[1, 2, 3], help="1=Active RL, 2=External Camera, 3=Kinematic Tunnel")
    parser.add_argument("--headless", action="store_true", help="Run without X11 window")
    parser.add_argument("--render-frames", type=int, default=0, help="Export N frames to disk")
    parser.add_argument("--output-dir", type=str, default="/mnt/ssd/render/visualWorldModel/frames")
    args = parser.parse_args()

    if args.headless:
        os.environ["SDL_VIDEODRIVER"] = "dummy"
    
    pygame.init()
    pygame.font.init()
    
    width, height = 1240, 720
    screen = pygame.display.set_mode((width, height))
    pygame.display.set_caption("PixelSense :: Visual World Model & Active Latent RL (Silver & Hassabis Lineage)")
    clock = pygame.time.Clock()

    font = pygame.font.SysFont("monospace", 13, bold=False)
    font_bold = pygame.font.SysFont("monospace", 15, bold=True)
    font_large = pygame.font.SysFont("monospace", 18, bold=True)

    device = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"[PixelSense] Active compute device: {device}")

    # Environments
    rl_arena = ActivePaddleArena(width=64, height=64)
    kinematic_arena = PhysicsArena(width=64, height=64, num_bodies=2)
    optical_receiver = OpticalStreamReceiver(port=5555, width=64, height=64)

    # Models
    vision_net = SpatialAutoencoder(latent_dim=16)
    world_net = LatentWorldModel(latent_dim=16, action_dim=3, hidden_dim=64)
    agent = DreamerLearner(vision_net, world_net, device=device, lr=1.5e-3)

    active_mode = args.mode  # 1=RL, 2=Camera, 3=Kinematic
    running = True
    online_learning = True
    frame_idx = 0
    total_reward = 0.0

    if args.render_frames > 0:
        os.makedirs(args.output_dir, exist_ok=True)
        print(f"[PixelSense] Rendering {args.render_frames} frames to {args.output_dir}")

    # Initial frame
    current_frame = rl_arena.render_frame()

    while running:
        frame_idx += 1

        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_ESCAPE or event.key == pygame.K_q:
                    running = False
                elif event.key == pygame.K_1:
                    active_mode = 1
                elif event.key == pygame.K_2:
                    active_mode = 2
                elif event.key == pygame.K_3:
                    active_mode = 3
                elif event.key == pygame.K_t:
                    online_learning = not online_learning
                elif event.key == pygame.K_r:
                    rl_arena.reset()
                    kinematic_arena.reset()
                elif event.key == pygame.K_SPACE:
                    kinematic_arena.apply_action(np.random.uniform(-3, 3), np.random.uniform(-4, -1))

        # --- STEP ENVIRONMENT & AGENT ---
        t_curr = torch.tensor(current_frame, dtype=torch.float32, device=device).unsqueeze(0).unsqueeze(0)
        with torch.no_grad():
            z_curr = vision_net.encode(t_curr)
            # Select action via active policy network
            chosen_action, action_probs = world_net.act(z_curr, deterministic=False)

        reward = 0.0
        if active_mode == 1:
            # Mode 1: Active Reinforcement Learning (David Silver style paddle)
            reward, done = rl_arena.step(chosen_action)
            next_frame = rl_arena.render_frame()
            total_reward += reward
        elif active_mode == 2:
            # Mode 2: Live External Optical Ingestion (Webcam stream from t520 / UDP)
            next_frame = optical_receiver.get_frame()
            chosen_action = 1  # Observation only
        else:
            # Mode 3: Kinematic Occluder Sandbox
            kinematic_arena.step()
            next_frame = kinematic_arena.render_frame()

        # Step Latent Dream Prediction
        with torch.no_grad():
            h_dummy = world_net.init_hidden(1, device=device)
            a_tensor = torch.tensor([chosen_action], dtype=torch.long, device=device)
            z_dream, _, r_pred = world_net.forward_transition(z_curr, a_tensor, h_dummy)
            dream_frame = vision_net.decode(z_dream).squeeze().cpu().numpy()
            
            pixel_diff = np.abs(next_frame - dream_frame)
            latent_surprise = torch.norm(vision_net.encode(torch.tensor(next_frame, dtype=torch.float32, device=device).unsqueeze(0).unsqueeze(0)) - z_dream).item()

        # Continual Online Training
        if online_learning:
            agent.push_transition(current_frame, chosen_action, reward, next_frame)
            loss_v, loss_w = agent.train_world_model_step(batch_size=16)
            # Train policy purely in imagination!
            if active_mode == 1 and frame_idx % 2 == 0:
                agent.train_policy_in_imagination(current_frame, horizon=8)

        current_frame = next_frame

        # --- DRAWING PIPELINE ---
        screen.fill((8, 10, 14))

        panel_y = 60
        panel_size = 280
        p1_x = 40
        p2_x = 360
        p3_x = 680

        # Viewport 1: OPTICAL REALITY
        surf_reality = render_surface_from_numpy(current_frame, (panel_size, panel_size), colormap="mono")
        screen.blit(surf_reality, (p1_x, panel_y))
        pygame.draw.rect(screen, (35, 45, 55), (p1_x - 1, panel_y - 1, panel_size + 2, panel_size + 2), 1)

        # Viewport 2: INTERNAL DREAM
        surf_dream = render_surface_from_numpy(dream_frame, (panel_size, panel_size), colormap="dream")
        screen.blit(surf_dream, (p2_x, panel_y))
        pygame.draw.rect(screen, (25, 60, 75), (p2_x - 1, panel_y - 1, panel_size + 2, panel_size + 2), 1)

        # Viewport 3: PREDICTION SURPRISE
        surf_surprise = render_surface_from_numpy(pixel_diff * 2.5, (panel_size, panel_size), colormap="surprise")
        screen.blit(surf_surprise, (p3_x, panel_y))
        pygame.draw.rect(screen, (80, 50, 20), (p3_x - 1, panel_y - 1, panel_size + 2, panel_size + 2), 1)

        # Headers
        mode_titles = {
            1: "ACTIVE RL PADDLE (David Silver)",
            2: "LIVE OPTICAL INGESTION (Webcam / UDP)",
            3: "KINEMATIC OCCLUDER (Object Permanence)"
        }
        screen.blit(font_bold.render(f"1. OPTICAL REALITY [{mode_titles[active_mode]}]", True, (200, 220, 240)), (p1_x, panel_y - 24))
        screen.blit(font_bold.render("2. INTERNAL DREAM (Mental Latent Rollout)", True, (60, 210, 255)), (p2_x, panel_y - 24))
        screen.blit(font_bold.render("3. PREDICTION SURPRISE (|Reality - Dream|)", True, (255, 160, 50)), (p3_x, panel_y - 24))

        # 4. LATENT SPACE HUD
        hud_x = 40
        hud_y = 380
        pygame.draw.rect(screen, (14, 18, 24), (hud_x, hud_y, 920, 160))
        pygame.draw.rect(screen, (30, 40, 50), (hud_x, hud_y, 920, 160), 1)

        screen.blit(font_large.render("LATENT MANIFOLD & ACTIVE POLICY PROBABILITIES", True, (180, 200, 220)), (hud_x + 16, hud_y + 14))

        z_np = z_curr.squeeze().cpu().numpy()
        bar_w = 40
        bar_spacing = 14
        bar_base_y = hud_y + 95
        for i, val in enumerate(z_np):
            bx = hud_x + 20 + i * (bar_w + bar_spacing)
            val_norm = np.clip(val / 3.0, -1.0, 1.0)
            bar_h = int(val_norm * 45)
            col = (40, 210, 240) if bar_h >= 0 else (240, 120, 50)
            pygame.draw.line(screen, (50, 60, 75), (bx, bar_base_y), (bx + bar_w, bar_base_y), 1)
            if bar_h >= 0:
                pygame.draw.rect(screen, col, (bx, bar_base_y - bar_h, bar_w, bar_h))
            else:
                pygame.draw.rect(screen, col, (bx, bar_base_y, bar_w, -bar_h))
            screen.blit(font.render(f"z{i}", True, (110, 130, 150)), (bx + 6, bar_base_y + 48))

        # 5. RIGHT TELEMETRY
        r_x = 980
        r_y = 60
        r_w = 220
        r_h = 480
        pygame.draw.rect(screen, (14, 18, 24), (r_x, r_y, r_w, r_h))
        pygame.draw.rect(screen, (30, 40, 50), (r_x, r_y, r_w, r_h), 1)

        screen.blit(font_bold.render("COGNITIVE TELEMETRY", True, (240, 205, 50)), (r_x + 12, r_y + 14))

        act_names = ["LEFT", "STAY", "RIGHT"]
        probs_np = action_probs.squeeze().cpu().numpy()
        
        telemetry = [
            f"Step:       {frame_idx}",
            f"Mode:       {active_mode} ({mode_titles[active_mode][:10]}...)",
            f"Buffer:     {len(agent.buffer)}/2500",
            f"Surprise:   {latent_surprise:.4f}",
            "",
            "ACTIVE RL (DREAMER):",
            f"Action:     {act_names[chosen_action]}",
            f"Policy:     L:{probs_np[0]:.2f} S:{probs_np[1]:.2f} R:{probs_np[2]:.2f}",
            f"Dream Rew:  {agent.last_dream_reward:+.3f}",
            f"Critic Val: {agent.last_value_estimate:+.3f}",
            f"Score:      {rl_arena.score} (Hits: {rl_arena.consecutive_hits})",
            "",
            "OPTICAL INGESTION:",
            f"UDP Packets:{optical_receiver.frames_received}",
            "",
            "HOTKEYS:",
            "[1] Active RL Paddle",
            "[2] Live Camera / UDP",
            "[3] Kinematic Occluder",
            "[T] Toggle Training",
            "[R] Reset Arena",
            "[Q] Exit"
        ]
        
        ty = r_y + 42
        for line in telemetry:
            if "Active RL" in line or "Hits:" in line:
                col = (60, 230, 140)
            elif "Surprise" in line or "Dream Rew" in line:
                col = (255, 180, 50)
            elif line.startswith("["):
                col = (140, 160, 180)
            else:
                col = (180, 200, 220)
            screen.blit(font.render(line, True, col), (r_x + 12, ty))
            ty += 17

        # Bottom Bar
        bot_y = 560
        pygame.draw.rect(screen, (14, 18, 24), (40, bot_y, 920, 130))
        pygame.draw.rect(screen, (30, 40, 50), (40, bot_y, 920, 130), 1)
        
        screen.blit(font_bold.render("PIXELSENSE :: ACTIVE REINFORCEMENT LEARNING & OPTICAL INGESTION", True, (240, 205, 50)), (56, bot_y + 14))
        desc = [
            "• ACTIVE RL IN IMAGINATION: The agent updates its Actor-Critic policy gradient on imagined mental rollouts.",
            "• PADDLE INTERCEPTION: Learns to position the paddle under the ball purely from pixel feedback & rewards.",
            "• OPTICAL STREAMING: Real-time UDP ingestion port 5555 allows feeding live photons directly into the latent space.",
            "• ZERO HUMAN LABELS: All visual representations, transition kinematics, and motor policies are self-discovered."
        ]
        dy = bot_y + 38
        for d in desc:
            screen.blit(font.render(d, True, (130, 150, 170)), (56, dy))
            dy += 20

        pygame.display.flip()

        if args.render_frames > 0:
            frame_path = os.path.join(args.output_dir, f"frame_{frame_idx:04d}.png")
            pygame.image.save(screen, frame_path)
            if frame_idx % 60 == 0:
                print(f"[PixelSense] Rendered {frame_idx}/{args.render_frames} frames ({frame_idx/args.render_frames*100:.1f}%)")
            if frame_idx >= args.render_frames:
                print(f"[PixelSense] Render complete: {args.render_frames} frames.")
                running = False

        clock.tick(60)

    pygame.quit()

if __name__ == "__main__":
    main()
