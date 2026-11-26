#include "window_input.h"

#include <cerrno>
#include <cstdint>
#include <unistd.h>

#include "base/time/time.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "third_party/blink/public/common/input/web_coalesced_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/web/web_widget.h"

static void quota_dispatch_pointer(blink::WebWidget& widget, bool pressed,
                            unsigned x, unsigned y) {
    using blink::WebMouseEvent;
    const gfx::PointF position(static_cast<float>(x), static_cast<float>(y));
    WebMouseEvent event(
        pressed ? WebMouseEvent::Type::kMouseDown : WebMouseEvent::Type::kMouseUp,
        position, position, WebMouseEvent::Button::kLeft, 1,
        WebMouseEvent::kNoModifiers, base::TimeTicks::Now());
    event.UpdateEventModifiersToMatchButton();
    widget.HandleInputEvent(blink::WebCoalescedInputEvent(event, ui::LatencyInfo()));
}

static void quota_dispatch_key(blink::WebWidget& widget, unsigned scalar) {
    using blink::WebKeyboardEvent;
    using Type = WebKeyboardEvent::Type;
    if ((scalar < 32 && scalar != 8 && scalar != 13) ||
        scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
        return;

    WebKeyboardEvent key(Type::kRawKeyDown, WebKeyboardEvent::kNoModifiers,
                         base::TimeTicks::Now());
    key.dom_key = ui::DomKey::FromCharacter(scalar);
    if (scalar == 8 || scalar == 13 || scalar == 32 ||
        (scalar >= '0' && scalar <= '9') || (scalar >= 'A' && scalar <= 'Z'))
        key.windows_key_code = scalar;
    else if (scalar >= 'a' && scalar <= 'z')
        key.windows_key_code = scalar - 'a' + 'A';

    auto deliver = [&widget](const WebKeyboardEvent& event) {
        return widget.HandleInputEvent(blink::WebCoalescedInputEvent(event, ui::LatencyInfo()));
    };
    auto result = deliver(key);
    if (scalar != 8 && result == blink::WebInputEventResult::kNotHandled) {
        WebKeyboardEvent character = key;
        character.SetType(Type::kChar);
        if (scalar <= 0xffff) {
            character.text[0] = static_cast<char16_t>(scalar);
        } else {
            character.text[0] = static_cast<char16_t>(0xd800 + ((scalar - 0x10000) >> 10));
            character.text[1] = static_cast<char16_t>(0xdc00 + ((scalar - 0x10000) & 0x3ff));
        }
        character.unmodified_text = character.text;
        deliver(character);
    }
    key.SetType(Type::kKeyUp);
    deliver(key);
}

int quota_read_window_input(int window_fd, blink::WebWidget& widget) {
    struct {
        uint32_t kind;
        uint32_t x;
        uint32_t y;
    } event;
    static_assert(sizeof(event) == 12);
    ssize_t count;
    do {
        count = read(window_fd, &event, sizeof(event));
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        return errno == EAGAIN ? 0 : -errno;
    if (count != sizeof(event))
        return -EIO;
    switch (event.kind) {
    case 1:
    case 2:
        quota_dispatch_pointer(widget, event.kind == 1, event.x, event.y);
        break;
    case 3:
        quota_dispatch_key(widget, event.x);
        break;
    default:
        return -EPROTO;
    }
    return 1;
}
