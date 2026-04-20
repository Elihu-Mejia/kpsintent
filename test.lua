local kps = require("kpsintent")

-- Usage: kps.play({ frequency=..., duration=..., [decay=...], [brightness=...], [volume=...], [vowel=...], [vowel_end=...] })
-- decay: 0.0 to 1.0 (sustain)
-- brightness: 0.0 to 1.0 (filter coefficient - lower is brighter)
-- volume: 0.0 to 1.0
-- vowel: 0.0 to 1.0 (start vowel: 0=A, 0.25=E, 0.5=I, 0.75=O, 1=U)
-- vowel_end: 0.0 to 1.0 (end vowel for sweep)
--- phaser_freq: speed in Hz (try 0.2 to 1.0)
-- phaser_depth: intensity (0.0 to 1.0)
-- phaser_feedback: resonance (0.0 to 0.9)

print("Playing an A Minor chord (A3, C4, E4)...")
kps.play({
    frequency = {220.0, 261.63, 329.63},
    duration = 2.5,
    decay = 0.997,
    brightness = 0.2,
    volume = 0.3
})

print("Playing with Vocoder Effect (Vowel 'O')...")
kps.play({
    frequency = {110.0, 164.81},
    duration = 2.0,
    decay = 0.998,
    brightness = 0.1,
    volume = 0.4,
    vowel = 0.75
})

print("Playing with Vocoder Effect (Vowel 'E')...")
kps.play({
    frequency = 110.0,
    duration = 2.0,
    decay = 0.998,
    brightness = 0.05,
    volume = 0.5,
    vowel = 0.25
})

print("Playing with Vowel Sweep (A -> U)...")
kps.play({
    frequency = 110.0,
    duration = 3.0,
    vowel = 0.0,
    vowel_end = 1.0,
    brightness = 0.1,
    volume = 0.5
})

print("Playing with ADSR Envelope Swell...")
kps.play({
    frequency = 220.0,
    duration = 4.0,
    attack = 1.5,
    decay_env = 0.5,
    sustain = 0.6,
    release = 1.0,
    brightness = 0.1,
    volume = 0.5
})

print("Playing Jarre-style Strings (High Phaser Feedback)...")
kps.play({
    frequency = {110.0, 164.81, 220.0},
    duration = 5.0,
    attack = 0.5,
    release = 1.0,
    brightness = 0.1,
    phaser_freq = 0.5,
    phaser_depth = 0.8,
    phaser_feedback = 0.7,
    volume = 0.4
})

print("Done.")
