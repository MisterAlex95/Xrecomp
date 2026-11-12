"""
The compat runner (XBA-62): prepare a title, boot it headless with a pad
script, and say how far it got.

    python -m tools.compat run SOURCE [SOURCE...] [--seconds 180] [--repeat N]
    python -m tools.compat report results.jsonl

One title at a time, at low priority, killed by PID at the end. What it reads
from the run (no screen needed):

  * the status file (RECOMP_STATUS, src/kernel/recomp_status.h): flips, how
    often the picture changed, whether it is lit, sound, pad reads;
  * the discovery file (src/kernel/recomp_discovery.h, docs/technical/discovery.md);
  * the run's stderr, for a crash site or an untranslated instruction.

Stages, each implying the ones before:

  prepares    tools.prepare made a build
  boots       the runtime came up and the title did something (a flip, a
              thread, a pad read, a sound)
  renders     it presented a frame that is not near-black
  responsive  the picture changed after a press, and the title reads the pad
  sustained   over the last --sustain seconds: flips every 10 s, the picture
              still changing, no exit

"playable" (30 minutes, saves) is a person's call, not this tool's. Nothing
from the game leaves the work folder: the results hold numbers, our own
stage names, and the first line of an error.
"""

STAGES = ("none", "prepares", "boots", "renders", "responsive", "sustained")
