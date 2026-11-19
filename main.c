#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <sys/wait.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <gdk/gdkkeysyms.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>

typedef enum {
    VIEW_MODE_LIST,
    VIEW_MODE_ICONS
} ViewMode;

typedef enum {
    SORT_NAME_ASC,
    SORT_NAME_DESC,
    SORT_SIZE,
    SORT_TIME
} SortMode;

enum {
    COL_PIXBUF,
    COL_NAME,
    COL_DISPLAY_NAME,
    COL_SIZE,
    COL_IS_DIR,
    COL_FULL_PATH,
    NUM_COLS
};

typedef enum {
    ACT_OPEN,
    ACT_NEW_FILE,
    ACT_NEW_DIR,
    ACT_RENAME,
    ACT_TRASH,
    ACT_COPY,
    ACT_CUT,
    ACT_PASTE,
    ACT_UNDO,
    ACT_TERM,
    ACT_UP,
    ACT_HOME,
    ACT_BACK,
    ACT_FORWARD,
    ACT_REFRESH,
    ACT_HIDDEN,
    ACT_FOCUS_PATH,
    ACT_SETTINGS,
    ACT_CUSTOM_MENU_CFG,
    ACT_VIEW_LIST,
    ACT_VIEW_ICONS,
    ACT_SORT_NAME,
    ACT_SORT_SIZE,
    ACT_SORT_TIME,
    ACT_PIN_CURRENT,
    ACT_COUNT
} ActionId;

typedef struct {
    const char *id;
    const char *label;
    guint keyval;
    GdkModifierType mods;
} Keybind;

typedef enum {
    UNDO_TRASH,
    UNDO_MOVE,
    UNDO_COPY,
    UNDO_RENAME,
    UNDO_CREATE,
    UNDO_RESTORE
} UndoType;

typedef struct {
    UndoType type;
    char *src;
    char *dst;
} UndoOp;

typedef struct {
    char *name;
    char *command;
} CustomAction;

typedef struct {
    char *full_path;
    int icon_size;
    guint gen_id;
} ThumbnailTask;

typedef struct {
    GtkWidget *window;
    GtkWidget *path_entry;
    GtkWidget *content_stack;
    GtkWidget *tree_view;
    GtkWidget *icon_view;
    GtkListStore *list_store;
    GtkWidget *bookmarks_box;
    GtkWidget *status_label;

    char *current_dir;
    gboolean show_hidden;
    char *terminal_cmd;
    ViewMode view_mode;
    SortMode sort_mode;
    int icon_size;

    GList *history_back;
    GList *history_fwd;
    GList *clipboard_paths;
    gboolean clipboard_is_cut;
    GList *undo_stack;
    GList *custom_actions;

    char search_query[256];

    Keybind binds[ACT_COUNT];
    char *config_path;
    GThreadPool *thread_pool;
    guint generation_id;

    GFileMonitor *dir_monitor;
    pthread_mutex_t state_mutex;
} AppState;

static AppState app = {0};

static void set_status(const char *fmt, ...);
static void set_status_markup(const char *fmt, ...);
static void free_string_list(GList **list);
static char *format_size(goffset size, gboolean is_dir);
static GList *get_selected_paths(void);
static void refresh_dir(void);
static void navigate_to(const char *path, gboolean record_history);
static void execute_action(ActionId act);
static void open_settings_dialog(void);
static void open_custom_actions_dialog(void);
static void load_bookmarks(void);
static void switch_view_mode(ViewMode mode);
static void update_status_bar_info(void);
static void save_config(void);
static void open_currently_selected(void);
static void on_menu_properties(GtkMenuItem *item, gpointer data);
static void unpin_path_from_bookmarks(const char *path);
static gboolean is_path_in_gtk_bookmarks(const char *path);
static void add_sidebar_item(const char *label, const char *path, const char *icon_name, gboolean is_trash, gboolean is_mount, GMount *mount);
static GdkPixbuf *get_fast_file_icon(gboolean is_dir, const char *name, int size);
static void push_undo(UndoType type, const char *src, const char *dst);
static void on_view_drag_data_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y, GtkSelectionData *selection_data, guint info, guint time, gpointer user_data);
static gboolean handle_context_menu(GdkEventButton *ev, gboolean is_row);
static void go_back(void);
static void go_forward(void);

typedef struct {
    GtkWidget *dialog;
    GtkWidget *progress_bar;
    GPid pid;
    guint timeout_id;
    char *success_message;
} ProgressCtx;

static gboolean on_progress_tick(gpointer user_data) {
    ProgressCtx *ctx = user_data;
    if (!ctx || !ctx->progress_bar) return G_SOURCE_REMOVE;
    
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(ctx->progress_bar));

    int status = 0;
    pid_t result = waitpid(ctx->pid, &status, WNOHANG);
    if (result != 0) {
        if (ctx->dialog && GTK_IS_WIDGET(ctx->dialog)) {
            gtk_widget_destroy(ctx->dialog);
        }
        g_spawn_close_pid(ctx->pid);
        refresh_dir();
        if (ctx->success_message) {
            set_status("%s", ctx->success_message);
            g_free(ctx->success_message);
        }
        g_free(ctx);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void run_command_with_progress(const char *title, const char *cmd, const char *success_msg) {
    GError *err = NULL;
    gchar **argv = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &argv, &err)) {
        set_status("Command parse error: %s", err ? err->message : "");
        if (err) g_error_free(err);
        return;
    }

    GPid pid = 0;
    if (!g_spawn_async(app.current_dir, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH, NULL, NULL, &pid, &err)) {
        set_status("Spawn error: %s", err ? err->message : "");
        if (err) g_error_free(err);
        g_strfreev(argv);
        return;
    }
    g_strfreev(argv);

    GtkWidget *dlg = gtk_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(app.window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 350, 100);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 16);
    gtk_box_set_spacing(GTK_BOX(content), 12);

    GtkWidget *lbl = gtk_label_new("Processing, please wait...");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    GtkWidget *pbar = gtk_progress_bar_new();

    gtk_box_pack_start(GTK_BOX(content), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), pbar, FALSE, FALSE, 0);
    gtk_widget_show_all(dlg);

    ProgressCtx *ctx = g_new0(ProgressCtx, 1);
    ctx->dialog = dlg;
    ctx->progress_bar = pbar;
    ctx->pid = pid;
    ctx->success_message = success_msg ? g_strdup(success_msg) : NULL;
    ctx->timeout_id = g_timeout_add(100, on_progress_tick, ctx);
}

static char *format_size(goffset size, gboolean is_dir) {
    if (is_dir) return g_strdup("<DIR>");
    if (size < 1024) return g_strdup_printf("%ld B", (long)size);
    if (size < 1024 * 1024) return g_strdup_printf("%.1f K", size / 1024.0);
    if (size < 1024 * 1024 * 1024) return g_strdup_printf("%.1f M", size / (1024.0 * 1024.0));
    return g_strdup_printf("%.1f G", size / (1024.0 * 1024.0 * 1024.0));
}

static char *get_free_space_string(const char *path) {
    struct statvfs stat;
    if (statvfs(path ? path : "/", &stat) == 0) {
        guint64 free_space = (guint64)stat.f_bavail * (guint64)stat.f_frsize;
        return format_size(free_space, FALSE);
    }
    return NULL;
}

static GList *get_selected_paths(void) {
    GList *paths = NULL;
    pthread_mutex_lock(&app.state_mutex);
    
    if (app.view_mode == VIEW_MODE_LIST) {
        if (!app.tree_view || !GTK_IS_TREE_VIEW(app.tree_view) || !gtk_widget_get_realized(app.tree_view)) {
            pthread_mutex_unlock(&app.state_mutex);
            return NULL;
        }
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.tree_view));
        if (!sel) {
            pthread_mutex_unlock(&app.state_mutex);
            return NULL;
        }
        
        GtkTreeModel *model = NULL;
        GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
        if (!rows || !model || !GTK_IS_TREE_MODEL(model)) {
            if (rows) g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
            pthread_mutex_unlock(&app.state_mutex);
            return NULL;
        }
        for (GList *l = rows; l != NULL; l = l->next) {
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter(model, &iter, l->data)) {
                char *full_path = NULL;
                gtk_tree_model_get(model, &iter, COL_FULL_PATH, &full_path, -1);
                if (full_path) paths = g_list_prepend(paths, full_path);
            }
        }
        g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    } else {
        if (!app.icon_view || !GTK_IS_ICON_VIEW(app.icon_view) || !gtk_widget_get_realized(app.icon_view)) {
            pthread_mutex_unlock(&app.state_mutex);
            return NULL;
        }
        GtkIconView *iv = GTK_ICON_VIEW(app.icon_view);
        if (!gtk_icon_view_get_model(iv)) {
            pthread_mutex_unlock(&app.state_mutex);
            return NULL;
        }
        GList *items = gtk_icon_view_get_selected_items(iv);
        if (!items) {
            pthread_mutex_unlock(&app.state_mutex);
            return NULL;
        }

        GtkTreeModel *model = gtk_icon_view_get_model(iv);
        if (!model || !GTK_IS_TREE_MODEL(model)) {
            g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
            pthread_mutex_unlock(&app.state_mutex);
            return NULL;
        }

        for (GList *l = items; l != NULL; l = l->next) {
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter(model, &iter, l->data)) {
                char *full_path = NULL;
                gtk_tree_model_get(model, &iter, COL_FULL_PATH, &full_path, -1);
                if (full_path) paths = g_list_prepend(paths, full_path);
            }
        }
        g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
    }
    pthread_mutex_unlock(&app.state_mutex);
    return g_list_reverse(paths);
}

static gboolean clear_selection_if_any(void) {
    GtkTreeSelection *tree_sel = NULL;
    GtkIconView *iv = NULL;
    gboolean had_selection = FALSE;

    pthread_mutex_lock(&app.state_mutex);
    if (app.view_mode == VIEW_MODE_LIST) {
        if (app.tree_view && GTK_IS_TREE_VIEW(app.tree_view)) {
            tree_sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.tree_view));
            if (tree_sel && gtk_tree_selection_count_selected_rows(tree_sel) > 0) {
                had_selection = TRUE;
            } else {
                tree_sel = NULL;
            }
        }
    } else {
        if (app.icon_view && GTK_IS_ICON_VIEW(app.icon_view)) {
            iv = GTK_ICON_VIEW(app.icon_view);
            GList *items = gtk_icon_view_get_selected_items(iv);
            if (items) {
                had_selection = TRUE;
                g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
            } else {
                iv = NULL;
            }
        }
    }
    pthread_mutex_unlock(&app.state_mutex);

    if (!had_selection) {
        return FALSE;
    }

    if (tree_sel) {
        gtk_tree_selection_unselect_all(tree_sel);
    } else if (iv) {
        gtk_icon_view_unselect_all(iv);
    }

    update_status_bar_info();
    return TRUE;
}

static void update_status_bar_info(void) {
    if (!app.window || !gtk_widget_get_realized(app.window)) return;
    if (app.search_query[0] != '\0') {
        set_status_markup("Search: <span foreground=\"#D35856\" font_weight=\"bold\"><b>%s</b></span>", app.search_query);
        return;
    }

    GList *sel = get_selected_paths();
    guint count = g_list_length(sel);
    char *free_space = get_free_space_string(app.current_dir);
    int items_count = app.list_store && GTK_IS_TREE_MODEL(app.list_store) ? gtk_tree_model_iter_n_children(GTK_TREE_MODEL(app.list_store), NULL) : 0;

    if (count == 0) {
        set_status("Free space: %s | Items: %d", free_space ? free_space : "N/A", items_count);
    } else if (count == 1) {
        char *path = sel->data;
        GFile *f = g_file_new_for_path(path);
        GFileInfo *fi = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                          G_FILE_QUERY_INFO_NONE, NULL, NULL);
        if (fi) {
            goffset sz = g_file_info_get_size(fi);
            gboolean is_dir = (g_file_info_get_file_type(fi) == G_FILE_TYPE_DIRECTORY);
            char *sz_str = format_size(sz, is_dir);
            char *base = g_path_get_basename(path);
            set_status("Selected: %s (%s) | Free space: %s | Items: %d", base ? base : "", sz_str, free_space ? free_space : "N/A", items_count);
            g_free(base);
            g_free(sz_str);
            g_object_unref(fi);
        }
        g_object_unref(f);
    } else {
        goffset total_size = 0;
        for (GList *l = sel; l != NULL; l = l->next) {
            char *p = l->data;
            GFile *f = g_file_new_for_path(p);
            GFileInfo *fi = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (fi) {
                total_size += g_file_info_get_size(fi);
                g_object_unref(fi);
            }
            g_object_unref(f);
        }
        char *sz_str = format_size(total_size, FALSE);
        set_status("Selected items: %u (Total size: %s) | Free space: %s | Items: %d", count, sz_str, free_space ? free_space : "N/A", items_count);
        g_free(sz_str);
    }

    g_free(free_space);
    free_string_list(&sel);
}

static void open_currently_selected(void) {
    GList *sel = get_selected_paths();
    if (!sel || !sel->data) {
        free_string_list(&sel);
        return;
    }
    const char *path = sel->data;
    if (g_str_has_prefix(app.current_dir, "trash:///")) {
        set_status("Operation not supported directly from trash view");
        free_string_list(&sel);
        return;
    }

    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        navigate_to(path, TRUE);
    } else {
        char *uri = g_filename_to_uri(path, NULL, NULL);
        if (uri) {
            g_app_info_launch_default_for_uri(uri, NULL, NULL);
            g_free(uri);
        }
    }
    free_string_list(&sel);
}

static void set_status(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (app.status_label && GTK_IS_LABEL(app.status_label)) {
        gtk_label_set_text(GTK_LABEL(app.status_label), buf);
    }
}

static void set_status_markup(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (app.status_label && GTK_IS_LABEL(app.status_label)) {
        gtk_label_set_markup(GTK_LABEL(app.status_label), buf);
    }
}

static void free_string_list(GList **list) {
    if (!list || !*list) return;
    g_list_free_full(*list, g_free);
    *list = NULL;
}

static GdkPixbuf *load_safe_icon(const char *icon_name, int size) {
    if (!icon_name || !*icon_name) return NULL;
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    if (!theme) return NULL;

    GError *error = NULL;
    GdkPixbuf *pix = gtk_icon_theme_load_icon(theme, icon_name, size, GTK_ICON_LOOKUP_FORCE_SIZE, &error);
    if (error) {
        g_error_free(error);
        pix = NULL;
    }
    return pix;
}

static GdkPixbuf *get_fast_file_icon(gboolean is_dir, const char *name, int size) {
    GdkPixbuf *pix = NULL;
    if (is_dir) {
        pix = load_safe_icon("folder", size);
        if (!pix) pix = load_safe_icon("inode-directory", size);
    } else {
        char *full_path = app.current_dir ? g_build_filename(app.current_dir, name ? name : "", NULL) : NULL;
        if (full_path) {
            GFile *file = g_file_new_for_path(full_path);
            GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_ICON "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                              G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (info) {
                GIcon *icon = g_file_info_get_icon(info);
                if (icon && G_IS_THEMED_ICON(icon)) {
                    const gchar *const *icon_names = g_themed_icon_get_names(G_THEMED_ICON(icon));
                    if (icon_names) {
                        for (int i = 0; icon_names[i] != NULL; i++) {
                            pix = load_safe_icon(icon_names[i], size);
                            if (pix) break;
                        }
                    }
                }
                g_object_unref(info);
            }
            g_object_unref(file);
            g_free(full_path);
        }

        if (!pix) {
            const char *ext = name ? strrchr(name, '.') : NULL;
            const char *icn = "text-x-generic";
            if (g_ascii_strcasecmp(name, "Makefile") == 0 || g_ascii_strcasecmp(name, "CMakeLists.txt") == 0) {
                icn = "text-x-script";
            } else if (ext) {
                if (g_ascii_strcasecmp(ext, ".png") == 0 || g_ascii_strcasecmp(ext, ".jpg") == 0 || 
                    g_ascii_strcasecmp(ext, ".jpeg") == 0 || g_ascii_strcasecmp(ext, ".gif") == 0 || 
                    g_ascii_strcasecmp(ext, ".webp") == 0 || g_ascii_strcasecmp(ext, ".bmp") == 0) {
                    icn = "image-x-generic";
                } else if (g_ascii_strcasecmp(ext, ".mp3") == 0 || g_ascii_strcasecmp(ext, ".flac") == 0 || g_ascii_strcasecmp(ext, ".wav") == 0) {
                    icn = "audio-x-generic";
                } else if (g_ascii_strcasecmp(ext, ".mp4") == 0 || g_ascii_strcasecmp(ext, ".mkv") == 0 || g_ascii_strcasecmp(ext, ".avi") == 0) {
                    icn = "video-x-generic";
                } else if (g_ascii_strcasecmp(ext, ".zip") == 0 || g_ascii_strcasecmp(ext, ".tar") == 0 || g_ascii_strcasecmp(ext, ".gz") == 0 || g_ascii_strcasecmp(ext, ".xz") == 0 || g_ascii_strcasecmp(ext, ".7z") == 0 || g_ascii_strcasecmp(ext, ".rar") == 0) {
                    icn = "package-x-generic";
                } else if (g_ascii_strcasecmp(ext, ".c") == 0 || g_ascii_strcasecmp(ext, ".cpp") == 0 || g_ascii_strcasecmp(ext, ".h") == 0) {
                    icn = "text-x-csrc";
                } else if (g_ascii_strcasecmp(ext, ".py") == 0 || g_ascii_strcasecmp(ext, ".sh") == 0) {
                    icn = "text-x-script";
                } else if (g_ascii_strcasecmp(ext, ".pdf") == 0) {
                    icn = "x-office-document";
                }
            }
            pix = load_safe_icon(icn, size);
        }
    }
    if (!pix) pix = load_safe_icon("text-x-generic", size);
    return pix;
}

