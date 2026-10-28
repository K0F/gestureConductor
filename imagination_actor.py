"""
imagination_actor.py - David Silver / Dreamer Style Imagination Learner
Updates the Actor-Critic policy by imagining counterfactual trajectory rollouts
inside the latent world model, learning motor control without trial-and-error in reality.
"""

import torch
import torch.nn as nn
import torch.optim as optim
import numpy as np
from collections import deque

class DreamerLearner:
    def __init__(self, vision_model, world_model, device="cpu", lr=1e-3, gamma=0.95):
        self.device = device
        self.V = vision_model.to(device)
        self.M = world_model.to(device)
        self.gamma = gamma
        
        # Optimizers
        self.opt_vision = optim.Adam(self.V.parameters(), lr=lr)
        self.opt_world = optim.Adam(self.M.parameters(), lr=lr)
        self.opt_actor_critic = optim.Adam([
            {'params': self.M.actor_head.parameters(), 'lr': lr * 0.5},
            {'params': self.M.critic_head.parameters(), 'lr': lr}
        ])
        
        self.mse_loss = nn.MSELoss()
        
        # Real-world transition buffer: (frame_t, action_t, reward_t, frame_next)
        self.buffer = deque(maxlen=2500)
        
        # Telemetry
        self.last_dream_reward = 0.0
        self.last_value_estimate = 0.0
        self.policy_entropy = 0.0

    def push_transition(self, frame_t, action_t, reward_t, frame_next):
        self.buffer.append((frame_t, int(action_t), float(reward_t), frame_next))

    def train_world_model_step(self, batch_size=16):
        """Train vision autoencoder, transition dynamics, and reward predictor from real frames."""
        if len(self.buffer) < batch_size:
            return 0.0, 0.0

        indices = np.random.choice(len(self.buffer), batch_size, replace=False)
        f_t, a_t, r_t, f_n = [], [], [], []
        for idx in indices:
            ft, at, rt, fn = self.buffer[idx]
            f_t.append(ft)
            a_t.append(at)
            r_t.append(rt)
            f_n.append(fn)

        t_ft = torch.tensor(np.array(f_t), dtype=torch.float32, device=self.device).unsqueeze(1)
        t_at = torch.tensor(np.array(a_t), dtype=torch.long, device=self.device)
        t_rt = torch.tensor(np.array(r_t), dtype=torch.float32, device=self.device).unsqueeze(1)
        t_fn = torch.tensor(np.array(f_n), dtype=torch.float32, device=self.device).unsqueeze(1)

        # 1. Vision Loss
        self.opt_vision.zero_grad()
        recon, z_t = self.V(t_ft)
        loss_v = self.mse_loss(recon, t_ft)
        loss_v.backward()
        self.opt_vision.step()

        # 2. World Model & Reward Predictor Loss
        self.opt_world.zero_grad()
        with torch.no_grad():
            z_target = self.V.encode(t_fn)
            z_curr = self.V.encode(t_ft)

        h_init = self.M.init_hidden(batch_size, device=self.device)
        z_pred, _, r_pred = self.M.forward_transition(z_curr, t_at, h_init)
        
        loss_dyn = self.mse_loss(z_pred, z_target)
        loss_rew = self.mse_loss(r_pred, t_rt)
        total_world_loss = loss_dyn + 2.0 * loss_rew
        total_world_loss.backward()
        self.opt_world.step()

        return loss_v.item(), total_world_loss.item()

    def train_policy_in_imagination(self, current_frame, horizon=8):
        """
        David Silver / MuZero / Dreamer Core:
        The agent sits at current latent state and unrolls dreams forward,
        optimizing its policy gradient on imagined trajectories.
        """
        t_frame = torch.tensor(current_frame, dtype=torch.float32, device=self.device).unsqueeze(0).unsqueeze(0)
        with torch.no_grad():
            z_0 = self.V.encode(t_frame)

        # Rollout in pure imagination
        states, rewards, actions, log_probs, values = self.M.imagine_rollout(z_0, horizon=horizon)
        
        # Compute discounted returns & advantages in dream space
        returns = []
        discounted_r = values[-1].detach()
        for t in reversed(range(len(rewards))):
            discounted_r = rewards[t] + self.gamma * discounted_r
            returns.insert(0, discounted_r)

        returns = torch.stack(returns)
        values_tensor = torch.stack(values[:-1])
        advantages = (returns - values_tensor).detach()

        # Actor Loss (Policy Gradient) & Critic Loss (MSE)
        actor_loss = 0.0
        critic_loss = 0.0
        for t in range(len(rewards)):
            actor_loss -= log_probs[t] * advantages[t]
            critic_loss += 0.5 * (values[t] - returns[t]) ** 2

        self.opt_actor_critic.zero_grad()
        total_ac_loss = actor_loss + critic_loss
        total_ac_loss.backward()
        self.opt_actor_critic.step()

        self.last_dream_reward = torch.sum(torch.stack(rewards)).item()
        self.last_value_estimate = values[0].item()

        return float(self.last_dream_reward)
