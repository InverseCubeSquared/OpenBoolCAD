#include "platform.h"

#if defined(__EMSCRIPTEN__)

#include <emscripten.h>
#include <emscripten/html5.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "app.h"
#include "ui.h"

/*
 * Browser file dialogs.
 *
 * A page has no filesystem, so the two directions are not symmetric and neither
 * one can be a blocking call:
 *
 *   open - a hidden <input type="file"> raises the host's own picker. The bytes
 *          come back through a FileReader, land in MEMFS, and the rest of the
 *          editor opens that path exactly as it would a real one.
 *   save - the file has to exist before it can be offered, so the action runs
 *          against MEMFS first and the bytes are then handed to the host.
 *          showSaveFilePicker is the real save dialog where it exists, and the
 *          handle is kept so a later Save writes back to the same file instead
 *          of asking again. Firefox and Safari have no such API, so they fall
 *          back to a download, which their own download settings can still turn
 *          into a native dialog.
 *
 * Both host calls need transient user activation. They are reached from a menu
 * click on the frame right after it, well inside the activation window.
 */

struct PendingFile {
    bool ready;
    int action;
    std::string path;
};

static PendingFile g_pending;

extern "C" EMSCRIPTEN_KEEPALIVE void obc_platform_file_ready(const char *path, int action) {
    g_pending.ready = true;
    g_pending.action = action;
    g_pending.path = path ? path : "";
}

EM_JS(void, obc_js_open_file, (const char *accept, int action), {
    var input = document.createElement('input');
    input.type = 'file';
    input.accept = UTF8ToString(accept);
    input.style.display = 'none';
    document.body.appendChild(input);

    var done = function() {
        if (input.parentNode) document.body.removeChild(input);
    };
    input.addEventListener('cancel', done);
    input.addEventListener('change', function() {
        var file = input.files && input.files[0];
        done();
        if (!file) return;

        var reader = new FileReader();
        reader.onload = function() {
            try { FS.mkdir('/uploads'); } catch (e) { /* already there */ }
            var path = '/uploads/' + file.name;
            FS.writeFile(path, new Uint8Array(reader.result));
            Module.ccall('obc_platform_file_ready', null, ['string', 'number'],
                         [path, action]);
        };
        reader.readAsArrayBuffer(file);
    });
    input.click();
});

EM_JS(void, obc_js_save_file, (const char *name, const char *mime,
                               const unsigned char *data, int length, int reuse), {
    var fname = UTF8ToString(name);
    /* slice, not subarray: the heap can move under an async write. */
    var blob = new Blob([HEAPU8.slice(data, data + length)], { type: UTF8ToString(mime) });

    (async function() {
        Module.obcSaveHandles = Module.obcSaveHandles || {};
        var handle = reuse ? Module.obcSaveHandles[fname] : null;

        if (!handle && window.showSaveFilePicker) {
            try {
                handle = await window.showSaveFilePicker({ suggestedName: fname });
            } catch (e) {
                return; // user cancelled
            }
            Module.obcSaveHandles[fname] = handle;
        }

        if (handle) {
            var writable = await handle.createWritable();
            await writable.write(blob);
            await writable.close();
            return;
        }

        var url = URL.createObjectURL(blob);
        var link = document.createElement('a');
        link.href = url;
        link.download = fname;
        document.body.appendChild(link);
        link.click();
        document.body.removeChild(link);
        setTimeout(function() { URL.revokeObjectURL(url); }, 1000);
    })();
});

void platform_init(void) {
    g_pending.ready = false;
}

void platform_frame_begin(App *app) {
    /* The canvas fills the page, so a window resize has to reach SDL. Comparing
     * against the last size applied rather than the drawable keeps this from
     * fighting the backend over rounding. */
    static int applied_w = 0;
    static int applied_h = 0;

    double css_w = 0.0, css_h = 0.0;
    if (emscripten_get_element_css_size("#canvas", &css_w, &css_h) != EMSCRIPTEN_RESULT_SUCCESS) {
        return;
    }

    int w = (int)css_w;
    int h = (int)css_h;
    if (w <= 0 || h <= 0) return;
    if (w == applied_w && h == applied_h) return;

    applied_w = w;
    applied_h = h;
    SDL_SetWindowSize(app->window, w, h);
}