static GdkPixbuf *get_heavy_thumbnail(const char *full_path, int size) {
    if (!full_path || !g_file_test(full_path, G_FILE_TEST_EXISTS)) return NULL;
    GdkPixbuf *pix = NULL;
    char *uri = g_filename_to_uri(full_path, NULL, NULL);
    if (!uri) return NULL;

    GChecksum *checksum = g_checksum_new(G_CHECKSUM_MD5);
    if (!checksum) {
        g_free(uri);
        return NULL;
    }

    g_checksum_update(checksum, (const guchar *)uri, strlen(uri));
    const char *hash = g_checksum_get_string(checksum);
    if (!hash) {
        g_checksum_free(checksum);
        g_free(uri);
        return NULL;
    }

    char *hash_filename = g_strconcat(hash, ".png", NULL);
    char *thumb_path_normal = g_build_filename(g_get_user_cache_dir(), "thumbnails", "normal", hash_filename, NULL);
    char *thumb_path_large = g_build_filename(g_get_user_cache_dir(), "thumbnails", "large", hash_filename, NULL);
    
    if (thumb_path_large && g_file_test(thumb_path_large, G_FILE_TEST_EXISTS) && size > 128) {
        pix = gdk_pixbuf_new_from_file_at_scale(thumb_path_large, size, size, TRUE, NULL);
    }
    if (!pix && thumb_path_normal && g_file_test(thumb_path_normal, G_FILE_TEST_EXISTS)) {
        pix = gdk_pixbuf_new_from_file_at_scale(thumb_path_normal, size, size, TRUE, NULL);
    }

    if (!pix) {
        const char *ext = strrchr(full_path, '.');
        gboolean is_mp3 = ext && (g_ascii_strcasecmp(ext, ".mp3") == 0 || g_ascii_strcasecmp(ext, ".flac") == 0);
        gboolean is_video = ext && (g_ascii_strcasecmp(ext, ".mp4") == 0 || 
                                   g_ascii_strcasecmp(ext, ".mkv") == 0 || 
                                   g_ascii_strcasecmp(ext, ".avi") == 0 || 
                                   g_ascii_strcasecmp(ext, ".webm") == 0 || 
                                   g_ascii_strcasecmp(ext, ".mov") == 0);

        char *tmp_img = g_strdup_printf("/tmp/fm_cover_%u.jpg", g_random_int());
        char *cmd = NULL;

        if (is_mp3) {
            cmd = g_strdup_printf("ffmpeg -y -i %s -an -vcodec copy %s 2>/dev/null", 
                                        g_shell_quote(full_path), 
                                        g_shell_quote(tmp_img));
        } else if (is_video) {
            cmd = g_strdup_printf("ffmpeg -y -ss 00:00:02 -i %s -vframes 1 %s 2>/dev/null", 
                                        g_shell_quote(full_path), 
                                        g_shell_quote(tmp_img));
        }

        if (cmd) {
            if (system(cmd) == 0 && tmp_img && g_file_test(tmp_img, G_FILE_TEST_EXISTS)) {
                pix = gdk_pixbuf_new_from_file_at_scale(tmp_img, size, size, TRUE, NULL);
            }
            if (tmp_img) {
                unlink(tmp_img);
                g_free(tmp_img);
            }
            g_free(cmd);
        } else {
            g_free(tmp_img);
        }

        if (!pix) {
            pix = gdk_pixbuf_new_from_file_at_scale(full_path, size, size, TRUE, NULL);
        }
        
        if (pix && thumb_path_normal) {
            char *normal_dir = g_build_filename(g_get_user_cache_dir(), "thumbnails", "normal", NULL);
            if (normal_dir) {
                g_mkdir_with_parents(normal_dir, 0755);
                gdk_pixbuf_save(pix, thumb_path_normal, "png", NULL, NULL);
                g_free(normal_dir);
            }
        }
    }

    if (thumb_path_normal) g_free(thumb_path_normal);
    if (thumb_path_large) g_free(thumb_path_large);
    if (hash_filename) g_free(hash_filename);
    if (checksum) g_checksum_free(checksum);
    if (uri) g_free(uri);

    return pix;
}

typedef struct {
    char *full_path;
    GdkPixbuf *pixbuf;
    guint gen_id;
} ThumbnailResult;

static gboolean apply_thumbnail_in_main(gpointer data) {
    ThumbnailResult *res = data;
    pthread_mutex_lock(&app.state_mutex);
    if (res->gen_id == app.generation_id && app.list_store && GTK_IS_LIST_STORE(app.list_store)) {
        GtkTreeModel *model = GTK_TREE_MODEL(app.list_store);
        GtkTreeIter iter;
        gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
        while (valid) {
            char *path = NULL;
            gtk_tree_model_get(model, &iter, COL_FULL_PATH, &path, -1);
            if (path && g_strcmp0(path, res->full_path) == 0) {
                gtk_list_store_set(app.list_store, &iter, COL_PIXBUF, res->pixbuf, -1);
                g_free(path);
                break;
            }
            g_free(path);
            valid = gtk_tree_model_iter_next(model, &iter);
        }
    }
    pthread_mutex_unlock(&app.state_mutex);

    if (res->pixbuf) g_object_unref(res->pixbuf);
    g_free(res->full_path);
    g_free(res);
    return G_SOURCE_REMOVE;
}

static void thumbnail_worker_thread(gpointer data, gpointer user_data) {
    (void)user_data;
    ThumbnailTask *task = data;
    if (!task) return;

    pthread_mutex_lock(&app.state_mutex);
    guint current_gen = app.generation_id;
    pthread_mutex_unlock(&app.state_mutex);

    if (task->gen_id == current_gen) {
        GdkPixbuf *pix = get_heavy_thumbnail(task->full_path, task->icon_size);
        
        pthread_mutex_lock(&app.state_mutex);
        guint latest_gen = app.generation_id;
        pthread_mutex_unlock(&app.state_mutex);

        if (task->gen_id == latest_gen && pix) {
            ThumbnailResult *res = g_new0(ThumbnailResult, 1);
            res->full_path = g_strdup(task->full_path);
            res->pixbuf = pix;
            res->gen_id = task->gen_id;
            g_idle_add(apply_thumbnail_in_main, res);
        } else {
            if (pix) g_object_unref(pix);
        }
    }
    g_free(task->full_path);
    g_free(task);
}

static void init_default_binds(void) {
    app.binds[ACT_OPEN]           = (Keybind){"open",           "Open File/Folder",GDK_KEY_Return,    0};
    app.binds[ACT_NEW_FILE]       = (Keybind){"new_file",       "New File",        GDK_KEY_n,         GDK_CONTROL_MASK};
    app.binds[ACT_NEW_DIR]        = (Keybind){"new_dir",        "New Directory",   GDK_KEY_n,         GDK_CONTROL_MASK | GDK_SHIFT_MASK};
    app.binds[ACT_RENAME]         = (Keybind){"rename",         "Rename",          GDK_KEY_3,         GDK_CONTROL_MASK};
    app.binds[ACT_TRASH]          = (Keybind){"trash",          "Move to Trash",   GDK_KEY_Delete,    0};
    app.binds[ACT_COPY]           = (Keybind){"copy",           "Copy",            GDK_KEY_c,         GDK_CONTROL_MASK};
    app.binds[ACT_CUT]            = (Keybind){"cut",            "Cut",             GDK_KEY_x,         GDK_CONTROL_MASK};
    app.binds[ACT_PASTE]          = (Keybind){"paste",          "Paste",           GDK_KEY_v,         GDK_CONTROL_MASK};
    app.binds[ACT_UNDO]           = (Keybind){"undo",           "Undo",            GDK_KEY_z,         GDK_CONTROL_MASK};
    app.binds[ACT_TERM]           = (Keybind){"terminal",       "Open Terminal",   GDK_KEY_t,         GDK_CONTROL_MASK};
    app.binds[ACT_UP]             = (Keybind){"up",             "Parent Directory",GDK_KEY_BackSpace, 0};
    app.binds[ACT_HOME]           = (Keybind){"home",           "Home Directory",  GDK_KEY_Tab,       GDK_CONTROL_MASK};
    app.binds[ACT_BACK]           = (Keybind){"back",           "History Back",    GDK_KEY_Left,      GDK_MOD1_MASK};
    app.binds[ACT_FORWARD]        = (Keybind){"forward",        "History Forward", GDK_KEY_Right,     GDK_MOD1_MASK};
    app.binds[ACT_REFRESH]        = (Keybind){"refresh",        "Refresh",         GDK_KEY_r,         GDK_CONTROL_MASK};
    app.binds[ACT_HIDDEN]         = (Keybind){"hidden",         "Toggle Hidden",   GDK_KEY_h,         GDK_CONTROL_MASK};
    app.binds[ACT_FOCUS_PATH]     = (Keybind){"focus_path",     "Focus Path Bar",  GDK_KEY_l,         GDK_CONTROL_MASK};
    app.binds[ACT_PIN_CURRENT]    = (Keybind){"pin_current",    "Pin Current Dir", GDK_KEY_d,         GDK_CONTROL_MASK};
    app.binds[ACT_SETTINGS]       = (Keybind){"settings",       "Preferences",     GDK_KEY_comma,     GDK_CONTROL_MASK};
    app.binds[ACT_CUSTOM_MENU_CFG]= (Keybind){"custom_menu",    "Custom Actions",  GDK_KEY_comma,     GDK_MOD1_MASK};
    app.binds[ACT_VIEW_LIST]      = (Keybind){"view_list",      "List View",       GDK_KEY_1,         GDK_CONTROL_MASK};
    app.binds[ACT_VIEW_ICONS]     = (Keybind){"view_icons",     "Icon View",       GDK_KEY_2,         GDK_CONTROL_MASK};
}

static void save_config(void) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf,  "General", "terminal", app.terminal_cmd ? app.terminal_cmd : "l-term");
    g_key_file_set_boolean(kf, "General", "show_hidden", app.show_hidden);
    g_key_file_set_integer(kf, "General", "view_mode", (int)app.view_mode);
    g_key_file_set_integer(kf, "General", "sort_mode", (int)app.sort_mode);
    g_key_file_set_integer(kf, "General", "icon_size", app.icon_size);

    for (int i = 0; i < ACT_COUNT; i++) {
        if (!app.binds[i].id) continue;
        char *accel = gtk_accelerator_name(app.binds[i].keyval, app.binds[i].mods);
        if (accel) {
            g_key_file_set_string(kf, "Keybinds", app.binds[i].id, accel);
            g_free(accel);
        }
    }

    g_key_file_remove_group(kf, "CustomActions", NULL);
    int idx = 0;
    for (GList *l = app.custom_actions; l != NULL; l = l->next, idx++) {
        CustomAction *ca = l->data;
        char key_name[32];
        snprintf(key_name, sizeof(key_name), "action_%d", idx);
        char *val = g_strdup_printf("%s|%s", ca->name, ca->command);
        g_key_file_set_string(kf, "CustomActions", key_name, val);
        g_free(val);
    }

    char *dir = g_path_get_dirname(app.config_path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    g_key_file_save_to_file(kf, app.config_path, NULL);
    g_key_file_free(kf);
}

static void load_config(void) {
    init_default_binds();
    const char *conf_dir = g_get_user_config_dir();
    app.config_path = g_build_filename(conf_dir, "fm", "config.ini", NULL);

    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, app.config_path, G_KEY_FILE_NONE, NULL)) {
        app.terminal_cmd = g_strdup("l-term");
        app.show_hidden = FALSE;
        app.view_mode = VIEW_MODE_ICONS;
        app.sort_mode = SORT_NAME_ASC;
        app.icon_size = 80;
        save_config();
        g_key_file_free(kf);
        return;
    }

    char *term = g_key_file_get_string(kf, "General", "terminal", NULL);
    app.terminal_cmd = term ? term : g_strdup("l-term");
    app.show_hidden = g_key_file_get_boolean(kf, "General", "show_hidden", NULL);
    app.view_mode = (ViewMode)g_key_file_get_integer(kf, "General", "view_mode", NULL);
    app.sort_mode = (SortMode)g_key_file_get_integer(kf, "General", "sort_mode", NULL);
    app.icon_size = g_key_file_get_integer(kf, "General", "icon_size", NULL);
    if (app.icon_size < 32) app.icon_size = 80;

    for (int i = 0; i < ACT_COUNT; i++) {
        if (!app.binds[i].id) continue;
        char *accel = g_key_file_get_string(kf, "Keybinds", app.binds[i].id, NULL);
        if (accel) {
            guint k = 0;
            GdkModifierType m = 0;
            gtk_accelerator_parse(accel, &k, &m);
            if (k != 0) {
                app.binds[i].keyval = k;
                app.binds[i].mods = m;
            }
            g_free(accel);
        }
    }

    gsize n_keys = 0;
    char **keys = g_key_file_get_keys(kf, "CustomActions", &n_keys, NULL);
    if (keys) {
        for (gsize i = 0; i < n_keys; i++) {
            char *val = g_key_file_get_string(kf, "CustomActions", keys[i], NULL);
            if (val) {
                char **parts = g_strsplit(val, "|", 2);
                if (parts[0] && parts[1]) {
                    CustomAction *ca = g_new0(CustomAction, 1);
                    ca->name = g_strdup(parts[0]);
                    ca->command = g_strdup(parts[1]);
                    app.custom_actions = g_list_append(app.custom_actions, ca);
                }
                g_strfreev(parts);
                g_free(val);
            }
        }
        g_strfreev(keys);
    }

    g_key_file_free(kf);
}

static gboolean delete_recursive(GFile *file, GError **error) {
    GFileType type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);
    if (type == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator *en = g_file_enumerate_children(file, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
        if (!en) return FALSE;
        GFileInfo *info;
        while ((info = g_file_enumerator_next_file(en, NULL, error)) != NULL) {
            GFile *child = g_file_get_child(file, g_file_info_get_name(info));
            gboolean ok = delete_recursive(child, error);
            g_object_unref(child);
            g_object_unref(info);
            if (!ok) {
                g_object_unref(en);
                return FALSE;
            }
        }
        g_object_unref(en);
    }
    return g_file_delete(file, NULL, error);
}

static gboolean copy_recursive(GFile *src, GFile *dst, GFileCopyFlags flags, GError **error) {
    GFileType type = g_file_query_file_type(src, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);

    if (type == G_FILE_TYPE_DIRECTORY) {
        GError *dir_err = NULL;
        if (!g_file_make_directory_with_parents(dst, NULL, &dir_err)) {
            if (!g_error_matches(dir_err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
                g_propagate_error(error, dir_err);
                return FALSE;
            }
            g_clear_error(&dir_err);
        }

        GFileEnumerator *en = g_file_enumerate_children(
            src,
            G_FILE_ATTRIBUTE_STANDARD_NAME,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
            NULL,
            error
        );
        if (!en) return FALSE;

        GFileInfo *info;
        while ((info = g_file_enumerator_next_file(en, NULL, error)) != NULL) {
            const char *name = g_file_info_get_name(info);
            GFile *child_src = g_file_get_child(src, name);
            GFile *child_dst = g_file_get_child(dst, name);

            gboolean ok = copy_recursive(child_src, child_dst, flags, error);

            g_object_unref(child_src);
            g_object_unref(child_dst);
            g_object_unref(info);

            if (!ok) {
                g_object_unref(en);
                return FALSE;
            }
        }

        g_object_unref(en);
        return TRUE;
    }

    return g_file_copy(src, dst, flags, NULL, NULL, NULL, error);
}

static char *find_latest_in_trash(const char *orig_path) {
    char *trash_info_dir = g_build_filename(g_get_user_data_dir(), "Trash", "info", NULL);
    char *trash_files_dir = g_build_filename(g_get_user_data_dir(), "Trash", "files", NULL);
    GDir *dir = g_dir_open(trash_info_dir, 0, NULL);
    if (!dir) {
        g_free(trash_info_dir);
        g_free(trash_files_dir);
        return NULL;
    }

    char *best_file = NULL;
    time_t best_mtime = 0;
    const char *name;

    while ((name = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(name, ".trashinfo")) continue;
        char *info_path = g_build_filename(trash_info_dir, name, NULL);
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, info_path, G_KEY_FILE_NONE, NULL)) {
            char *raw_path = g_key_file_get_string(kf, "Trash Info", "Path", NULL);
            if (raw_path) {
                char *unescaped = g_uri_unescape_string(raw_path, NULL);
                if (unescaped && g_strcmp0(unescaped, orig_path) == 0) {
                    struct stat st;
                    if (stat(info_path, &st) == 0 && st.st_mtime >= best_mtime) {
                        best_mtime = st.st_mtime;
                        g_free(best_file);
                        char *base = g_strndup(name, strlen(name) - strlen(".trashinfo"));
                        best_file = g_build_filename(trash_files_dir, base, NULL);
                        g_free(base);
                    }
                }
                g_free(unescaped);
                g_free(raw_path);
            }
        }
        g_key_file_free(kf);
        g_free(info_path);
    }

    g_dir_close(dir);
    g_free(trash_info_dir);
    g_free(trash_files_dir);
    return best_file;
}

