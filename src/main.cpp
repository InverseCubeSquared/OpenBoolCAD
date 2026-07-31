#include "app.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    App app;
    if (!app_init(&app)) {
        app_shutdown(&app);
        return 1;
    }
    app_run(&app);
    app_shutdown(&app);
    return 0;
}
