// SPDX-License-Identifier: Apache-2.0
package com.github.gh6497.deskflowotg;

import android.content.Context;
import android.net.Uri;

/** User-selected destination policy for files received from the desktop. */
public final class TransferSettings {
    private static final String PREFS = "transfer_settings";
    private static final String AUTO_PUBLIC = "auto_public";
    private static final String TREE_URI = "tree_uri";

    public static boolean autoPublic(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getBoolean(AUTO_PUBLIC, false);
    }

    public static void setAutoPublic(Context context, boolean enabled) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit().putBoolean(AUTO_PUBLIC, enabled).apply();
    }

    public static String treeUri(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getString(TREE_URI, "");
    }

    public static void setTreeUri(Context context, String uri) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit().putString(TREE_URI, uri == null ? "" : uri).apply();
    }

    public static boolean hasTreePermission(Context context) {
        String value = treeUri(context);
        if (value.isEmpty()) return false;
        Uri uri;
        try { uri = Uri.parse(value); }
        catch (RuntimeException e) { return false; }
        for (android.content.UriPermission permission : context.getContentResolver()
                .getPersistedUriPermissions()) {
            if (uri.equals(permission.getUri()) && permission.isWritePermission()) return true;
        }
        return false;
    }

    private TransferSettings() {}
}