static void push_undo(UndoType type, const char *src, const char *dst) {
    UndoOp *op = g_new0(UndoOp, 1);
    op->type = type;
    op->src = g_strdup(src);
    op->dst = g_strdup(dst);
    app.undo_stack = g_list_prepend(app.undo_stack, op);
}

static void free_undo_op(UndoOp *op) {
    if (!op) return;
    g_free(op->src);
    g_free(op->dst);
    g_free(op);
}

static int compare_file_info(gconstpointer a, gconstpointer b, gpointer user_data) {
    (void)user_data;
    GFileInfo *fa = (GFileInfo *)a;
    GFileInfo *fb = (GFileInfo *)b;

    gboolean a_is_dir = (g_file_info_get_file_type(fa) == G_FILE_TYPE_DIRECTORY);
    gboolean b_is_dir = (g_file_info_get_file_type(fb) == G_FILE_TYPE_DIRECTORY);

    if (a_is_dir && !b_is_dir) return -1;
    if (!a_is_dir && b_is_dir) return 1;

    const char *na = g_file_info_get_name(fa);
    const char *nb = g_file_info_get_name(fb);
    if (!na) na = "";
    if (!nb) nb = "";

    if (app.sort_mode == SORT_NAME_ASC) {
        return g_utf8_collate(na, nb);
    } else if (app.sort_mode == SORT_NAME_DESC) {
        return g_utf8_collate(nb, na);
    } else if (app.sort_mode == SORT_SIZE) {
        goffset sa = g_file_info_get_size(fa);
        goffset sb = g_file_info_get_size(fb);
        if (sa < sb) return 1;
        if (sa > sb) return -1;
        return g_utf8_collate(na, nb);
    } else if (app.sort_mode == SORT_TIME) {
        guint64 ta = g_file_info_get_attribute_uint64(fa, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        guint64 tb = g_file_info_get_attribute_uint64(fb, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        if (ta < tb) return 1;
        if (ta > tb) return -1;
        return g_utf8_collate(na, nb);
    }

    return g_utf8_collate(na, nb);
}

static void refresh_dir(void) {
    if (!app.list_store || !app.path_entry || !app.current_dir) return;

    pthread_mutex_lock(&app.state_mutex);
    app.generation_id++;
    pthread_mutex_unlock(&app.state_mutex);

    app.search_query[0] = '\0';

    gtk_list_store_clear(app.list_store);
    gtk_entry_set_text(GTK_ENTRY(app.path_entry), app.current_dir);

    int current_icon_size = (app.view_mode == VIEW_MODE_ICONS) ? app.icon_size : 24;

    if (g_str_has_prefix(app.current_dir, "trash:///")) {
        char *trash_files = g_build_filename(g_get_user_data_dir(), "Trash", "files", NULL);
        GFile *dir_file = g_file_new_for_path(trash_files);
        GFileEnumerator *en = g_file_enumerate_children(dir_file, G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                                        G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                                        G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                                        G_FILE_QUERY_INFO_NONE, NULL, NULL);
        if (en) {
            GFileInfo *info;
            while ((info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
                GtkTreeIter iter;
                const char *name = g_file_info_get_name(info);
                char *full = g_build_filename(trash_files, name, NULL);
                GdkPixbuf *pix = get_fast_file_icon(FALSE, name, current_icon_size);

                gtk_list_store_append(app.list_store, &iter);
                gtk_list_store_set(app.list_store, &iter,
                                   COL_PIXBUF, pix,
                                   COL_NAME, name,
                                   COL_DISPLAY_NAME, name,
                                   COL_SIZE, format_size(g_file_info_get_size(info), FALSE),
                                   COL_IS_DIR, FALSE,
                                   COL_FULL_PATH, full, -1);
                if (pix) g_object_unref(pix);
                g_free(full);
                g_object_unref(info);
            }
            g_object_unref(en);
        }
        g_object_unref(dir_file);
        g_free(trash_files);
        update_status_bar_info();
        load_bookmarks(); 
        return;
    }

    GFile *dir_file = g_file_new_for_path(app.current_dir);
    GError *err = NULL;
    GFileEnumerator *en = g_file_enumerate_children(
        dir_file,
        G_FILE_ATTRIBUTE_STANDARD_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_SIZE ","
        G_FILE_ATTRIBUTE_TIME_MODIFIED ","
        G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
        G_FILE_QUERY_INFO_NONE, NULL, &err);

    if (!en) {
        set_status("Error: %s", err ? err->message : "failed to open directory");
        if (err) g_error_free(err);
        g_object_unref(dir_file);
        load_bookmarks();
        return;
    }

    GList *list = NULL;
    GFileInfo *info;

    while ((info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
        const char *name = g_file_info_get_name(info);
        gboolean hidden = (name && name[0] == '.');
        if (!app.show_hidden && hidden) {
            g_object_unref(info);
            continue;
        }
        list = g_list_prepend(list, info);
    }

    list = g_list_sort_with_data(list, (GCompareDataFunc)compare_file_info, NULL);

    for (GList *l = list; l != NULL; l = l->next) {
        GFileInfo *fi = l->data;
        GtkTreeIter iter;
        const char *name = g_file_info_get_name(fi);
        char *full_path = g_build_filename(app.current_dir, name ? name : "", NULL);
        gboolean is_dir = (g_file_info_get_file_type(fi) == G_FILE_TYPE_DIRECTORY);
        
        GdkPixbuf *pix = get_fast_file_icon(is_dir, name, current_icon_size);
        char *sz = is_dir ? g_strdup("<DIR>") : format_size(g_file_info_get_size(fi), FALSE);

        gtk_list_store_append(app.list_store, &iter);
        gtk_list_store_set(app.list_store, &iter,
                           COL_PIXBUF, pix,
                           COL_NAME, name ? name : "",
                           COL_DISPLAY_NAME, name ? name : "",
                           COL_SIZE, sz,
                           COL_IS_DIR, is_dir,
                           COL_FULL_PATH, full_path, -1);
        
        gboolean can_have_thumbnail = !is_dir;

        if (can_have_thumbnail) {
            ThumbnailTask *task = g_new0(ThumbnailTask, 1);
            task->full_path = g_build_filename(app.current_dir, name ? name : "", NULL);
            task->icon_size = current_icon_size;
            
            pthread_mutex_lock(&app.state_mutex);
            task->gen_id = app.generation_id;
            pthread_mutex_unlock(&app.state_mutex);
            
            g_thread_pool_push(app.thread_pool, task, NULL);
        }

        if (pix) g_object_unref(pix);
        g_free(sz);
        g_free(full_path);
        g_object_unref(fi);
    }

    g_list_free(list);
    g_object_unref(en);
    g_object_unref(dir_file);
    
    update_status_bar_info();
    load_bookmarks(); 
}

static void on_directory_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data) {
    (void)monitor; (void)file; (void)other_file; (void)user_data;
    if (event_type == G_FILE_MONITOR_EVENT_CHANGED ||
        event_type == G_FILE_MONITOR_EVENT_CREATED ||
        event_type == G_FILE_MONITOR_EVENT_DELETED ||
        event_type == G_FILE_MONITOR_EVENT_MOVED) {
        refresh_dir();
    }
}

static void setup_directory_monitor(const char *path) {
    if (app.dir_monitor) {
        g_file_monitor_cancel(app.dir_monitor);
        g_object_unref(app.dir_monitor);
        app.dir_monitor = NULL;
    }

    if (!path || g_str_has_prefix(path, "trash:///")) return;

    GFile *file = g_file_new_for_path(path);
    GError *err = NULL;
    app.dir_monitor = g_file_monitor_directory(file, G_FILE_MONITOR_NONE, NULL, &err);
    if (app.dir_monitor) {
        g_signal_connect(app.dir_monitor, "changed", G_CALLBACK(on_directory_changed), NULL);
    } else {
        if (err) g_error_free(err);
    }
    g_object_unref(file);
}

static char *expand_tilde_path(const char *path) {
    if (!path || path[0] != '~') {
        return g_strdup(path ? path : "");
    }
    const char *home = g_get_home_dir();
    if (!home || !*home) {
        return g_strdup(path);
    }
    if (path[1] == '\0') {
        return g_strdup(home);
    }
    if (path[1] == '/') {
        return g_build_filename(home, path + 2, NULL);
    }
    return g_strdup(path);
}

static void navigate_to(const char *path, gboolean record_history) {
    if (!path) return;

    if (g_str_has_prefix(path, "trash:///")) {
        if (record_history && app.current_dir && g_strcmp0(app.current_dir, path) != 0) {
            app.history_back = g_list_prepend(app.history_back, g_strdup(app.current_dir));
            free_string_list(&app.history_fwd);
        }
        g_free(app.current_dir);
        app.current_dir = g_strdup(path);
        setup_directory_monitor(app.current_dir);
        refresh_dir();
        return;
    }

    char *expanded = expand_tilde_path(path);
    char *real = realpath(expanded, NULL);
    const char *target = real ? real : expanded;

    if (!g_file_test(target, G_FILE_TEST_IS_DIR)) {
        set_status("Directory not found: %s", target);
        free(real);
        g_free(expanded);
        return;
    }

    if (record_history && app.current_dir && g_strcmp0(app.current_dir, target) != 0) {
        app.history_back = g_list_prepend(app.history_back, g_strdup(app.current_dir));
        free_string_list(&app.history_fwd);
    }

    g_free(app.current_dir);
    app.current_dir = g_strdup(target);
    free(real);
    g_free(expanded);

    setup_directory_monitor(app.current_dir);
    refresh_dir();
}

static void go_back(void) {
    if (!app.history_back) return;
    char *prev = app.history_back->data;
    app.history_back = g_list_delete_link(app.history_back, app.history_back);
    app.history_fwd = g_list_prepend(app.history_fwd, g_strdup(app.current_dir));
    navigate_to(prev, FALSE);
    g_free(prev);
}

static void go_forward(void) {
    if (!app.history_fwd) return;
    char *next = app.history_fwd->data;
    app.history_fwd = g_list_delete_link(app.history_fwd, app.history_fwd);
    app.history_back = g_list_prepend(app.history_back, g_strdup(app.current_dir));
    navigate_to(next, FALSE);
    g_free(next);
}

static int process_file_operation(const char *src_path, const char *dest_dir, gboolean is_move) {
    char *basename = g_path_get_basename(src_path);
    char *dest_path = g_build_filename(dest_dir, basename, NULL);

    if (g_strcmp0(src_path, dest_path) == 0) {
        g_free(basename);
        g_free(dest_path);
        return 0; 
    }

    gboolean skip = FALSE;
    if (g_file_test(dest_path, G_FILE_TEST_EXISTS)) {
        GtkWidget *dialog = gtk_message_dialog_new(
            GTK_WINDOW(app.window),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_QUESTION,
            GTK_BUTTONS_NONE,
            "File already exists: %s", basename
        );
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dialog),
            "A file with the same name already exists in this folder. Do you want to overwrite it or skip?"
        );
        gtk_dialog_add_button(GTK_DIALOG(dialog), "Skip", GTK_RESPONSE_NO);
        gtk_dialog_add_button(GTK_DIALOG(dialog), "Overwrite", GTK_RESPONSE_YES);

        int response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);

        if (response != GTK_RESPONSE_YES) {
            skip = TRUE;
        }
    }

    int success = 0;
    if (!skip) {
        GFile *f_src = g_file_new_for_path(src_path);
        GFile *f_dst = g_file_new_for_path(dest_path);
        GError *err = NULL;
        gboolean res = FALSE;

        GFileCopyFlags flags = G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS;

        if (is_move) {
            GError *move_err = NULL;
            res = g_file_move(f_src, f_dst, flags, NULL, NULL, NULL, &move_err);
            if (!res) {
                g_clear_error(&move_err);
                if (copy_recursive(f_src, f_dst, flags, &err)) {
                    res = delete_recursive(f_src, &err);
                }
            }
        } else {
            res = copy_recursive(f_src, f_dst, flags, &err);
        }

        if (res) {
            success = 1;
        } else {
            if (err) {
                set_status("Error: %s", err->message);
                g_error_free(err);
            }
        }

        g_object_unref(f_src);
        g_object_unref(f_dst);
    }

    g_free(basename);
    g_free(dest_path);
    return success;
}

static void on_view_drag_data_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y, GtkSelectionData *selection_data, guint info, guint time, gpointer user_data) {
    (void)widget; (void)info; (void)time; (void)user_data;
    const guchar *data = gtk_selection_data_get_data(selection_data);
    if (!data || *data == '\0') {
        gdk_drag_status(context, 0, time);
        gtk_drag_finish(context, FALSE, FALSE, time);
        return;
    }

    gchar **uris = gtk_selection_data_get_uris(selection_data);
    if (!uris) {
        gtk_drag_finish(context, FALSE, FALSE, time);
        return;
    }

    char *dest_dir = g_strdup(app.current_dir);
    if (app.view_mode == VIEW_MODE_LIST) {
        GtkTreePath *path = NULL;
        GtkTreeView *tv = GTK_TREE_VIEW(app.tree_view);
        if (gtk_tree_view_get_path_at_pos(tv, x, y, &path, NULL, NULL, NULL)) {
            GtkTreeModel *model = GTK_TREE_MODEL(app.list_store);
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter(model, &iter, path)) {
                gboolean is_dir = FALSE;
                char *fpath = NULL;
                gtk_tree_model_get(model, &iter, COL_IS_DIR, &is_dir, COL_FULL_PATH, &fpath, -1);
                if (is_dir && fpath) {
                    g_free(dest_dir);
                    dest_dir = g_strdup(fpath);
                }
                g_free(fpath);
            }
            gtk_tree_path_free(path);
        }
    } else {
        GtkTreePath *path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(app.icon_view), x, y);
        if (path) {
            GtkTreeModel *model = GTK_TREE_MODEL(app.list_store);
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter(model, &iter, path)) {
                gboolean is_dir = FALSE;
                char *fpath = NULL;
                gtk_tree_model_get(model, &iter, COL_IS_DIR, &is_dir, COL_FULL_PATH, &fpath, -1);
                if (is_dir && fpath) {
                    g_free(dest_dir);
                    dest_dir = g_strdup(fpath);
                }
                g_free(fpath);
            }
            gtk_tree_path_free(path);
        }
    }

    GdkDragAction action = gdk_drag_context_get_selected_action(context);
    gboolean is_move = (action == GDK_ACTION_MOVE);

    int success_count = 0;
    for (int i = 0; uris[i] != NULL; i++) {
        char *src_path = g_filename_from_uri(uris[i], NULL, NULL);
        if (!src_path) continue;

        success_count += process_file_operation(src_path, dest_dir, is_move);
        g_free(src_path);
    }

    if (success_count > 0) {
        refresh_dir();
        set_status("%s %d items successfully", is_move ? "Moved" : "Copied", success_count);
        gtk_drag_finish(context, TRUE, is_move, time);
    } else {
        gtk_drag_finish(context, FALSE, FALSE, time);
    }

    g_strfreev(uris);
    g_free(dest_dir);
}

typedef struct {
    gdouble start_x;
    gdouble start_y;
    gboolean dragging_active;
} DragState;

static DragState drag_state = {0};

static gboolean on_view_button_press(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    (void)data;
    if (ev->button == 8) {
        go_back();
        return TRUE;
    }
    if (ev->button == 9) {
        go_forward();
        return TRUE;
    }

    if (ev->button == GDK_BUTTON_PRIMARY && ev->type == GDK_2BUTTON_PRESS) {
        open_currently_selected();
        return TRUE;
    }

    if (ev->button == GDK_BUTTON_PRIMARY && ev->type == GDK_BUTTON_PRESS) {
        drag_state.start_x = ev->x;
        drag_state.start_y = ev->y;
        drag_state.dragging_active = FALSE;
    }
    
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) {
        if (GTK_IS_TREE_VIEW(widget)) {
            GtkTreePath *path = NULL;
            GtkTreeView *tv = GTK_TREE_VIEW(widget);
            gboolean clicked_on_row = gtk_tree_view_get_path_at_pos(tv, (gint)ev->x, (gint)ev->y, &path, NULL, NULL, NULL);
            if (clicked_on_row) {
                GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
                if (!gtk_tree_selection_path_is_selected(sel, path)) {
                    gtk_tree_selection_unselect_all(sel);
                    gtk_tree_selection_select_path(sel, path);
                }
                gtk_tree_path_free(path);
                return handle_context_menu(ev, TRUE);
            } else {
                if (path) gtk_tree_path_free(path);
                return handle_context_menu(ev, FALSE);
            }
        } else if (GTK_IS_ICON_VIEW(widget)) {
            GtkTreePath *path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(widget), (gint)ev->x, (gint)ev->y);
            if (path) {
                GtkIconView *iv = GTK_ICON_VIEW(widget);
                if (!gtk_icon_view_path_is_selected(iv, path)) {
                    gtk_icon_view_unselect_all(iv);
                    gtk_icon_view_select_path(iv, path);
                }
                gtk_tree_path_free(path);
                return handle_context_menu(ev, TRUE);
            } else {
                return handle_context_menu(ev, FALSE);
            }
        }
    }
    
    return FALSE; 
}

