#ifndef OBC_APP_H
#define OBC_APP_H

#include <SDL.h>

#include "camera.h"
#include "project.h"
#include "scene.h"
#include "ui.h"
#include "undo.h"

/* How often the project preview is read back off the framebuffer. */
#define OBC_THUMBNAIL_INTERVAL_MS 500

struct App {
    SDL_Window *window;
    SDL_GLContext gl_context;
    bool running;

    /*
     * Two sizes, and they are not interchangeable.
     *
     * window_* is the coordinate space the UI is laid out in - ImGui's
     * DisplaySize. fb_* is the framebuffer in real pixels, which is what
     * glViewport and glReadPixels want. They differ by the display scale:
     * identical at 100%, and 1.5x apart on a Windows desktop at 150%, which is
     * the common case. Mixing the two put the 3D view and the bookshelf off the
     * side of the canvas, so the layout must use window_* and only the GL
     * rectangles may use fb_*.
     */
    int window_w;
    int window_h;
    int fb_w;
    int fb_h;

    Scene scene;
    Camera camera;
    UiState ui;

    /* Preview of the last rendered frame, kept for the next save, plus when it
     * was taken - the readback is throttled, see app.cpp. */
    Thumbnail thumbnail;
    Uint32 last_thumbnail_ms;

    UndoStack undo;
};

bool app_init(App *app);
void app_run(App *app);
void app_shutdown(App *app);

#endif