bool platform_native_file_dialogs(void) {
    return true;
}

bool platform_can_quit(void) {
    return false;
}

static const char *accept_filter(int prompt) {
    switch (prompt) {
    case FILE_PROMPT_IMPORT_STL:
    case FILE_PROMPT_EXPORT:     return ".stl";
    case FILE_PROMPT_IMPORT_SVG: return ".svg";
    case FILE_PROMPT_SAVE_AS:    return ".obc";
    default:                     return ".obc";
    }
}

static std::string path_leaf(const std::string &path) {
    size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

/* Offers a file the editor has just written to MEMFS to the host. */
static void offer_saved_file(App *app, const std::string &vpath, const char *mime,
                             bool reuse_handle) {
    FILE *f = fopen(vpath.c_str(), "rb");
    if (!f) return; // the action failed and already said so in the status line

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return;
    }

    std::vector<unsigned char> bytes((size_t)size);
    size_t got = fread(&bytes[0], 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) return;

    std::string name = path_leaf(vpath);
    obc_js_save_file(name.c_str(), mime, &bytes[0], (int)size, reuse_handle ? 1 : 0);

    /* MEMFS is only a staging area; the host now holds the real copy. */
    remove(vpath.c_str());

    char msg[600];
    snprintf(msg, sizeof(msg), "Saved %s.", name.c_str());
    ui_set_status(app, msg, false);
}

void platform_prompt_path(App *app, int prompt) {
    /* The pending state lives here, not in UiState: a host dialog can be
     * dismissed without telling us, and a stuck file_action would block the
     * next prompt. */
    app->ui.file_action = FILE_PROMPT_NONE;

    if (prompt == FILE_PROMPT_SAVE || prompt == FILE_PROMPT_SAVE_AS ||
        prompt == FILE_PROMPT_EXPORT) {
        bool is_export = (prompt == FILE_PROMPT_EXPORT);
        std::string name;
        if (is_export) {
            name = "export.stl";
        } else {
            name = app->ui.project_path.empty() ? std::string("project.obc")
                                                : path_leaf(app->ui.project_path);
        }

        /* Written to the MEMFS root so the path the status line reports is just
         * the file name the user will see in the host dialog. */
        std::string vpath = "/" + name;
        ui_dispatch_path(app, prompt, vpath.c_str());

        /* Only a plain Save may reuse the handle from last time. Save As has
         * to ask, or it would silently overwrite the file the user is trying
         * to save away from, and an export asks too since its target is rarely
         * the same twice. */
        bool reuse = (prompt == FILE_PROMPT_SAVE);
        offer_saved_file(app, vpath, is_export ? "model/stl" : "application/octet-stream",
                         reuse);
        if (!is_export) app->ui.project_path = name;
        return;
    }

    obc_js_open_file(accept_filter(prompt), prompt);
}

void platform_poll_files(App *app) {
    if (!g_pending.ready) return;
    g_pending.ready = false;
    if (g_pending.path.empty()) return;

    int action = g_pending.action;
    std::string vpath = g_pending.path;
    ui_dispatch_path(app, action, vpath.c_str());

    /* /uploads is where the bytes were staged, not where the user thinks the
     * file lives, so it must not show up in the status line or come back as a
     * save suggestion. */
    /* ui_action_open only adopts the path when the load succeeded, so that is
     * also what says whether the status line may be rewritten - a failure has
     * its own message and must keep it. */
    if (action == FILE_PROMPT_OPEN && app->ui.project_path == vpath) {
        std::string name = path_leaf(vpath);
        app->ui.project_path = name;

        char msg[600];
        snprintf(msg, sizeof(msg), "Opened %s.", name.c_str());
        ui_set_status(app, msg, false);
    }

    /* The SVG options dialog reads the file only once a mode is picked, so that
     * one has to survive; everything else is fully read by now. */
    if (action != FILE_PROMPT_IMPORT_SVG) remove(vpath.c_str());
}

#else /* desktop: the in-app file browser handles all of this */

void platform_init(void) {}
void platform_frame_begin(App *app) { (void)app; }
bool platform_native_file_dialogs(void) { return false; }
bool platform_can_quit(void) { return true; }
void platform_prompt_path(App *app, int prompt) { (void)app; (void)prompt; }
void platform_poll_files(App *app) { (void)app; }

#endif
