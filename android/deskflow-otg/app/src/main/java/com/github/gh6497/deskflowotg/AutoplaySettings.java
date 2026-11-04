/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

import android.content.Context;
import android.content.SharedPreferences;

/** User opt-in only. Changing settings never evaluates an old connection report. */
final class AutoplaySettings {
    static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences("autoplay", Context.MODE_PRIVATE);
    }

    static boolean enabled(Context context) {
        return prefs(context).getBoolean("enabled", false);
    }

    static String target(Context context) {
        return prefs(context).getString("target", "");
    }

    private AutoplaySettings() {}
}
