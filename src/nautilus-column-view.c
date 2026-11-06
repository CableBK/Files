/*
 * SPDX-FileCopyrightText: 2026 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * NautilusColumnView:
 *
 * A Miller columns view, in the style of the macOS Finder column view.
 *
 * The rightmost list shows the current location and is backed by the regular
 * #NautilusViewModel, so selection, context menus, drag and drop, renaming and
 * all other file operations behave exactly like in the list and grid views.
 *
 * To its left, read-only columns show the parent folders, up to a "root"
 * folder, each one highlighting the folder that leads to the next column.
 * Clicking an item in one of those columns navigates the window slot there,
 * so the path bar and the history follow the clicked folder, as in Finder.
 *
 * When a single file is selected, a preview pane is shown after the last
 * column.
 */

#include "nautilus-column-view.h"

#include <glib/gi18n.h>

#include "nautilus-directory.h"
#include "nautilus-enums.h"
#include "nautilus-file.h"
#include "nautilus-global-preferences.h"
#include "nautilus-image.h"
#include "nautilus-list-base-private.h"
#include "nautilus-name-cell.h"
#include "nautilus-scheme.h"
#include "nautilus-view-cell.h"
#include "nautilus-view-item.h"
#include "nautilus-view-model.h"
#include "nautilus-window-slot.h"

#define COLUMN_WIDTH 240
#define PREVIEW_ICON_SIZE 128

#define PARENT_COLUMN_ATTRIBUTES \
        G_FILE_ATTRIBUTE_STANDARD_NAME "," \
        G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," \
        G_FILE_ATTRIBUTE_STANDARD_ICON "," \
        G_FILE_ATTRIBUTE_STANDARD_TYPE "," \
        G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," \
        G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP

typedef struct _ParentColumn ParentColumn;

struct _NautilusColumnView
{
    NautilusListBase parent_instance;

    /* Current location column */
    GtkListView *view_ui;
    GtkWidget *current_column;

    /* Layout */
    GtkWidget *viewport;
    GtkWidget *columns_box;

    /* Preview pane */
    GtkWidget *preview_area;
    GtkWidget *preview;
    GtkWidget *preview_image;
    GtkWidget *preview_name;
    GtkWidget *preview_type;
    GtkWidget *preview_size;
    GtkWidget *preview_modified;

    /* Parent folder columns, ordered from the root to the parent of the
     * current location. */
    GPtrArray *parent_columns;
    GFile *root;
    GFile *location;
    /* Location we asked the slot to open from a parent column. */
    GFile *pending_location;

    GSignalGroup *model_signals;

    gboolean show_hidden;
    gboolean directories_first;
    int zoom_level;
    GQuark sort_attribute;
    gboolean reversed;
};

G_DEFINE_FINAL_TYPE (NautilusColumnView, nautilus_column_view, NAUTILUS_TYPE_LIST_BASE)

struct _ParentColumn
{
    NautilusColumnView *view; /* unowned */

    GFile *directory;
    GFile *selected_child;
    gboolean scrolled_to_selection;

    GtkWidget *widget;
    GtkListView *list_view;
    GtkSingleSelection *selection;
    GtkFilter *filter;
};

#define get_view_item(li) \
        (NAUTILUS_VIEW_ITEM (gtk_tree_list_row_get_item (GTK_TREE_LIST_ROW (gtk_list_item_get_item (li)))))

static const NautilusViewInfo column_view_info =
{
    .view_id = NAUTILUS_VIEW_COLUMN_ID,
    .zoom_level_min = NAUTILUS_LIST_ZOOM_LEVEL_SMALL,
    .zoom_level_max = NAUTILUS_LIST_ZOOM_LEVEL_MEDIUM,
    .zoom_level_standard = NAUTILUS_LIST_ZOOM_LEVEL_SMALL,
};

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static gboolean
is_ancestor_or_equal (GFile *ancestor,
                      GFile *descendant)
{
    return g_file_equal (ancestor, descendant) || g_file_has_prefix (descendant, ancestor);
}

/* Virtual locations which list files from many places don't have meaningful
 * parent folders, so they get no parent columns. */
static gboolean
location_supports_parent_columns (GFile *location)
{
    return !(g_file_has_uri_scheme (location, SCHEME_SEARCH) ||
             g_file_has_uri_scheme (location, SCHEME_RECENT) ||
             g_file_has_uri_scheme (location, SCHEME_STARRED) ||
             g_file_has_uri_scheme (location, SCHEME_NETWORK_VIEW) ||
             g_file_has_uri_scheme (location, SCHEME_NETWORK));
}

typedef struct
{
    NautilusColumnView *self;
    GFile *location;
    NautilusFile *selection;
} NavigationData;

static gboolean
navigate_idle_cb (gpointer user_data)
{
    NavigationData *data = user_data;
    NautilusWindowSlot *slot;

    slot = NAUTILUS_WINDOW_SLOT (gtk_widget_get_ancestor (GTK_WIDGET (data->self),
                                                          NAUTILUS_TYPE_WINDOW_SLOT));
    if (slot != NULL)
    {
        g_autolist (NautilusFile) selection = NULL;

        if (data->selection != NULL)
        {
            selection = g_list_prepend (NULL, nautilus_file_ref (data->selection));
        }

        g_set_object (&data->self->pending_location, data->location);
        nautilus_window_slot_open_location_full (slot, data->location, selection);
    }

    g_object_unref (data->self);
    g_object_unref (data->location);
    g_clear_pointer (&data->selection, nautilus_file_unref);
    g_free (data);

    return G_SOURCE_REMOVE;
}

/* Navigation is deferred to an idle callback, because it may end up
 * destroying the column which received the click. */
