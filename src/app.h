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

    int window_w;
    int window_h;

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