static gboolean on_view_motion_notify(GtkWidget *widget, GdkEventMotion *ev, gpointer data) {
    (void)data;
    if ((ev->state & GDK_BUTTON1_MASK) && (ev->state & GDK_MOD1_MASK)) {
        if (!drag_state.dragging_active) {
            if (gtk_drag_check_threshold(widget, (gint)drag_state.start_x, (gint)drag_state.start_y, (gint)ev->x, (gint)ev->y)) {
                drag_state.dragging_active = TRUE;

                GtkTargetList *targets = gtk_target_list_new(NULL, 0);
                gtk_target_list_add_uri_targets(targets, 0);

                gtk_drag_begin_with_coordinates(
                    widget, targets, 
                    GDK_ACTION_COPY | GDK_ACTION_MOVE, 
                    1, (GdkEvent *)ev, 
                    (gint)ev->x, (gint)ev->y
                );

                gtk_target_list_unref(targets);
            }
        }
    }
    return FALSE;
}

static gboolean on_view_button_release(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    (void)widget; (void)data;
    if (ev->button == GDK_BUTTON_PRIMARY) {
        drag_state.dragging_active = FALSE;
    }
    return FALSE;
}

static void on_view_drag_data_get(GtkWidget *widget, GdkDragContext *context, GtkSelectionData *selection_data, guint info, guint time, gpointer user_data) {
    (void)widget; (void)context; (void)info; (void)time; (void)user_data;
    GList *sel = get_selected_paths();
    if (!sel) return;

    GString *uri_list = g_string_new(NULL);
    for (GList *l = sel; l != NULL; l = l->next) {
        char *uri = g_filename_to_uri((const char *)l->data, NULL, NULL);
        if (uri) {
            g_string_append_printf(uri_list, "%s\r\n", uri);
            g_free(uri);
        }
    }
    gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), 8, (const guchar *)uri_list->str, (gint)uri_list->len);
    g_string_free(uri_list, TRUE);
    free_string_list(&sel);
}

static char *prompt_text(const char *title, const char *label_txt, const char *initial) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        title, GTK_WINDOW(app.window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL,
        "OK", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 8);

    GtkWidget *lbl = gtk_label_new(label_txt);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    if (initial) gtk_entry_set_text(GTK_ENTRY(entry), initial);

    gtk_box_pack_start(GTK_BOX(content), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 0);
    gtk_widget_show_all(dlg);

    char *res = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        const char *txt = gtk_entry_get_text(GTK_ENTRY(entry));
        if (txt && *txt) res = g_strdup(txt);
    }
    gtk_widget_destroy(dlg);
    return res;
}

static char *get_orig_path_from_trash_info(const char *trash_file_path) {
    char *base = g_path_get_basename(trash_file_path);
    char *info_name = g_strconcat(base ? base : "", ".trashinfo", NULL);
    char *info_path = g_build_filename(g_get_user_data_dir(), "Trash", "info", info_name, NULL);
    g_free(base);
    g_free(info_name);

    GKeyFile *kf = g_key_file_new();
    char *orig_path = NULL;
    if (g_key_file_load_from_file(kf, info_path, G_KEY_FILE_NONE, NULL)) {
        char *raw_path = g_key_file_get_string(kf, "Trash Info", "Path", NULL);
        if (raw_path) {
            orig_path = g_uri_unescape_string(raw_path, NULL);
            g_free(raw_path);
        }
    }
    g_key_file_free(kf);
    g_free(info_path);
    return orig_path;
}

static void perform_undo(void) {
    if (!app.undo_stack) {
        set_status("Nothing to undo");
        return;
    }

    UndoOp *op = app.undo_stack->data;
    app.undo_stack = g_list_delete_link(app.undo_stack, app.undo_stack);

    GError *err = NULL;
    gboolean ok = FALSE;

    switch (op->type) {
        case UNDO_TRASH:
        case UNDO_RESTORE: {
            GFile *f_trash = g_file_new_for_path(op->src);
            GFile *f_orig = g_file_new_for_path(op->dst);
            ok = g_file_move(f_trash, f_orig, G_FILE_COPY_NONE, NULL, NULL, NULL, &err);
            if (ok) {
                char *base = g_path_get_basename(op->src);
                char *info_name = g_strconcat(base ? base : "", ".trashinfo", NULL);
                char *info_path = g_build_filename(g_get_user_data_dir(), "Trash", "info", info_name, NULL);
                unlink(info_path);
                g_free(base);
                g_free(info_name);
                g_free(info_path);
                set_status("Restored: %s", op->dst);
            }
            g_object_unref(f_trash);
            g_object_unref(f_orig);
            break;
        }
        case UNDO_RENAME:
        case UNDO_MOVE: {
            GFile *f_new = g_file_new_for_path(op->dst);
            GFile *f_old = g_file_new_for_path(op->src);
            ok = g_file_move(f_new, f_old, G_FILE_COPY_NONE, NULL, NULL, NULL, &err);
            if (ok) set_status("Reverted: %s", op->src);
            g_object_unref(f_new);
            g_object_unref(f_old);
            break;
        }
        case UNDO_COPY:
        case UNDO_CREATE: {
            GFile *f_created = g_file_new_for_path(op->dst);
            ok = delete_recursive(f_created, &err);
            if (ok) set_status("Removed: %s", op->dst);
            g_object_unref(f_created);
            break;
        }
    }

    if (!ok && err) {
        set_status("Undo error: %s", err->message);
        g_error_free(err);
    }

    free_undo_op(op);
    refresh_dir();
}

static void clipboard_get_func(GtkClipboard *clipboard, GtkSelectionData *selection_data, guint info, gpointer user_data) {
    (void)clipboard; (void)user_data;
    if (!app.clipboard_paths) return;

    if (info == 0) {
        GString *uri_list = g_string_new(NULL);
        for (GList *l = app.clipboard_paths; l != NULL; l = l->next) {
            char *uri = g_filename_to_uri((const char *)l->data, NULL, NULL);
            if (uri) {
                g_string_append_printf(uri_list, "%s\r\n", uri);
                g_free(uri);
            }
        }
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), 8, (const guchar *)uri_list->str, (gint)uri_list->len);
        g_string_free(uri_list, TRUE);
    } else if (info == 1) {
        GString *txt = g_string_new(NULL);
        for (GList *l = app.clipboard_paths; l != NULL; l = l->next) {
            g_string_append_printf(txt, "%s\n", (const char *)l->data);
        }
        gtk_selection_data_set_text(selection_data, txt->str, (gint)txt->len);
        g_string_free(txt, TRUE);
    }
}

static void clipboard_clear_func(GtkClipboard *clipboard, gpointer user_data) {
    (void)clipboard; (void)user_data;
}

static GList *get_clipboard_paths_from_system(void) {
    GtkClipboard *clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    GList *paths = NULL;
    
    GtkSelectionData *sdata = gtk_clipboard_wait_for_contents(clip, gdk_atom_intern("text/uri-list", FALSE));
    if (sdata) {
        gchar **uris = gtk_selection_data_get_uris(sdata);
        if (uris) {
            for (int i = 0; uris[i] != NULL; i++) {
                char *path = g_filename_from_uri(uris[i], NULL, NULL);
                if (path) {
                    paths = g_list_append(paths, path);
                }
            }
            g_strfreev(uris);
        }
        gtk_selection_data_free(sdata);
    }

    if (!paths) {
        gchar *txt = gtk_clipboard_wait_for_text(clip);
        if (txt) {
            gchar **lines = g_strsplit(txt, "\n", -1);
            for (int i = 0; lines[i] != NULL; i++) {
                char *line = g_strstrip(lines[i]);
                if (*line != '\0' && g_file_test(line, G_FILE_TEST_EXISTS)) {
                    paths = g_list_append(paths, g_strdup(line));
                }
            }
            g_strfreev(lines);
            g_free(txt);
        }
    }

    return paths;
}

static void switch_view_mode(ViewMode mode) {
    app.view_mode = mode;
    save_config();
    if (app.view_mode == VIEW_MODE_LIST) {
        gtk_stack_set_visible_child_name(GTK_STACK(app.content_stack), "list");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(app.content_stack), "icons");
    }
    refresh_dir();
}

static void set_sort_mode(SortMode mode) {
    app.sort_mode = mode;
    save_config();
    refresh_dir();
}

static gboolean is_path_in_gtk_bookmarks(const char *path) {
    if (!path) return FALSE;
    char *bm_path = g_build_filename(g_get_user_config_dir(), "gtk-3.0", "bookmarks", NULL);
    if (!g_file_test(bm_path, G_FILE_TEST_EXISTS)) {
        g_free(bm_path);
        bm_path = g_build_filename(g_get_home_dir(), ".gtk-bookmarks", NULL);
    }

    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (!uri) {
        g_free(bm_path);
        return FALSE;
    }

    gboolean found = FALSE;
    char *content = NULL;
    if (g_file_get_contents(bm_path, &content, NULL, NULL)) {
        char **lines = g_strsplit(content, "\n", -1);
        for (int i = 0; lines[i] != NULL; i++) {
            char *line = g_strstrip(lines[i]);
            if (*line == '\0' || *line == '#') continue;
            char *space = strchr(line, ' ');
            char *u = space ? g_strndup(line, space - line) : g_strdup(line);
            if (g_strcmp0(u, uri) == 0) {
                found = TRUE;
                g_free(u);
                break;
            }
            g_free(u);
        }
        g_strfreev(lines);
        g_free(content);
    }
    g_free(uri);
    g_free(bm_path);
    return found;
}

static void pin_path_to_bookmarks(const char *path) {
    if (!path || !g_file_test(path, G_FILE_TEST_IS_DIR)) return;
    char *bm_path = g_build_filename(g_get_user_config_dir(), "gtk-3.0", "bookmarks", NULL);
    if (!g_file_test(bm_path, G_FILE_TEST_EXISTS)) {
        g_free(bm_path);
        bm_path = g_build_filename(g_get_home_dir(), ".gtk-bookmarks", NULL);
    }

    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (!uri) {
        g_free(bm_path);
        return;
    }

    gboolean exists = is_path_in_gtk_bookmarks(path);

    if (!exists) {
        FILE *fp = fopen(bm_path, "a");
        if (fp) {
            fprintf(fp, "%s\n", uri);
            fclose(fp);
            set_status("Pinned to bookmarks: %s", path);
            load_bookmarks();
        }
    } else {
        set_status("Already pinned");
    }
    g_free(uri);
    g_free(bm_path);
}

static void unpin_path_from_bookmarks(const char *path) {
    if (!path) return;
    char *bm_path = g_build_filename(g_get_user_config_dir(), "gtk-3.0", "bookmarks", NULL);
    if (!g_file_test(bm_path, G_FILE_TEST_EXISTS)) {
        g_free(bm_path);
        bm_path = g_build_filename(g_get_home_dir(), ".gtk-bookmarks", NULL);
    }

    char *uri = g_filename_to_uri(path, NULL, NULL);
    char *content = NULL;
    if (g_file_get_contents(bm_path, &content, NULL, NULL)) {
        char **lines = g_strsplit(content, "\n", -1);
        FILE *fp = fopen(bm_path, "w");
        if (fp) {
            for (int i = 0; lines[i] != NULL; i++) {
                if (lines[i][0] == '\0') continue;
                char *space = strchr(lines[i], ' ');
                char *u = space ? g_strndup(lines[i], space - lines[i]) : g_strdup(lines[i]);
                if (uri && g_strcmp0(u, uri) != 0) {
                    fprintf(fp, "%s\n", lines[i]);
                }
                g_free(u);
            }
            fclose(fp);
        }
        g_strfreev(lines);
        g_free(content);
        set_status("Unpinned");
        load_bookmarks();
    }
    g_free(uri);
    g_free(bm_path);
}

static void empty_trash_fully(void) {
    char *trash_files = g_build_filename(g_get_user_data_dir(), "Trash", "files", NULL);
    char *trash_info = g_build_filename(g_get_user_data_dir(), "Trash", "info", NULL);
    
    GFile *f_files = g_file_new_for_path(trash_files);
    GFile *f_info = g_file_new_for_path(trash_info);
    
    GError *err = NULL;
    delete_recursive(f_files, &err);
    delete_recursive(f_info, &err);
    
    g_mkdir_with_parents(trash_files, 0755);
    g_mkdir_with_parents(trash_info, 0755);

    g_object_unref(f_files);
    g_object_unref(f_info);
    g_free(trash_files);
    g_free(trash_info);

    set_status("Trash emptied");
    refresh_dir();
}

static void execute_custom_action(GtkMenuItem *item, gpointer data) {
    (void)item;
    char *cmd_template = (char *)data;
    GList *sel = get_selected_paths();
    if (!sel || !sel->data) {
        free_string_list(&sel);
        return;
    }
    char *path = sel->data;
    
    char *full_cmd = NULL;
    if (strstr(cmd_template, "%s")) {
        full_cmd = g_strdup_printf(cmd_template, path);
    } else {
        full_cmd = g_strdup_printf("%s \"%s\"", cmd_template, path);
    }

    char *argv[] = { "/bin/sh", "-c", full_cmd, NULL };
    GError *err = NULL;
    if (!g_spawn_async(app.current_dir, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err)) {
        set_status("Action error: %s", err ? err->message : "");
        if (err) g_error_free(err);
    } else {
        set_status("Executed custom action");
    }

    g_free(full_cmd);
    free_string_list(&sel);
}

static void on_menu_compress_zip(GtkMenuItem *item, gpointer data) {
    (void)item; (void)data;
    GList *sel = get_selected_paths();
    if (!sel) return;
    char *archive_name = prompt_text("Compress to ZIP", "Archive name (.zip):", "archive.zip");
    if (!archive_name) {
        free_string_list(&sel);
        return;
    }
    GString *cmd = g_string_new("zip -r ");
    char *q_name = g_shell_quote(archive_name);
    g_string_append(cmd, q_name);
    g_free(q_name);

    for (GList *l = sel; l != NULL; l = l->next) {
        g_string_append(cmd, " ");
        char *q_path = g_shell_quote((char *)l->data);
        g_string_append(cmd, q_path);
        g_free(q_path);
    }
    run_command_with_progress("Compressing to ZIP...", cmd->str, "Archive operation completed");
    g_string_free(cmd, TRUE);
    g_free(archive_name);
    free_string_list(&sel);
}

static void on_menu_compress_tar(GtkMenuItem *item, gpointer data) {
    (void)item; (void)data;
    GList *sel = get_selected_paths();
    if (!sel) return;
    char *archive_name = prompt_text("Compress to TAR.GZ", "Archive name (.tar.gz):", "archive.tar.gz");
    if (!archive_name) {
        free_string_list(&sel);
        return;
    }
    GString *cmd = g_string_new("tar -czf ");
    char *q_name = g_shell_quote(archive_name);
    g_string_append(cmd, q_name);
    g_free(q_name);

    for (GList *l = sel; l != NULL; l = l->next) {
        g_string_append(cmd, " ");
        char *q_path = g_shell_quote((char *)l->data);
        g_string_append(cmd, q_path);
        g_free(q_path);
    }
    run_command_with_progress("Compressing to TAR.GZ...", cmd->str, "Archive operation completed");
    g_string_free(cmd, TRUE);
    g_free(archive_name);
    free_string_list(&sel);
}

static void on_menu_compress_7z(GtkMenuItem *item, gpointer data) {
    (void)item; (void)data;
    GList *sel = get_selected_paths();
    if (!sel) return;
    char *archive_name = prompt_text("Compress to 7Z", "Archive name (.7z):", "archive.7z");
    if (!archive_name) {
        free_string_list(&sel);
        return;
    }
    GString *cmd = g_string_new("7z a ");
    char *q_name = g_shell_quote(archive_name);
    g_string_append(cmd, q_name);
    g_free(q_name);

    for (GList *l = sel; l != NULL; l = l->next) {
        g_string_append(cmd, " ");
        char *q_path = g_shell_quote((char *)l->data);
        g_string_append(cmd, q_path);
        g_free(q_path);
    }
    run_command_with_progress("Compressing to 7Z...", cmd->str, "Archive operation completed");
    g_string_free(cmd, TRUE);
    g_free(archive_name);
    free_string_list(&sel);
}

static void on_menu_extract_archive(GtkMenuItem *item, gpointer data) {
    (void)item; (void)data;
    GList *sel = get_selected_paths();
    if (!sel || !sel->data) {
        free_string_list(&sel);
        return;
    }
    char *path = sel->data;
    const char *ext = strrchr(path, '.');
    char cmd[1024];
    char *q_path = g_shell_quote(path);

    if (ext && g_ascii_strcasecmp(ext, ".zip") == 0) {
        snprintf(cmd, sizeof(cmd), "unzip %s", q_path);
    } else if (ext && g_ascii_strcasecmp(ext, ".7z") == 0) {
        snprintf(cmd, sizeof(cmd), "7z x %s", q_path);
    } else if (ext && g_ascii_strcasecmp(ext, ".rar") == 0) {
        snprintf(cmd, sizeof(cmd), "unrar x %s", q_path);
    } else {
        snprintf(cmd, sizeof(cmd), "tar -xf %s", q_path);
    }
    g_free(q_path);

    run_command_with_progress("Extracting Archive...", cmd, "Archive extraction completed");
    free_string_list(&sel);
}