static void
navigate_to (NautilusColumnView *self,
             GFile              *location,
             GFile              *file_to_select)
{
    NavigationData *data = g_new0 (NavigationData, 1);

    data->self = g_object_ref (self);
    data->location = g_object_ref (location);
    if (file_to_select != NULL)
    {
        data->selection = nautilus_file_get (file_to_select);
    }

    g_idle_add (navigate_idle_cb, data);
}

/* ------------------------------------------------------------------------ */
/* Parent folder columns                                                    */
/* ------------------------------------------------------------------------ */

static gboolean
parent_column_filter (gpointer item,
                      gpointer user_data)
{
    NautilusColumnView *self = user_data;
    GFileInfo *info = item;

    if (self->show_hidden)
    {
        return TRUE;
    }

    return !(g_file_info_get_is_hidden (info) || g_file_info_get_is_backup (info));
}

static const char *
get_collate_key (GFileInfo *info)
{
    const char *key = g_object_get_data (G_OBJECT (info), "nautilus-collate-key");

    if (key == NULL)
    {
        char *new_key = g_utf8_collate_key_for_filename (g_file_info_get_display_name (info), -1);

        g_object_set_data_full (G_OBJECT (info), "nautilus-collate-key", new_key, g_free);
        key = new_key;
    }

    return key;
}

static int
parent_column_sort (gconstpointer a,
                    gconstpointer b,
                    gpointer      user_data)
{
    NautilusColumnView *self = user_data;
    GFileInfo *info_a = G_FILE_INFO ((gpointer) a);
    GFileInfo *info_b = G_FILE_INFO ((gpointer) b);

    if (self->directories_first)
    {
        gboolean dir_a = g_file_info_get_file_type (info_a) == G_FILE_TYPE_DIRECTORY;
        gboolean dir_b = g_file_info_get_file_type (info_b) == G_FILE_TYPE_DIRECTORY;

        if (dir_a != dir_b)
        {
            return dir_a ? -1 : 1;
        }
    }

    return strcmp (get_collate_key (info_a), get_collate_key (info_b));
}

static void
parent_column_sync_selection (ParentColumn *column)
{
    GListModel *model = G_LIST_MODEL (column->selection);
    guint n_items = g_list_model_get_n_items (model);

    if (column->selected_child == NULL)
    {
        gtk_single_selection_set_selected (column->selection, GTK_INVALID_LIST_POSITION);
        return;
    }

    for (guint i = 0; i < n_items; i++)
    {
        g_autoptr (GFileInfo) info = g_list_model_get_item (model, i);
        GFile *file = G_FILE (g_file_info_get_attribute_object (info, "standard::file"));

        if (file != NULL && g_file_equal (file, column->selected_child))
        {
            if (gtk_single_selection_get_selected (column->selection) != i)
            {
                gtk_single_selection_set_selected (column->selection, i);
            }

            if (!column->scrolled_to_selection)
            {
                column->scrolled_to_selection = TRUE;
                gtk_list_view_scroll_to (column->list_view, i, GTK_LIST_SCROLL_NONE, NULL);
            }

            return;
        }
    }

    gtk_single_selection_set_selected (column->selection, GTK_INVALID_LIST_POSITION);
}

static void
on_parent_column_items_changed (GListModel *model,
                                guint       position,
                                guint       removed,
                                guint       added,
                                gpointer    user_data)
{
    parent_column_sync_selection (user_data);
}

static void
parent_column_set_selected_child (ParentColumn *column,
                                  GFile        *child)
{
    if (column->selected_child != NULL && child != NULL &&
        g_file_equal (column->selected_child, child))
    {
        return;
    }

    g_set_object (&column->selected_child, child);
    column->scrolled_to_selection = FALSE;
    parent_column_sync_selection (column);
}

static void
on_parent_row_pressed (GtkGestureClick *gesture,
                       int              n_press,
                       double           x,
                       double           y,
                       gpointer         user_data)
{
    /* Claim the sequence so that GtkListView doesn't change the selection on
     * its own. The selection follows the navigation instead. */
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_parent_row_released (GtkGestureClick *gesture,
                        int              n_press,
                        double           x,
                        double           y,
                        gpointer         user_data)
{
    ParentColumn *column = user_data;
    GtkWidget *row = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
    GFileInfo *info = g_object_get_data (G_OBJECT (row), "file-info");
    GFile *file;

    if (info == NULL || n_press != 1)
    {
        return;
    }

    file = G_FILE (g_file_info_get_attribute_object (info, "standard::file"));
    if (file == NULL)
    {
        return;
    }

    if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
        /* Show this folder's contents in the next column. */
        navigate_to (column->view, file, NULL);
    }
    else
    {
        /* Drop the columns after this one, and select the file. */
        navigate_to (column->view, column->directory, file);
    }
}

static void
setup_parent_row (GtkSignalListItemFactory *factory,
                  GtkListItem              *listitem,
                  gpointer                  user_data)
{
    ParentColumn *column = user_data;
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new ();
    GtkWidget *label = gtk_label_new (NULL);
    GtkWidget *chevron = gtk_image_new_from_icon_name ("go-next-symbolic");
    GtkGesture *gesture;

    gtk_widget_add_css_class (box, "column-view-parent-row");
    gtk_image_set_pixel_size (GTK_IMAGE (icon), NAUTILUS_LIST_ICON_SIZE_SMALL);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand (label, TRUE);
    gtk_widget_add_css_class (chevron, "dimmed");

    gtk_box_append (GTK_BOX (box), icon);
    gtk_box_append (GTK_BOX (box), label);
    gtk_box_append (GTK_BOX (box), chevron);

    g_object_set_data (G_OBJECT (box), "icon", icon);
    g_object_set_data (G_OBJECT (box), "label", label);
    g_object_set_data (G_OBJECT (box), "chevron", chevron);

    gesture = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), GDK_BUTTON_PRIMARY);
    g_signal_connect (gesture, "pressed", G_CALLBACK (on_parent_row_pressed), column);
    g_signal_connect (gesture, "released", G_CALLBACK (on_parent_row_released), column);
    gtk_widget_add_controller (box, GTK_EVENT_CONTROLLER (gesture));

    gtk_list_item_set_child (listitem, box);
}

