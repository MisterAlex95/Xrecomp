"""XEX2 front end for the Xbox 360 spike (XBA-230): the container, its optional headers, decryption with a key the
user supplies, basic decompression, the PE image inside. `python3 -m tools.xex FILE.xex [--key-file K] [--json F]
[--image OUT]`. The Xenon decoder and lifter on top are in consoles/x360/tools/x360_tools."""
