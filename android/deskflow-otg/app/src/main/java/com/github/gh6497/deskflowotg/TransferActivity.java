// SPDX-License-Identifier: Apache-2.0
package com.github.gh6497.deskflowotg;

import android.app.*;
import android.content.*;
import android.net.Uri;
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
    private TextView destination;
    private Switch autoPublic;
    private String savingId;
    private boolean busy;
    private boolean enablePublicAfterFolder;
    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (saved != null) savingId = saved.getString("savingId");
        setContentView(R.layout.activity_transfer);
        status = findViewById(R.id.transfer_status);
        destination = findViewById(R.id.transfer_destination);
        rows = findViewById(R.id.transfer_rows);
        autoPublic = findViewById(R.id.transfer_auto_public);
        if (TransferSettings.autoPublic(this)
                && !TransferSettings.hasTreePermission(this))
            TransferSettings.setAutoPublic(this, false);
        if (!TransferSettings.treeUri(this).isEmpty()
                && !TransferSettings.hasTreePermission(this))
            TransferSettings.setTreeUri(this, "");
        autoPublic.setChecked(TransferSettings.autoPublic(this));
        autoPublic.setOnCheckedChangeListener((button, checked) -> {
            if (checked && TransferSettings.treeUri(this).isEmpty()) {
                enablePublicAfterFolder = true;
                button.setChecked(false);
                status.setText(R.string.transfer_choose_folder_first);
                chooseFolder();
                return;
            }
            TransferSettings.setAutoPublic(this, checked);
            updateDestination();
        });
        findViewById(R.id.transfer_choose_folder).setOnClickListener(v -> chooseFolder());
        findViewById(R.id.transfer_refresh).setOnClickListener(v -> refresh());
        updateDestination();
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
                int labelId = "in".equals(e.direction) ? R.string.transfer_incoming : R.string.transfer_outgoing;
                String labelText = getString(labelId, e.name, e.size);
                if ("published".equals(e.publicState)) labelText += " · " + getString(R.string.transfer_public_saved);
                else if ("publishing".equals(e.publicState)) labelText += " · " + getString(R.string.transfer_public_saving);
                else if ("failed".equals(e.publicState)) labelText += " · " + getString(R.string.transfer_public_failed);
                label.setText(labelText);
                if ("in".equals(e.direction)) {
                    Button save = row.findViewById(R.id.transfer_save);
                    save.setVisibility("published".equals(e.publicState) || "publishing".equals(e.publicState)
                            ? View.GONE : View.VISIBLE);
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
    private void chooseFolder() {
        if (busy) return;
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        picker.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(picker, 2);
    }
    private void updateDestination() {
        if (destination == null) return;
        String tree = TransferSettings.treeUri(this);
        if (!TransferSettings.autoPublic(this)) {
            destination.setText(R.string.transfer_destination_private);
        } else if (tree.isEmpty()) {
            destination.setText(R.string.transfer_destination_media_only);
        } else {
            destination.setText(R.string.transfer_destination_folder);
        }
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == 2 && result == RESULT_OK && data != null && data.getData() != null) {
            Uri uri = data.getData();
            try {
                int flags = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
                getContentResolver().takePersistableUriPermission(uri, flags);
                TransferSettings.setTreeUri(this, uri.toString());
                if (enablePublicAfterFolder) {
                    enablePublicAfterFolder = false;
                    TransferSettings.setAutoPublic(this, true);
                    autoPublic.setChecked(true);
                }
                updateDestination();
            } catch (SecurityException ignored) {
                enablePublicAfterFolder = false;
                status.setText(R.string.transfer_folder_failed);
            }
            return;
        }
        if (request == 2) enablePublicAfterFolder = false;
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