static void
bind_parent_row (GtkSignalListItemFactory *factory,
                 GtkListItem              *listitem,
                 gpointer                  user_data)
{
    GtkWidget *box = gtk_list_item_get_child (listitem);
    GFileInfo *info = G_FILE_INFO (gtk_list_item_get_item (listitem));
    GIcon *gicon = g_file_info_get_icon (info);
    gboolean is_directory = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
    const char *display_name = g_file_info_get_display_name (info);

    g_object_set_data_full (G_OBJECT (box), "file-info", g_object_ref (info), g_object_unref);

    gtk_image_set_from_gicon (GTK_IMAGE (g_object_get_data (G_OBJECT (box), "icon")), gicon);
    gtk_label_set_text (GTK_LABEL (g_object_get_data (G_OBJECT (box), "label")), display_name);
    gtk_widget_set_visible (GTK_WIDGET (g_object_get_data (G_OBJECT (box), "chevron")), is_directory);
    gtk_widget_set_tooltip_text (box, display_name);

    if (g_file_info_get_is_hidden (info) || g_file_info_get_is_backup (info))
    {
        gtk_widget_add_css_class (box, "dimmed");
    }
    else
    {
        gtk_widget_remove_css_class (box, "dimmed");
    }

    gtk_accessible_update_property (GTK_ACCESSIBLE (listitem),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, display_name,
                                    -1);
}

static void
unbind_parent_row (GtkSignalListItemFactory *factory,
                   GtkListItem              *listitem,
                   gpointer                  user_data)
{
    GtkWidget *box = gtk_list_item_get_child (listitem);

    g_object_set_data (G_OBJECT (box), "file-info", NULL);
}

static ParentColumn *
parent_column_new (NautilusColumnView *self,
                   GFile              *directory)
{
    ParentColumn *column = g_new0 (ParentColumn, 1);
    g_autoptr (GtkListItemFactory) factory = gtk_signal_list_item_factory_new ();
    GtkDirectoryList *directory_list;
    GtkCustomSorter *sorter;
    GtkFilterListModel *filter_model;
    GtkSortListModel *sort_model;
    GtkWidget *scrolled_window;
    g_autofree char *name = NULL;

    column->view = self;
    column->directory = g_object_ref (directory);

    directory_list = gtk_directory_list_new (PARENT_COLUMN_ATTRIBUTES, directory);
    column->filter = GTK_FILTER (gtk_custom_filter_new (parent_column_filter, self, NULL));
    filter_model = gtk_filter_list_model_new (G_LIST_MODEL (directory_list),
                                              GTK_FILTER (g_object_ref (column->filter)));
    sorter = gtk_custom_sorter_new (parent_column_sort, self, NULL);
    sort_model = gtk_sort_list_model_new (G_LIST_MODEL (filter_model), GTK_SORTER (sorter));

    column->selection = gtk_single_selection_new (G_LIST_MODEL (sort_model));
    gtk_single_selection_set_autoselect (column->selection, FALSE);
    gtk_single_selection_set_can_unselect (column->selection, TRUE);
    gtk_single_selection_set_selected (column->selection, GTK_INVALID_LIST_POSITION);
    g_signal_connect (column->selection, "items-changed",
                      G_CALLBACK (on_parent_column_items_changed), column);

    g_signal_connect (factory, "setup", G_CALLBACK (setup_parent_row), column);
    g_signal_connect (factory, "bind", G_CALLBACK (bind_parent_row), column);
    g_signal_connect (factory, "unbind", G_CALLBACK (unbind_parent_row), column);

    column->list_view = GTK_LIST_VIEW (gtk_list_view_new (GTK_SELECTION_MODEL (g_object_ref (column->selection)),
                                                          g_steal_pointer (&factory)));
    /* Keyboard navigation happens in the current folder column. Moving left
     * from there goes to the parent folder. */
    gtk_widget_set_can_focus (GTK_WIDGET (column->list_view), FALSE);

    name = g_file_get_basename (directory);
    gtk_accessible_update_property (GTK_ACCESSIBLE (column->list_view),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, name,
                                    -1);

    scrolled_window = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled_window),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled_window),
                                   GTK_WIDGET (column->list_view));
    gtk_widget_set_vexpand (scrolled_window, TRUE);

    column->widget = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (column->widget, "column-view-parent");
    gtk_widget_set_size_request (scrolled_window, COLUMN_WIDTH, -1);
    gtk_box_append (GTK_BOX (column->widget), scrolled_window);
    gtk_box_append (GTK_BOX (column->widget), gtk_separator_new (GTK_ORIENTATION_VERTICAL));

    return column;
}

static void
parent_column_free (ParentColumn *column)
{
    NautilusColumnView *self = column->view;

    g_signal_handlers_disconnect_by_data (column->selection, column);

    if (gtk_widget_get_parent (column->widget) == self->columns_box)
    {
        gtk_box_remove (GTK_BOX (self->columns_box), column->widget);
    }

    g_clear_object (&column->selection);
    g_clear_object (&column->filter);
    g_clear_object (&column->directory);
    g_clear_object (&column->selected_child);
    g_free (column);
}

