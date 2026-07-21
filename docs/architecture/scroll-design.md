# Scroll Design

## Current State

Page Up / Page Down scrolling works. The `ln X/Y` indicator in the status bar
shows position. Three gaps:

| Gap | Why it matters |
|-----|----------------|
| No mouse wheel | PgUp/PgDn is inconvenient for quick scanning |
| No arrow-key scroll | Line-by-line precision scrolling missing |
| Auto-scroll always jumps to bottom | If user scrolled up, a new status message forces viewport back to bottom |

## Changes

### 1. Mouse support (constructor, `tui.cpp`)

Add to `Tui::Tui()`:

```cpp
mousemask(ALL_MOUSE_EVENTS, nullptr);
mouseinterval(0);  // no click debounce, report immediately
```

### 2. Event handling (main loop, `tui.cpp`)

Insert before the `KEY_NPAGE` / `KEY_PPAGE` handlers:

```cpp
if (ch == KEY_MOUSE) {
    MEVENT ev;
    if (getmouse(&ev) == OK) {
        if (ev.bstate & BUTTON4_PRESSED) {  // wheel up
            win().scroll_top = std::max(0, win().scroll_top - 3);
            draw(); draw_input(input); continue;
        }
        if (ev.bstate & BUTTON5_PRESSED) {  // wheel down
            win().scroll_top = std::min(max_scroll(),
                                        win().scroll_top + 3);
            draw(); draw_input(input); continue;
        }
    }
}
```

```cpp
if (ch == KEY_UP && !drawer_open_) {
    win().scroll_top = std::max(0, win().scroll_top - 1);
    draw(); draw_input(input); continue;
}
if (ch == KEY_DOWN && !drawer_open_) {
    win().scroll_top = std::min(max_scroll(),
                                win().scroll_top + 1);
    draw(); draw_input(input); continue;
}
```

### 3. Smart auto-scroll (`tui_render.cpp`)

In `append_line_ts()`, replace:

```cpp
win().scroll_top = max_scroll();  // always jump to bottom
```

with:

```cpp
// Only auto-scroll if user was already at or near the bottom
int max = max_scroll();
if (win().scroll_top >= max - 2)
    win().scroll_top = max;
```

This means: if the user has scrolled up more than 2 lines from the bottom,
new content appends without forcing the viewport down. The user stays where
they are reading. When they press PgDn / wheel down to the bottom, auto-scroll
resumes.

### Constants

| Key | Action | Lines |
|-----|--------|-------|
| PgUp | Scroll up one page | `chat_height()` |
| PgDn | Scroll down one page | `chat_height()` |
| ↑ | Scroll up one line | 1 |
| ↓ | Scroll down one line | 1 |
| Mouse wheel up | Scroll up 3 lines | 3 |
| Mouse wheel down | Scroll down 3 lines | 3 |
