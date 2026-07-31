#include "ui.h"

#include <algorithm>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>

#include "app.h"
#include "imgui.h"

/*
 * ImGui file browser, ported from the emu1806 debugger and extended for saving.
 *
 * Uses dirent and stat rather than std::filesystem: it is the same "C with
 * vectors" level as the rest of the codebase and needs no extra link flags.
 *
 * Open mode picks an existing file; save mode adds a name field, so the same
 * widget serves Open, Save As and Export.
 */

static bool has_suffix(const std::string &name, const std::string &suffix) {
    if (suffix.empty()) return true;
    if (name.size() < suffix.size()) return false;

    size_t offset = name.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        char a = name[offset + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static bool path_exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static std::string join_path(const std::string &dir, const std::string &name) {
    if (dir.empty() || dir == "/") return "/" + name;
    return dir + "/" + name;
}

/* Absolute where possible, so ".." navigation has something to walk up. */
static std::string absolute_path(const std::string &path) {
    char *resolved = realpath(path.c_str(), NULL);
    if (!resolved) return path;
    std::string out = resolved;
    free(resolved);
    return out;
}

static void browser_refresh(FileBrowser *fb) {
    fb->dirs.clear();
    fb->files.clear();

    DIR *d = opendir(fb->dir.c_str());
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        std::string name = ent->d_name;
        if (name == ".") continue;
        if (name == "..") continue; // offered explicitly, always first

        std::string full = join_path(fb->dir, name);
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            fb->dirs.push_back(name);
        } else if (has_suffix(name, fb->filter)) {
            /* Directories are never filtered: they have to stay walkable even
             * when nothing in them matches. */
            fb->files.push_back(name);
        }
    }
    closedir(d);

    std::sort(fb->dirs.begin(), fb->dirs.end());
    std::sort(fb->files.begin(), fb->files.end());
}

void ui_file_browser_init(FileBrowser *fb) {
    fb->open = false;
    fb->save_mode = false;
    fb->dir = absolute_path(".");
    fb->dirs.clear();
    fb->files.clear();
    fb->chosen.clear();
    fb->name_buf[0] = 0;
    fb->filter.clear();
    fb->title.clear();
}

void ui_file_browser_request(FileBrowser *fb, const char *title, const char *filter,
                             bool save_mode, const char *suggested_name) {
    fb->open = true;
    fb->save_mode = save_mode;
    fb->title = title ? title : "Choose a file";
    fb->filter = filter ? filter : "";
    fb->chosen.clear();
    snprintf(fb->name_buf, sizeof(fb->name_buf), "%s", suggested_name ? suggested_name : "");

    /* The directory is deliberately kept between uses, so a second save lands
     * where the first one did. */
    if (fb->dir.empty()) fb->dir = absolute_path(".");
    browser_refresh(fb);
}

bool ui_file_browser_draw(FileBrowser *fb, std::string *out_path) {
    if (fb->open) {
        ImGui::OpenPopup(fb->title.c_str());
        fb->open = false; // OpenPopup only needs calling once
    }

    bool picked = false;
    ImGui::SetNextWindowSize(ImVec2(560.0f, 430.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal(fb->title.c_str(), NULL, ImGuiWindowFlags_NoSavedSettings)) {
        return false;
    }

    ImGui::TextDisabled("%s", fb->dir.c_str());
    ImGui::Separator();

    /* Two rows are reserved at the bottom: the name or selection line and the
     * button row. */
    float reserved = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginChild("##listing", ImVec2(0.0f, -reserved), ImGuiChildFlags_Borders)) {
        if (ImGui::Selectable("../")) {
            size_t slash = fb->dir.find_last_of('/');
            fb->dir = (slash == std::string::npos || slash == 0) ? "/" : fb->dir.substr(0, slash);
            browser_refresh(fb);
        }

        for (size_t i = 0; i < fb->dirs.size(); ++i) {
            std::string label = fb->dirs[i] + "/";
            if (ImGui::Selectable(label.c_str())) {
                fb->dir = join_path(fb->dir, fb->dirs[i]);
                fb->chosen.clear();
                browser_refresh(fb);
                break; // the listing just changed under us
            }
        }

        for (size_t i = 0; i < fb->files.size(); ++i) {
            const std::string &name = fb->files[i];
            bool selected = (fb->chosen == name);
            if (ImGui::Selectable(name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                fb->chosen = name;
                /* Clicking an existing file in save mode fills the name in, so
                 * overwriting is a click rather than retyping. */
                snprintf(fb->name_buf, sizeof(fb->name_buf), "%s", name.c_str());
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    *out_path = join_path(fb->dir, name);
                    picked = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
    }
    ImGui::EndChild();

    bool commit = false;
    if (fb->save_mode) {
        ImGui::TextUnformatted("Name");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        commit = ImGui::InputText("##name", fb->name_buf, sizeof(fb->name_buf),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    } else {
        ImGui::TextDisabled("%s", fb->chosen.empty() ? "(no file selected)" : fb->chosen.c_str());
    }

    std::string target;
    if (fb->save_mode) {
        if (fb->name_buf[0] != 0) target = join_path(fb->dir, fb->name_buf);
    } else if (!fb->chosen.empty()) {
        target = join_path(fb->dir, fb->chosen);
    }

    bool exists = !target.empty() && path_exists(target);
    const char *action = "Open";
    if (fb->save_mode) action = exists ? "Overwrite" : "Save";

    if (target.empty()) ImGui::BeginDisabled();
    bool action_clicked = ImGui::Button(action);
    if (target.empty()) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return false;
    }

    /* Saying so beats a silent replacement, and there is no undo for a file. */
    if (fb->save_mode && exists) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.72f, 0.45f, 0.10f, 1.0f), "file exists");
    }
    if (!fb->save_mode && !target.empty() && !exists) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.72f, 0.15f, 0.15f, 1.0f), "missing");
    }

    if ((action_clicked || commit) && !target.empty()) {
        *out_path = target;
        picked = true;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
    return picked;
}
