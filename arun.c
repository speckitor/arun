#include <X11/Xutil.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <X11/XKBlib.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>
#include <xcb/xproto.h>

#include "config.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define MAX_BINS_SIZE 16384

#define MAX_INPUT_SIZE 257
#define VALUE_LIST_SIZE 32

typedef struct {
    uint16_t width;
    uint16_t height;
    char buf[MAX_INPUT_SIZE];
    size_t top;
    size_t cursor;
    size_t rrange_s;
    size_t rrange_e;
} input_bar_t;

typedef struct {
    char *all[MAX_BINS_SIZE];
    size_t top;
    const char *drawable[MAX_BINS_SIZE];
    size_t dtop;
    size_t cursor;
    size_t prevcursor;
    size_t rrange_s;
    size_t rrange_e;
} bins_t;

static int mon_x;
static int mon_y;
static int mon_width;
static int mon_height;

static Display *dpy;
static int xlib_scr;
static Visual *visual;
static Colormap cmap;

static xcb_connection_t *c;
static xcb_screen_t *scr;
static xcb_window_t root;
static xcb_generic_event_t *ev;

static xcb_get_input_focus_reply_t *last_focus;

static xcb_window_t wid;
static int window_width;
static int window_height;

static input_bar_t input = {0};
static xcb_gcontext_t input_bar_gc;
static xcb_gcontext_t cursor_gc;

static bins_t bins;
static bool parse_bins;
static xcb_gcontext_t bin_gc;
static xcb_gcontext_t selected_gc;

static XftFont *font;
static XftDraw *font_draw;
static XftColor input_font_color;
static XftColor bin_font_color;
static XftColor selected_font_color;

static uint32_t value_mask;
static uint32_t value_list[VALUE_LIST_SIZE];

static void die(const char *msg)
{
    fprintf(stderr, "%s", msg);
    exit(1);
}

static void cleanup(void)
{
    if (last_focus) {
        xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, last_focus->focus, XCB_CURRENT_TIME);
    }
    for (size_t i = 0; i < MAX_BINS_SIZE; ++i) {
        free(bins.all[i]);
    }
    XftColorFree(dpy, visual, cmap, &input_font_color);
    XftColorFree(dpy, visual, cmap, &bin_font_color);
    XftColorFree(dpy, visual, cmap, &selected_font_color);
    XftDrawDestroy(font_draw);
    xcb_ungrab_button(c, XCB_BUTTON_INDEX_ANY, root, XCB_MOD_MASK_ANY);
    xcb_destroy_window(c, wid);
    XCloseDisplay(dpy);
}

static void run_command(void)
{
    char *selected = strdup(bins.drawable[bins.cursor]);
    pid_t pid = fork();
    if (pid == 0) {
        if (strstr(selected, input.buf) != NULL) {
            execl("/bin/sh", "sh", "-c", selected, (char *)NULL);
        } else {
            execl("/bin/sh", "sh", "-c", input.buf, (char *)NULL);
        }
        perror("execl");
        _exit(EXIT_FAILURE);
    } else if (pid < 0) {
        perror("fork");
        cleanup();
        free(selected);
        exit(1);
    } else {
        cleanup();
        free(selected);
        exit(0);
    }
}

static int cmpstrs(const void *p1, const void *p2)
{
    return strcmp(*(const char **)p1, *(const char **)p2);
}

static void parce_dir(char *dirpath)
{
    DIR *dir = opendir(dirpath);

    if (!dir) return; 

    size_t i;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, "..") == 0) continue;
        if (strcmp(entry->d_name, ".") == 0) continue;
        for (i = 0; i < bins.top; ++i) {
            if (strcmp(bins.all[i], entry->d_name) == 0) {
                break;
            } 
        }
        if (bins.top == i) {
            bins.all[bins.top++] = strdup(entry->d_name);
        }
    }

    closedir(dir);
}