static void on_menu_restore_from_trash(GtkMenuItem *item, gpointer data) {
    (void)item; (void)data;
    GList *sel = get_selected_paths();
    if (!sel) return;
    int count = 0;
    for (GList *l = sel; l != NULL; l = l->next) {
        char *trash_file_path = l->data;
        char *orig_path = get_orig_path_from_trash_info(trash_file_path);
        if (orig_path) {
            GFile *f_trash = g_file_new_for_path(trash_file_path);
            GFile *f_orig = g_file_new_for_path(orig_path);
            GError *err = NULL;
            if (g_file_move(f_trash, f_orig, G_FILE_COPY_NONE, NULL, NULL, NULL, &err)) {
                char *base = g_path_get_basename(trash_file_path);
                char *info_name = g_strconcat(base ? base : "", ".trashinfo", NULL);
                char *info_path = g_build_filename(g_get_user_data_dir(), "Trash", "info", info_name, NULL);
                unlink(info_path);
                g_free(base);
                g_free(info_name);
                g_free(info_path);

                push_undo(UNDO_RESTORE, trash_file_path, orig_path);
                count++;
            } else {
                set_status("Restore error: %s", err ? err->message : "");
                if (err) g_error_free(err);
            }
            g_object_unref(f_trash);
            g_object_unref(f_orig);
            g_free(orig_path);
        }
    }
    free_string_list(&sel);
    refresh_dir();
    if (count > 0) set_status("Restored items: %d", count);
}

static void on_menu_delete_permanently(GtkMenuItem *item, gpointer data) {
    (void)item; (void)data;
    GList *sel = get_selected_paths();
    if (!sel) return;
    int count = 0;
    for (GList *l = sel; l != NULL; l = l->next) {
        char *trash_file_path = l->data;
        GFile *f_trash = g_file_new_for_path(trash_file_path);
        GError *err = NULL;
        if (delete_recursive(f_trash, &err)) {
            char *base = g_path_get_basename(trash_file_path);
            char *info_name = g_strconcat(base ? base : "", ".trashinfo", NULL);
            char *info_path = g_build_filename(g_get_user_data_dir(), "Trash", "info", info_name, NULL);
            unlink(info_path);
            g_free(base);
            g_free(info_name);
            g_free(info_path);
            count++;
        } else {
            set_status("Deletion error: %s", err ? err->message : "");
            if (err) g_error_free(err);
        }
        g_object_unref(f_trash);
    }
    free_string_list(&sel);
    refresh_dir();
    if (count > 0) set_status("Deleted permanently: %d items", count);
}

static void on_menu_set_default_app(GtkMenuItem *item, gpointer data) {
    (void)item; (void)data;
    GList *sel = get_selected_paths();
    if (!sel || !sel->data) {
        free_string_list(&sel);
        return;
    }
    char *path = sel->data;
    GFile *file = g_file_new_for_path(path);
    GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (info) {
        const char *ct = g_file_info_get_content_type(info);
        if (ct) {
            GList *apps = g_app_info_get_all_for_type(ct);
            if (apps) {
                GtkWidget *dlg = gtk_dialog_new();
                gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(app.window));
                gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
                gtk_window_set_destroy_with_parent(GTK_WINDOW(dlg), TRUE);
                gtk_window_set_title(GTK_WINDOW(dlg), "Set Default Application");
                gtk_window_set_default_size(GTK_WINDOW(dlg), 350, 250);

                GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
                gtk_container_set_border_width(GTK_CONTAINER(content), 12);

                GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
                gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
                GtkWidget *list_box = gtk_list_box_new();
                gtk_container_add(GTK_CONTAINER(scroll), list_box);
                gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);

                for (GList *l = apps; l != NULL; l = l->next) {
                    GAppInfo *appinfo = G_APP_INFO(l->data);
                    GtkWidget *row_lbl = gtk_label_new(g_app_info_get_name(appinfo));
                    gtk_widget_set_halign(row_lbl, GTK_ALIGN_START);
                    GtkWidget *row = gtk_list_box_row_new();
                    gtk_container_add(GTK_CONTAINER(row), row_lbl);
                    g_object_set_data(G_OBJECT(row), "appinfo", appinfo);
                    gtk_container_add(GTK_CONTAINER(list_box), row);
                }

                GtkWidget *btn_box = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
                gtk_button_box_set_layout(GTK_BUTTON_BOX(btn_box), GTK_BUTTONBOX_END);
                GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
                GtkWidget *btn_select = gtk_button_new_with_label("Select");
                gtk_container_add(GTK_CONTAINER(btn_box), btn_cancel);
                gtk_container_add(GTK_CONTAINER(btn_box), btn_select);
                gtk_box_pack_start(GTK_BOX(content), btn_box, FALSE, FALSE, 0);

                g_signal_connect_swapped(btn_cancel, "clicked", G_CALLBACK(gtk_widget_destroy), dlg);
                g_signal_connect_swapped(btn_select, "clicked", G_CALLBACK(gtk_window_close), dlg);

                gtk_widget_show_all(dlg);
                if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
                    GtkListBoxRow *selected_row = gtk_list_box_get_selected_row(GTK_LIST_BOX(list_box));
                    if (selected_row) {
                        GAppInfo *chosen = g_object_get_data(G_OBJECT(selected_row), "appinfo");
                        GError *err = NULL;
                        if (g_app_info_set_as_default_for_type(chosen, ct, &err)) {
                            set_status("Default app updated for %s", ct);
                        } else {
                            set_status("Error setting default app: %s", err ? err->message : "");
                            if (err) g_error_free(err);
                        }
                    }
                }
                gtk_widget_destroy(dlg);
                g_list_free_full(apps, g_object_unref);
            } else {
                set_status("No applications found for type %s", ct);
            }
        }
        g_object_unref(info);
    }
    g_object_unref(file);
    free_string_list(&sel);
}

typedef struct {
    char *path;
    GtkWidget *size_label;
    guint64 total_size;
    guint64 file_count;
    guint64 dir_count;
    gboolean cancelled;
} DirSizeTask;

static void calculate_dir_size_recursive(const char *dir_path, DirSizeTask *task) {
    if (task->cancelled) return;

    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) return;

    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (task->cancelled) break;
        char *full_path = g_build_filename(dir_path, name, NULL);
        
        struct stat st;
        if (lstat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                task->dir_count++;
                calculate_dir_size_recursive(full_path, task);
            } else if (S_ISREG(st.st_mode)) {
                task->file_count++;
                task->total_size += st.st_size;
            }
        }
        g_free(full_path);
    }
    g_dir_close(dir);
}

static gboolean update_size_label_idle(gpointer user_data) {
    DirSizeTask *task = user_data;
    if (!task->cancelled && task->size_label && GTK_IS_WIDGET(task->size_label)) {
        char *sz_str = format_size(task->total_size, FALSE);
        char buf[256];
        snprintf(buf, sizeof(buf), "%s (%llu bytes, %llu files, %llu folders)", 
                 sz_str, (unsigned long long)task->total_size, 
                 (unsigned long long)task->file_count, 
                 (unsigned long long)task->dir_count);
        gtk_label_set_text(GTK_LABEL(task->size_label), buf);
        g_free(sz_str);
    }
    g_free(task->path);
    g_free(task);
    return G_SOURCE_REMOVE;
}

static gpointer dir_size_worker(gpointer data) {
    DirSizeTask *task = data;
    calculate_dir_size_recursive(task->path, task);
    g_idle_add(update_size_label_idle, task);
    return NULL;
}

static void open_file_properties_dialog(const char *path) {
    if (!path) return;

    struct stat st;
    if (lstat(path, &st) != 0) {
        set_status("Cannot stat file: %s", strerror(errno));
        return;
    }

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "File Properties", GTK_WINDOW(app.window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Close", GTK_RESPONSE_CLOSE, NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 14);
    gtk_box_set_spacing(GTK_BOX(content), 10);

    GtkWidget *hbox_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    char *base_name = g_path_get_basename(path);
    GdkPixbuf *pix = get_fast_file_icon(S_ISDIR(st.st_mode), base_name, 48);
    GtkWidget *img = gtk_image_new_from_pixbuf(pix);
    if (pix) g_object_unref(pix);

    GtkWidget *lbl_name = gtk_label_new(base_name);
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(lbl_name), attrs);
    pango_attr_list_unref(attrs);
    gtk_widget_set_halign(lbl_name, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_name), PANGO_ELLIPSIZE_MIDDLE);

    gtk_box_pack_start(GTK_BOX(hbox_top), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_top), lbl_name, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), hbox_top, FALSE, FALSE, 0);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(content), sep, FALSE, FALSE, 4);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

    int row = 0;
    GtkWidget *size_val_lbl = NULL;

    #define ADD_INFO_ROW(k_text, v_text, is_size_row) do { \
        GtkWidget *k_lbl = gtk_label_new(k_text); \
        gtk_widget_set_halign(k_lbl, GTK_ALIGN_START); \
        PangoAttrList *k_attrs = pango_attr_list_new(); \
        pango_attr_list_insert(k_attrs, pango_attr_weight_new(PANGO_WEIGHT_SEMIBOLD)); \
        gtk_label_set_attributes(GTK_LABEL(k_lbl), k_attrs); \
        pango_attr_list_unref(k_attrs); \
        GtkWidget *v_lbl = gtk_label_new(v_text); \
        gtk_widget_set_halign(v_lbl, GTK_ALIGN_START); \
        gtk_label_set_selectable(GTK_LABEL(v_lbl), TRUE); \
        if (is_size_row) size_val_lbl = v_lbl; \
        gtk_grid_attach(GTK_GRID(grid), k_lbl, 0, row, 1, 1); \
        gtk_grid_attach(GTK_GRID(grid), v_lbl, 1, row, 1, 1); \
        row++; \
    } while(0)

    ADD_INFO_ROW("Path:", path, FALSE);

    char type_buf[64];
    if (S_ISDIR(st.st_mode)) strcpy(type_buf, "Directory");
    else if (S_ISLNK(st.st_mode)) strcpy(type_buf, "Symbolic Link");
    else if (S_ISREG(st.st_mode)) strcpy(type_buf, "Regular File");
    else strcpy(type_buf, "Special File");
    ADD_INFO_ROW("Type:", type_buf, FALSE);

    if (S_ISDIR(st.st_mode)) {
        ADD_INFO_ROW("Size:", "Calculating...", TRUE);
        DirSizeTask *task = g_new0(DirSizeTask, 1);
        task->path = g_strdup(path);
        task->size_label = size_val_lbl;
        g_thread_new("dir-size-scanner", dir_size_worker, task);
    } else {
        char *sz_str = format_size(st.st_size, FALSE);
        char sz_detailed[128];
        snprintf(sz_detailed, sizeof(sz_detailed), "%s (%lld bytes)", sz_str, (long long)st.st_size);
        ADD_INFO_ROW("Size:", sz_detailed, FALSE);
        g_free(sz_str);
    }

    char time_buf[128];
    struct tm *timeinfo = localtime(&st.st_mtime);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", timeinfo);
    ADD_INFO_ROW("Modified:", time_buf, FALSE);

    char uid_buf[32], gid_buf[32];
    snprintf(uid_buf, sizeof(uid_buf), "%d", st.st_uid);
    snprintf(gid_buf, sizeof(gid_buf), "%d", st.st_gid);
    char owner_str[128];
    snprintf(owner_str, sizeof(owner_str), "UID: %s, GID: %s", uid_buf, gid_buf);
    ADD_INFO_ROW("Owner/Group:", owner_str, FALSE);

    #undef ADD_INFO_ROW

    gtk_box_pack_start(GTK_BOX(content), grid, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(base_name);
}

static void on_menu_properties(GtkMenuItem *item, gpointer data) {
    (void)item; (void)data;
    GList *sel = get_selected_paths();
    if (!sel || !sel->data) {
        free_string_list(&sel);
        return;
    }
    open_file_properties_dialog((const char *)sel->data);
    free_string_list(&sel);
}

static void execute_action(ActionId act) {
    switch (act) {
        case ACT_OPEN:
            open_currently_selected();
            if (app.search_query[0] != '\0') {
                app.search_query[0] = '\0';
                update_status_bar_info();
            }
            break;
        case ACT_NEW_FILE: {
            char *name = prompt_text("New File", "File name:", NULL);
            if (!name) break;
            char *full = g_build_filename(app.current_dir, name, NULL);
            FILE *fp = fopen(full, "wx");
            if (fp) {
                fclose(fp);
                push_undo(UNDO_CREATE, NULL, full);
                set_status("Created file: %s", name);
            } else {
                set_status("Creation error: %s", strerror(errno));
            }
            g_free(full);
            g_free(name);
            break;
        }
        case ACT_PIN_CURRENT:
            if (app.current_dir && !g_str_has_prefix(app.current_dir, "trash:///")) {
                pin_path_to_bookmarks(app.current_dir);
            }
            break;
        case ACT_NEW_DIR: {
            char *name = prompt_text("New Directory", "Directory name:", NULL);
            if (!name) break;
            char *full = g_build_filename(app.current_dir, name, NULL);
            if (mkdir(full, 0755) == 0) {
                push_undo(UNDO_CREATE, NULL, full);
                set_status("Created directory: %s", name);
            } else {
                set_status("Creation error: %s", strerror(errno));
            }
            g_free(full);
            g_free(name);
            break;
        }
        case ACT_RENAME: {
            GList *sel = get_selected_paths();
            if (!sel || !sel->data) {
                free_string_list(&sel);
                break;
            }
            char *old_path = sel->data;
            char *old_name = g_path_get_basename(old_path);
            char *new_name = prompt_text("Rename", "New name:", old_name);
            if (new_name && g_strcmp0(old_name, new_name) != 0) {
                char *new_path = g_build_filename(app.current_dir, new_name, NULL);
                if (rename(old_path, new_path) == 0) {
                    push_undo(UNDO_RENAME, old_path, new_path);
                    set_status("Renamed to %s", new_name);
                } else {
                    set_status("Error: %s", strerror(errno));
                }
                g_free(new_path);
            }
            g_free(new_name);
            g_free(old_name);
            free_string_list(&sel);
            break;
        }
        case ACT_TRASH: {
            GList *sel = get_selected_paths();
            if (!sel) break;
            int count = 0;
            for (GList *l = sel; l != NULL; l = l->next) {
                const char *path = l->data;
                GFile *f = g_file_new_for_path(path);
                GError *err = NULL;
                if (g_file_trash(f, NULL, &err)) {
                    char *in_trash = find_latest_in_trash(path);
                    if (in_trash) {
                        push_undo(UNDO_TRASH, in_trash, path);
                        g_free(in_trash);
                    }
                    count++;
                } else {
                    if (err) g_error_free(err);
                    err = NULL;
                    
                    if (delete_recursive(f, &err)) {
                        count++;
                        set_status("Disk doesn't support trash. Deleted permanently: %s", path);
                    } else {
                        set_status("Deletion error: %s", err ? err->message : "unknown");
                        if (err) g_error_free(err);
                    }
                }
                g_object_unref(f);
            }
            free_string_list(&sel);
            if (count > 0 && !strstr(app.status_label ? gtk_label_get_text(GTK_LABEL(app.status_label)) : "", "Deleted permanently")) {
                set_status("Moved to trash / deleted: %d items", count);
            }
            refresh_dir();
            break;
        }
        case ACT_COPY:
        case ACT_CUT: {
            GList *sel = get_selected_paths();
            if (!sel) break;
            free_string_list(&app.clipboard_paths);
            app.clipboard_paths = sel;
            app.clipboard_is_cut = (act == ACT_CUT);
            
            GtkClipboard *clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
            static GtkTargetEntry targets[] = {
                { (gchar *)"text/uri-list", 0, 0 },
                { (gchar *)"UTF8_STRING", 0, 1 },
                { (gchar *)"STRING", 0, 1 }
            };
            
            gtk_clipboard_set_with_data(clip, targets, 3, clipboard_get_func, clipboard_clear_func, NULL);

            set_status("%s: %d items", app.clipboard_is_cut ? "Cut" : "Copied",
                       g_list_length(app.clipboard_paths));
            break;
        }
        case ACT_PASTE: {
            GList *paths_to_paste = app.clipboard_paths;
            gboolean is_cut = app.clipboard_is_cut;
            gboolean from_internal_clip = (paths_to_paste != NULL);

            if (!paths_to_paste) {
                paths_to_paste = get_clipboard_paths_from_system();
                is_cut = FALSE;
            }

            if (!paths_to_paste) {
                set_status("Nothing to paste");
                break;
            }

            if (from_internal_clip) {
                app.clipboard_paths = NULL;
                app.clipboard_is_cut = FALSE;
            }

            int success_count = 0;
            for (GList *l = paths_to_paste; l != NULL; l = l->next) {
                char *src_path = (char *)l->data;
                success_count += process_file_operation(src_path, app.current_dir, is_cut);
            }

            if (success_count > 0) {
                refresh_dir();
                set_status("%s %d items successfully", is_cut ? "Moved" : "Pasted", success_count);
            }

            free_string_list(&paths_to_paste);
            break;
        }
        case ACT_UNDO:
            perform_undo();
            break;
        case ACT_TERM: {
            char *argv[] = { "/bin/sh", "-c", app.terminal_cmd, NULL };
            GError *err = NULL;
            if (!g_spawn_async(app.current_dir, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err)) {
                set_status("Terminal error: %s", err ? err->message : "");
                if (err) g_error_free(err);
            }
            break;
        }
        case ACT_UP: {
            char *parent = g_path_get_dirname(app.current_dir);
            if (g_strcmp0(parent, app.current_dir) != 0) {
                navigate_to(parent, TRUE);
            }
            g_free(parent);
            break;
        }
        case ACT_HOME: {
            const char *home = g_get_home_dir();
            if (home && *home) {
                navigate_to(home, TRUE);
            }
            break;
        }
        case ACT_BACK:
            go_back();
            break;
        case ACT_FORWARD:
            go_forward();
            break;
        case ACT_REFRESH:
            refresh_dir();
            break;
        case ACT_HIDDEN:
            app.show_hidden = !app.show_hidden;
            save_config();
            refresh_dir();
            break;
        case ACT_FOCUS_PATH:
            gtk_widget_grab_focus(app.path_entry);
            gtk_editable_select_region(GTK_EDITABLE(app.path_entry), 0, -1);
            break;
        case ACT_SETTINGS:
            open_settings_dialog();
            break;
        case ACT_CUSTOM_MENU_CFG:
            open_custom_actions_dialog();
            break;
        case ACT_VIEW_LIST:
            switch_view_mode(VIEW_MODE_LIST);
            break;
        case ACT_VIEW_ICONS:
            switch_view_mode(VIEW_MODE_ICONS);
            break;
        case ACT_SORT_NAME:
            set_sort_mode(app.sort_mode == SORT_NAME_ASC ? SORT_NAME_DESC : SORT_NAME_ASC);
            break;
        case ACT_SORT_SIZE:
            set_sort_mode(SORT_SIZE);
            break;
        case ACT_SORT_TIME:
            set_sort_mode(SORT_TIME);
            break;
        default:
            break;
    }
}

