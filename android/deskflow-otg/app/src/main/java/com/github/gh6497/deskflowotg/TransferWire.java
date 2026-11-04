// SPDX-License-Identifier: Apache-2.0
package com.github.gh6497.deskflowotg;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.util.Arrays;

/** Bounded streaming protocol, deliberately independent of Android for host tests. */
public final class TransferWire {
    public static final long MAX_FILE = 1024L * 1024 * 1024;
    public static final int MAX_NAME = 1024;
    private static final byte[] MAGIC = "DFOTGFT1".getBytes(StandardCharsets.US_ASCII);
    public interface Progress { void update(long bytes) throws IOException; }
    public static final class Header {
        public final String name;
        public final long size;
        Header(String name, long size) { this.name = name; this.size = size; }
    }
    public static MessageDigest digest() {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException e) { throw new AssertionError(e); }
    }
    public static String hex(byte[] bytes) {
        StringBuilder s = new StringBuilder();
        for (byte b : bytes) s.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
        return s.toString();
    }
    public static String safeName(String value) {
        String name = value == null ? "file" : value;
        // Untrusted providers can return very long display names. Bound work
        // before regex sanitization and byte-length trimming, not afterwards.
        if (name.length() > 180) {
            int end = Character.isHighSurrogate(name.charAt(179)) ? 179 : 180;
            name = name.substring(0, end);
        }
        name = name.replaceAll("[\\p{Cntrl}\\\\/:*?\"<>|\\p{Cf}]", "_").trim();
        name = name.replaceAll("[. ]+$", "");
        if (name.isEmpty() || name.equals(".") || name.equals("..")) name = "file";
        if (name.matches("(?i)(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(\\..*)?")) name = "_" + name;
        // Bound encoded bytes as well as characters (filesystem component limits).
        while (name.getBytes(StandardCharsets.UTF_8).length > 180)
            name = name.substring(0, name.offsetByCodePoints(name.length(), -1));
        return name;
    }
    public static Header readHeader(InputStream input) throws IOException {
        DataInputStream in = new DataInputStream(input);
        byte[] magic = new byte[8]; in.readFully(magic);
        if (!Arrays.equals(magic, MAGIC)) throw new IOException("Protocol version");
        int n = in.readInt();
        if (n < 1 || n > MAX_NAME) throw new IOException("Name length");
        byte[] name = new byte[n]; in.readFully(name);
        long size = in.readLong();
        if (size < 0 || size > MAX_FILE) throw new IOException("File size limit (1 GiB)");
        return new Header(safeName(new String(name, StandardCharsets.UTF_8)), size);
    }
    public static byte[] copy(InputStream in, OutputStream out, long size, Progress progress)
            throws IOException {
        MessageDigest digest = digest();
        byte[] buffer = new byte[65536];
        long total = 0;
        while (size < 0 || total < size) {
            if (Thread.currentThread().isInterrupted()) throw new InterruptedIOException();
            int count = in.read(buffer, 0, size < 0 ? buffer.length
                    : (int) Math.min(buffer.length, size - total));
            if (count < 0) {
                if (size >= 0) throw new EOFException("Truncated file");
                break;
            }
            if (count == 0) continue;
            total += count;
            if (total > MAX_FILE) throw new IOException("File size limit (1 GiB)");
            out.write(buffer, 0, count);
            digest.update(buffer, 0, count);
            if (progress != null) progress.update(total);
        }
        return digest.digest();
    }
    public static byte[] receive(InputStream in, OutputStream out, long size, Progress progress)
            throws IOException {
        byte[] actual = copy(in, out, size, progress);
        byte[] expected = new byte[32]; new DataInputStream(in).readFully(expected);
        if (!MessageDigest.isEqual(actual, expected)) throw new IOException("SHA-256 mismatch");
        if (in.read() != -1) throw new IOException("Trailing data");
        return actual;
    }
    private TransferWire() {}
}
