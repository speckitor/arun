#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>

#include <string.h>
#include <xcb/xcb.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <X11/XKBlib.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>
#include <xcb/xproto.h>

#define MAX_INPUT_SIZE 256
#define VALUE_LIST_SIZE 32

#define WINDOW_WIDTH 300
#define WINDOW_HEIGHT 400

#define INPUT_HEIGHT 23

typedef struct {
    uint16_t width;
    uint16_t height;
    char buf[MAX_INPUT_SIZE];
    size_t top;
    size_t cursor;
} input_bar_t;

static Display *dpy;
static int xlib_scr;
static Visual *visual;
static Colormap cmap;

static xcb_connection_t *c;
static xcb_screen_t *scr;
static xcb_window_t root;
static xcb_generic_event_t *ev;

static xcb_window_t wid;

static input_bar_t input_bar = {0};
static xcb_gcontext_t input_bar_gc;
static xcb_gcontext_t cursor_gc;

static const char *fontname = "Adwaita Mono:size=13.5";
static XftFont *font;
static XftDraw *font_draw;
static XftColor font_color;

static uint32_t value_mask;
static uint32_t value_list[VALUE_LIST_SIZE];

static void die(const char *msg)
{
    fprintf(stderr, "%s", msg);
    exit(1);
}

static void cleanup(void)
{
    XftColorFree(dpy, visual, cmap, &font_color);
    XftDrawDestroy(font_draw);
    xcb_destroy_window(c, wid);
    XCloseDisplay(dpy);
}

static void run_command(void)
{
    if (fork() == 0) {
        execl("/bin/sh", "sh", "-c", input_bar.buf, (char *)NULL);
        _exit(EXIT_FAILURE);
    }
    cleanup();
    exit(0);
}

static void setup_x11(void)
{
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        die("Failed to open X11 display\n");
    }

    xlib_scr = DefaultScreen(dpy);
    visual = DefaultVisual(dpy, xlib_scr);
    cmap = DefaultColormap(dpy, xlib_scr);

    c = XGetXCBConnection(dpy);
    scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    root = scr->root;
}

static void setup_font(void)
{
    font = XftFontOpenName(dpy, xlib_scr, fontname);

    if (!font) {
        die("Failed to open font\n");
    }
    if (!XftColorAllocName(dpy, visual, cmap, "#ffffff", &font_color)) {
        die("Failed to allocate font color\n");
    }
    
    font_draw = XftDrawCreate(dpy, wid, visual, cmap);
}

static void setup_window(void)
{
    wid = xcb_generate_id(c);

    value_mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    value_list[0] = 0xFF0000;
    value_list[1] = 1;
    value_list[2] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE;

    xcb_create_window(
        c,
        XCB_COPY_FROM_PARENT,
        wid,
        root,
        1920/2 - WINDOW_WIDTH/2, 1080/2 - WINDOW_HEIGHT/2, WINDOW_WIDTH, WINDOW_HEIGHT, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        scr->root_visual,
        value_mask, value_list
    );
}

static void setup_gc(void)
{
    input_bar_gc = xcb_generate_id(c);

    value_mask = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    value_list[0] = scr->black_pixel;
    value_list[1] = 0;
    xcb_create_gc(c, input_bar_gc, root, value_mask, value_list);

    cursor_gc = xcb_generate_id(c);

    value_mask = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    value_list[0] = scr->white_pixel;
    value_list[1] = 0;
    xcb_create_gc(c, cursor_gc, root, value_mask, value_list);
}

static XKeyEvent cast_key_press_event(xcb_key_press_event_t *e)
{
    XKeyEvent xkey;
    xkey.display = dpy;
    xkey.window = e->event;
    xkey.root = e->root;
    xkey.subwindow = e->child;
    xkey.time = e->time;
    xkey.x = e->event_x;
    xkey.y = e->event_y;
    xkey.x_root = e->root_x;
    xkey.y_root = e->root_y;
    xkey.state = e->state;
    xkey.keycode = e->detail;
    xkey.same_screen = e->same_screen;
    xkey.type = KeyPress;

    return xkey;
}

static void draw_input_bar(void)
{
    const xcb_rectangle_t rectangle[] = {
        {0, 0, input_bar.width, input_bar.height}
    };
    
    xcb_poly_fill_rectangle(
        c,
        wid,
        input_bar_gc,
        1,
        rectangle
    );

    XftDrawStringUtf8(font_draw, &font_color, font, 10, font->height-2, (const FcChar8 *)input_bar.buf, input_bar.top);
    
    const xcb_rectangle_t cursor[] = {
        {10+font->max_advance_width*input_bar.cursor, 2, 1, font->height}
    };
    
    xcb_poly_fill_rectangle(
        c,
        wid,
        cursor_gc,
        1,
        cursor
    );

    XFlush(dpy);
    xcb_flush(c);
}

static void handle_key_press(xcb_generic_event_t *ev)
{
    XKeyEvent e = cast_key_press_event((xcb_key_press_event_t*) ev);

    char buf[64];
    KeySym keysym;
    int len = XLookupString(&e, buf, sizeof(buf), &keysym, NULL);

    if (keysym == XK_Return) {
        run_command();
        input_bar.top = 0;
        input_bar.cursor = 0;
        return;
    }

    if (keysym == XK_Right && input_bar.cursor < input_bar.top) {
        input_bar.cursor++;
        return;
    }

    if (keysym == XK_Left && input_bar.cursor > 0) {
        input_bar.cursor--;
        return;
    }

    if (keysym == XK_Right && input_bar.cursor < input_bar.top) {
        input_bar.cursor++;
        return;
    }

    if (keysym == XK_BackSpace) {
        if (input_bar.cursor) {
            memccpy(&input_bar.buf[input_bar.cursor - 1], &input_bar.buf[input_bar.cursor], '\0', input_bar.top - input_bar.cursor);
            input_bar.cursor--;
            input_bar.top--;
        }
        return;
    }

    if (keysym == XK_Escape) {
        cleanup();
        exit(1);
        return;
    }

    if (isprint(buf[0]) && input_bar.top < MAX_INPUT_SIZE) {
        memccpy(&input_bar.buf[input_bar.cursor + 1], &input_bar.buf[input_bar.cursor], '\0', input_bar.top - input_bar.cursor);
        input_bar.buf[input_bar.cursor++] = buf[0];
        input_bar.top++;
    }
}    

int main(void)
{
    setup_x11();

    setup_window();

    setup_gc();

    setup_font();

    input_bar.width = WINDOW_WIDTH;
    input_bar.height = font->height + 5;
    input_bar.top = 0;
    input_bar.cursor = 0;

    xcb_map_window(c, wid);

    xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, wid, XCB_CURRENT_TIME);

    xcb_flush(c);

    while (c && !xcb_connection_has_error(c)) {
        ev = xcb_wait_for_event(c);
        switch (ev->response_type & ~0x80) {
        case XCB_EXPOSE:
            draw_input_bar();
            break;
        case XCB_KEY_PRESS:
            handle_key_press(ev);
            draw_input_bar();
            break;
        case XCB_FOCUS_OUT:
            cleanup();
            exit(1);          
            break;
        }
        free(ev);
    }

    cleanup();
    return 0;
}