static void on_menu_new_file(GtkMenuItem *item, gpointer data) { (void)item; (void)data; execute_action(ACT_NEW_FILE); }
static void on_menu_new_dir(GtkMenuItem *item, gpointer data)  { (void)item; (void)data; execute_action(ACT_NEW_DIR); }
static void on_menu_paste(GtkMenuItem *item, gpointer data)    { (void)item; (void)data; execute_action(ACT_PASTE); }
static void on_menu_refresh(GtkMenuItem *item, gpointer data)  { (void)item; (void)data; execute_action(ACT_REFRESH); }
static void on_menu_term(GtkMenuItem *item, gpointer data)     { (void)item; (void)data; execute_action(ACT_TERM); }
static void on_menu_settings(GtkMenuItem *item, gpointer data) { (void)item; (void)data; execute_action(ACT_SETTINGS); }
static void on_menu_view_list(GtkMenuItem *item, gpointer data)  { (void)item; (void)data; execute_action(ACT_VIEW_LIST); }
static void on_menu_view_icons(GtkMenuItem *item, gpointer data) { (void)item; (void)data; execute_action(ACT_VIEW_ICONS); }
static void on_menu_sort_name(GtkMenuItem *item, gpointer data) { (void)item; (void)data; execute_action(ACT_SORT_NAME); }
static void on_menu_sort_size(GtkMenuItem *item, gpointer data) { (void)item; (void)data; execute_action(ACT_SORT_SIZE); }
static void on_menu_sort_time(GtkMenuItem *item, gpointer data) { (void)item; (void)data; execute_action(ACT_SORT_TIME); }

static void on_menu_open(GtkMenuItem *item, gpointer data) {
    (void)item; (void)data;
    GList *sel = get_selected_paths();
    if (!sel || !sel->data) {
        free_string_list(&sel);
        return;
    }
    const char *path = sel->data;
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        navigate_to(path, TRUE);
    } else {
        char *uri = g_filename_to_uri(path, NULL, NULL);
        if (uri) {
            g_app_info_launch_default_for_uri(uri, NULL, NULL);
            g_free(uri);
        }
    }
    free_string_list(&sel);
}

static void on_menu_pin_folder(GtkMenuItem *item, gpointer data) {
    (void)item; (void)data;
    GList *sel = get_selected_paths();
    if (sel && sel->data) {
        pin_path_to_bookmarks((const char *)sel->data);
    }
    free_string_list(&sel);
}

static void on_menu_rename(GtkMenuItem *item, gpointer data) { (void)item; (void)data; execute_action(ACT_RENAME); }
static void on_menu_copy(GtkMenuItem *item, gpointer data)   { (void)item; (void)data; execute_action(ACT_COPY); }
static void on_menu_cut(GtkMenuItem *item, gpointer data)    { (void)item; (void)data; execute_action(ACT_CUT); }
static void on_menu_trash(GtkMenuItem *item, gpointer data)  { (void)item; (void)data; execute_action(ACT_TRASH); }

static gboolean handle_context_menu(GdkEventButton *ev, gboolean is_row) {
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item;

    gboolean in_trash = g_str_has_prefix(app.current_dir, "trash:///");

    if (is_row) {
        item = gtk_menu_item_new_with_label("Open");
        g_signal_connect(item, "activate", G_CALLBACK(on_menu_open), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

        GList *sel = get_selected_paths();
        if (sel && sel->data) {
            const char *path = sel->data;

            if (in_trash) {
                item = gtk_menu_item_new_with_label("Restore");
                g_signal_connect(item, "activate", G_CALLBACK(on_menu_restore_from_trash), NULL);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

                item = gtk_menu_item_new_with_label("Delete Permanently");
                g_signal_connect(item, "activate", G_CALLBACK(on_menu_delete_permanently), NULL);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
            } else {
                if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
                    item = gtk_menu_item_new_with_label("Pin to Bookmarks");
                    g_signal_connect(item, "activate", G_CALLBACK(on_menu_pin_folder), NULL);
                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
                } else {
                    item = gtk_menu_item_new_with_label("Set Default App...");
                    g_signal_connect(item, "activate", G_CALLBACK(on_menu_set_default_app), NULL);
                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
                }

                GtkWidget *compress_menu = gtk_menu_new();
                GtkWidget *compress_item = gtk_menu_item_new_with_label("Compress to...");
                gtk_menu_item_set_submenu(GTK_MENU_ITEM(compress_item), compress_menu);

                GtkWidget *sub_zip = gtk_menu_item_new_with_label("ZIP (.zip)");
                g_signal_connect(sub_zip, "activate", G_CALLBACK(on_menu_compress_zip), NULL);
                gtk_menu_shell_append(GTK_MENU_SHELL(compress_menu), sub_zip);

                GtkWidget *sub_tar = gtk_menu_item_new_with_label("TAR.GZ (.tar.gz)");
                g_signal_connect(sub_tar, "activate", G_CALLBACK(on_menu_compress_tar), NULL);
                gtk_menu_shell_append(GTK_MENU_SHELL(compress_menu), sub_tar);

                GtkWidget *sub_7z = gtk_menu_item_new_with_label("7Z (.7z)");
                g_signal_connect(sub_7z, "activate", G_CALLBACK(on_menu_compress_7z), NULL);
                gtk_menu_shell_append(GTK_MENU_SHELL(compress_menu), sub_7z);

                gtk_menu_shell_append(GTK_MENU_SHELL(menu), compress_item);

                const char *ext = strrchr(path, '.');
                gboolean is_arch = ext && (
                    g_ascii_strcasecmp(ext, ".zip") == 0 || g_ascii_strcasecmp(ext, ".7z") == 0 ||
                    g_ascii_strcasecmp(ext, ".rar") == 0 || g_ascii_strcasecmp(ext, ".tar") == 0 ||
                    g_ascii_strcasecmp(ext, ".gz") == 0  || g_ascii_strcasecmp(ext, ".xz") == 0
                );
                if (is_arch) {
                    GtkWidget *extract_menu = gtk_menu_new();
                    GtkWidget *extract_item = gtk_menu_item_new_with_label("Extract...");
                    gtk_menu_item_set_submenu(GTK_MENU_ITEM(extract_item), extract_menu);

                    GtkWidget *sub_ext = gtk_menu_item_new_with_label("Extract Here");
                    g_signal_connect(sub_ext, "activate", G_CALLBACK(on_menu_extract_archive), NULL);
                    gtk_menu_shell_append(GTK_MENU_SHELL(extract_menu), sub_ext);

                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), extract_item);
                }
            }
        }

        if (!in_trash) {
            if (app.custom_actions) {
                GtkWidget *custom_submenu = gtk_menu_new();
                GtkWidget *custom_item = gtk_menu_item_new_with_label("Custom Actions");
                gtk_menu_item_set_submenu(GTK_MENU_ITEM(custom_item), custom_submenu);

                for (GList *l = app.custom_actions; l != NULL; l = l->next) {
                    CustomAction *ca = l->data;
                    GtkWidget *sub_i = gtk_menu_item_new_with_label(ca->name);
                    g_signal_connect(sub_i, "activate", G_CALLBACK(execute_custom_action), ca->command);
                    gtk_menu_shell_append(GTK_MENU_SHELL(custom_submenu), sub_i);
                }
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), custom_item);
            }

            free_string_list(&sel);

            item = gtk_menu_item_new_with_label("Rename");
            g_signal_connect(item, "activate", G_CALLBACK(on_menu_rename), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

            item = gtk_menu_item_new_with_label("Copy");
            g_signal_connect(item, "activate", G_CALLBACK(on_menu_copy), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

            item = gtk_menu_item_new_with_label("Cut");
            g_signal_connect(item, "activate", G_CALLBACK(on_menu_cut), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
            item = gtk_menu_item_new_with_label("Properties...");
            g_signal_connect(item, "activate", G_CALLBACK(on_menu_properties), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
            item = gtk_menu_item_new_with_label("Move to Trash");
            g_signal_connect(item, "activate", G_CALLBACK(on_menu_trash), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        } else {
            free_string_list(&sel);
        }
    } else {
        if (!in_trash) {
            item = gtk_menu_item_new_with_label("New File");
            g_signal_connect(item, "activate", G_CALLBACK(on_menu_new_file), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

            item = gtk_menu_item_new_with_label("New Directory");
            g_signal_connect(item, "activate", G_CALLBACK(on_menu_new_dir), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

            item = gtk_menu_item_new_with_label("Paste");
            g_signal_connect(item, "activate", G_CALLBACK(on_menu_paste), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        }

        GtkWidget *sort_submenu = gtk_menu_new();
        GtkWidget *sort_item = gtk_menu_item_new_with_label("Sort By");
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(sort_item), sort_submenu);

        item = gtk_menu_item_new_with_label("Name");
        g_signal_connect(item, "activate", G_CALLBACK(on_menu_sort_name), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(sort_submenu), item);

        item = gtk_menu_item_new_with_label("Size");
        g_signal_connect(item, "activate", G_CALLBACK(on_menu_sort_size), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(sort_submenu), item);

        item = gtk_menu_item_new_with_label("Date Modified");
        g_signal_connect(item, "activate", G_CALLBACK(on_menu_sort_time), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(sort_submenu), item);

        gtk_menu_shell_append(GTK_MENU_SHELL(menu), sort_item);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

        item = gtk_menu_item_new_with_label("View as List");
        g_signal_connect(item, "activate", G_CALLBACK(on_menu_view_list), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

        item = gtk_menu_item_new_with_label("View as Icons");
        g_signal_connect(item, "activate", G_CALLBACK(on_menu_view_icons), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

        if (!in_trash) {
            item = gtk_menu_item_new_with_label("Open Terminal Here");
            g_signal_connect(item, "activate", G_CALLBACK(on_menu_term), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }

        item = gtk_menu_item_new_with_label("Refresh");
        g_signal_connect(item, "activate", G_CALLBACK(on_menu_refresh), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

        if (!in_trash) {
            item = gtk_menu_item_new_with_label("Preferences");
            g_signal_connect(item, "activate", G_CALLBACK(on_menu_settings), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

            item = gtk_menu_item_new_with_label("Custom Actions Editor");
            g_signal_connect(item, "activate", G_CALLBACK(open_custom_actions_dialog), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }
    }

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

static gboolean on_view_scroll_event(GtkWidget *widget, GdkEventScroll *ev, gpointer data) {
    (void)widget; (void)data;
    if ((ev->state & GDK_CONTROL_MASK) != 0) {
        if (ev->direction == GDK_SCROLL_UP || (ev->direction == GDK_SCROLL_SMOOTH && ev->delta_y < 0)) {
            app.icon_size += 16;
            if (app.icon_size > 256) app.icon_size = 256;
        } else if (ev->direction == GDK_SCROLL_DOWN || (ev->direction == GDK_SCROLL_SMOOTH && ev->delta_y > 0)) {
            app.icon_size -= 16;
            if (app.icon_size < 32) app.icon_size = 32;
        }
        if (app.icon_view && GTK_IS_ICON_VIEW(app.icon_view)) {
            gtk_icon_view_set_item_width(GTK_ICON_VIEW(app.icon_view), app.icon_size + 12);
        }
        save_config();
        refresh_dir();
        return TRUE;
    }
    return FALSE;
}

typedef struct {
    int action_idx;
    GtkWidget *btn;
} BindButtonCtx;

static gboolean on_capture_key_press(GtkWidget *dlg, GdkEventKey *ev, gpointer user_data) {
    if (ev->keyval == GDK_KEY_Control_L || ev->keyval == GDK_KEY_Control_R ||
        ev->keyval == GDK_KEY_Shift_L   || ev->keyval == GDK_KEY_Shift_R ||
        ev->keyval == GDK_KEY_Alt_L     || ev->keyval == GDK_KEY_Alt_R ||
        ev->keyval == GDK_KEY_Super_L   || ev->keyval == GDK_KEY_Super_R) {
        return TRUE;
    }

    BindButtonCtx *ctx = user_data;
    GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask();
    guint keyval = gdk_keyval_to_lower(ev->keyval);
    if (ev->keyval >= 0xFF00) keyval = ev->keyval;

    app.binds[ctx->action_idx].keyval = keyval;
    app.binds[ctx->action_idx].mods = mods;

    char *label = gtk_accelerator_get_label(keyval, mods);
    gtk_button_set_label(GTK_BUTTON(ctx->btn), label);
    g_free(label);

    save_config();
    gtk_widget_destroy(dlg);
    return TRUE;
}

static void on_bind_button_clicked(GtkButton *btn, gpointer user_data) {
    BindButtonCtx *ctx = user_data;
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(btn));
    GtkWidget *cap = gtk_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(cap), GTK_WINDOW(top));
    gtk_window_set_modal(GTK_WINDOW(cap), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(cap), TRUE);
    gtk_window_set_title(GTK_WINDOW(cap), "Keybinding");

    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(cap));
    gtk_container_set_border_width(GTK_CONTAINER(area), 20);
    GtkWidget *lbl = gtk_label_new("Press desired key combination...");
    gtk_box_pack_start(GTK_BOX(area), lbl, TRUE, TRUE, 0);

    g_signal_connect(cap, "key-press-event", G_CALLBACK(on_capture_key_press), ctx);
    gtk_widget_show_all(cap);
    gtk_dialog_run(GTK_DIALOG(cap));
    if (GTK_IS_WIDGET(cap)) gtk_widget_destroy(cap);
}

static void open_settings_dialog(void) {
    GtkWidget *dlg = gtk_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(app.window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_title(GTK_WINDOW(dlg), "Preferences");
    gtk_window_set_default_size(GTK_WINDOW(dlg), 480, 560);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 10);

    GtkWidget *term_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *term_lbl = gtk_label_new("Terminal command:");
    GtkWidget *term_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(term_entry), app.terminal_cmd);
    gtk_box_pack_start(GTK_BOX(term_box), term_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(term_box), term_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), term_box, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 6);

    BindButtonCtx *ctxs = g_new0(BindButtonCtx, ACT_COUNT);

    int valid_count = 0;
    for (int i = 0; i < ACT_COUNT; i++) {
        if (!app.binds[i].id) continue;

        GtkWidget *lbl = gtk_label_new(app.binds[i].label);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_widget_set_hexpand(lbl, TRUE);

        char *accel_lbl = gtk_accelerator_get_label(app.binds[i].keyval, app.binds[i].mods);
        GtkWidget *btn = gtk_button_new_with_label(accel_lbl ? accel_lbl : "");
        g_free(accel_lbl);

        ctxs[valid_count].action_idx = i;
        ctxs[valid_count].btn = btn;
        g_signal_connect(btn, "clicked", G_CALLBACK(on_bind_button_clicked), &ctxs[valid_count]);

        gtk_grid_attach(GTK_GRID(grid), lbl, 0, valid_count, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), btn, 1, valid_count, 1, 1);
        valid_count++;
    }

    gtk_container_add(GTK_CONTAINER(scroll), grid);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);
    gtk_widget_show_all(dlg);

    gtk_dialog_run(GTK_DIALOG(dlg));

    g_free(app.terminal_cmd);
    app.terminal_cmd = g_strdup(gtk_entry_get_text(GTK_ENTRY(term_entry)));
    save_config();

    g_free(ctxs);
    gtk_widget_destroy(dlg);
}

static void refresh_custom_actions_list(GtkWidget *list_box);

static void on_edit_custom_action(GtkButton *btn, gpointer user_data) {
    CustomAction *ca = user_data;
    if (!ca) return;

    char *new_name = prompt_text("Edit Custom Action", "Action Name:", ca->name);
    if (!new_name) return;
    char *new_cmd = prompt_text("Edit Custom Action", "Shell Command (use %s for file):", ca->command);
    if (!new_cmd) {
        g_free(new_name);
        return;
    }

    g_free(ca->name);
    g_free(ca->command);
    ca->name = new_name;
    ca->command = new_cmd;
    save_config();

    GtkWidget *list_box = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_LIST_BOX);
    if (list_box) {
        refresh_custom_actions_list(list_box);
    }
}

static void on_delete_custom_action(GtkButton *btn, gpointer user_data) {
    CustomAction *ca = user_data;
    if (!ca) return;

    app.custom_actions = g_list_remove(app.custom_actions, ca);
    g_free(ca->name);
    g_free(ca->command);
    g_free(ca);
    save_config();

    GtkWidget *list_box = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_LIST_BOX);
    if (list_box) {
        refresh_custom_actions_list(list_box);
    }
}

static void on_move_custom_action_up(GtkButton *btn, gpointer user_data) {
    CustomAction *ca = user_data;
    GList *link = g_list_find(app.custom_actions, ca);
    if (!link || !link->prev) return;

    gpointer prev_data = link->prev->data;
    link->prev->data = link->data;
    link->data = prev_data;

    save_config();

    GtkWidget *list_box = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_LIST_BOX);
    if (list_box) {
        refresh_custom_actions_list(list_box);
    }
}

static void on_move_custom_action_down(GtkButton *btn, gpointer user_data) {
    CustomAction *ca = user_data;
    GList *link = g_list_find(app.custom_actions, ca);
    if (!link || !link->next) return;

    gpointer next_data = link->next->data;
    link->next->data = link->data;
    link->data = next_data;

    save_config();

    GtkWidget *list_box = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_LIST_BOX);
    if (list_box) {
        refresh_custom_actions_list(list_box);
    }
}