static void setup(void)
{
    char *res = getenv("PATH");
    char buf[1024];
    size_t cursor = 0;
    for (size_t i = 0; i < strlen(res); ++cursor, ++i) {
        if (res[i] == ':') {
            buf[cursor] = '\0';
            parce_dir(buf);
            cursor = -1;
        } else {
            buf[cursor] = res[i];
        }
    }

    qsort(bins.all, bins.top, sizeof(char *), cmpstrs);
    
    bins.rrange_e = COMPLETIONS_NUMBER;
    input.rrange_e = TEXT_LENGTH;

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

    xcb_query_pointer_reply_t *pointer_reply = xcb_query_pointer_reply(c, xcb_query_pointer(c, root), NULL); 
    int16_t ppx = pointer_reply->root_x;
    int16_t ppy = pointer_reply->root_y;

    if (!pointer_reply) {
        cleanup();
        die("Failed to get pointer reply\n");
    }

    xcb_randr_get_screen_resources_current_reply_t *res_reply = xcb_randr_get_screen_resources_current_reply(c, xcb_randr_get_screen_resources_current(c, root), NULL);

    if (!res_reply) {
        cleanup();
        die("Failed to get screen resources reply\n");
    }

    int32_t len = xcb_randr_get_screen_resources_current_outputs_length(res_reply);
    xcb_randr_output_t *outputs = xcb_randr_get_screen_resources_current_outputs(res_reply);

    for (int i = 0; i < len; ++i) {
        xcb_randr_get_output_info_cookie_t output_cookie = xcb_randr_get_output_info(c, outputs[i], XCB_CURRENT_TIME);
        xcb_randr_get_output_info_reply_t *output_reply = xcb_randr_get_output_info_reply(c, output_cookie, NULL);

        if (!output_reply || output_reply->crtc == XCB_NONE) {
            continue;
        }

        xcb_randr_get_crtc_info_cookie_t crtc_cookie = xcb_randr_get_crtc_info(c, output_reply->crtc, XCB_CURRENT_TIME);
        xcb_randr_get_crtc_info_reply_t *crtc_reply = xcb_randr_get_crtc_info_reply(c, crtc_cookie, NULL);

        if (!crtc_reply) {
            free(output_reply);
            continue;
        }

        if ((ppx >= crtc_reply->x) && (ppx <= crtc_reply->x + crtc_reply->width) &&
            (ppy >= crtc_reply->y) && (ppy <= crtc_reply->y + crtc_reply->height)) {
            mon_x = crtc_reply->x;
            mon_y = crtc_reply->y;
            mon_width = crtc_reply->width;
            mon_height = crtc_reply->height;
            break;
        }

        free(output_reply);
        free(crtc_reply);
    }

    free(pointer_reply);
    free(res_reply);

    wid = xcb_generate_id(c);

    font = XftFontOpenName(dpy, xlib_scr, fontname);

    if (!font) {
        die("Failed to open font\n");
    }

    if ((!XftColorAllocName(dpy, visual, cmap, input_fg_color, &input_font_color)) ||
        (!XftColorAllocName(dpy, visual, cmap, bin_fg_color, &bin_font_color)) ||
        (!XftColorAllocName(dpy, visual, cmap, selected_bin_fg_color, &selected_font_color))) {
        die("Failed to allocate font color\n");
    }

    font_draw = XftDrawCreate(dpy, wid, visual, cmap);
    if (!font_draw) {
        die("Failed to allocate font draw\n");
    }

    window_width = TEXT_OFFSET_X * 2 + TEXT_LENGTH * font->max_advance_width;
    window_height = (TEXT_OFFSET_Y * 2 + font->height) * (COMPLETIONS_NUMBER + 1);

    value_mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    value_list[0] = BG_COLOR;
    value_list[1] = 1;
    value_list[2] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_BUTTON_PRESS;

    xcb_create_window(
        c,
        XCB_COPY_FROM_PARENT,
        wid,
        root,
        mon_x + mon_width/2 - window_width/2, mon_y + mon_height/2 - window_height/2, window_width, window_height, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        scr->root_visual,
        value_mask, value_list
    );

    xcb_grab_button(c, 0, root, XCB_EVENT_MASK_BUTTON_PRESS, XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_SYNC, XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY);

    input_bar_gc = xcb_generate_id(c);

    value_mask = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    value_list[0] = INPUT_BG_COLOR;
    value_list[1] = 0;
    xcb_create_gc(c, input_bar_gc, root, value_mask, value_list);

    cursor_gc = xcb_generate_id(c);

    value_mask = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    value_list[0] = INPUT_CURSOR_COLOR;
    value_list[1] = 0;
    xcb_create_gc(c, cursor_gc, root, value_mask, value_list);

    bin_gc = xcb_generate_id(c);

    value_mask = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    value_list[0] = BIN_BG_COLOR;
    value_list[1] = 0;
    xcb_create_gc(c, bin_gc, root, value_mask, value_list);

    selected_gc = xcb_generate_id(c);

    value_mask = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    value_list[0] = SELECTED_BIN_BG_COLOR;
    value_list[1] = 0;
    xcb_create_gc(c, selected_gc, root, value_mask, value_list);
}

