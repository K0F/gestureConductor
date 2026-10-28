// =============================================================================
// synthdefs.scd - Compiles Kof Gesture Conductor SynthDefs to .scsyndef files
// The resulting binary .scsyndef files are stored in ./synthdefs/ next to the
// gesture_conductor binary, and loaded dynamically at runtime by libscsynth.
//
// To recompile after editing:
//   sclang synthdefs.scd
// =============================================================================

var outDir = thisProcess.nowExecutingPath.dirname +/+ "synthdefs";
File.mkdir(outDir);
(">>> Output directory for .scsyndef files: " ++ outDir).postln;

// -----------------------------------------------------------------------------
// 1. Ictus Strike (Resonant modal chime on baton downbeat)
// -----------------------------------------------------------------------------
SynthDef(\ictusStrike, { |freq = 220, decay = 0.45, amp = 0.16, pan = 0.0, brightness = 1.0|
    var excitation, freqs, ringTimes, modal, sig;
    
    excitation = Decay2.ar(Impulse.ar(0), 0.0005, 0.008) * WhiteNoise.ar(0.25)
               + (Dust2.ar(60) * 0.04);
    
    freqs = [freq, freq * 1.5, freq * 2.01, freq * 2.76, freq * 3.45, freq * 4.2];
    ringTimes = [decay, decay * 0.7, decay * 0.5, decay * 0.35, decay * 0.2, decay * 0.1];
    
    modal = Ringz.ar(excitation, freqs, ringTimes).sum * 0.07;
    modal = LPF.ar(modal, (freq * 5 * brightness).clip(200, 14000));
    
    sig = modal * amp;
    sig = sig + (CombN.ar(sig, 0.2, 0.06, 0.3) * 0.2);
    sig = Sanitize.ar(LeakDC.ar(sig));
    sig = Limiter.ar(sig, 0.25);
    
    DetectSilence.ar(sig, amp: 0.0001, time: 0.05, doneAction: 2);
    Out.ar(0, Pan2.ar(sig, pan));
}).writeDefFile(outDir);

// -----------------------------------------------------------------------------
// 2. Tutti Thump (Sub-bass transient punch on dual downbeats)
// -----------------------------------------------------------------------------
SynthDef(\tuttiThump, { |amp = 0.22, pan = 0.0|
    var env = EnvGen.ar(Env([0.001, 1, 0.0001], [0.003, 0.8], [3, -4]), doneAction: 2);
    var f = 34 + (env ** 16 * 58) + BrownNoise.ar(env ** 8 * 6);
    var sig = SinOsc.ar(f, env ** 40 * 6pi) * 0.7;
    sig = sig + SinOsc.ar(48, env ** 0.9 * 4pi) * 0.3;
    sig = LPF.ar(sig, 380);
    sig = sig * env * amp;
    sig = Sanitize.ar(LeakDC.ar(sig));
    sig = Limiter.ar(sig, 0.28);
    Out.ar(0, Pan2.ar(sig, pan));
}).writeDefFile(outDir);

// -----------------------------------------------------------------------------
// 3. Conductor Flow (Lead synthesizer voice with Moog filter)
// -----------------------------------------------------------------------------
SynthDef(\conductorFlow, { |pitch = 110, cutoff = 1200, pan = 0.0, res = 1.0, energy = 0.2, masterVol = 0.18|
    var lPitch = Lag.kr(pitch, 0.03);
    var lCutoff = Lag.kr(cutoff, 0.03);
    var lPan = Lag.kr(pan, 0.03);
    var lRes = Lag.kr(res, 0.04);
    var lEnergy = Lag.kr(energy, 0.04);
    
    var sub = SinOsc.ar(lPitch * 0.5) * 0.3;
    var osc1 = VarSaw.ar(lPitch, 0, (0.1 + lEnergy * 0.4).clip(0.05, 0.95)) * 0.22;
    var osc2 = Pulse.ar(lPitch * 1.005, (0.2 + lEnergy * 0.3).clip(0.1, 0.9)) * 0.18;
    
    var mixed = LeakDC.ar(osc1 + osc2 + sub);
    var filtered = MoogFF.ar(mixed, lCutoff.clip(60, 14000), lRes.clip(0.1, 2.0));
    var saturated = (filtered * (1.0 + lEnergy * 1.5)).tanh;
    
    var sig = saturated * (0.04 + (lEnergy * 0.1)).clip(0.02, 0.2) * masterVol;
    sig = Sanitize.ar(LeakDC.ar(Pan2.ar(sig, lPan)));
    sig = Limiter.ar(sig, 0.22);
    
    Out.ar(0, sig);
}).writeDefFile(outDir);

// -----------------------------------------------------------------------------
// 4. Conductor Wash (Kof's signature Perlin3 / Formant atmospheric drone)
// -----------------------------------------------------------------------------
SynthDef(\conductorWash, { |pitchNorm = 0.3, cutoffNorm = 0.4, pan = 0.0, energy = 0.2, span = 0.5, masterVol = 0.16|
    var lPitch = Lag.kr(pitchNorm, 0.04);
    var lCutoff = Lag.kr(cutoffNorm, 0.04);
    var lPan = Lag.kr(pan, 0.04);
    var lEnergy = Lag.kr(energy, 0.05);
    var lSpan = Lag.kr(span, 0.05);

    var base = (55.0 * (2.0 ** (lPitch * 3.5))).clip(45, 880);
    var detune = SinOsc.ar([2.5, 3.1, 4.2] + (lEnergy * 1.5)).range(-1, 1);
    var ang = LFSaw.ar([1/100, 1/250, 1/41] * (1.0 + lEnergy)).range(0, 2pi);
    var perl = Perlin3.ar(ang[0].cos, ang[1].sin, ang[2].cos);
    
    var sig = SinOsc.ar(base + detune, mul: 0.15)
            + (Formant.ar(base, base / 2, (base * 3 * (0.6 + lCutoff * 2.0)).clip(80, 12000)) * 0.12)
            + (perl * 0.04);
            
    sig = Vibrato.ar(sig, [5, 6, 7, 8], (0.04 + (lEnergy * 0.15)).clip(0.02, 0.3));
    sig = LeakDC.ar(sig);
    sig = RLPF.ar(sig, (base * 3.5 * (0.7 + lCutoff * 2.5)).clip(100, 12000), (0.2 + (lSpan * 0.4)).clip(0.1, 0.7));
    sig = GVerb.ar(HPF.ar(sig, 80), 80, 2.5) / 12 + sig;
    sig = CompanderD.ar(sig, 3/4, 1/2, 1) / 3 + sig;
    
    sig = Splay.ar(sig, (0.4 + lSpan * 0.6).clip(0.2, 1.0), 0.2);
    sig = Balance2.ar(sig[0], sig[1], lPan);
    sig = Sanitize.ar(LeakDC.ar(sig * masterVol));
    sig = Limiter.ar(sig, 0.22);
    
    Out.ar(0, sig);
}).writeDefFile(outDir);

(">>> SynthDefs compiled successfully to " ++ outDir).postln;
0.exit;
