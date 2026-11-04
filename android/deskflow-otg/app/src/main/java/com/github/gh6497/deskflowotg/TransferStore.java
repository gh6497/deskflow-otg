// SPDX-License-Identifier: Apache-2.0
package com.github.gh6497.deskflowotg;

import java.io.*;
import java.util.*;

/** App-private inbox/outbox. Only complete, verified directories become visible. */
public final class TransferStore {
    public static final int MAX_FILES = 32;
    public static final long MAX_TOTAL = 2L * TransferWire.MAX_FILE;
    public static final class Entry {
        public final String id, name, direction, sha256;
        public final long size;
        Entry(String id, Properties p) throws IOException {
            this.id = id;
            name = p.getProperty("name"); direction = p.getProperty("direction");
            sha256 = p.getProperty("sha256");
            try { size = Long.parseLong(p.getProperty("size", "-1")); }
            catch (NumberFormatException e) { throw new IOException(e); }
            if (name == null || !("in".equals(direction) || "out".equals(direction))
                    || sha256 == null || !sha256.matches("[0-9a-f]{64}")
                    || size < 0 || size > TransferWire.MAX_FILE) throw new IOException("Invalid entry");
        }
    }
    private final File root;
    private int reserved;
    private long reservedBytes;
    public TransferStore(File root) throws IOException {
        this.root = root;
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("Cannot create transfer storage");
        // A new store is created once per app process. Remove only our own incomplete entries.
        File[] dirs = root.listFiles();
        if (dirs != null) for (File d : dirs)
            if (d.getName().matches("\\.[0-9a-f]{32}\\.part")) removeDirectory(d);
    }
    public static boolean validId(String id) { return id != null && id.matches("[0-9a-f]{32}"); }
    public static String newId() { return UUID.randomUUID().toString().replace("-", ""); }
    public synchronized List<Entry> list(String direction) throws IOException {
        List<Entry> result = new ArrayList<>();
        File[] dirs = root.listFiles();
        if (dirs != null) for (File d : dirs) {
            if (!validId(d.getName())) continue;
            Entry entry = get(d.getName());
            if (direction == null || direction.equals(entry.direction)) result.add(entry);
        }
        result.sort(Comparator.comparing(e -> e.id));
        return result;
    }
    public synchronized Entry get(String id) throws IOException {
        if (!validId(id)) throw new IOException("Invalid transfer id");
        Properties p = new Properties();
        try (InputStream in = new FileInputStream(new File(new File(root, id), "metadata"))) {
            p.load(in);
        }
        return new Entry(id, p);
    }
    public synchronized File file(String id, String direction) throws IOException {
        if (!get(id).direction.equals(direction)) throw new IOException("Wrong transfer direction");
        return new File(new File(root, id), "data");
    }
    public synchronized void delete(String id, String direction) throws IOException {
        file(id, direction);
        removeDirectory(new File(root, id));
    }
    private static void removeDirectory(File dir) throws IOException {
        // Never recursively follow names supplied by a sender.
        for (String name : new String[]{"data", "metadata"}) {
            File f = new File(dir, name);
            if (f.exists() && !f.delete()) throw new IOException("Cannot delete transfer");
        }
        if (dir.exists() && !dir.delete()) throw new IOException("Cannot delete transfer directory");
    }
    private synchronized long reserve(long requested) throws IOException {
        List<Entry> entries = list(null);
        long total = 0;
        for (Entry e : entries) total += e.size;
        long available = MAX_TOTAL - total - reservedBytes;
        long bytes = requested < 0 ? Math.min(TransferWire.MAX_FILE, available) : requested;
        if (entries.size() + reserved >= MAX_FILES || bytes < 0 || bytes > available)
            throw new IOException("Transfer storage full; delete saved/sent items first");
        if (root.getUsableSpace() < 32L * 1024 * 1024) throw new IOException("Not enough free space");
        ++reserved;
        reservedBytes += bytes;
        return bytes;
    }
    public Entry stage(String id, String name, String direction, InputStream input,
                       long size, boolean framed, TransferWire.Progress progress) throws IOException {
        if (!validId(id) || !("in".equals(direction) || "out".equals(direction))
                || size < -1 || size > TransferWire.MAX_FILE) throw new IOException("Invalid transfer");
        long reservation = reserve(size);
        File temp = new File(root, "." + id + ".part");
        boolean committed = false, created = false;
        try {
            synchronized (this) {
                if (new File(root, id).exists() || !temp.mkdir()) throw new IOException("Duplicate transfer");
                created = true;
            }
            File data = new File(temp, "data");
            byte[] hash;
            TransferWire.Progress bounded = bytes -> {
                if (bytes > reservation) throw new IOException("Transfer storage full");
                if (root.getUsableSpace() < 16L * 1024 * 1024) throw new IOException("Not enough free space");
                if (progress != null) progress.update(bytes);
            };
            try (FileOutputStream out = new FileOutputStream(data)) {
                hash = framed ? TransferWire.receive(input, out, size, bounded)
                        : TransferWire.copy(input, out, size, bounded);
                out.getFD().sync();
            }
            Properties p = new Properties();
            p.setProperty("name", TransferWire.safeName(name));
            p.setProperty("direction", direction);
            p.setProperty("size", Long.toString(data.length()));
            p.setProperty("sha256", TransferWire.hex(hash));
            try (FileOutputStream out = new FileOutputStream(new File(temp, "metadata"))) {
                p.store(out, "deskflow-otg transfer v1"); out.getFD().sync();
            }
            synchronized (this) {
                if (new File(root, id).exists() || !temp.renameTo(new File(root, id)))
                    throw new IOException("Cannot commit transfer");
                committed = true;
                return get(id);
            }
        } finally {
            synchronized (this) { --reserved; reservedBytes -= reservation; }
            if (created && !committed) removeDirectory(temp);
        }
    }
}
