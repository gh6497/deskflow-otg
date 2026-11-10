// SPDX-License-Identifier: Apache-2.0
package com.github.gh6497.deskflowotg;

import android.content.*;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.*;
import android.util.Log;
import org.json.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/** Only the already-authorized ADB shell (DUMP) may access this provider. No LAN listener. */
public final class TransferProvider extends ContentProvider {
    public static final String AUTHORITY = "com.github.gh6497.deskflowotg.transfer";
    private static final String TAG = "DeskflowTransfer";
    private static TransferStore store;
    private final AtomicBoolean receiving = new AtomicBoolean();
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final ScheduledExecutorService timer = Executors.newSingleThreadScheduledExecutor();
    public static synchronized TransferStore store(Context context) throws IOException {
        if (store == null) store = new TransferStore(new File(context.getFilesDir(), "transfers"));
        return store;
    }
    @Override public boolean onCreate() { return true; }
    private void authorize() {
        // Explicit check also protects methods such as call/getType, for which
        // manifest read/write permission enforcement alone is insufficient.
        if (Binder.getCallingUid() != android.os.Process.myUid()
                && getContext().checkCallingPermission("android.permission.DUMP")
                != PackageManager.PERMISSION_GRANTED) throw new SecurityException("ADB shell only");
    }
    private JSONObject json(TransferStore.Entry e) throws JSONException {
        return new JSONObject().put("id", e.id).put("name", e.name)
                .put("size", e.size).put("sha256", e.sha256);
    }
    private ParcelFileDescriptor document(JSONObject value) throws IOException {
        byte[] bytes = value.toString().getBytes(StandardCharsets.UTF_8);
        return openPipeHelper(Uri.EMPTY, "application/json", null, bytes,
                (fd, uri, mime, options, data) -> {
                    try (OutputStream out = new ParcelFileDescriptor.AutoCloseOutputStream(fd)) {
                        out.write(data);
                    } catch (IOException exception) {
                        Log.w(TAG, "could not write transfer document", exception);
                    }
                });
    }
    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        authorize();
        try {
            List<String> path = uri.getPathSegments();
            TransferStore s = store(getContext());
            if ("r".equals(mode) && path.size() == 1 && "manifest".equals(path.get(0))) {
                JSONArray files = new JSONArray();
                for (TransferStore.Entry e : s.list("out")) files.put(json(e));
                return document(new JSONObject().put("version", 1).put("files", files));
            }
            if (path.size() != 2 || !TransferStore.validId(path.get(1))) throw new IOException("Invalid URI");
            String id = path.get(1);
            if ("r".equals(mode) && "out".equals(path.get(0)))
                return ParcelFileDescriptor.open(s.file(id, "out"), ParcelFileDescriptor.MODE_READ_ONLY);
            if ("r".equals(mode) && "receipt".equals(path.get(0))) {
                try {
                    return document(new JSONObject().put("version", 1)
                            .put("file", json(s.getReceipt(id))));
                } catch (FileNotFoundException ignored) { }
                try {
                    TransferStore.Entry e = s.get(id);
                    if (!"in".equals(e.direction)) throw new IOException("Wrong direction");
                    if ("publishing".equals(e.publicState))
                        return document(new JSONObject().put("version", 1).put("pending", true));
                    if ("failed".equals(e.publicState))
                        return document(new JSONObject().put("version", 1).put("failed", true));
                    return document(new JSONObject().put("version", 1).put("file", json(e)));
                } catch (FileNotFoundException e) {
                    return document(new JSONObject().put("version", 1).put("pending", true));
                }
            }
            if (!"w".equals(mode) || !"in".equals(path.get(0))) throw new IOException("Unsupported operation");
            if (!receiving.compareAndSet(false, true)) throw new IOException("Another upload is active");
            ParcelFileDescriptor[] pipe;
            try { pipe = ParcelFileDescriptor.createReliablePipe(); }
            catch (IOException e) { receiving.set(false); throw e; }
            AtomicLong last = new AtomicLong(SystemClock.elapsedRealtime());
            long started = last.get();
            final boolean autoPublic = TransferSettings.autoPublic(getContext());
            final String publicTree = TransferSettings.treeUri(getContext());
            ScheduledFuture<?> watchdog = timer.scheduleWithFixedDelay(() -> {
                long now = SystemClock.elapsedRealtime();
                if (now - last.get() > 30000 || now - started > 15 * 60 * 1000) {
                    try { pipe[0].close(); }
                    catch (IOException exception) {
                        Log.w(TAG, "could not close timed-out transfer pipe", exception);
                    }
                }
            }, 5, 5, TimeUnit.SECONDS);
            worker.execute(() -> {
                try (InputStream input = new ParcelFileDescriptor.AutoCloseInputStream(pipe[0])) {
                    TransferWire.Header header = TransferWire.readHeader(input);
                    TransferStore.Entry entry = s.stage(id, header.name, "in", input, header.size, true,
                            bytes -> last.set(SystemClock.elapsedRealtime()));
                    if (autoPublic) {
                        s.setPublicState(id, "in", "publishing");
                        try {
                            TransferPublisher.publish(getContext(), entry, s.file(id, "in"),
                                    publicTree);
                            s.writeReceipt(entry);
                            s.delete(id, "in");
                        } catch (IOException | RuntimeException exception) {
                            Log.w(TAG, "public publish failed", exception);
                            try { s.setPublicState(id, "in", "failed"); }
                            catch (IOException stateException) {
                                Log.e(TAG, "could not mark failed transfer", stateException);
                            }
                        }
                    }
                } catch (IOException | RuntimeException exception) {
                    // No receipt is committed on failure. Never log content or source URIs.
                    Log.e(TAG, "transfer worker failed", exception);
                } finally {
                    watchdog.cancel(false);
                    receiving.set(false);
                }
            });
            return pipe[1];
        } catch (IOException | JSONException e) {
            Log.e(TAG, "could not open transfer", e);
            FileNotFoundException failure = new FileNotFoundException(e.getMessage());
            failure.initCause(e);
            throw failure;
        }
    }
    @Override public int delete(Uri uri, String selection, String[] args) {
        authorize();
        List<String> path = uri.getPathSegments();
        if (path.size() != 2 || !"out".equals(path.get(0)) || !TransferStore.validId(path.get(1)))
            throw new IllegalArgumentException("Invalid URI");
        try { store(getContext()).delete(path.get(1), "out"); return 1; }
        catch (IOException e) {
            Log.e(TAG, "could not acknowledge transfer", e);
            throw new IllegalArgumentException("Cannot acknowledge transfer", e);
        }
    }
    @Override public String getType(Uri uri) { authorize(); return "application/octet-stream"; }
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        authorize(); throw new UnsupportedOperationException();
    }
    @Override public Uri insert(Uri uri, ContentValues values) { authorize(); throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) {
        authorize(); throw new UnsupportedOperationException();
    }
    @Override public Bundle call(String method, String arg, Bundle extras) {
        authorize(); throw new UnsupportedOperationException();
    }
}
