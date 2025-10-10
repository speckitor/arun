#include <stdlib.h>
#include <stdbool.h>

#include <X11/Xlib-xcb.h>
#include <X11/Xft/Xft.h>

#define MAX_INPUT_SIZE 256
#define VALUE_LIST_SIZE 32

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

static xcb_gcontext_t gc;
static xcb_window_t wid;

static uint16_t window_width;
static uint16_t window_height;
static input_bar_t input_bar = {0};

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
        1920 + 1920/2 - 200/2, 1080/2 - 400 / 2, 200, 400, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        scr->root_visual,
        value_mask, value_list
    );
}

static void setup_gc(void)
{
    gc = xcb_generate_id(c);

    value_mask = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    value_list[0] = scr->black_pixel;
    value_list[1] = 0;
    xcb_create_gc(c, gc, root, value_mask, value_list);
}

static void draw_input_bar(void)
{
    const xcb_rectangle_t rectangle[] = {
        {0, 0, input_bar.width, input_bar.height}
    };
    
    xcb_poly_fill_rectangle(
        c,
        wid,
        gc,
        1,
        rectangle
    );

    XftDrawStringUtf8(font_draw, &font_color, font, 10, 17, (const FcChar8 *)"Hello Xft!", 10);

    XFlush(dpy);
    xcb_flush(c);
}

int main(void)
{
    window_width = 200;
    window_height = 400;

    input_bar.width = window_width;
    input_bar.height = 20;
    input_bar.top = 0;
    input_bar.cursor = 0;

    setup_x11();

    setup_window();

    setup_gc();

    setup_font();

    xcb_map_window(c, wid);

    xcb_flush(c);

    bool focused = false;
    xcb_generic_event_t *ev;
    while (c && !xcb_connection_has_error(c) && !focused) {
        ev = xcb_wait_for_event(c);
        switch (ev->response_type & ~0x80) {
        case XCB_EXPOSE:
            draw_input_bar();
            break;
        case XCB_KEY_PRESS:
            break;
        case XCB_FOCUS_OUT:
            focused = true;
            break;
        }
        free(ev);
    }
    
    XftColorFree(dpy, visual, cmap, &font_color);
    XftDrawDestroy(font_draw);
    xcb_destroy_window(c, wid);
    xcb_disconnect(c);
    XCloseDisplay(dpy);
    return 0;
}
