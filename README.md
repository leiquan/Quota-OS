# Quota OS

Quota OS is an experimental real-time microkernel for ARM64, currently running on QEMU. Its core idea is simple: nothing runs without a quota. CPU time and memory are reserved up front and locked in at admission (GPU reservation is planned).

Workloads fall into three tiers:

- **System tier**: vendor components such as drivers and the instrument cluster. Their resources are reserved at the factory, and they must never stall.
- **App tier**: developer apps that declare their quotas and need user approval. If the system can't guarantee a request, the app doesn't start.
- **Default tier**: undeclared tasks that share a best-effort pool. They may slow down, but they can't drag down the tiers above them.

The long-term goal is a single automotive OS that can replace today's QNX + Android dual-system setup.

**Status:** Early prototype running on QEMU (ARM64). Not production-ready or safety-certified. Real-time guarantees are under active testing; tier-1 deadline results will be published with reproducible scripts and logs.
