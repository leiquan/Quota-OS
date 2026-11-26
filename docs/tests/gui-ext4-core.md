# Graphical ext4 root integration

The public root service API is declared in `os/include/root_service.h`.
Enable `QUOTA_GUI_EXT4` when building a graphical candidate. Assign the
distinct writable root image to QEMU `virtio-mmio-bus.30` with serial
`QUOTA-ROOT`. Preserve the existing boot arguments. Put Notes explicitly
on a different bus (for example, `virtio-mmio-bus.29`). Use
`-global virtio-mmio.force-legacy=false` and a raw root drive with
`cache=writeback,werror=report,rerror=report`. Copy the frozen test root
image for each run. Keep Notes and the Web runtime on their own images.
The guest verifies the root serial and negotiates FLUSH before mounting.

From the authenticated Tier 3 frontend, submit one list, read, save, or
remove request. Poll `root_service_take` from the normal event loop.
Input is copied, and disk waits occur on a separate, transient Tier 3 job.
Use a 4 KiB output buffer to collect results. Listing returns newline-delimited
names for this test profile. Render filenames and contents as data.
Display save success only after the service reports EXT4_OK.
For session exit, finish an accepted mutation and consume or discard
its result before handing the mailbox to another session.

Use one isolated QEMU and dedicated disk copies per version. Test Files
browsing and ELF/Notes regressions where the edition exposes those paths.
For Touch UI, capture actual Todo list movement plus successful real
root read and journaled write completions in the same interval. Compare
frame releases and commits on the guest counter at 10 and 40 ms.
Report misses, deadlines, commit gaps, and Tier 2 shortfalls individually.
A trace with drops cannot establish a pass.