static void
refilter_parent_columns (NautilusColumnView *self)
{
    for (guint i = 0; i < self->parent_columns->len; i++)
    {
        ParentColumn *column = g_ptr_array_index (self->parent_columns, i);

        gtk_filter_changed (column->filter, GTK_FILTER_CHANGE_DIFFERENT);
    }
}

static void
on_show_hidden_changed (NautilusColumnView *self)
{
    self->show_hidden = g_settings_get_boolean (gtk_filechooser_preferences,
                                                NAUTILUS_PREFERENCES_SHOW_HIDDEN_FILES);
    refilter_parent_columns (self);
}

/* Decides which folder is shown in the first column for a new location.
 *
 * Like in Finder, the columns are kept when the new location is reached by
 * clicking in a column, going into a subfolder, or going back up towards the
 * first column. Otherwise (e.g. from the sidebar or the location entry), the
 * new location starts a new set of columns. */
static void
update_root (NautilusColumnView *self,
             GFile              *location)
{
    gboolean keep_root = FALSE;

    if (!location_supports_parent_columns (location))
    {
        g_set_object (&self->root, location);
        g_clear_object (&self->pending_location);
        return;
    }

    if (self->root == NULL)
    {
        /* When switching into this view, show some context: start from the
         * home folder if the location is inside it. */
        g_autoptr (GFile) home = g_file_new_for_path (g_get_home_dir ());

        GFile *root = g_file_has_prefix (location, home) ? home : location;

        g_set_object (&self->root, root);
        return;
    }

    if (is_ancestor_or_equal (self->root, location))
    {
        if (self->pending_location != NULL && g_file_equal (self->pending_location, location))
        {
            keep_root = TRUE;
        }
        else if (self->location != NULL &&
                 (is_ancestor_or_equal (location, self->location) ||
                  g_file_has_prefix (location, self->location)))
        {
            keep_root = TRUE;
        }
    }

    g_clear_object (&self->pending_location);

    if (!keep_root)
    {
        g_set_object (&self->root, location);
    }
}

static void
scroll_to_last_column (NautilusColumnView *self)
{
    GtkWidget *target = gtk_widget_get_visible (self->preview) ?
                        self->preview_area : self->current_column;

    gtk_viewport_scroll_to (GTK_VIEWPORT (self->viewport), target, NULL);
}

static void
rebuild_parent_columns (NautilusColumnView *self)
{
    g_autoptr (GPtrArray) directories = g_ptr_array_new_with_free_func (g_object_unref);
    guint n_kept = 0;

    /* Collect the folders between the root and the current location. */
    if (self->root != NULL && self->location != NULL &&
        g_file_has_prefix (self->location, self->root))
    {
        g_autoptr (GFile) directory = g_file_get_parent (self->location);

        while (directory != NULL)
        {
            gboolean is_root = g_file_equal (directory, self->root);

            g_ptr_array_insert (directories, 0, g_object_ref (directory));
            if (is_root)
            {
                break;
            }

            GFile *parent = g_file_get_parent (directory);
            g_object_unref (directory);
            directory = parent;
        }
    }

    /* Keep the columns which are still on the path. */
    while (n_kept < self->parent_columns->len &&
           n_kept < directories->len)
    {
        ParentColumn *column = g_ptr_array_index (self->parent_columns, n_kept);

        if (!g_file_equal (column->directory, g_ptr_array_index (directories, n_kept)))
        {
            break;
        }

        n_kept++;
    }

    g_ptr_array_set_size (self->parent_columns, n_kept);

    for (guint i = n_kept; i < directories->len; i++)
    {
        ParentColumn *column = parent_column_new (self, g_ptr_array_index (directories, i));
        GtkWidget *previous = NULL;

        if (i > 0)
        {
            previous = ((ParentColumn *) g_ptr_array_index (self->parent_columns, i - 1))->widget;
        }

        gtk_box_insert_child_after (GTK_BOX (self->columns_box), column->widget, previous);
        g_ptr_array_add (self->parent_columns, column);
    }

    /* Highlight the path to the current location. */
    for (guint i = 0; i < self->parent_columns->len; i++)
    {
        ParentColumn *column = g_ptr_array_index (self->parent_columns, i);
        GFile *next = (i + 1 < directories->len) ?
                      g_ptr_array_index (directories, i + 1) :
                      self->location;

        parent_column_set_selected_child (column, next);
    }

    scroll_to_last_column (self);
}

/* ------------------------------------------------------------------------ */
/* Preview pane                                                             */
/* ------------------------------------------------------------------------ */

static void
set_preview_value (GtkWidget    *label,
                   NautilusFile *file,
                   const char   *attribute)
{
    g_autofree char *value = nautilus_file_get_string_attribute (file, attribute);

    gtk_label_set_text (GTK_LABEL (label), value != NULL ? value : "—");
}

