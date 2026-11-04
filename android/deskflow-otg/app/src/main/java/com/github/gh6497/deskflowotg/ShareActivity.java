// SPDX-License-Identifier: Apache-2.0
package com.github.gh6497.deskflowotg;

import android.app.Activity;
import android.content.*;
import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.SystemClock;
import android.provider.OpenableColumns;
import android.widget.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;

/** User-confirmed shares. Stage while URI grants are valid, never scan other files. */
public final class ShareActivity extends Activity {
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private volatile boolean cancelled;
    private volatile InputStream activeInput;
    private final CancellationSignal sourceCancellation = new CancellationSignal();
    private TextView status;
    private Button send;
    private ProgressBar progress;
    private final List<Uri> sources = new ArrayList<>();
    private String text;
    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL);
        int p = (int) (24 * getResources().getDisplayMetrics().density); root.setPadding(p,p,p,p);
        status = new TextView(this); root.addView(status);
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(100); root.addView(progress);
        send = new Button(this); send.setText(R.string.share_confirm); root.addView(send);
        Button cancel = new Button(this); cancel.setText(android.R.string.cancel); root.addView(cancel);
        cancel.setOnClickListener(v -> { cancel(); finish(); });
        setContentView(root);
        try {
            Intent intent = getIntent();
            if (!Intent.ACTION_SEND.equals(intent.getAction())
                    && !Intent.ACTION_SEND_MULTIPLE.equals(intent.getAction())) throw new IOException();
            LinkedHashSet<Uri> uris = new LinkedHashSet<>();
            if (Intent.ACTION_SEND_MULTIPLE.equals(intent.getAction())) {
                ArrayList<Uri> list = intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM);
                if (list != null) uris.addAll(list);
            } else {
                Uri uri = intent.getParcelableExtra(Intent.EXTRA_STREAM);
                if (uri != null) uris.add(uri);
            }
            ClipData clip = intent.getClipData();
            if (clip != null) for (int i=0; i<clip.getItemCount(); ++i) {
                Uri uri = clip.getItemAt(i).getUri(); if (uri != null) uris.add(uri);
            }
            Object shared = intent.getExtras() == null ? null : intent.getExtras().get(Intent.EXTRA_TEXT);
            StringBuilder sharedText = new StringBuilder();
            if (shared instanceof CharSequence) {
                if (((CharSequence) shared).length() > 262144) throw new IOException();
                sharedText.append((CharSequence) shared);
            } else if (shared instanceof ArrayList<?>) {
                for (Object item : (ArrayList<?>) shared) {
                    if (!(item instanceof CharSequence)) throw new IOException();
                    if (sharedText.length() + ((CharSequence) item).length() + 1 > 262144) throw new IOException();
                    if (sharedText.length() > 0) sharedText.append('\n');
                    sharedText.append((CharSequence) item);
                }
            } else if (shared != null) throw new IOException();
            text = sharedText.toString();
            for (Uri uri : uris) {
                if (uri == null || !"content".equals(uri.getScheme())) throw new IOException();
                sources.add(uri);
            }
            int count = sources.size() + (text.isEmpty() ? 0 : 1);
            if (count == 0 || count > TransferStore.MAX_FILES) throw new IOException();
            status.setText(getString(R.string.share_description, count));
            send.setOnClickListener(v -> {
                send.setEnabled(false);
                progress.setIndeterminate(true);
                worker.execute(this::stage);
            });
        } catch (IOException | RuntimeException e) {
            status.setText(R.string.share_invalid); send.setEnabled(false);
        }
    }
    private void stage() {
        List<String> staged = new ArrayList<>();
        try {
            TransferStore store = TransferProvider.store(this);
            if (!text.isEmpty()) {
                checkCancelled();
                byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
                String id = TransferStore.newId();
                store.stage(id, "shared-text.txt", "out", new ByteArrayInputStream(bytes), bytes.length, false, null);
                staged.add(id);
            }
            for (Uri uri : sources) {
                checkCancelled();
                String name = "file";
                try (Cursor c = getContentResolver().query(uri,
                        new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null, sourceCancellation)) {
                    if (c != null && c.moveToFirst()) name = c.getString(0);
                }
                String id = TransferStore.newId();
                final String displayName = TransferWire.safeName(name);
                final long[] lastUpdate = {0};
                try (AssetFileDescriptor asset = getContentResolver().openAssetFileDescriptor(uri, "r", sourceCancellation)) {
                    if (asset == null) throw new IOException();
                    try (InputStream input = asset.createInputStream()) {
                        activeInput = input;
                        store.stage(id, name, "out", input, -1, false, bytes -> {
                            checkCancelled();
                            long now = SystemClock.elapsedRealtime();
                            if (now - lastUpdate[0] >= 200) {
                                lastUpdate[0] = now;
                                runOnUiThread(() -> {
                                    if (!isDestroyed()) status.setText(getString(R.string.share_preparing, displayName, bytes));
                                });
                            }
                        });
                        staged.add(id);
                    }
                } finally { activeInput = null; }
                int percent = 100 * staged.size() / (sources.size() + (text.isEmpty() ? 0 : 1));
                runOnUiThread(() -> progress.setProgress(percent));
            }
            checkCancelled();
            runOnUiThread(() -> {
                if (!isDestroyed()) { progress.setIndeterminate(false); progress.setProgress(100); status.setText(R.string.share_queued); }
            });
        } catch (IOException | RuntimeException e) {
            for (String id : staged) try { TransferProvider.store(this).delete(id, "out"); }
            catch (IOException ignored) {}
            runOnUiThread(() -> {
                if (!isDestroyed()) { progress.setIndeterminate(false); status.setText(R.string.transfer_failed); }
            });
        }
    }
    private void checkCancelled() throws IOException {
        if (cancelled || Thread.currentThread().isInterrupted()) throw new InterruptedIOException();
    }
    private void cancel() {
        cancelled = true; worker.shutdownNow();
        InputStream input = activeInput;
        // Closing a remote content stream can block; never block the UI thread.
        new Thread(() -> {
            sourceCancellation.cancel();
            if (input != null) try { input.close(); } catch (IOException ignored) {}
        }).start();
    }
    @Override protected void onDestroy() { cancel(); super.onDestroy(); }
}
