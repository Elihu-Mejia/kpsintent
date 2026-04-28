return {
    bpm = 100.0,
    tracks = {
        { -- Atmospheric Pad
            frequency = "A",
            sine = true,          -- Clean source makes glitches pop
            unison = 7,          -- Stack 7 detuned oscillators
            detune = 0.2,       -- Lush chorus width
            volume = 0.7,
            attack = 1.0,        -- Long swell
            release = 1,       -- Long tail
            vowel = 0,         -- Center on 'E'
            vowel_lfo_freq = 1, -- Modulate vowel every whole note
            vowel_lfo_depth = 0.2, -- Morph significantly
            vocal_grit = 0,   -- Add a bit of breathiness
            glitch_amount = 0.9, -- 40% intensity for digital artifacts
            fm_freq_ratio = 1.0,
            fm_amount = 0.1,     -- Subtle shimmer
            delay_time = 1/64,
            delay_feedback = 0.3,
            stutter_speed = 1/16, -- Fast pulsing
            stutter_gate = 0.9,
            stutter_random = 0.9, 
            reverb_mix = 0.3,    
            reverb_room = 0.8,  
            reverb_damp = 0.4,   
            delay_is_pingpong = true,
            lpf_freq = 1200,
            lpf_q = 1.0
        },
        { -- Atmospheric Pad
            frequency = "C5",
            --sine = true,          -- Clean source makes glitches pop
            unison = 7,          -- Stack 7 detuned oscillators
            detune = 0.1,       -- Lush chorus width
            volume = 0.7,
            attack = 1.0,        -- Long swell
            release = 1,       -- Long tail
            vowel = 0,         -- Center on 'E'
            vowel_lfo_freq = 1, -- Modulate vowel every whole note
            vowel_lfo_depth = 0.2, -- Morph significantly
            vocal_grit = 0,   -- Add a bit of breathiness
            glitch_amount = 0.3, -- 40% intensity for digital artifacts
            fm_freq_ratio = 1.0,
            fm_amount = 0.1,     -- Subtle shimmer
            delay_time = 1/4,
            delay_feedback = 0.3,
            stutter_speed = 1/64, -- Fast pulsing
            stutter_gate = 0.9,
            stutter_random = 0.9, 
            reverb_mix = 0.39,    
            reverb_room = 0.8,  
            reverb_damp = 0.24,   
            delay_is_pingpong = true,
            lpf_freq = 900,
            lpf_q = 1.0
        },
    }
}
