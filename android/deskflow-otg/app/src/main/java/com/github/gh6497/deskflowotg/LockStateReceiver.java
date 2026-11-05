/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

import android.app.KeyguardManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;

/** Returns the framework lock state to the already-authorized ADB shell. */
public final class LockStateReceiver extends BroadcastReceiver {
    static final String ACTION = "com.github.gh6497.deskflowotg.LOCK_STATE";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !ACTION.equals(intent.getAction())) return;
        KeyguardManager keyguard = (KeyguardManager) context.getSystemService(Context.KEYGUARD_SERVICE);
        if (keyguard == null) {
            setResultCode(1);
            setResultData("unknown");
            return;
        }
        // A visible non-secure keyguard must not receive a PIN sequence.
        boolean locked = Build.VERSION.SDK_INT >= Build.VERSION_CODES.M
                ? keyguard.isDeviceLocked() : keyguard.isKeyguardLocked();
        setResultCode(0);
        setResultData(locked ? "locked" : "unlocked");
    }
}
