Both boot modes passed 10/10 under QEMU TCG icount shift=4,sleep=off.

Image SHA256: `bb9fd254bed75f2eab9222b6baeae43202ea954c427cdca4c4831562f967e280`

During the 100-period observation, dashboard submitted 100 updates with zero
dashboard deadline misses. Video submitted zero updates during the deliberate
stall and resumed afterwards. Music continued submitting to its own viewport.

This checks guest framebuffer writes. Media decoding, sound output, physical
scanout, full Tier 2/3 stress isolation, and EL0 isolation for dashboard/video
were outside this run.
