#pragma once

// The archived NVRHI snapshot used by the NVIDIA sample includes PIX calls
// but does not ship the Windows PIX header. The smoke test does not need PIX
// markers, so use a no-op compatibility fallback for this diagnostic build.
#ifndef PIXBeginEvent
#define PIXBeginEvent(...) ((void)0)
#endif
#ifndef PIXEndEvent
#define PIXEndEvent(...) ((void)0)
#endif