static void draw_input_bar(void)
{
    const xcb_rectangle_t rectangle[] = {
        {0, 0, input.width, input.height}
    };

    xcb_poly_fill_rectangle(
        c,
        wid,
        input_bar_gc,
        1,
        rectangle
    );

    if (input.cursor > input.rrange_e) {
        input.rrange_s++;
        input.rrange_e++;
    } else if (input.cursor < input.rrange_s || (input.top < input.rrange_e && input.top > TEXT_LENGTH - 1)) {
        input.rrange_s--;
        input.rrange_e--;
    }
    
    XftDrawStringUtf8(
        font_draw,
        &input_font_color,
        font,
        TEXT_OFFSET_X,
        TEXT_OFFSET_Y + (font->height / 1.25),
        (const FcChar8 *)&input.buf[input.rrange_s],
        MIN(TEXT_LENGTH, input.top)
    );

    int cursor_factor = input.cursor - input.rrange_s;
    const xcb_rectangle_t cursor[] = {
        {TEXT_OFFSET_X + font->max_advance_width * cursor_factor, TEXT_OFFSET_Y, 1, font->height}
    };

    XFlush(dpy);

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

static void draw_bin(const char *cmd, bool selected, int y)
{
    const xcb_rectangle_t bin_rect[] = {
        {0, y, window_width, 2 * TEXT_OFFSET_Y + font->height}
    };

    size_t len = strlen(cmd);

    if (selected) {
        xcb_poly_fill_rectangle(
            c,
            wid,
            selected_gc,
            1,
            bin_rect
        );

        XftDrawStringUtf8(
            font_draw,
            &selected_font_color,
            font,
            TEXT_OFFSET_X,
            y + font->height,
            (const FcChar8 *)cmd,
            MIN(len, TEXT_LENGTH)
        );
    } else {
        xcb_poly_fill_rectangle(
            c,
            wid,
            bin_gc,
            1,
            bin_rect
        );

        XftDrawStringUtf8(
            font_draw,
            &bin_font_color,
            font,
            TEXT_OFFSET_X,
            y + font->height,
            (const FcChar8 *)cmd,
            MIN(len, TEXT_LENGTH)
        );
    }

    XFlush(dpy);
    xcb_flush(c);
}

static void redraw_all(void)
{
    int dy = 2 * TEXT_OFFSET_Y + font->height;
    for (size_t i = bins.rrange_s; i < bins.rrange_e; ++i) {
        draw_bin(bins.drawable[i], bins.cursor == i, dy);
        dy += 2 * TEXT_OFFSET_Y + font->height;
    }
}

static void redraw_diff(void)
{
    int dy = 2 * TEXT_OFFSET_Y + font->height;
    for (size_t i = bins.rrange_s; i < bins.rrange_e; ++i) {
        if (i == bins.cursor || i == bins.prevcursor) {
            draw_bin(bins.drawable[i], bins.cursor == i, dy);
        }
        dy += 2 * TEXT_OFFSET_Y + font->height;
    }
}

static void draw_bins(bool parse_bins)
{
    if (parse_bins) {
        bins.dtop = 0;
        for (size_t i = 0; i < bins.top; ++i) {
            if (strstr(bins.all[i], input.buf) != NULL) {
                bins.drawable[bins.dtop++] = bins.all[i];
            }
        }

        if (bins.cursor > bins.dtop) {
            bins.cursor = 0;
            bins.prevcursor = 0;
            bins.rrange_s = 0;
            bins.rrange_e = COMPLETIONS_NUMBER;
        }
    }

    if (bins.cursor >= bins.rrange_e) {
        bins.rrange_s++;
        bins.rrange_e++;
        redraw_all();
    } else if (bins.cursor < bins.rrange_s) {
        bins.rrange_s--;
        bins.rrange_e--;
        redraw_all();
    } else if (parse_bins) {
        redraw_all();
    } else {
        redraw_diff();
    }

    if (bins.dtop < COMPLETIONS_NUMBER) {
        int dy = (bins.dtop + 1) * (2 * TEXT_OFFSET_Y + font->height); 
        const xcb_rectangle_t void_rect[] = {
            {0, dy, window_width, (COMPLETIONS_NUMBER - bins.dtop) * (2 * TEXT_OFFSET_Y + font->height)}
        };

        xcb_poly_fill_rectangle(
            c,
            wid,
            bin_gc,
            1,
            void_rect
        );
    }
    XFlush(dpy);
    xcb_flush(c);
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

static bool handle_key_press(xcb_generic_event_t *ev)
{
    XKeyEvent e = cast_key_press_event((xcb_key_press_event_t *)ev);
    char buf;
    KeySym keysym;
    XLookupString(&e, &buf, 1, &keysym, NULL);

    if (e.state & XCB_MOD_MASK_CONTROL) {
        switch (keysym) {
        case XK_f:
            if (input.cursor < input.top) input.cursor++;
            break;
        case XK_b:
            if (input.cursor > 0) input.cursor--;
            break;
        case XK_a:
            input.cursor = 0;
            input.rrange_s = 0;
            input.rrange_e = TEXT_LENGTH;
            break;
        case XK_e:
            input.cursor = input.top;
            if (input.top > TEXT_LENGTH) {
                input.rrange_s = input.top - TEXT_LENGTH;
                input.rrange_e = input.top;
            }
            break;
        case XK_u:
            if (input.cursor > 0) {
                memmove(input.buf, &input.buf[input.cursor], input.top - input.cursor);
                input.top -= input.cursor;
                input.buf[input.top] = '\0';
                input.cursor = 0;
                input.rrange_s = 0;
                input.rrange_e = TEXT_LENGTH;
                return true;
            }
            break;
        case XK_k:
            input.top = input.cursor;
            input.buf[input.top] = '\0';
            if (input.cursor > TEXT_LENGTH) {
                input.rrange_s = input.cursor - TEXT_LENGTH;
                input.rrange_e = input.cursor;
            } else {
                input.rrange_s = 0;
                input.rrange_e = TEXT_LENGTH;
            }
            return true;
        case XK_h:
            if (input.cursor > 0) {
                memmove(&input.buf[input.cursor - 1], &input.buf[input.cursor], input.top - input.cursor);
                input.cursor--;
                input.buf[--input.top] = '\0';
                return true;
            }
            break;
        case XK_d:
            if (input.cursor < input.top) {
                memmove(&input.buf[input.cursor], &input.buf[input.cursor + 1], input.top - input.cursor);
                input.buf[--input.top] = '\0';
                return true;
            }
            break;
        case XK_w:
            if (input.cursor > 0) {
                size_t i = input.cursor - 1;
                while ((i > 0) && ((input.buf[i] == ' ') || (input.buf[i - 1] != ' '))) i--;
                memmove(&input.buf[i], &input.buf[input.cursor], input.top - input.cursor);
                input.top -= (input.cursor - i);
                input.buf[input.top] = '\0';
                input.cursor = i;
                if (input.cursor > TEXT_LENGTH) {
                    input.rrange_s = input.cursor - TEXT_LENGTH;
                    input.rrange_e = input.cursor;
                } else {
                    input.rrange_s = 0;
                    input.rrange_e = TEXT_LENGTH;
                }
                return true;
            }
            break;
        }
    } else if (e.state & XCB_MOD_MASK_1) {
        switch (keysym) {
            case XK_f:
                if (input.cursor == input.top) break;
                input.cursor++;
                while ((input.cursor < input.top) &&
                        (input.buf[input.cursor] == ' ')) {
                    input.cursor++;
                }
                while ((input.cursor < input.top) &&
                       (input.buf[input.cursor] != ' ')) {
                    input.cursor++;
                }
                if (input.cursor > TEXT_LENGTH) {
                    input.rrange_s = input.cursor - TEXT_LENGTH;
                    input.rrange_e = input.cursor;
                }
                break;
            case XK_b:
                if (input.cursor == 0) break;
                input.cursor--;
                while ((input.cursor > 0) &&
                       ((input.buf[input.cursor] == ' ') ||
                       (input.buf[input.cursor - 1] != ' '))) {
                    input.cursor--;
                }
                if (input.cursor < input.rrange_s) {
                    input.rrange_s = input.cursor;
                    input.rrange_e = MIN(input.cursor + TEXT_LENGTH, input.top);
                }
                break;
            case XK_d:
                if (input.cursor < input.top) {
                    size_t i = input.cursor;
                    while ((i < input.top) && (input.buf[i] == ' ')) i++;
                    while ((i < input.top) && (input.buf[i] != ' ')) i++;
                    memmove(&input.buf[input.cursor], &input.buf[i], input.top - i);
                    input.top -= (i - input.cursor);
                    input.buf[input.top] = '\0';
                    if (input.cursor > TEXT_LENGTH) {
                        input.rrange_s = input.cursor - TEXT_LENGTH;
                        input.rrange_e = input.cursor;
                    } else {
                        input.rrange_s = 0;
                        input.rrange_e = TEXT_LENGTH;
                    }
                    return true;
                }
                break;
        }
    } else {
        switch (keysym) {
        case XK_BackSpace:
            if (input.cursor > 0) {
                memmove(&input.buf[input.cursor - 1], &input.buf[input.cursor], input.top - input.cursor);
                input.cursor--;
                input.buf[--input.top] = '\0';
                return true;
            }
            break;
        case XK_Delete:
            if (input.cursor < input.top) {
                memmove(&input.buf[input.cursor], &input.buf[input.cursor + 1], input.top - input.cursor);
                input.buf[--input.top] = '\0';
                return true;
            }
            break;
        case XK_Return:
            run_command();
            break;
        case XK_Down:
            if (bins.cursor + 1 < bins.dtop) bins.prevcursor = bins.cursor++;
            break;
        case XK_Up:
            if (bins.cursor > 0) bins.prevcursor = bins.cursor--;
            break;
        case XK_Right:
            if (input.cursor < input.top) input.cursor++;
            break;
        case XK_Left:
            if (input.cursor > 0) input.cursor--;
            break;
        case XK_Escape:
            cleanup();
            exit(1);
        default:
            if (isprint(buf) && input.top < MAX_INPUT_SIZE) {
                memmove(&input.buf[input.cursor + 1], &input.buf[input.cursor], input.top - input.cursor);
                input.buf[input.cursor++] = buf;
                input.top++;
                return true;
            }
            break;
        }
    }

    return false;
}

int main(void)
{
    setup();

    input.width = window_width;
    input.height = font->height + TEXT_OFFSET_Y * 2;
    input.top = 0;
    input.cursor = 0;

    xcb_map_window(c, wid);

    last_focus = xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL);
    xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, wid, XCB_CURRENT_TIME);

    xcb_flush(c);

    while (c && !xcb_connection_has_error(c)) {
        ev = xcb_wait_for_event(c);
        switch (ev->response_type & ~0x80) {
        case XCB_EXPOSE:
            draw_input_bar();
            draw_bins(true);
            break;
        case XCB_KEY_PRESS:
            parse_bins = handle_key_press(ev);
            draw_input_bar();
            draw_bins(parse_bins);
            break;
        case XCB_BUTTON_PRESS:
            cleanup();
            exit(1);
            break;
        }
        free(ev);
    }

    cleanup();
    return 0;
}
