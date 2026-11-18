# XRecomp 0.13.8

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### Docs

- Xbox 360 MSAA switch, memory export in both shader stages (XBA-296) (629d0351)
- the Xbox 360 kernel surface and renderer after XBA-296 (f484e9d0)

### GameCube

- the paired-single multiply-adds return the NaN the architecture names (XBA-292) (7fbeca0e)

### PPC

- the fused multiply-adds return the NaN the architecture names (96eda8ba, XBA-292)

### PPC discovery

- hooks for how a console layer keeps words and functions (a103fe2a, XBA-293)

### x360

- one HLE dispatch table, hle_coverage read exactly (XBA-296) (c6dbd8f0)
- HLE groups wired into the builds, duplicate exports refused (XBA-296) (966851b2)
- DPCs, APCs, timers, queues, SLists and read-write locks (XBA-296) (3205be4d)
- display, audio, objects and every remaining kernel export answered (XBA-296) (7156f1b5)
- kobjects, a test title for the kernel objects through lifted code (XBA-296) (549eba14)
- the XAM side of the HLE: content, profile, notifications, UI, tasks (XBA-296) (1ab9a581)
- tests of the XAM HLE group (XBA-296) (0196c88a)
- XAM overlapped failures complete as the console's do, the DVD tray (XBA-296) (37f464d5)
- the printf family on one uncapped formatter (XBA-296) (f36c16d9)
- the Rtl counted strings, case and UTF-8 calls (XBA-296) (1ac67c0e)
- stack, image and module helpers of the Rtl group (XBA-296) (98786c70)
- drop a stack check the back chain test already covers (XBA-296) (0a6d9b2f)
- RtlVirtualUnwind finds the function from the pc (XBA-296) (6d86aab1)
- content is really deleted, completion routines run, one CONTEXT layout (XBA-296) (f8499ba6)
- MD5, SHA-224/256/384/512 and HMAC exports, state in guest memory (XBA-296) (8b297b69)
- AES and DES exports with their published vectors (XBA-296) (6bd3dcc9)
- big-number, PKCS #1, Parve exports; private-key operations refused (XBA-296) (6e4f030e)
- SigFormat left without an HLE, long-message and full Parve tests (XBA-296) (ea042d3b)
- a refusal is visible and fits the export's return type (XBA-296) (601d01cd)

### X360

- XMA decoded by libavcodec, a shared LGPL library, on the desktop and in the APK (XBA-299) (d2214ed6)
- the retail key built into the APK from a build secret (XBA-300) (271c2787)
- the devkit key built into the APK from its own build secret (XBA-300) (de8cd4d2)

### Xbox 360 lift

- peak memory no longer grows by an object per guest word (98a889d8, XBA-293)

### Xenos

- translate the texture queries (getBCF, getCompTexLOD, getGradients, getWeights) (ba5bdfbf, XBA-296)
- line loops, quad strips, polygons and immediate indices (c2fa957b, XBA-296)
- decode packed mip tails for 2D, cube and 3D textures (caed8e19, XBA-296)
- memory export of 8- and 16-bit formats, and from pixel shaders (60266321, XBA-296)
- the stencil of depth resolves (8ea90bcc, XBA-296)
- eDRAM aliasing between formats and positions (82686816, XBA-296)
- multisampled render targets (4e73a469, XBA-296)
- fixes from review of the GPU round (38fcb0e8, XBA-296)

<!-- release:changes end -->