static void refresh_custom_actions_list(GtkWidget *list_box) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(list_box));
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);

    for (GList *l = app.custom_actions; l != NULL; l = l->next) {
        CustomAction *ca = l->data;

        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(row_box), 4);

        char *txt = g_strdup_printf("<b>%s</b>\n<small>%s</small>", ca->name, ca->command);
        GtkWidget *lbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(lbl), txt);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_widget_set_hexpand(lbl, TRUE);
        g_free(txt);

        GtkWidget *btn_up = gtk_button_new_from_icon_name("go-up", GTK_ICON_SIZE_BUTTON);
        GtkWidget *btn_down = gtk_button_new_from_icon_name("go-down", GTK_ICON_SIZE_BUTTON);
        GtkWidget *btn_edit = gtk_button_new_with_label("Edit");
        GtkWidget *btn_del = gtk_button_new_with_label("Delete");

        g_signal_connect(btn_up, "clicked", G_CALLBACK(on_move_custom_action_up), ca);
        g_signal_connect(btn_down, "clicked", G_CALLBACK(on_move_custom_action_down), ca);
        g_signal_connect(btn_edit, "clicked", G_CALLBACK(on_edit_custom_action), ca);
        g_signal_connect(btn_del, "clicked", G_CALLBACK(on_delete_custom_action), ca);

        gtk_box_pack_start(GTK_BOX(row_box), lbl, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(row_box), btn_up, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row_box), btn_down, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row_box), btn_edit, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row_box), btn_del, FALSE, FALSE, 0);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), row_box);
        gtk_container_add(GTK_CONTAINER(list_box), row);
    }
    gtk_widget_show_all(list_box);
}

static void on_add_custom_action_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GtkWidget *list_box = user_data;
    char *name = prompt_text("New Custom Action", "Action Name:", NULL);
    if (!name) return;
    char *cmd = prompt_text("New Custom Action", "Shell Command (use %s for file):", NULL);
    if (!cmd) {
        g_free(name);
        return;
    }

    CustomAction *ca = g_new0(CustomAction, 1);
    ca->name = name;
    ca->command = cmd;
    app.custom_actions = g_list_append(app.custom_actions, ca);
    save_config();

    refresh_custom_actions_list(list_box);
}

static void open_custom_actions_dialog(void) {
    GtkWidget *dlg = gtk_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(app.window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_title(GTK_WINDOW(dlg), "Custom Actions Editor");
    gtk_window_set_default_size(GTK_WINDOW(dlg), 500, 420);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 8);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    GtkWidget *list_box = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scroll), list_box);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);

    refresh_custom_actions_list(list_box);

    GtkWidget *add_btn = gtk_button_new_with_label("Add New Action");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_custom_action_clicked), list_box);
    gtk_box_pack_start(GTK_BOX(content), add_btn, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void on_sidebar_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    navigate_to((const char *)user_data, TRUE);
}

typedef struct {
    char *path;
    gboolean is_trash;
    gboolean is_mount;
    GMount *mount;
} SidebarCtx;

static void free_sidebar_ctx(gpointer data, GClosure *closure) {
    (void)closure;
    SidebarCtx *ctx = data;
    if (!ctx) return;
    g_free(ctx->path);
    if (ctx->mount) g_object_unref(ctx->mount);
    g_free(ctx);
}

static void closure_free_notify(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static void on_sidebar_unpin(GtkMenuItem *item, gpointer data) {
    (void)item;
    SidebarCtx *ctx = data;
    if (ctx && ctx->path) {
        unpin_path_from_bookmarks(ctx->path);
    }
}

static void on_sidebar_empty_trash(GtkMenuItem *item, gpointer data) {
    (void)item; (void)data;
    empty_trash_fully();
}

static void on_sidebar_eject(GtkMenuItem *item, gpointer data) {
    (void)item;
    SidebarCtx *ctx = data;
    if (ctx && ctx->mount) {
        g_mount_eject_with_operation(ctx->mount, G_MOUNT_UNMOUNT_NONE, NULL, NULL, NULL, NULL);
        set_status("Ejected device");
        load_bookmarks();
    }
}

static gboolean on_sidebar_button_press(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    (void)widget;
    SidebarCtx *ctx = data;
    
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) {
        gboolean is_default_system = (ctx->path && (g_strcmp0(ctx->path, g_get_home_dir()) == 0)) || 
                                     (ctx->path && (g_strcmp0(ctx->path, g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP) ? g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP) : "") == 0));
        
        if (is_default_system && !is_path_in_gtk_bookmarks(ctx->path)) {
            return TRUE; 
        }

        GtkWidget *menu = gtk_menu_new();
        GtkWidget *item;

        if (ctx->is_trash) {
            item = gtk_menu_item_new_with_label("Empty Trash");
            g_signal_connect(item, "activate", G_CALLBACK(on_sidebar_empty_trash), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        } else if (ctx->is_mount && ctx->mount) {
            item = gtk_menu_item_new_with_label("Eject");
            g_signal_connect(item, "activate", G_CALLBACK(on_sidebar_eject), ctx);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        } else if (ctx->path) {
            item = gtk_menu_item_new_with_label("Unpin");
            g_signal_connect(item, "activate", G_CALLBACK(on_sidebar_unpin), ctx);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }

        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
        return TRUE;
    }
    return FALSE;
}

static void on_bookmark_drag_data_get(GtkWidget *widget, GdkDragContext *context, GtkSelectionData *selection_data, guint info, guint time, gpointer data) {
    (void)context; (void)info; (void)time;
    SidebarCtx *ctx = data;
    if (ctx && ctx->path) {
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), 8, (const guchar *)ctx->path, strlen(ctx->path));
    } else {
        const char *name = gtk_widget_get_name(widget);
        if (name) gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), 8, (const guchar *)name, strlen(name));
    }
}

static void on_bookmark_drag_data_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y, GtkSelectionData *selection_data, guint info, guint time, gpointer data) {
    (void)context; (void)info; (void)time; (void)x;
    const guchar *src_data = gtk_selection_data_get_data(selection_data);
    if (!src_data) return;
    SidebarCtx *target_ctx = data;
    if (!target_ctx || !target_ctx->path) return;

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    gboolean insert_after = (y > alloc.height / 2);

    char *bm_path = g_build_filename(g_get_user_config_dir(), "gtk-3.0", "bookmarks", NULL);
    if (!g_file_test(bm_path, G_FILE_TEST_EXISTS)) {
        g_free(bm_path);
        bm_path = g_build_filename(g_get_home_dir(), ".gtk-bookmarks", NULL);
    }

    char *content = NULL;
    if (g_file_get_contents(bm_path, &content, NULL, NULL)) {
        char **lines = g_strsplit(content, "\n", -1);
        GPtrArray *new_lines = g_ptr_array_new_with_free_func(g_free);
        char *src_uri = g_filename_to_uri((const char *)src_data, NULL, NULL);
        char *target_uri = g_filename_to_uri(target_ctx->path, NULL, NULL);

        char *moved_line = NULL;
        for (int i = 0; lines[i] != NULL; i++) {
            if (lines[i][0] == '\0') continue;
            if (src_uri && g_str_has_prefix(lines[i], src_uri)) {
                moved_line = g_strdup(lines[i]);
            } else {
                g_ptr_array_add(new_lines, g_strdup(lines[i]));
            }
        }

        if (moved_line) {
            GPtrArray *final_lines = g_ptr_array_new_with_free_func(g_free);
            gboolean inserted = FALSE;
            for (guint i = 0; i < new_lines->len; i++) {
                char *l = g_ptr_array_index(new_lines, i);
                if (!inserted && target_uri && g_str_has_prefix(l, target_uri)) {
                    if (!insert_after) {
                        g_ptr_array_add(final_lines, g_strdup(moved_line));
                        inserted = TRUE;
                    }
                    g_ptr_array_add(final_lines, g_strdup(l));
                    if (insert_after) {
                        g_ptr_array_add(final_lines, g_strdup(moved_line));
                        inserted = TRUE;
                    }
                } else {
                    g_ptr_array_add(final_lines, g_strdup(l));
                }
            }
            if (!inserted) {
                g_ptr_array_add(final_lines, g_strdup(moved_line));
            }

            FILE *fp = fopen(bm_path, "w");
            if (fp) {
                for (guint i = 0; i < final_lines->len; i++) {
                    fprintf(fp, "%s\n", (char *)g_ptr_array_index(final_lines, i));
                }
                fclose(fp);
            }
            g_ptr_array_free(final_lines, TRUE);
            g_free(moved_line);
        }

        g_free(src_uri);
        g_free(target_uri);
        g_ptr_array_free(new_lines, TRUE);
        g_strfreev(lines);
        g_free(content);
        load_bookmarks();
    }
    g_free(bm_path);
}

static void add_sidebar_item(const char *label, const char *path, const char *icon_name, gboolean is_trash, gboolean is_mount, GMount *mount) {
    if (!path && !is_trash && !is_mount) return;

    GtkWidget *btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_widget_set_halign(btn, GTK_ALIGN_FILL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *img = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
    GtkWidget *lbl = gtk_label_new(label);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);

    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), lbl, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(btn), box);

    SidebarCtx *ctx = g_new0(SidebarCtx, 1);
    ctx->path = g_strdup(path);
    ctx->is_trash = is_trash;
    ctx->is_mount = is_mount;
    ctx->mount = mount ? g_object_ref(mount) : NULL;

    if (!is_trash && path) {
        g_signal_connect_data(btn, "clicked", G_CALLBACK(on_sidebar_clicked),
                              g_strdup(path), closure_free_notify, 0);
    } else if (is_trash) {
        g_signal_connect_data(btn, "clicked", G_CALLBACK(on_sidebar_clicked),
                              g_strdup("trash:///"), closure_free_notify, 0);
    } else if (is_mount && path) {
        g_signal_connect_data(btn, "clicked", G_CALLBACK(on_sidebar_clicked),
                              g_strdup(path), closure_free_notify, 0);
    }

    g_signal_connect_data(btn, "button-press-event", G_CALLBACK(on_sidebar_button_press),
                          ctx, (GClosureNotify)free_sidebar_ctx, 0);

    if (path && !is_trash && !is_mount) {
        static GtkTargetEntry targets[] = { { (gchar *) "STRING", 0, 0 } };
        gtk_drag_source_set(btn, GDK_BUTTON1_MASK, targets, 1, GDK_ACTION_MOVE);
        gtk_drag_dest_set(btn, GTK_DEST_DEFAULT_HIGHLIGHT | GTK_DEST_DEFAULT_ALL, targets, 1, GDK_ACTION_MOVE);
        g_signal_connect(btn, "drag-data-get", G_CALLBACK(on_bookmark_drag_data_get), ctx);
        g_signal_connect(btn, "drag-data-received", G_CALLBACK(on_bookmark_drag_data_received), ctx);
    }

    gtk_box_pack_start(GTK_BOX(app.bookmarks_box), btn, FALSE, FALSE, 0);
}

static void load_bookmarks(void) {
    if (!app.bookmarks_box) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(app.bookmarks_box));
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);

    add_sidebar_item("Home", g_get_home_dir(), "user-home", FALSE, FALSE, NULL);
    
    const char *desktop_path = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    if (!desktop_path) {
        desktop_path = g_build_filename(g_get_home_dir(), "Desktop", NULL);
    }
    add_sidebar_item("Desktop", desktop_path, "user-desktop", FALSE, FALSE, NULL);
    
    char *trash_files_dir = g_build_filename(g_get_user_data_dir(), "Trash", "files", NULL);
    gboolean trash_has_items = FALSE;
    GDir *tdir = g_dir_open(trash_files_dir, 0, NULL);
    if (tdir) {
        if (g_dir_read_name(tdir) != NULL) {
            trash_has_items = TRUE;
        }
        g_dir_close(tdir);
    }
    g_free(trash_files_dir);

    add_sidebar_item("Trash", "trash:///", trash_has_items ? "user-trash-full" : "user-trash", TRUE, FALSE, NULL);

    GVolumeMonitor *monitor = g_volume_monitor_get();
    GList *mounts = g_volume_monitor_get_mounts(monitor);
    for (GList *l = mounts; l != NULL; l = l->next) {
        GMount *mount = G_MOUNT(l->data);
        GFile *root = g_mount_get_root(mount);
        char *path = g_file_get_path(root);
        char *name = g_mount_get_name(mount);

        if (path && name) {
            add_sidebar_item(name, path, "drive-removable-media", FALSE, TRUE, mount);
        }

        g_free(path);
        g_free(name);
        g_object_unref(root);
        g_object_unref(mount);
    }
    g_list_free(mounts);
    g_object_unref(monitor);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(app.bookmarks_box), sep, FALSE, FALSE, 4);

    char *bm_path = g_build_filename(g_get_user_config_dir(), "gtk-3.0", "bookmarks", NULL);
    if (!g_file_test(bm_path, G_FILE_TEST_EXISTS)) {
        g_free(bm_path);
        bm_path = g_build_filename(g_get_home_dir(), ".gtk-bookmarks", NULL);
    }

    char *content = NULL;
    if (g_file_get_contents(bm_path, &content, NULL, NULL)) {
        char **lines = g_strsplit(content, "\n", -1);
        for (int i = 0; lines[i] != NULL; i++) {
            char *line = g_strstrip(lines[i]);
            if (*line == '\0' || *line == '#') continue; 

            char *space = strchr(line, ' ');
            char *uri = space ? g_strndup(line, space - line) : g_strdup(line);
            char *custom_name = space ? g_strstrip(space + 1) : NULL;

            char *path = g_filename_from_uri(uri, NULL, NULL);
            if (path) {
                char *base = g_path_get_basename(path);
                add_sidebar_item((custom_name && *custom_name) ? custom_name : (base ? base : ""), path, "folder", FALSE, FALSE, NULL);
                g_free(base);
                g_free(path);
            }
            g_free(uri);
        }
        g_strfreev(lines);
        g_free(content);
    }
    g_free(bm_path);
    gtk_widget_show_all(app.bookmarks_box);
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data) {
    (void)col; (void)data;
    GtkTreeModel *model = gtk_tree_view_get_model(tv);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) return;

    char *full_path = NULL;
    gboolean is_dir = FALSE;
    gtk_tree_model_get(model, &iter, COL_FULL_PATH, &full_path, COL_IS_DIR, &is_dir, -1);

    if (g_str_has_prefix(app.current_dir, "trash:///")) {
        set_status("Operation not supported directly from trash view");
        g_free(full_path);
        return;
    }

    if (is_dir) {
        navigate_to(full_path, TRUE);
    } else {
        char *uri = g_filename_to_uri(full_path, NULL, NULL);
        if (uri) {
            g_app_info_launch_default_for_uri(uri, NULL, NULL);
            g_free(uri);
        }
    }
    g_free(full_path);
}

static void on_icon_activated(GtkIconView *iv, GtkTreePath *path, gpointer data) {
    (void)data;
    GtkTreeModel *model = gtk_icon_view_get_model(iv);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) return;

    char *full_path = NULL;
    gboolean is_dir = FALSE;
    gtk_tree_model_get(model, &iter, COL_FULL_PATH, &full_path, COL_IS_DIR, &is_dir, -1);

    if (g_str_has_prefix(app.current_dir, "trash:///")) {
        set_status("Operation not supported directly from trash view");
        g_free(full_path);
        return;
    }

    if (is_dir) {
        navigate_to(full_path, TRUE);
    } else {
        char *uri = g_filename_to_uri(full_path, NULL, NULL);
        if (uri) {
            g_app_info_launch_default_for_uri(uri, NULL, NULL);
            g_free(uri);
        }
    }
    g_free(full_path);
}

