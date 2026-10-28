"""
latent_dynamics.py - Recurrent Latent World Model (M) + Actor-Critic (David Silver RL)
Learns causal physics AND predicts scalar rewards purely in abstract latent space.
Includes Actor (Policy pi) and Critic (Value V) heads that plan and learn inside
mental imagination rollouts (Dreamer / MuZero architecture).
"""

import torch
import torch.nn as nn
import torch.nn.functional as F

class LatentWorldModel(nn.Module):
    def __init__(self, latent_dim=16, action_dim=3, hidden_dim=64):
        super().__init__()
        self.latent_dim = latent_dim
        self.action_dim = action_dim  # 3 discrete actions: 0=Left, 1=Stay, 2=Right
        self.hidden_dim = hidden_dim
        
        # Action embedding: maps discrete action (0, 1, 2) to continuous embedding
        self.action_embed = nn.Embedding(action_dim, 8)
        
        # Input projection: latent state z_t + action embedding
        self.input_proj = nn.Sequential(
            nn.Linear(latent_dim + 8, hidden_dim),
            nn.LayerNorm(hidden_dim),
            nn.LeakyReLU(0.1, inplace=True)
        )
        
        # Recurrent Core: GRUCell capturing momentum, trajectory, and multi-step dynamics
        self.rnn = nn.GRUCell(hidden_dim, hidden_dim)
        
        # Transition Predictor: delta_z = z_{t+1} - z_t
        self.delta_head = nn.Sequential(
            nn.Linear(hidden_dim, hidden_dim),
            nn.LeakyReLU(0.1, inplace=True),
            nn.Linear(hidden_dim, latent_dim)
        )
        
        # 1. Reward Predictor Head: Predicts scalar reward r_{t+1} from imagined latent state
        self.reward_head = nn.Sequential(
            nn.Linear(latent_dim, 32),
            nn.LeakyReLU(0.1, inplace=True),
            nn.Linear(32, 1)
        )
        
        # 2. Actor Head (Policy pi(a | z)): Action probabilities
        self.actor_head = nn.Sequential(
            nn.Linear(latent_dim, 32),
            nn.LeakyReLU(0.1, inplace=True),
            nn.Linear(32, action_dim)
        )
        
        # 3. Critic Head (Value V(z)): Expected discounted return
        self.critic_head = nn.Sequential(
            nn.Linear(latent_dim, 32),
            nn.LeakyReLU(0.1, inplace=True),
            nn.Linear(32, 1)
        )

    def init_hidden(self, batch_size=1, device="cpu"):
        return torch.zeros(batch_size, self.hidden_dim, device=device)

    def forward_transition(self, z_t, action_idx, h_prev):
        """
        Transitions one step forward in latent space given discrete action (0, 1, or 2).
        Returns: (z_next, h_next, predicted_reward)
        """
        a_emb = self.action_embed(action_idx)
        inp = torch.cat([z_t, a_emb], dim=-1)
        feat = self.input_proj(inp)
        h_next = self.rnn(feat, h_prev)
        delta_z = self.delta_head(h_next)
        z_next = z_t + delta_z
        pred_reward = self.reward_head(z_next)
        return z_next, h_next, pred_reward

    def evaluate_state(self, z):
        """Returns policy distribution and value estimate for latent state z."""
        logits = self.actor_head(z)
        value = self.critic_head(z)
        return logits, value

    def act(self, z, deterministic=False):
        """Select action via policy network."""
        logits = self.actor_head(z)
        probs = F.softmax(logits, dim=-1)
        if deterministic:
            action = torch.argmax(probs, dim=-1)
        else:
            dist = torch.distributions.Categorical(probs)
            action = dist.sample()
        return action.item(), probs.detach()

    def imagine_rollout(self, z_start, horizon=8):
        """
        David Silver / MuZero style Mental Simulation:
        Rolls out H steps forward in pure imagination, following current policy.
        Returns imagined states, rewards, values, and log_probs.
        """
        device = z_start.device
        h_curr = self.init_hidden(z_start.size(0), device=device)
        
        states = [z_start]
        rewards = []
        actions = []
        log_probs = []
        values = []

        z_curr = z_start
        for step in range(horizon):
            logits, val = self.evaluate_state(z_curr)
            dist = torch.distributions.Categorical(logits=logits)
            act = dist.sample()
            
            z_curr, h_curr, r_pred = self.forward_transition(z_curr, act, h_curr)
            
            states.append(z_curr)
            rewards.append(r_pred.squeeze(-1))
            actions.append(act)
            log_probs.append(dist.log_prob(act))
            values.append(val.squeeze(-1))

        # Final bootstrap value
        _, final_val = self.evaluate_state(z_curr)
        values.append(final_val.squeeze(-1))

        return states, rewards, actions, log_probs, values
