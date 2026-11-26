#ifndef QUOTA_DESKTOP_HEAP_H
#define QUOTA_DESKTOP_HEAP_H

/* The Web desktop keeps the WAP controls and overlapping native windows live. */
#ifdef QUOTA_NODE_WEB
#define LVGL_HEAP_BYTES (320u * 1024u)
#else
#define LVGL_HEAP_BYTES (256u * 1024u)
#endif

#endif