static void on_selection_changed(gpointer data) {
    (void)data;
    update_status_bar_info();
}

static void on_path_activate(GtkEntry *entry, gpointer data) {
    (void)data;
    navigate_to(gtk_entry_get_text(entry), TRUE);
    if (app.view_mode == VIEW_MODE_LIST && app.tree_view && GTK_IS_WIDGET(app.tree_view)) {
        gtk_widget_grab_focus(app.tree_view);
    } else if (app.icon_view && GTK_IS_WIDGET(app.icon_view)) {
        gtk_widget_grab_focus(app.icon_view);
    }
}

static int count_substring_matches(const char *str, const char *sub) {
    if (!str || !sub || *sub == '\0') return 0;
    int count = 0;
    char *lower_str = g_utf8_strdown(str, -1);
    char *lower_sub = g_utf8_strdown(sub, -1);
    char *p = lower_str;
    while ((p = strstr(p, lower_sub)) != NULL) {
        count++;
        p += strlen(lower_sub);
    }
    g_free(lower_str);
    g_free(lower_sub);
    return count;
}

static void process_quick_search_input(guint keyval) {
    if (keyval == GDK_KEY_BackSpace) {
        int len = strlen(app.search_query);
        if (len > 0) {
            while (len > 0) {
                len--;
                if ((app.search_query[len] & 0xC0) != 0x80) {
                    app.search_query[len] = '\0';
                    break;
                }
                app.search_query[len] = '\0';
            }
        }
    } else {
        gunichar uc = gdk_keyval_to_unicode(keyval);
        if (uc != 0 && g_unichar_isprint(uc)) {
            char buf[8] = {0};
            int bytes = g_unichar_to_utf8(uc, buf);
            if (strlen(app.search_query) + bytes < sizeof(app.search_query) - 1) {
                strcat(app.search_query, buf);
            }
        }
    }

    if (app.search_query[0] == '\0') {
        update_status_bar_info();
        return;
    }

    set_status_markup("Search: <span foreground=\"#e5c07b\" font_weight=\"bold\"><b>%s</b></span>", app.search_query);

    if (!app.list_store || !GTK_IS_TREE_MODEL(app.list_store)) return;
    GtkTreeModel *model = GTK_TREE_MODEL(app.list_store);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    
    int best_score = -1;
    GtkTreePath *best_path = NULL;

    while (valid) {
        char *name = NULL;
        gtk_tree_model_get(model, &iter, COL_NAME, &name, -1);
        if (name) {
            int score = count_substring_matches(name, app.search_query);
            char *lower_name = g_utf8_strdown(name, -1);
            char *lower_query = g_utf8_strdown(app.search_query, -1);
            if (g_str_has_prefix(lower_name, lower_query)) {
                score += 1000;
            }
            g_free(lower_name);
            g_free(lower_query);

            if (score > best_score) {
                best_score = score;
                if (best_path) gtk_tree_path_free(best_path);
                best_path = gtk_tree_model_get_path(model, &iter);
            }
            g_free(name);
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }

    if (best_path && best_score > 0) {
        if (app.view_mode == VIEW_MODE_LIST && app.tree_view && GTK_IS_TREE_VIEW(app.tree_view)) {
            GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.tree_view));
            gtk_tree_selection_unselect_all(sel);
            gtk_tree_selection_select_path(sel, best_path);
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(app.tree_view), best_path, NULL, FALSE);
            gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(app.tree_view), best_path, NULL, FALSE, 0, 0);
        } else if (app.icon_view && GTK_IS_ICON_VIEW(app.icon_view)) {
            gtk_icon_view_unselect_all(GTK_ICON_VIEW(app.icon_view));
            gtk_icon_view_select_path(GTK_ICON_VIEW(app.icon_view), best_path);
            gtk_icon_view_set_cursor(GTK_ICON_VIEW(app.icon_view), best_path, NULL, FALSE);
            gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(app.icon_view), best_path, FALSE, 0, 0);
        }
        gtk_tree_path_free(best_path);
    }
}

static void start_path_entry_with_text(const char *initial_text) {
    if (!app.path_entry || !GTK_IS_ENTRY(app.path_entry)) return;
    if (app.search_query[0] != '\0') {
        app.search_query[0] = '\0';
        update_status_bar_info();
    }
    gtk_widget_grab_focus(app.path_entry);
    gtk_entry_set_text(GTK_ENTRY(app.path_entry), initial_text ? initial_text : "");
    gtk_editable_set_position(GTK_EDITABLE(app.path_entry), -1);
}

static gboolean on_main_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    (void)w; (void)data;

    if (gtk_widget_has_focus(app.path_entry)) {
        if (ev->keyval == GDK_KEY_Escape) {
            gtk_entry_set_text(GTK_ENTRY(app.path_entry), app.current_dir);
            if (app.view_mode == VIEW_MODE_LIST && app.tree_view && GTK_IS_WIDGET(app.tree_view)) gtk_widget_grab_focus(app.tree_view);
            else if (app.icon_view && GTK_IS_WIDGET(app.icon_view)) gtk_widget_grab_focus(app.icon_view);
            return TRUE;
        }
        return FALSE;
    }

    if (ev->keyval == GDK_KEY_Escape) {
        if (app.search_query[0] != '\0') {
            app.search_query[0] = '\0';
            update_status_bar_info();
            return TRUE;
        }
        if (clear_selection_if_any()) {
            return TRUE;
        }
    }

    if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) {
        open_currently_selected();
        if (app.search_query[0] != '\0') {
            app.search_query[0] = '\0';
            update_status_bar_info();
        }
        return TRUE;
    }

    if (ev->keyval == GDK_KEY_BackSpace) {
        if (app.search_query[0] != '\0') {
            process_quick_search_input(ev->keyval);
            return TRUE;
        }
    }

    if (ev->keyval == GDK_KEY_Up || ev->keyval == GDK_KEY_Down) {
        if (app.view_mode == VIEW_MODE_LIST && app.tree_view && GTK_IS_TREE_VIEW(app.tree_view)) {
            GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.tree_view));
            if (!gtk_tree_selection_count_selected_rows(sel)) {
                GtkTreeIter iter;
                if (app.list_store && GTK_IS_TREE_MODEL(app.list_store) && gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app.list_store), &iter)) {
                    GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(app.list_store), &iter);
                    gtk_tree_selection_select_path(sel, path);
                    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(app.tree_view), path, NULL, FALSE, 0, 0);
                    gtk_tree_path_free(path);
                    return TRUE;
                }
            }
        } else if (app.view_mode == VIEW_MODE_ICONS && app.icon_view && GTK_IS_ICON_VIEW(app.icon_view)) {
            GList *selected = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(app.icon_view));
            if (!selected) {
                GtkTreeIter iter;
                if (app.list_store && GTK_IS_TREE_MODEL(app.list_store) && gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app.list_store), &iter)) {
                    GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(app.list_store), &iter);
                    gtk_icon_view_select_path(GTK_ICON_VIEW(app.icon_view), path);
                    gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(app.icon_view), path, FALSE, 0, 0);
                    gtk_tree_path_free(path);
                    return TRUE;
                }
            }
            g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
        }
        return FALSE;
    }

    GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask();
    guint keyval = (ev->keyval >= 0xFF00) ? ev->keyval : gdk_keyval_to_lower(ev->keyval);

    for (int i = 0; i < ACT_COUNT; i++) {
        if (!app.binds[i].id) continue;
        if (app.binds[i].keyval == keyval && app.binds[i].mods == mods) {
            execute_action((ActionId)i);
            return TRUE;
        }
    }

    if ((mods & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK)) != 0) {
        return FALSE;
    }

    if (ev->keyval == GDK_KEY_slash) {
        start_path_entry_with_text("/");
        return TRUE;
    }

    if (ev->keyval == GDK_KEY_asciitilde) {
        start_path_entry_with_text("~/");
        return TRUE;
    }

    if (gdk_keyval_to_unicode(ev->keyval) != 0 && g_unichar_isprint(gdk_keyval_to_unicode(ev->keyval))) {
        process_quick_search_input(ev->keyval);
        return TRUE;
    }

    return FALSE;
}

static void build_ui(void) {
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "fm");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 850, 600);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(app.window, "key-press-event", G_CALLBACK(on_main_key_press), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app.window), vbox);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 4);

    GtkWidget *btn_back  = gtk_button_new_from_icon_name("go-previous", GTK_ICON_SIZE_BUTTON);
    GtkWidget *btn_fwd   = gtk_button_new_from_icon_name("go-next", GTK_ICON_SIZE_BUTTON);
    GtkWidget *btn_up    = gtk_button_new_from_icon_name("go-up", GTK_ICON_SIZE_BUTTON);

    g_signal_connect_swapped(btn_back, "clicked", G_CALLBACK(execute_action), GINT_TO_POINTER(ACT_BACK));
    g_signal_connect_swapped(btn_fwd,  "clicked", G_CALLBACK(execute_action), GINT_TO_POINTER(ACT_FORWARD));
    g_signal_connect_swapped(btn_up,   "clicked", G_CALLBACK(execute_action), GINT_TO_POINTER(ACT_UP));

    app.path_entry = gtk_entry_new();
    g_signal_connect(app.path_entry, "activate", G_CALLBACK(on_path_activate), NULL);

    gtk_box_pack_start(GTK_BOX(toolbar), btn_back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_fwd,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_up,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app.path_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    GtkWidget *main_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(main_paned), 180);

    GtkWidget *bm_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(bm_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    app.bookmarks_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(app.bookmarks_box), 4);
    gtk_container_add(GTK_CONTAINER(bm_scroll), app.bookmarks_box);
    load_bookmarks();

    app.list_store = gtk_list_store_new(NUM_COLS, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_STRING);

    app.tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app.list_store));
    gtk_tree_view_set_rubber_banding(GTK_TREE_VIEW(app.tree_view), TRUE);
    
    GtkTreeSelection *tree_sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.tree_view));
    gtk_tree_selection_set_mode(tree_sel, GTK_SELECTION_MULTIPLE);
    g_signal_connect_swapped(tree_sel, "changed", G_CALLBACK(on_selection_changed), NULL);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(app.tree_view), COL_NAME);
    g_signal_connect(app.tree_view, "row-activated", G_CALLBACK(on_row_activated), NULL);

    GtkCellRenderer *icon_r = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer *name_r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col_name = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col_name, "Name");
    gtk_tree_view_column_set_expand(col_name, TRUE);
    gtk_tree_view_column_pack_start(col_name, icon_r, FALSE);
    gtk_tree_view_column_pack_start(col_name, name_r, TRUE);
    gtk_tree_view_column_add_attribute(col_name, icon_r, "pixbuf", COL_PIXBUF);
    gtk_tree_view_column_add_attribute(col_name, name_r, "text", COL_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree_view), col_name);

    GtkCellRenderer *size_r = gtk_cell_renderer_text_new();
    gtk_cell_renderer_set_alignment(size_r, 1.0, 0.5);
    GtkTreeViewColumn *col_size = gtk_tree_view_column_new_with_attributes("Size", size_r, "text", COL_SIZE, NULL);
    gtk_tree_view_column_set_min_width(col_size, 90);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree_view), col_size);

    GtkWidget *tree_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(tree_scroll), app.tree_view);

    app.icon_view = gtk_icon_view_new_with_model(GTK_TREE_MODEL(app.list_store));

    gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(app.icon_view), COL_PIXBUF);
    gtk_icon_view_set_text_column(GTK_ICON_VIEW(app.icon_view), COL_NAME);
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(app.icon_view), GTK_SELECTION_MULTIPLE);
    
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(app.icon_view));
    for (GList *l = cells; l != NULL; l = l->next) {
        if (GTK_IS_CELL_RENDERER_TEXT(l->data)) {
            g_object_set(G_OBJECT(l->data),
                         "wrap-mode", PANGO_WRAP_WORD_CHAR,
                         "wrap-width", 140,
                         "ellipsize", PANGO_ELLIPSIZE_NONE,
                         NULL);
        }
    }
    g_list_free(cells);

    gtk_icon_view_set_item_width(GTK_ICON_VIEW(app.icon_view), app.icon_size + 24);
    gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(app.icon_view), 6);
    gtk_icon_view_set_column_spacing(GTK_ICON_VIEW(app.icon_view), 6);
    gtk_icon_view_set_margin(GTK_ICON_VIEW(app.icon_view), 6);

    g_signal_connect_swapped(app.icon_view, "selection-changed", G_CALLBACK(on_selection_changed), NULL);
    g_signal_connect_swapped(app.icon_view, "item-activated", G_CALLBACK(on_icon_activated), NULL);
    g_signal_connect_swapped(app.icon_view, "scroll-event", G_CALLBACK(on_view_scroll_event), NULL);

    {
        static GtkTargetEntry drag_targets[] = {
            { (gchar *)"text/uri-list", 0, 0 }
        };

        gtk_drag_dest_set(app.tree_view, GTK_DEST_DEFAULT_ALL, drag_targets, 1, GDK_ACTION_COPY | GDK_ACTION_MOVE);
        g_signal_connect(app.tree_view, "drag-data-received", G_CALLBACK(on_view_drag_data_received), NULL);

        gtk_drag_dest_set(app.icon_view, GTK_DEST_DEFAULT_ALL, drag_targets, 1, GDK_ACTION_COPY | GDK_ACTION_MOVE);
        g_signal_connect(app.icon_view, "drag-data-received", G_CALLBACK(on_view_drag_data_received), NULL);

        gtk_widget_add_events(app.tree_view, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
        g_signal_connect(app.tree_view, "button-press-event", G_CALLBACK(on_view_button_press), NULL);
        g_signal_connect(app.tree_view, "motion-notify-event", G_CALLBACK(on_view_motion_notify), NULL);
        g_signal_connect(app.tree_view, "button-release-event", G_CALLBACK(on_view_button_release), NULL);
        g_signal_connect(app.tree_view, "drag-data-get", G_CALLBACK(on_view_drag_data_get), NULL);

        gtk_widget_add_events(app.icon_view, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
        g_signal_connect(app.icon_view, "button-press-event", G_CALLBACK(on_view_button_press), NULL);
        g_signal_connect(app.icon_view, "motion-notify-event", G_CALLBACK(on_view_motion_notify), NULL);
        g_signal_connect(app.icon_view, "button-release-event", G_CALLBACK(on_view_button_release), NULL);
        g_signal_connect(app.icon_view, "drag-data-get", G_CALLBACK(on_view_drag_data_get), NULL);
    }

    GtkWidget *icon_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(icon_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(icon_scroll), app.icon_view);

    app.content_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(app.content_stack), tree_scroll, "list");
    gtk_stack_add_named(GTK_STACK(app.content_stack), icon_scroll, "icons");

    if (app.view_mode == VIEW_MODE_LIST) {
        gtk_stack_set_visible_child_name(GTK_STACK(app.content_stack), "list");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(app.content_stack), "icons");
    }

    gtk_paned_pack1(GTK_PANED(main_paned), bm_scroll, FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(main_paned), app.content_stack, TRUE, FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), main_paned, TRUE, TRUE, 0);

    app.status_label = gtk_label_new("");
    gtk_label_set_use_markup(GTK_LABEL(app.status_label), TRUE);
    gtk_widget_set_halign(app.status_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(app.status_label, 6);
    gtk_widget_set_margin_bottom(app.status_label, 3);
    gtk_box_pack_start(GTK_BOX(vbox), app.status_label, FALSE, FALSE, 0);

    gtk_widget_show_all(app.window);

    gtk_icon_view_set_item_width(GTK_ICON_VIEW(app.icon_view), app.icon_size + 24);
    
    if (app.view_mode == VIEW_MODE_ICONS) {
        gtk_stack_set_visible_child_name(GTK_STACK(app.content_stack), "icons");
        gtk_widget_grab_focus(app.icon_view);
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(app.content_stack), "list");
        gtk_widget_grab_focus(app.tree_view); 
    }
    
    refresh_dir();
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    pthread_mutex_init(&app.state_mutex, NULL);

    int num_cores = g_get_num_processors();
    int thread_count = (num_cores > 2) ? (num_cores > 8 ? 8 : num_cores) : 2;
    app.thread_pool = g_thread_pool_new(thumbnail_worker_thread, NULL, thread_count, FALSE, NULL);

    GtkSettings *settings = gtk_settings_get_default();
    if (settings) {
        g_object_set(settings, "gtk-application-prefer-dark-theme", TRUE, NULL);
    }

    load_config();
    build_ui();

    const char *start_dir = (argc > 1) ? argv[1] : g_get_home_dir();
    navigate_to(start_dir ? start_dir : "/", FALSE);

    gtk_main();

    if (app.dir_monitor) {
        g_file_monitor_cancel(app.dir_monitor);
        g_object_unref(app.dir_monitor);
    }

    if (app.thread_pool) {
        g_thread_pool_free(app.thread_pool, FALSE, TRUE);
    }

    pthread_mutex_destroy(&app.state_mutex);

    return 0;
}