static void
update_preview (NautilusColumnView *self)
{
    NautilusViewModel *model = nautilus_list_base_get_model (NAUTILUS_LIST_BASE (self));
    NautilusFile *file = NULL;
    g_autoptr (NautilusViewItem) item = NULL;

    if (model != NULL)
    {
        g_autoptr (GtkBitset) selection = gtk_selection_model_get_selection (GTK_SELECTION_MODEL (model));

        if (gtk_bitset_get_size (selection) == 1)
        {
            g_autoptr (GtkTreeListRow) row = g_list_model_get_item (G_LIST_MODEL (model),
                                                                    gtk_bitset_get_minimum (selection));

            if (row != NULL)
            {
                item = NAUTILUS_VIEW_ITEM (gtk_tree_list_row_get_item (row));
                file = nautilus_view_item_get_file (item);
            }
        }
    }

    if (file == NULL || nautilus_file_is_directory (file))
    {
        gtk_widget_set_visible (self->preview, FALSE);
        return;
    }

    int scale_factor = gtk_widget_get_scale_factor (GTK_WIDGET (self));
    g_autoptr (GdkPaintable) icon = nautilus_file_get_icon_paintable (file, PREVIEW_ICON_SIZE,
                                                                      scale_factor,
                                                                      NAUTILUS_FILE_ICON_FLAGS_NONE);
    g_autoptr (GFile) location = nautilus_file_get_location (file);
    const char *name = nautilus_file_get_display_name (file);

    nautilus_image_set_size (NAUTILUS_IMAGE (self->preview_image), PREVIEW_ICON_SIZE);
    nautilus_image_set_fallback (NAUTILUS_IMAGE (self->preview_image), icon);
    nautilus_image_set_source (NAUTILUS_IMAGE (self->preview_image),
                               nautilus_file_should_show_thumbnail (file) ? location : NULL);

    gtk_label_set_text (GTK_LABEL (self->preview_name), name);
    set_preview_value (self->preview_type, file, "detailed_type");
    set_preview_value (self->preview_size, file, "size");
    set_preview_value (self->preview_modified, file, "date_modified");

    if (!gtk_widget_get_visible (self->preview))
    {
        gtk_widget_set_visible (self->preview, TRUE);
        scroll_to_last_column (self);
    }
}

