#ifndef OBC_PLATFORM_H
#define OBC_PLATFORM_H

struct App;

/*
 * Everything that differs between the desktop build and the browser build,
 * collected so no other file needs an #ifdef. On the desktop each of these is
 * inert and the compiler drops the branch that calls it.
 */

void platform_init(void);

/* Browser: matches the drawable to the canvas when the page is resized.
 * Desktop: nothing, SDL already reports the window size. */
void platform_frame_begin(App *app);

/*
 * True where the host supplies its own file dialogs, which is the browser: a
 * page cannot walk the user's disk, and would not be allowed to if it could.
 * Everywhere else the in-app browser in ui_filebrowser.cpp is used.
 */
bool platform_native_file_dialogs(void);

/* False in a browser tab, where closing is the browser's job and quitting would
 * leave nothing but a frozen canvas. */
bool platform_can_quit(void);

/* Opens the host dialog for one of the FILE_PROMPT_* actions. Both are
 * asynchronous in the browser, so the result arrives via platform_poll_files. */
void platform_prompt_path(App *app, int prompt);
void platform_poll_files(App *app);

#endif
