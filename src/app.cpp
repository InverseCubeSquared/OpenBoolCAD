#include "app.h"

#include <stdio.h>

#include "fonts.h"
#include "gl_compat.h"
#include "imgui.h"
#include "imgui_backend.h"
#include "platform.h"
#include "render.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

/* Window and GL context */

static void app_apply_style(void) {
    ImGui::StyleColorsLight();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.FrameRounding = 3.0f;
    s.GrabRounding = 3.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    /* Tighter than the default: the embedded pixel face is wider per character
     * than ImGui's built-in one, and the toolbar row ran off its end. */
    s.ItemSpacing = ImVec2(4.0f, 4.0f);
    s.FramePadding = ImVec2(3.0f, 3.0f);
    s.ScrollbarSize = 12.0f;

    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg]    = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    c[ImGuiCol_MenuBarBg]   = ImVec4(0.97f, 0.97f, 0.97f, 1.00f);
    c[ImGuiCol_Button]      = ImVec4(0.90f, 0.90f, 0.91f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.80f, 0.86f, 0.94f, 1.00f);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.62f, 0.78f, 0.92f, 1.00f);
    c[ImGuiCol_Header]      = ImVec4(0.62f, 0.78f, 0.92f, 0.70f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.62f, 0.78f, 0.92f, 0.90f);
    c[ImGuiCol_HeaderActive]  = ImVec4(0.40f, 0.66f, 0.92f, 1.00f);
}

bool app_init(App *app) {
    app->window = NULL;
    app->gl_context = NULL;
    app->running = false;
    app->window_w = 1600;
    app->window_h = 900;
    app->fb_w = app->window_w;
    app->fb_h = app->window_h;
    app->last_thumbnail_ms = 0;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

#if defined(__EMSCRIPTEN__)
    /* WebGL 1 is GLES 2.0. The fixed function calls the renderer makes are
     * emulated in gl_compat.cpp rather than forked out of render.cpp. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    /* Compatibility profile: the renderer targets GL 1.2 as a floor and 2.1
     * where available, so a core profile would break it. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    app->window = SDL_CreateWindow("OpenBoolCAD",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   app->window_w, app->window_h, window_flags);
    if (!app->window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    app->gl_context = SDL_GL_CreateContext(app->window);
    if (!app->gl_context) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(app->window, app->gl_context);
#if !defined(__EMSCRIPTEN__)
    /* The browser paces us through requestAnimationFrame, and asking for a swap
     * interval before a main loop exists is an error there. */
    SDL_GL_SetSwapInterval(1);
#endif

    const char *version = (const char *)glGetString(GL_VERSION);
    printf("OpenGL: %s\n", version ? version : "unknown");

    /* Builds the fixed function emulation. A no-op on the desktop. */
    obc_gl_init();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL; // no layout persistence until docking is wired up
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    app_apply_style();

    /*
     * The UI font, from the same embedded set the text generator draws on.
     * A pixel face wants a whole-number size or the hinting smears, and
     * FontDataOwnedByAtlas has to be off: the bytes are const rodata, not
     * something ImGui may free.
     */
    const EmbeddedFont *ui_font = font_find("PixelatedElegance");
    if (ui_font) {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        /* 12 px: this face is wider than ImGui's default, and at 16 the
         * toolbar ran off the end of its row. */
        io.Fonts->AddFontFromMemoryTTF((void *)ui_font->data, (int)ui_font->size,
                                       12.0f, &cfg);
    }

    if (!ImGui_ImplSDL2_InitForOpenGL(app->window, app->gl_context)) {
        fprintf(stderr, "ImGui SDL2 backend init failed\n");
        return false;
    }
    if (!obc_imgui_renderer_init()) {
        fprintf(stderr, "ImGui renderer backend init failed\n");
        return false;
    }

    platform_init();

    scene_new_empty(&app->scene);
    undo_init(&app->undo);
    camera_init(&app->camera);
    ui_init(&app->ui);

    app->running = true;
    return true;
}

/* Main loop */

static void app_frame(App *app) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT) app->running = false;
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_CLOSE &&
            event.window.windowID == SDL_GetWindowID(app->window)) {
            app->running = false;
        }
    }

    if (SDL_GetWindowFlags(app->window) & SDL_WINDOW_MINIMIZED) {
        SDL_Delay(10);
        return;
    }

    /* Browser: picks up a page resize. Desktop: nothing. */
    platform_frame_begin(app);

    obc_imgui_renderer_new_frame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    /*
     * The layout size is taken from ImGui rather than from SDL, and after
     * NewFrame rather than before, so it is by construction the same space
     * ImGui just laid itself out in. Reading the *drawable* size here instead
     * is what broke the browser build on a scaled display: every pane is
     * positioned in ImGui units, so feeding it pixels pushed the bookshelf off
     * the canvas and left the 3D view drawing outside its own rectangle.
     */
    ImGuiIO &frame_io = ImGui::GetIO();
    app->window_w = (int)frame_io.DisplaySize.x;
    app->window_h = (int)frame_io.DisplaySize.y;
    SDL_GL_GetDrawableSize(app->window, &app->fb_w, &app->fb_h);
    if (app->fb_w <= 0 || app->fb_h <= 0) {
        app->fb_w = app->window_w;
        app->fb_h = app->window_h;
    }

    ui_draw(app);

    ImGui::Render();

    /* The whole window is cleared first, then the 3D view redraws its own
     * rectangle; ImGui panels land on top. */
    glViewport(0, 0, app->fb_w, app->fb_h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.94f, 0.94f, 0.94f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    RenderOverlay overlay;
    ui_build_overlay(app, &overlay);
    render_frame(app->scene, app->camera, app->ui.viewport, app->ui.snap_grid_mm, overlay);

    /* Read back before the ImGui panels are drawn over it, so the preview is
     * the model rather than the surrounding chrome.
     *
     * Sampled a few times a second rather than every frame: glReadPixels is a
     * full pipeline stall, which costs little on the desktop but in WebGL costs
     * more than the entire rest of the frame - it measured at over 90% of frame
     * time before this was throttled. The preview was already a frame behind,
     * so letting it be a fraction of a second behind changes nothing about it.
     */
    Uint32 now = SDL_GetTicks();
    if (now - app->last_thumbnail_ms >= OBC_THUMBNAIL_INTERVAL_MS ||
        app->thumbnail.rgb.empty()) {
        app->last_thumbnail_ms = now;
        render_capture_thumbnail(app->ui.viewport, 256, &app->thumbnail);
    }

    glViewport(0, 0, app->fb_w, app->fb_h);
    obc_imgui_renderer_render(ImGui::GetDrawData());

    SDL_GL_SwapWindow(app->window);
}

#if defined(__EMSCRIPTEN__)

/* The browser owns the frame clock, so the loop is inverted: the page calls us
 * once per animation frame instead of us spinning. */
static void app_frame_callback(void *arg) {
    app_frame((App *)arg);
}

void app_run(App *app) {
    emscripten_set_main_loop_arg(app_frame_callback, app, 0, 1);
}

#else

void app_run(App *app) {
    while (app->running) app_frame(app);
}

#endif

void app_shutdown(App *app) {
    obc_imgui_renderer_shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    obc_gl_shutdown();

    if (app->gl_context) SDL_GL_DeleteContext(app->gl_context);
    if (app->window) SDL_DestroyWindow(app->window);
    SDL_Quit();
}