static GtkWidget *
add_preview_row (GtkGrid    *grid,
                 int         row,
                 const char *title)
{
    GtkWidget *title_label = gtk_label_new (title);
    GtkWidget *value_label = gtk_label_new (NULL);

    gtk_label_set_xalign (GTK_LABEL (title_label), 1.0);
    gtk_widget_set_valign (title_label, GTK_ALIGN_START);
    gtk_widget_add_css_class (title_label, "dimmed");

    gtk_label_set_xalign (GTK_LABEL (value_label), 0.0);
    gtk_label_set_wrap (GTK_LABEL (value_label), TRUE);
    gtk_label_set_wrap_mode (GTK_LABEL (value_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable (GTK_LABEL (value_label), TRUE);
    gtk_widget_set_hexpand (value_label, TRUE);

    gtk_grid_attach (grid, title_label, 0, row, 1, 1);
    gtk_grid_attach (grid, value_label, 1, row, 1, 1);

    return value_label;
}

static void
create_preview (NautilusColumnView *self)
{
    GtkWidget *details = gtk_grid_new ();

    self->preview = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class (self->preview, "column-view-preview");
    gtk_widget_set_size_request (self->preview, COLUMN_WIDTH, -1);
    gtk_widget_set_halign (self->preview, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (self->preview, GTK_ALIGN_START);
    gtk_widget_set_visible (self->preview, FALSE);

    self->preview_image = GTK_WIDGET (nautilus_image_new ());
    gtk_widget_set_halign (self->preview_image, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request (self->preview_image, PREVIEW_ICON_SIZE, PREVIEW_ICON_SIZE);
    gtk_box_append (GTK_BOX (self->preview), self->preview_image);

    self->preview_name = gtk_label_new (NULL);
    gtk_label_set_wrap (GTK_LABEL (self->preview_name), TRUE);
    gtk_label_set_wrap_mode (GTK_LABEL (self->preview_name), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_justify (GTK_LABEL (self->preview_name), GTK_JUSTIFY_CENTER);
    gtk_label_set_selectable (GTK_LABEL (self->preview_name), TRUE);
    gtk_widget_add_css_class (self->preview_name, "heading");
    gtk_box_append (GTK_BOX (self->preview), self->preview_name);

    gtk_grid_set_row_spacing (GTK_GRID (details), 6);
    gtk_grid_set_column_spacing (GTK_GRID (details), 12);
    self->preview_type = add_preview_row (GTK_GRID (details), 0, _("Type"));
    self->preview_size = add_preview_row (GTK_GRID (details), 1, _("Size"));
    self->preview_modified = add_preview_row (GTK_GRID (details), 2, _("Modified"));
    gtk_box_append (GTK_BOX (self->preview), details);

    /* The area after the last column takes the remaining width, so that the
     * columns stay aligned to the start. */
    self->preview_area = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand (self->preview_area, TRUE);
    gtk_box_append (GTK_BOX (self->preview_area), self->preview);
}

/* ------------------------------------------------------------------------ */
/* Current folder column                                                    */
/* ------------------------------------------------------------------------ */

static gint
nautilus_column_view_sort (gconstpointer a,
                           gconstpointer b,
                           gpointer      user_data)
{
    NautilusColumnView *self = user_data;
    NautilusFile *file_a = nautilus_view_item_get_file ((NautilusViewItem *) a);
    NautilusFile *file_b = nautilus_view_item_get_file ((NautilusViewItem *) b);

    return nautilus_file_compare_for_sort_by_attribute_q (file_a, file_b,
                                                          self->sort_attribute,
                                                          self->directories_first,
                                                          self->reversed);
}

static void
update_sort_directories_first (NautilusColumnView *self)
{
    NautilusFile *directory_as_file = nautilus_list_base_get_directory_as_file (NAUTILUS_LIST_BASE (self));
    NautilusViewModel *model = nautilus_list_base_get_model (NAUTILUS_LIST_BASE (self));
    gboolean preference = g_settings_get_boolean (gtk_filechooser_preferences,
                                                  NAUTILUS_PREFERENCES_SORT_DIRECTORIES_FIRST);

    if (directory_as_file != NULL &&
        (nautilus_file_is_in_search (directory_as_file) ||
         nautilus_file_is_in_recent (directory_as_file)))
    {
        self->directories_first = FALSE;
    }
    else
    {
        self->directories_first = preference;
    }

    if (model != NULL)
    {
        nautilus_view_model_sort (model);
    }

    for (guint i = 0; i < self->parent_columns->len; i++)
    {
        ParentColumn *column = g_ptr_array_index (self->parent_columns, i);
        GtkSortListModel *sort_model = GTK_SORT_LIST_MODEL (gtk_single_selection_get_model (column->selection));

        gtk_sorter_changed (gtk_sort_list_model_get_sorter (sort_model), GTK_SORTER_CHANGE_DIFFERENT);
    }
}

static void
real_setup_directory (NautilusListBase  *list_base,
                      NautilusDirectory *directory)
{
    NautilusColumnView *self = NAUTILUS_COLUMN_VIEW (list_base);
    g_autoptr (GFile) location = nautilus_directory_get_location (directory);

    NAUTILUS_LIST_BASE_CLASS (nautilus_column_view_parent_class)->setup_directory (list_base, directory);

    update_sort_directories_first (self);

    update_root (self, location);
    g_set_object (&self->location, location);
    rebuild_parent_columns (self);
    update_preview (self);
}

static NautilusViewInfo
real_get_view_info (NautilusListBase *list_base)
{
    return column_view_info;
}

static guint
real_get_icon_size (NautilusListBase *list_base)
{
    NautilusColumnView *self = NAUTILUS_COLUMN_VIEW (list_base);

    return self->zoom_level == NAUTILUS_LIST_ZOOM_LEVEL_SMALL ?
           NAUTILUS_LIST_ICON_SIZE_SMALL :
           NAUTILUS_LIST_ICON_SIZE_MEDIUM;
}

static int
real_get_zoom_level (NautilusListBase *list_base)
{
    NautilusColumnView *self = NAUTILUS_COLUMN_VIEW (list_base);

    return self->zoom_level;
}

static void
real_set_zoom_level (NautilusListBase *list_base,
                     int               new_level)
{
    NautilusColumnView *self = NAUTILUS_COLUMN_VIEW (list_base);

    g_return_if_fail (new_level >= column_view_info.zoom_level_min &&
                      new_level <= column_view_info.zoom_level_max);

    self->zoom_level = new_level;
    g_object_notify (G_OBJECT (self), "icon-size");
}

static void
real_scroll_to (NautilusListBase   *list_base,
                guint               position,
                GtkListScrollFlags  flags,
                GtkScrollInfo      *scroll)
{
    NautilusColumnView *self = NAUTILUS_COLUMN_VIEW (list_base);

    gtk_list_view_scroll_to (self->view_ui, position, flags, scroll);
}

static GVariant *
real_get_sort_state (NautilusListBase *list_base)
{
    NautilusColumnView *self = NAUTILUS_COLUMN_VIEW (list_base);

    return g_variant_take_ref (g_variant_new ("(sb)",
                                              g_quark_to_string (self->sort_attribute),
                                              self->reversed));
}

static void
real_set_sort_state (NautilusListBase *list_base,
                     GVariant         *value)
{
    NautilusColumnView *self = NAUTILUS_COLUMN_VIEW (list_base);
    NautilusViewModel *model = nautilus_list_base_get_model (list_base);
    const gchar *target_name;
    g_autoptr (GtkCustomSorter) sorter = NULL;

    /* Sort state binding should be set after the model is set.*/
    g_return_if_fail (model != NULL);

    g_variant_get (value, "(&sb)", &target_name, &self->reversed);
    self->sort_attribute = g_quark_from_string (target_name);

    sorter = gtk_custom_sorter_new (nautilus_column_view_sort, self, NULL);
    nautilus_view_model_set_sorter (model, GTK_SORTER (sorter));
}

static void
real_set_enable_rubberband (NautilusListBase *list_base,
                            gboolean          enabled)
{
    NautilusColumnView *self = NAUTILUS_COLUMN_VIEW (list_base);

    gtk_list_view_set_enable_rubberband (self->view_ui, enabled);
}

static NautilusFile *
get_single_selected_file (NautilusColumnView *self)
{
    NautilusViewModel *model = nautilus_list_base_get_model (NAUTILUS_LIST_BASE (self));
    g_autoptr (GtkBitset) selection = NULL;
    g_autoptr (GtkTreeListRow) row = NULL;
    g_autoptr (NautilusViewItem) item = NULL;

    if (model == NULL)
    {
        return NULL;
    }

    selection = gtk_selection_model_get_selection (GTK_SELECTION_MODEL (model));
    if (gtk_bitset_get_size (selection) != 1)
    {
        return NULL;
    }

    row = g_list_model_get_item (G_LIST_MODEL (model), gtk_bitset_get_minimum (selection));
    if (row == NULL)
    {
        return NULL;
    }

    item = NAUTILUS_VIEW_ITEM (gtk_tree_list_row_get_item (row));

    return nautilus_view_item_get_file (item);
}

/* Left and Right move between columns, like in Finder. */
static gboolean
on_key_pressed (GtkEventControllerKey *controller,
                guint                  keyval,
                guint                  keycode,
                GdkModifierType        state,
                gpointer               user_data)
{
    NautilusColumnView *self = NAUTILUS_COLUMN_VIEW (user_data);
    gboolean rtl = gtk_widget_get_direction (GTK_WIDGET (self)) == GTK_TEXT_DIR_RTL;
    gboolean left = (keyval == GDK_KEY_Left || keyval == GDK_KEY_KP_Left);
    gboolean right = (keyval == GDK_KEY_Right || keyval == GDK_KEY_KP_Right);
    gboolean into, out;

    if ((state & gtk_accelerator_get_default_mod_mask ()) != 0)
    {
        return GDK_EVENT_PROPAGATE;
    }

    into = rtl ? left : right;
    out = rtl ? right : left;

    if (into)
    {
        NautilusFile *file = get_single_selected_file (self);

        if (file != NULL && nautilus_file_is_directory (file))
        {
            nautilus_list_base_activate_selection (NAUTILUS_LIST_BASE (self), FALSE);
            return GDK_EVENT_STOP;
        }
    }
    else if (out)
    {
        if (self->parent_columns->len > 0)
        {
            gtk_widget_activate_action (GTK_WIDGET (self), "slot.up", NULL);
            return GDK_EVENT_STOP;
        }
    }

    return GDK_EVENT_PROPAGATE;
}

/* We only care about the keyboard activation part that GtkListView provides.
 * Clicks are handled by NautilusListBase. See nautilus-network-view.c */
static void
on_list_view_item_activated (GtkListView *list_view,
                             guint        position,
                             gpointer     user_data)
{
    nautilus_list_base_activate_selection (NAUTILUS_LIST_BASE (user_data), FALSE);
}

static void
setup_cell (GtkSignalListItemFactory *factory,
            GtkListItem              *listitem,
            gpointer                  user_data)
{
    NautilusColumnView *self = NAUTILUS_COLUMN_VIEW (user_data);
    NautilusViewCell *cell = nautilus_name_cell_new (NAUTILUS_LIST_BASE (self));
    GtkWidget *content = nautilus_name_cell_get_content (NAUTILUS_NAME_CELL (cell));
    GtkWidget *chevron = gtk_image_new_from_icon_name ("go-next-symbolic");
    GtkExpression *expression;

    gtk_widget_add_css_class (chevron, "dimmed");
    gtk_widget_set_valign (chevron, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (content), chevron);
    g_object_set_data (G_OBJECT (cell), "chevron", chevron);

    gtk_list_item_set_child (listitem, GTK_WIDGET (cell));
    setup_cell_common (G_OBJECT (listitem), cell, content);

    g_object_bind_property (self, "icon-size",
                            cell, "icon-size",
                            G_BINDING_SYNC_CREATE);

    /* Use file display name as accessible label. Explaining in pseudo-code:
     * listitem:accessible-name :- listitem:item:item:file:a11y-name */
    expression = gtk_property_expression_new (GTK_TYPE_LIST_ITEM, NULL, "item");
    expression = gtk_property_expression_new (GTK_TYPE_TREE_LIST_ROW, expression, "item");
    expression = gtk_property_expression_new (NAUTILUS_TYPE_VIEW_ITEM, expression, "file");
    expression = gtk_property_expression_new (NAUTILUS_TYPE_FILE, expression, "a11y-name");
    gtk_expression_bind (expression, listitem, "accessible-label", listitem);
}

static void
bind_cell (GtkSignalListItemFactory *factory,
           GtkListItem              *listitem,
           gpointer                  user_data)
{
    GtkWidget *cell = gtk_list_item_get_child (listitem);
    g_autoptr (NautilusViewItem) item = get_view_item (listitem);
    GtkWidget *chevron = g_object_get_data (G_OBJECT (cell), "chevron");

    g_return_if_fail (item != NULL);

    nautilus_view_item_set_item_ui (item, cell);
    gtk_widget_set_visible (chevron, nautilus_file_is_directory (nautilus_view_item_get_file (item)));
}

static void
unbind_cell (GtkSignalListItemFactory *factory,
             GtkListItem              *listitem,
             gpointer                  user_data)
{
    g_autoptr (NautilusViewItem) item = get_view_item (listitem);

    /* item may be NULL when row has just been destroyed. */
    if (item != NULL)
    {
        nautilus_view_item_set_item_ui (item, NULL);
    }
}

static void
on_model_changed (NautilusColumnView *self)
{
    NautilusViewModel *model = nautilus_list_base_get_model (NAUTILUS_LIST_BASE (self));

    if (model != NULL)
    {
        gtk_list_view_set_enable_rubberband (self->view_ui,
                                             !nautilus_view_model_get_single_selection (model));
    }

    g_signal_group_set_target (self->model_signals, model);
    gtk_list_view_set_model (self->view_ui, GTK_SELECTION_MODEL (model));
    update_preview (self);
}

static GtkListView *
create_view_ui (NautilusColumnView *self)
{
    g_autoptr (GtkListItemFactory) factory = gtk_signal_list_item_factory_new ();
    GtkListView *list_view;
    GtkEventController *controller;

    g_signal_connect (factory, "setup", G_CALLBACK (setup_cell), self);
    g_signal_connect (factory, "bind", G_CALLBACK (bind_cell), self);
    g_signal_connect (factory, "unbind", G_CALLBACK (unbind_cell), self);

    list_view = GTK_LIST_VIEW (gtk_list_view_new (NULL, g_steal_pointer (&factory)));

    /* See the comment in nautilus-grid-view.c's create_view_ui(). */
    gtk_list_view_set_single_click_activate (list_view, FALSE);
    gtk_list_view_set_tab_behavior (list_view, GTK_LIST_TAB_ITEM);

    gtk_accessible_update_property (GTK_ACCESSIBLE (list_view),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    _("Content View"),
                                    GTK_ACCESSIBLE_PROPERTY_ROLE_DESCRIPTION,
                                    _("View of the current location"),
                                    -1);

    g_signal_connect (list_view, "activate", G_CALLBACK (on_list_view_item_activated), self);

    controller = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
    g_signal_connect (controller, "key-pressed", G_CALLBACK (on_key_pressed), self);
    gtk_widget_add_controller (GTK_WIDGET (list_view), controller);

    return list_view;
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                  */
/* ------------------------------------------------------------------------ */

static void
nautilus_column_view_dispose (GObject *object)
{
    NautilusColumnView *self = NAUTILUS_COLUMN_VIEW (object);

    if (self->parent_columns != NULL)
    {
        g_ptr_array_set_size (self->parent_columns, 0);
    }

    if (self->model_signals != NULL)
    {
        g_signal_group_set_target (self->model_signals, NULL);
    }

    G_OBJECT_CLASS (nautilus_column_view_parent_class)->dispose (object);
}

static void
nautilus_column_view_finalize (GObject *object)
{
    NautilusColumnView *self = NAUTILUS_COLUMN_VIEW (object);

    g_clear_pointer (&self->parent_columns, g_ptr_array_unref);
    g_clear_object (&self->model_signals);
    g_clear_object (&self->root);
    g_clear_object (&self->location);
    g_clear_object (&self->pending_location);

    G_OBJECT_CLASS (nautilus_column_view_parent_class)->finalize (object);
}

static void
nautilus_column_view_class_init (NautilusColumnViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    NautilusListBaseClass *list_base_class = NAUTILUS_LIST_BASE_CLASS (klass);

    object_class->dispose = nautilus_column_view_dispose;
    object_class->finalize = nautilus_column_view_finalize;

    list_base_class->get_icon_size = real_get_icon_size;
    list_base_class->get_sort_state = real_get_sort_state;
    list_base_class->get_view_info = real_get_view_info;
    list_base_class->get_zoom_level = real_get_zoom_level;
    list_base_class->scroll_to = real_scroll_to;
    list_base_class->set_enable_rubberband = real_set_enable_rubberband;
    list_base_class->set_sort_state = real_set_sort_state;
    list_base_class->set_zoom_level = real_set_zoom_level;
    list_base_class->setup_directory = real_setup_directory;
}

static void
nautilus_column_view_init (NautilusColumnView *self)
{
    NautilusListBase *list_base = NAUTILUS_LIST_BASE (self);
    GtkWidget *scrolled_window = nautilus_list_base_get_scrolled_window (list_base);
    GtkWidget *overlay = nautilus_list_base_get_overlay (list_base);
    GtkWidget *outer_scrolled_window;

    gtk_widget_add_css_class (GTK_WIDGET (self), "nautilus-column-view");

    self->parent_columns = g_ptr_array_new_with_free_func ((GDestroyNotify) parent_column_free);
    self->sort_attribute = g_quark_from_static_string ("name");
    self->zoom_level = column_view_info.zoom_level_standard;
    self->show_hidden = g_settings_get_boolean (gtk_filechooser_preferences,
                                                NAUTILUS_PREFERENCES_SHOW_HIDDEN_FILES);
    self->directories_first = g_settings_get_boolean (gtk_filechooser_preferences,
                                                      NAUTILUS_PREFERENCES_SORT_DIRECTORIES_FIRST);

    /* Current folder column */
    self->view_ui = create_view_ui (self);
    nautilus_list_base_setup_gestures (list_base);
    nautilus_list_base_set_single_click_folders (list_base, TRUE);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled_window),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled_window),
                                   GTK_WIDGET (self->view_ui));

    self->model_signals = g_signal_group_new (NAUTILUS_TYPE_VIEW_MODEL);
    g_signal_group_connect_swapped (self->model_signals, "selection-changed",
                                    G_CALLBACK (update_preview), self);
    g_signal_group_connect_swapped (self->model_signals, "items-changed",
                                    G_CALLBACK (update_preview), self);
    g_signal_connect_swapped (self, "notify::model", G_CALLBACK (on_model_changed), self);

    /* Move the base class' overlay (holding the current folder list and the
     * empty folder page) into a row of columns. */
    self->current_column = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (self->current_column, "column-view-current");

    g_object_ref (overlay);
    adw_bin_set_child (ADW_BIN (self), NULL);
    gtk_widget_set_size_request (overlay, COLUMN_WIDTH, -1);
    gtk_box_append (GTK_BOX (self->current_column), overlay);
    gtk_box_append (GTK_BOX (self->current_column), gtk_separator_new (GTK_ORIENTATION_VERTICAL));
    g_object_unref (overlay);

    create_preview (self);

    self->columns_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand (self->columns_box, TRUE);
    gtk_box_append (GTK_BOX (self->columns_box), self->current_column);
    gtk_box_append (GTK_BOX (self->columns_box), self->preview_area);

    self->viewport = gtk_viewport_new (NULL, NULL);
    gtk_viewport_set_child (GTK_VIEWPORT (self->viewport), self->columns_box);

    outer_scrolled_window = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (outer_scrolled_window),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (outer_scrolled_window), self->viewport);
    adw_bin_set_child (ADW_BIN (self), outer_scrolled_window);

    g_signal_connect_object (gtk_filechooser_preferences,
                             "changed::" NAUTILUS_PREFERENCES_SORT_DIRECTORIES_FIRST,
                             G_CALLBACK (update_sort_directories_first),
                             self,
                             G_CONNECT_SWAPPED);
    g_signal_connect_object (gtk_filechooser_preferences,
                             "changed::" NAUTILUS_PREFERENCES_SHOW_HIDDEN_FILES,
                             G_CALLBACK (on_show_hidden_changed),
                             self,
                             G_CONNECT_SWAPPED);

    nautilus_list_base_set_zoom_level (list_base, column_view_info.zoom_level_standard);
}

NautilusColumnView *
nautilus_column_view_new (void)
{
    return g_object_new (NAUTILUS_TYPE_COLUMN_VIEW, NULL);
}
