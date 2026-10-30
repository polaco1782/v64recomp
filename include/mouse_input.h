#ifndef RECOMP_MOUSE_INPUT_H
#define RECOMP_MOUSE_INPUT_H
#include <stdbool.h>
#include <stdint.h>

/* Host submits while guest execution is parked. The selected guest update
 * consumes deltas once; held movement/buttons survive successive consumes.
 * No SDL or guest addresses here, so other games can reuse this transport. */
typedef struct RecompMouseFrame {
    double dx, dy;
    int8_t move_x, move_y;
    bool active;
    int32_t wheel; /* Positive = up. Queued detents, not held input. */
} RecompMouseFrame;
typedef struct RecompMouseBuffer { RecompMouseFrame pending; } RecompMouseBuffer;
static inline int32_t recomp_mouse_wheel_sum(int32_t pending, int64_t delta) {
    int64_t total = pending + delta;
    return (int32_t)(total < -32 ? -32 : total > 32 ? 32 : total);
}
static inline void recomp_mouse_submit(RecompMouseBuffer *buffer, RecompMouseFrame frame) {
    if (frame.active && buffer->pending.active) {
        frame.dx += buffer->pending.dx;
        frame.dy += buffer->pending.dy;
        frame.wheel = recomp_mouse_wheel_sum(buffer->pending.wheel, frame.wheel);
    }
    if (!frame.active) { RecompMouseFrame empty = {0}; frame = empty; }
    buffer->pending = frame;
}
static inline RecompMouseFrame recomp_mouse_consume(RecompMouseBuffer *buffer) {
    RecompMouseFrame frame = buffer->pending;
    buffer->pending.dx = buffer->pending.dy = 0;
    // Deliver each detent once, including when several arrive between updates.
    frame.wheel = (frame.wheel > 0) - (frame.wheel < 0);
    buffer->pending.wheel -= frame.wheel;
    return frame;
}
static inline void recomp_mouse_clear(RecompMouseBuffer *buffer) {
    RecompMouseFrame empty = {0};
    buffer->pending = empty;
}
#endif
