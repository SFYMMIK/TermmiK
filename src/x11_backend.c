/*
 * TermmiK
 * Copyright (C) 2026 Szymon Grajner (SfymmiK)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "backend.h"
#include "config.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static Display *g_dpy;
static Window g_win;
static GC g_gc;
static XImage *g_ximage;
static XShmSegmentInfo g_shminfo;
static int g_shm_supported = 0;
static Visual *g_visual;
static int g_depth;

static char *x11_clipboard_text = NULL;
static Atom atom_CLIPBOARD, atom_UTF8_STRING, atom_TARGETS;

static void x11_set_clipboard(const char *text) {
    if (x11_clipboard_text) free(x11_clipboard_text);
    x11_clipboard_text = strdup(text);
    XSetSelectionOwner(g_dpy, atom_CLIPBOARD, g_win, CurrentTime);
}

static void x11_get_clipboard(void) {
    XConvertSelection(g_dpy, atom_CLIPBOARD, atom_UTF8_STRING, atom_CLIPBOARD, g_win, CurrentTime);
}

static int x11_get_timer_fd(void) {
    return -1; // X11 does key repeats automatically
}

static int x11_init(const char *font_pattern) {
    (void)font_pattern; // Fonts are handled by render.c
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) return -1;

    int screen = DefaultScreen(g_dpy);
    Window root = RootWindow(g_dpy, screen);

    XVisualInfo vinfo;
    if (g_config.opacity < 1.0f && XMatchVisualInfo(g_dpy, screen, 32, TrueColor, &vinfo)) {
        g_visual = vinfo.visual;
        g_depth = vinfo.depth;
    } else {
        g_visual = DefaultVisual(g_dpy, screen);
        g_depth = DefaultDepth(g_dpy, screen);
    }
    
    XSetWindowAttributes attr;
    attr.colormap = XCreateColormap(g_dpy, root, g_visual, AllocNone);
    attr.border_pixel = 0;
    attr.background_pixel = 0;
    attr.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;

    g_win = XCreateWindow(g_dpy, root, 0, 0, g_width, g_height, 0,
                          g_depth, InputOutput, g_visual,
                          CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &attr);

    atom_CLIPBOARD = XInternAtom(g_dpy, "CLIPBOARD", False);
    atom_UTF8_STRING = XInternAtom(g_dpy, "UTF8_STRING", False);
    atom_TARGETS = XInternAtom(g_dpy, "TARGETS", False);

    XClassHint *class_hint = XAllocClassHint();
    if (class_hint) {
        class_hint->res_name = "TermmiK";
        class_hint->res_class = "TermmiK";
        XSetClassHint(g_dpy, g_win, class_hint);
        XFree(class_hint);
    }

    if (g_config.opacity < 1.0f) {
        unsigned long opacity = (unsigned long)(0xFFFFFFFFul * g_config.opacity);
        Atom atom_opacity = XInternAtom(g_dpy, "_NET_WM_WINDOW_OPACITY", False);
        XChangeProperty(g_dpy, g_win, atom_opacity, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&opacity, 1);
    }

    g_gc = XCreateGC(g_dpy, g_win, 0, NULL);
    XMapWindow(g_dpy, g_win);

    int major, minor, pixmaps;
    if (XShmQueryVersion(g_dpy, &major, &minor, (Bool*)&pixmaps)) {
        g_shm_supported = 1;
    }

    if (g_shm_supported) {
        g_ximage = XShmCreateImage(g_dpy, g_visual, g_depth, ZPixmap, NULL, &g_shminfo, g_width, g_height);
        g_shminfo.shmid = shmget(IPC_PRIVATE, g_ximage->bytes_per_line * g_ximage->height, IPC_CREAT|0600);
        g_shminfo.shmaddr = g_ximage->data = shmat(g_shminfo.shmid, 0, 0);
        g_shminfo.readOnly = False;
        XShmAttach(g_dpy, &g_shminfo);
        XSync(g_dpy, False);
        shmctl(g_shminfo.shmid, IPC_RMID, 0);
        g_framebuffer = (uint32_t *)g_ximage->data;
    } else {
        // Fallback
        g_framebuffer = malloc(g_width * g_height * 4);
        g_ximage = XCreateImage(g_dpy, g_visual, g_depth, ZPixmap, 0, (char *)g_framebuffer, g_width, g_height, 32, 0);
    }

    return 0;
}

static void x11_cleanup(void) {
    if (g_shm_supported) {
        XShmDetach(g_dpy, &g_shminfo);
        XSync(g_dpy, False);
        g_ximage->data = NULL;
        XDestroyImage(g_ximage);
        shmdt(g_shminfo.shmaddr);
    } else {
        XDestroyImage(g_ximage);
    }
    XFreeGC(g_dpy, g_gc);
    XDestroyWindow(g_dpy, g_win);
    XCloseDisplay(g_dpy);
}

static int x11_poll_events(void) {
    while (XPending(g_dpy) > 0) {
        XEvent ev;
        XNextEvent(g_dpy, &ev);
        
        if (ev.type == KeyPress) {
            KeySym keysym;
            char chars[32];
            int len = XLookupString(&ev.xkey, chars, sizeof(chars), &keysym, NULL);
            
            if ((ev.xkey.state & ControlMask) && (ev.xkey.state & ShiftMask)) {
                if (keysym == XK_C || keysym == XK_c) {
                    extern void term_copy(void);
                    term_copy();
                    continue;
                } else if (keysym == XK_V || keysym == XK_v) {
                    x11_get_clipboard();
                    continue;
                }
            }

            if (keysym == XK_BackSpace) {
                chars[0] = '\x7f';
                len = 1;
            } else if (keysym == XK_Up) {
                strcpy(chars, "\033[A");
                len = 3;
            } else if (keysym == XK_Down) {
                strcpy(chars, "\033[B");
                len = 3;
            } else if (keysym == XK_Right) {
                strcpy(chars, "\033[C");
                len = 3;
            } else if (keysym == XK_Left) {
                strcpy(chars, "\033[D");
                len = 3;
            }

            if (len > 0) {
                term_send_input(chars, len);
            }
        } else if (ev.type == ConfigureNotify) {
            int nw = ev.xconfigure.width;
            int nh = ev.xconfigure.height;
            if (nw != g_width || nh != g_height) {
                g_width = nw;
                g_height = nh;
                
                if (g_shm_supported) {
                    XShmDetach(g_dpy, &g_shminfo);
                    XSync(g_dpy, False);
                    g_ximage->data = NULL;
                    XDestroyImage(g_ximage);
                    shmdt(g_shminfo.shmaddr);
                    shmctl(g_shminfo.shmid, IPC_RMID, 0);
                    
                    g_ximage = XShmCreateImage(g_dpy, g_visual, g_depth, ZPixmap, NULL, &g_shminfo, g_width, g_height);
                    g_shminfo.shmid = shmget(IPC_PRIVATE, g_ximage->bytes_per_line * g_ximage->height, IPC_CREAT|0600);
                    g_shminfo.shmaddr = g_ximage->data = shmat(g_shminfo.shmid, 0, 0);
                    g_shminfo.readOnly = False;
                    XShmAttach(g_dpy, &g_shminfo);
                    XSync(g_dpy, False);
                    shmctl(g_shminfo.shmid, IPC_RMID, 0);
                    g_framebuffer = (uint32_t *)g_ximage->data;
                }
                
                term_resize(g_width, g_height);
            }
        } else if (ev.type == Expose) {
            if (ev.xexpose.count == 0) {
                term_resize(g_width, g_height);
            }
        } else if (ev.type == ButtonPress) {
            if (ev.xbutton.button == 4) { // Scroll up
                term_scroll(3);
            } else if (ev.xbutton.button == 5) { // Scroll down
                term_scroll(-3);
            } else if (ev.xbutton.button == 1) { // Left click
                extern void term_mouse_down(int x, int y);
                term_mouse_down(ev.xbutton.x, ev.xbutton.y);
            }
        } else if (ev.type == ButtonRelease) {
            if (ev.xbutton.button == 1) {
                extern void term_mouse_up(int x, int y);
                extern void term_copy(void);
                term_mouse_up(ev.xbutton.x, ev.xbutton.y);
                term_copy();
            }
        } else if (ev.type == MotionNotify) {
            if (ev.xmotion.state & Button1Mask) {
                extern void term_mouse_motion(int x, int y);
                term_mouse_motion(ev.xmotion.x, ev.xmotion.y);
            }
        } else if (ev.type == SelectionRequest) {
            XSelectionRequestEvent *req = &ev.xselectionrequest;
            XSelectionEvent res = {0};
            res.type = SelectionNotify;
            res.display = req->display;
            res.requestor = req->requestor;
            res.selection = req->selection;
            res.target = req->target;
            res.time = req->time;
            res.property = None;

            if (req->target == atom_TARGETS) {
                Atom targets[] = { atom_TARGETS, atom_UTF8_STRING };
                XChangeProperty(g_dpy, req->requestor, req->property, XA_ATOM, 32,
                                PropModeReplace, (unsigned char *)&targets, 2);
                res.property = req->property;
            } else if (req->target == atom_UTF8_STRING && x11_clipboard_text) {
                XChangeProperty(g_dpy, req->requestor, req->property, req->target, 8,
                                PropModeReplace, (unsigned char *)x11_clipboard_text, strlen(x11_clipboard_text));
                res.property = req->property;
            }
            XSendEvent(g_dpy, req->requestor, False, 0, (XEvent *)&res);
        } else if (ev.type == SelectionNotify) {
            XSelectionEvent *res = &ev.xselection;
            if (res->property != None) {
                Atom actual_type;
                int actual_format;
                unsigned long nitems, bytes_after;
                unsigned char *data = NULL;
                XGetWindowProperty(g_dpy, g_win, res->property, 0, 1024 * 1024, False,
                                   AnyPropertyType, &actual_type, &actual_format,
                                   &nitems, &bytes_after, &data);
                if (data) {
                    term_send_input((char *)data, nitems);
                    XFree(data);
                }
                XDeleteProperty(g_dpy, g_win, res->property);
            }
        }
    }
    return 0;
}

static int x11_get_fd(void) {
    return ConnectionNumber(g_dpy);
}

static void x11_flush(void) {
    if (g_shm_supported) {
        XShmPutImage(g_dpy, g_win, g_gc, g_ximage, 0, 0, 0, 0, g_width, g_height, False);
    } else {
        XPutImage(g_dpy, g_win, g_gc, g_ximage, 0, 0, 0, 0, g_width, g_height);
    }
    XFlush(g_dpy);
}

static WindowBackend _x11_backend = {
    .init = x11_init,
    .cleanup = x11_cleanup,
    .poll_events = x11_poll_events,
    .get_fd = x11_get_fd,
    .flush = x11_flush,
    .get_timer_fd = x11_get_timer_fd,
    .set_clipboard = x11_set_clipboard,
    .get_clipboard = x11_get_clipboard,
    .handle_timer = NULL
};

WindowBackend* get_x11_backend(void) {
    return &_x11_backend;
}
