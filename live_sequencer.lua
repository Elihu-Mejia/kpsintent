local kps = require("kpsintent")

-- The file you will edit live
local PATTERN_PATH = "./pattern.lua"

local function start_sequencer()
    print("========================================")
    print("8-BIT LIVE CODING SEQUENCER")
    print("Editing: " .. PATTERN_PATH)
    print("========================================")

    local last_chunk = nil

    while true do
        -- Attempt to load and run the pattern file
        local chunk, load_err = loadfile(PATTERN_PATH)
        if chunk then
            last_chunk = chunk
        elseif load_err and not last_chunk then
            print("Syntax Error in pattern: " .. tostring(load_err))
            os.execute("sleep 0.5")
        end

        if last_chunk then
            local ok, params = pcall(last_chunk)
            if ok and type(params) == "table" then
                if params.stop then
                    print("Stop signal detected in pattern. Exiting...")
                    break
                end

                -- play_8bit blocks for 'duration', serving as our clock
                kps.play_8bit(params)
            else
                print("Runtime Error in pattern: " .. tostring(params))
                os.execute("sleep 0.5")
            end
        end
    end
end

start_sequencer()