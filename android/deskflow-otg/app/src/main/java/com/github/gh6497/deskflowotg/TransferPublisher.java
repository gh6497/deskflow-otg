// SPDX-License-Identifier: Apache-2.0
package com.github.gh6497.deskflowotg;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.webkit.MimeTypeMap;
import java.io.*;
import java.util.HashSet;
import java.util.Set;

/** Publishes a verified private transfer into MediaStore or a user-granted SAF tree. */
public final class TransferPublisher {
    public static void publish(Context context, TransferStore.Entry entry, File source,
                               String treeUriValue) throws IOException {
        String mime = mime(entry.name);
        if (mime.startsWith("image/") || mime.startsWith("video/") || mime.startsWith("audio/")) {
            publishMedia(context, entry, source, mime);
            return;
        }
        if (treeUriValue == null || treeUriValue.isEmpty())
            throw new IOException("Choose a public folder for non-media files");
        publishTree(context, entry, source, mime, Uri.parse(treeUriValue));
    }

    private static String mime(String name) {
        int dot = name.lastIndexOf('.');
        if (dot >= 0 && dot + 1 < name.length()) {
            String value = MimeTypeMap.getSingleton().getMimeTypeFromExtension(
                    name.substring(dot + 1).toLowerCase(java.util.Locale.ROOT));
            if (value != null) return value;
        }
        return "application/octet-stream";
    }

    private static void publishMedia(Context context, TransferStore.Entry entry, File source,
                                     String mime) throws IOException {
        ContentResolver resolver = context.getContentResolver();
        Uri collection;
        String relative;
        if (mime.startsWith("image/")) {
            collection = MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY);
            relative = "Pictures/deskflow-otg";
        } else if (mime.startsWith("video/")) {
            collection = MediaStore.Video.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY);
            relative = "Movies/deskflow-otg";
        } else {
            collection = MediaStore.Audio.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY);
            relative = "Music/deskflow-otg";
        }
        ContentValues values = new ContentValues();
        values.put(MediaStore.MediaColumns.DISPLAY_NAME, entry.name);
        values.put(MediaStore.MediaColumns.MIME_TYPE, mime);
        values.put(MediaStore.MediaColumns.RELATIVE_PATH, relative);
        values.put(MediaStore.MediaColumns.IS_PENDING, 1);
        Uri target = resolver.insert(collection, values);
        if (target == null) throw new IOException("Cannot create media item");
        boolean committed = false;
        try {
            copyVerified(resolver.openOutputStream(target, "w"), source, entry);
            ContentValues ready = new ContentValues();
            ready.put(MediaStore.MediaColumns.IS_PENDING, 0);
            if (resolver.update(target, ready, null, null) != 1)
                throw new IOException("Cannot publish media item");
            committed = true;
        } finally {
            if (!committed) resolver.delete(target, null, null);
        }
    }

    private static void publishTree(Context context, TransferStore.Entry entry, File source,
                                    String mime, Uri tree) throws IOException {
        if (!DocumentsContract.isTreeUri(tree)) throw new IOException("Invalid public folder");
        ContentResolver resolver = context.getContentResolver();
        boolean granted = false;
        for (android.content.UriPermission permission : resolver.getPersistedUriPermissions()) {
            if (tree.equals(permission.getUri()) && permission.isWritePermission()) {
                granted = true; break;
            }
        }
        if (!granted) throw new IOException("Public folder permission is missing");

        String name = uniqueChildName(resolver, tree, entry.name);
        Uri parent = DocumentsContract.buildDocumentUriUsingTree(
                tree, DocumentsContract.getTreeDocumentId(tree));
        if (parent == null) throw new IOException("Cannot resolve public folder");
        try {
            publishTreeWithRename(resolver, parent, mime, name, entry, source);
        } catch (IllegalArgumentException | UnsupportedOperationException rejected) {
            // Some OEM DocumentsProviders reject temporary display names or renameDocument,
            // even though the same persisted tree grants create/write access. Retry with the
            // final collision-free name; incomplete files are deleted on ordinary failures.
            publishTreeDirect(resolver, parent, mime, name, entry, source);
        }
    }

    private static void publishTreeWithRename(ContentResolver resolver, Uri parent, String mime,
                                              String name, TransferStore.Entry entry, File source)
            throws IOException {
        Uri temporary = DocumentsContract.createDocument(
                resolver, parent, mime, "deskflow-" + entry.id + ".part");
        if (temporary == null) throw new IOException("Cannot create public file");
        boolean renamed = false;
        try {
            copyVerified(resolver.openOutputStream(temporary, "w"), source, entry);
            Uri finalUri = DocumentsContract.renameDocument(resolver, temporary, name);
            if (finalUri == null) throw new IOException("Cannot finalize public file");
            renamed = true;
        } finally {
            if (!renamed) resolver.delete(temporary, null, null);
        }
    }

    private static void publishTreeDirect(ContentResolver resolver, Uri parent, String mime,
                                          String name, TransferStore.Entry entry, File source)
            throws IOException {
        Uri target = DocumentsContract.createDocument(resolver, parent, mime, name);
        if (target == null) throw new IOException("Cannot create public file");
        boolean complete = false;
        try {
            copyVerified(resolver.openOutputStream(target, "w"), source, entry);
            complete = true;
        } finally {
            if (!complete) resolver.delete(target, null, null);
        }
    }

    private static String uniqueChildName(ContentResolver resolver, Uri tree, String name)
            throws IOException {
        Set<String> names = new HashSet<>();
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(
                tree, DocumentsContract.getTreeDocumentId(tree));
        try (Cursor cursor = resolver.query(children,
                new String[]{DocumentsContract.Document.COLUMN_DISPLAY_NAME},
                null, null, null)) {
            if (cursor != null) while (cursor.moveToNext()) names.add(cursor.getString(0));
        } catch (RuntimeException e) {
            throw new IOException("Cannot inspect public folder", e);
        }
        if (!names.contains(name)) return name;
        int dot = name.lastIndexOf('.');
        String stem = dot > 0 ? name.substring(0, dot) : name;
        String extension = dot > 0 ? name.substring(dot) : "";
        for (int n = 1; n < 100000; ++n) {
            String candidate = stem + " (" + n + ")" + extension;
            if (!names.contains(candidate)) return candidate;
        }
        throw new IOException("Too many files with the same name");
    }

    private static void copyVerified(OutputStream output, File source, TransferStore.Entry entry)
            throws IOException {
        if (output == null) throw new IOException("Cannot open public file");
        try (OutputStream out = output; InputStream input = new FileInputStream(source)) {
            byte[] hash = TransferWire.copy(input, out, entry.size, null);
            out.flush();
            if (!TransferWire.hex(hash).equals(entry.sha256))
                throw new IOException("Public file hash mismatch");
        }
    }

    private TransferPublisher() {}
}
