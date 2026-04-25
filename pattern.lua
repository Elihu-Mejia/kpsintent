-- Edit this table and save the file to update the sound live!
return {
    bpm = 140.0, -- Beats Per Minute (120 BPM = 2.0s loop for 4 beats)
    stop = false,
    tracks = {
        {
            frequency = {440.0, 554.37, 659.25, 830.61},
            arp_speed = 1/16,    -- 16th notes
            arp_random = true,
            duty_cycle = 0.25,
            pwm_lfo_freq = 1/4,  -- Sync LFO cycle to quarter notes
            pwm_lfo_depth = 0.2,
            bit_depth = 5,
            downsample = 7,
            volume = 0.7,
            solo = false, -- Now only this track will play
            release = 0.0,
            triangle = true,
            attack = 0.2,
            delay_is_pingpong = true,
        },
        { -- Layer 2: Fast Arp
            frequency = {440.0, 554.37, 659.25, 830.61},
            arp_speed = 1/32,    -- 32nd notes
            arp_random = true,
            duty_cycle = 0.5,
            pwm_lfo_freq = 1/4,
            pwm_lfo_depth = 0.2,
            bit_depth = 3,
            downsample = 2,
            volume = 0.2,
            release = 0,
            attack = 0.2,
            mute = false, -- Silencing this layer
        },
        { -- Layer 3: Echo/Delay track
            frequency = {110.0, 164.81, 220.0, 277.18},
            arp_speed = 1/16,
            duty_cycle = 0.25,
            delay_time = 1/8,       -- Sync delay to 8th notes
            delay_feedback = 0.85,
            delay_damping = 0.6,
            delay_is_pingpong = true, -- Enable ping-pong mode
            bit_depth = 4, 
            volume = 0.3,
            attack = 0.01,
            release = 0.1,
            arp_random = true
        }
    }
}