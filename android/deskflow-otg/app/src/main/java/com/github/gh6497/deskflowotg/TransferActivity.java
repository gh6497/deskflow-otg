// SPDX-License-Identifier: Apache-2.0
package com.github.gh6497.deskflowotg;

import android.app.*;
import android.content.Intent;
import android.os.*;
import android.view.View;
import android.widget.*;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;

/** App-private received files are saved only through the user's system picker. */
public final class TransferActivity extends Activity {
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private LinearLayout rows;
    private TextView status;
    private String savingId;
    private boolean busy;
    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (saved != null) savingId = saved.getString("savingId");
        setContentView(R.layout.activity_transfer);
        status = findViewById(R.id.transfer_status);
        rows = findViewById(R.id.transfer_rows);
        findViewById(R.id.transfer_refresh).setOnClickListener(v -> refresh());
        refresh();
    }
    @Override protected void onSaveInstanceState(Bundle out) {
        out.putString("savingId", savingId); super.onSaveInstanceState(out);
    }
    private void refresh() {
        if (busy) return;
        rows.removeAllViews();
        try {
            List<TransferStore.Entry> entries = TransferProvider.store(this).list(null);
            if (entries.isEmpty()) {
                getLayoutInflater().inflate(R.layout.transfer_empty, rows, true);
            }
            for (TransferStore.Entry e : entries) {
                View row = getLayoutInflater().inflate(R.layout.item_transfer, rows, false);
                rows.addView(row);
                TextView label = row.findViewById(R.id.transfer_label);
                label.setText(getString("in".equals(e.direction) ? R.string.transfer_incoming : R.string.transfer_outgoing,
                        e.name, e.size));
                if ("in".equals(e.direction)) {
                    Button save = row.findViewById(R.id.transfer_save);
                    save.setVisibility(View.VISIBLE);
                    save.setOnClickListener(v -> {
                        if (busy) return;
                        savingId = e.id;
                        Intent picker = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                        picker.addCategory(Intent.CATEGORY_OPENABLE);
                        picker.setType("application/octet-stream"); picker.putExtra(Intent.EXTRA_TITLE, e.name);
                        startActivityForResult(picker, 1);
                    });
                }
                row.findViewById(R.id.transfer_delete).setOnClickListener(v -> {
                    if (busy) return;
                    new AlertDialog.Builder(this)
                        .setMessage(R.string.transfer_delete_confirm)
                        .setPositiveButton(android.R.string.ok, (d,w) -> {
                            try { TransferProvider.store(this).delete(e.id, e.direction); refresh(); }
                            catch (IOException ex) { status.setText(R.string.transfer_failed); }
                        }).setNegativeButton(android.R.string.cancel, null).show();
                });
            }
        } catch (IOException e) { status.setText(R.string.transfer_failed); }
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != 1 || result != RESULT_OK || data == null || data.getData() == null || savingId == null) return;
        String id = savingId; savingId = null; busy = true;
        status.setText(R.string.transfer_saving);
        worker.execute(() -> {
            boolean ok = false;
            try {
                try (InputStream in = new FileInputStream(TransferProvider.store(this).file(id, "in"));
                     OutputStream out = getContentResolver().openOutputStream(data.getData(), "w")) {
                    if (out == null) throw new IOException();
                    TransferWire.copy(in, out, TransferProvider.store(this).get(id).size, null);
                    out.flush();
                }
                ok = true;
            } catch (IOException | RuntimeException ignored) {}
            boolean success = ok;
            runOnUiThread(() -> {
                busy = false;
                if (!isDestroyed()) { status.setText(success ? R.string.transfer_saved : R.string.transfer_failed); refresh(); }
            });
        });
    }
    @Override protected void onDestroy() { worker.shutdownNow(); super.onDestroy(); }
}
