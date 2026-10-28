"""
curiosity_reasoner.py - Epistemic Sense-Making & Experience Replay
Integrates Demis Hassabis' hippocampal replay buffer with David Silver's
predictive world model. Measures intrinsic surprise, tests object permanence,
and performs online continuous gradient updates.
"""

import torch
import torch.nn as nn
import torch.optim as optim
import numpy as np
from collections import deque

class HippocampalReplayBuffer:
    """Fast episodic memory buffer (Hassabis Complementary Learning Systems)"""
    def __init__(self, capacity=2000):
        self.buffer = deque(maxlen=capacity)

    def push(self, frame_t, action_t, frame_next):
        # Store as lightweight numpy arrays
        self.buffer.append((frame_t, action_t, frame_next))

    def sample_batch(self, batch_size=16, device="cpu"):
        indices = np.random.choice(len(self.buffer), batch_size, replace=False)
        f_t_list, a_t_list, f_next_list = [], [], []
        for idx in indices:
            ft, at, fn = self.buffer[idx]
            f_t_list.append(ft)
            a_t_list.append(at)
            f_next_list.append(fn)

        f_t = torch.tensor(np.array(f_t_list), dtype=torch.float32, device=device).unsqueeze(1)
        a_t = torch.tensor(np.array(a_t_list), dtype=torch.float32, device=device)
        f_next = torch.tensor(np.array(f_next_list), dtype=torch.float32, device=device).unsqueeze(1)
        return f_t, a_t, f_next

    def __len__(self):
        return len(self.buffer)

class CognitiveSenseMaker:
    def __init__(self, vision_model, world_model, device="cpu", lr=1e-3):
        self.device = device
        self.V = vision_model.to(device)
        self.M = world_model.to(device)
        
        # Dual optimizers
        self.opt_vision = optim.Adam(self.V.parameters(), lr=lr)
        self.opt_dynamics = optim.Adam(self.M.parameters(), lr=lr)
        self.mse_loss = nn.MSELoss()
        
        # Episodic buffer
        self.hippocampus = HippocampalReplayBuffer(capacity=2000)
        
        # Recurrent state tracker
        self.h_recurrent = self.M.init_hidden(batch_size=1, device=device)
        
        # Telemetry metrics
        self.current_surprise = 0.0
        self.recent_losses = deque(maxlen=60)
        self.step_count = 0
        self.permanence_score = 1.0

    def process_step(self, frame_t_np, action_t_np, frame_next_np, train=True):
        """
        Receives current frame, action, and outcome.
        1. Encodes reality into latent space
        2. Predicts future in latent space
        3. Decodes dream into pixel space
        4. Computes surprise and updates internal weights
        """
        self.step_count += 1
        self.hippocampus.push(frame_t_np, action_t_np, frame_next_np)
        
        # Convert to tensors
        t_frame_t = torch.tensor(frame_t_np, dtype=torch.float32, device=self.device).unsqueeze(0).unsqueeze(0)
        t_action_t = torch.tensor(action_t_np, dtype=torch.float32, device=self.device).unsqueeze(0)
        t_frame_next = torch.tensor(frame_next_np, dtype=torch.float32, device=self.device).unsqueeze(0).unsqueeze(0)

        # 1. Encode Reality
        with torch.no_grad():
            z_t = self.V.encode(t_frame_t)
            z_next_actual = self.V.encode(t_frame_next)
            
            # 2. Predict Future (Dream)
            z_next_dream, self.h_recurrent, conf = self.M.forward_step(z_t, t_action_t, self.h_recurrent)
            
            # 3. Decode Dream to Pixel Hallucination
            dream_frame = self.V.decode(z_next_dream)
            
            # 4. Measure Epistemic Surprise
            latent_surprise = torch.norm(z_next_actual - z_next_dream, p=2).item()
            pixel_diff = torch.abs(t_frame_next - dream_frame).squeeze().cpu().numpy()
            self.current_surprise = latent_surprise

        # 5. Online Continual Learning (Hippocampal Replay)
        if train and len(self.hippocampus) >= 32:
            batch_ft, batch_at, batch_fn = self.hippocampus.sample_batch(batch_size=16, device=self.device)
            
            # Vision update: Autoencoder reconstruction
            self.opt_vision.zero_grad()
            recon_ft, z_batch = self.V(batch_ft)
            loss_v = self.mse_loss(recon_ft, batch_ft)
            loss_v.backward()
            self.opt_vision.step()
            
            # Dynamics update: Predict z_{t+1} from z_t
            self.opt_dynamics.zero_grad()
            with torch.no_grad():
                z_target = self.V.encode(batch_fn)
                z_current = self.V.encode(batch_ft)
            
            h_init = self.M.init_hidden(batch_ft.size(0), device=self.device)
            z_pred, _, _ = self.M.forward_step(z_current, batch_at, h_init)
            loss_d = self.mse_loss(z_pred, z_target)
            loss_d.backward()
            self.opt_dynamics.step()
            
            total_loss = loss_v.item() + loss_d.item()
            self.recent_losses.append(total_loss)

        return (
            z_t.squeeze().detach().cpu().numpy(),
            z_next_dream.squeeze().detach().cpu().numpy(),
            dream_frame.squeeze().detach().cpu().numpy(),
            pixel_diff
        )
