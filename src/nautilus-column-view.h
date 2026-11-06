/*
 * SPDX-FileCopyrightText: 2026 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "nautilus-list-base.h"

G_BEGIN_DECLS

#define NAUTILUS_TYPE_COLUMN_VIEW (nautilus_column_view_get_type())

G_DECLARE_FINAL_TYPE (NautilusColumnView, nautilus_column_view, NAUTILUS, COLUMN_VIEW, NautilusListBase)

NautilusColumnView *nautilus_column_view_new (void);

G_END_DECLS
