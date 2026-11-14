-- PCSX-Redux side of tools/consoles/run_homebrew.sh (XBA-149): watches the XBA result block of our homebrew
-- programs (consoles/ps1/tests/homebrew/README.md, « Result protocol »): 512 bytes at physical 0x001F0000, little
-- endian. At each VSync it reads `status`; once it is non-zero (or after `limit` VSyncs) it prints the whole block
-- as one line « XBA-BLOCK <vsyncs> <1024 hex digits> » and quits. The shell side parses that line.
-- Loaded with -dofile, started with -exec "xba_watch(<limit>)" (PCSX-Redux runs -exec after -dofile).
local BLOCK, SIZE = 0x001F0000, 512

local function u32(mem, off)
    local p = BLOCK + off
    return mem[p] + mem[p + 1] * 0x100 + mem[p + 2] * 0x10000 + mem[p + 3] * 0x1000000
end

function xba_watch(limit)
    local vsyncs = 0
    -- A global, so the listener is not garbage-collected (which would unregister it).
    xba_listener = PCSX.Events.createEventListener('GPU::Vsync', function()
        vsyncs = vsyncs + 1
        local mem = PCSX.getMemPtr()
        if u32(mem, 4) == 0 and vsyncs < limit then return end
        local hex = {}
        for i = 0, SIZE - 1 do hex[#hex + 1] = string.format('%02x', mem[BLOCK + i]) end
        print(string.format('XBA-BLOCK %dvsyncs %s', vsyncs, table.concat(hex)))
        PCSX.quit(0)
    end)
end
