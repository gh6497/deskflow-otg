// SPDX-License-Identifier: Apache-2.0
package com.github.gh6497.deskflowotg;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Runs on a host JDK, with no Android services, hardware or external libraries. */
public final class TransferTest {
    interface Action { void run() throws Exception; }
    static void check(boolean value) { if (!value) throw new AssertionError(); }
    static void rejects(Action action) throws Exception {
        try { action.run(); } catch (IOException expected) { return; }
        throw new AssertionError("Expected IOException");
    }
    static byte[] frame(String name, byte[] content) throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(bytes);
        byte[] utf = name.getBytes(StandardCharsets.UTF_8);
        out.write("DFOTGFT1".getBytes(StandardCharsets.US_ASCII));
        out.writeInt(utf.length); out.write(utf); out.writeLong(content.length);
        out.write(content); out.write(TransferWire.digest().digest(content));
        return bytes.toByteArray();
    }
    static void verify(byte[] frame, byte[] content) throws Exception {
        InputStream input = new ByteArrayInputStream(frame);
        TransferWire.Header header = TransferWire.readHeader(input);
        check(header.size == content.length);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        TransferWire.receive(input, out, header.size, null);
        check(Arrays.equals(content, out.toByteArray()));
    }
    public static void main(String[] args) throws Exception {
        File root = new File(args[0], "store");
        TransferStore store = new TransferStore(root);
        byte[] content = new byte[65536 * 3 + 7];
        new Random(42).nextBytes(content);
        verify(frame("图片.png", content), content);
        verify(frame("empty", new byte[0]), new byte[0]);
        byte[] good = frame("../../a.png", content);
        for (int n : new int[]{0, 7, 8, 11, 20, good.length - 1}) {
            byte[] truncated = Arrays.copyOf(good, n);
            rejects(() -> verify(truncated, content));
        }
        byte[] corrupt = good.clone(); corrupt[corrupt.length - 1] ^= 1;
        rejects(() -> verify(corrupt, content));
        byte[] trailing = Arrays.copyOf(good, good.length + 1);
        rejects(() -> verify(trailing, content));
        byte[] version = good.clone(); version[7] = '2';
        rejects(() -> verify(version, content));
        ByteArrayOutputStream header = new ByteArrayOutputStream();
        DataOutputStream h = new DataOutputStream(header);
        h.writeBytes("DFOTGFT1"); h.writeInt(1); h.writeByte('a'); h.writeLong(TransferWire.MAX_FILE + 1);
        rejects(() -> TransferWire.readHeader(new ByteArrayInputStream(header.toByteArray())));
        check(TransferWire.safeName("CON.txt").equals("_CON.txt"));
        check(TransferWire.safeName("../a\\b\n.txt").equals(".._a_b_.txt"));
        check(TransferWire.safeName("..").equals("file"));
        check(TransferWire.safeName("图".repeat(1000)).getBytes(StandardCharsets.UTF_8).length <= 180);

        String id = TransferStore.newId();
        TransferStore.Entry e = store.stage(id, "图片.png", "out", new ByteArrayInputStream(content), -1, false, null);
        check(e.size == content.length && e.sha256.equals(TransferWire.hex(TransferWire.digest().digest(content))));
        check(Arrays.equals(Files.readAllBytes(store.file(id, "out").toPath()), content));
        rejects(() -> store.file(id, "in"));
        rejects(() -> store.get("../metadata"));
        rejects(() -> store.stage(id, "overwrite", "out", new ByteArrayInputStream(content), -1, false, null));
        check(store.get(id).name.equals("图片.png"));
        check(store.list("out").size() == 1);
        TransferStore reopened = new TransferStore(root);
        check(reopened.get(id).size == content.length);

        String received = TransferStore.newId();
        ByteArrayInputStream input = new ByteArrayInputStream(good);
        TransferWire.Header meta = TransferWire.readHeader(input);
        store.stage(received, meta.name, "in", input, meta.size, true, null);
        check(store.list("in").size() == 1);
        check(!store.get(received).name.contains("/"));
        store.setPublicState(received, "in", "published");
        check(store.get(received).publicState.equals("published"));
        store.writeReceipt(store.get(received));
        check(store.getReceipt(received).sha256.equals(store.get(received).sha256));
        int before = store.list(null).size();
        String failed = TransferStore.newId();
        rejects(() -> store.stage(failed, "bad", "in", new ByteArrayInputStream(corrupt), content.length, true, null));
        check(!new File(root, failed).exists());
        check(!new File(root, "." + failed + ".part").exists());
        rejects(() -> store.stage(TransferStore.newId(), "cancelled", "out", new ByteArrayInputStream(content), -1, false,
                bytes -> { throw new InterruptedIOException(); }));
        rejects(() -> store.stage(TransferStore.newId(), "read-error", "out", new InputStream() {
            @Override public int read() throws IOException { throw new IOException("Source failed"); }
        }, -1, false, null));
        check(store.list(null).size() == before);
        check(Arrays.stream(Objects.requireNonNull(root.list())).noneMatch(n -> n.startsWith(".")));

        // Fill the bounded queue with zero-byte files; no large fixture is needed.
        while (store.list(null).size() < TransferStore.MAX_FILES)
            store.stage(TransferStore.newId(), "empty", "out", new ByteArrayInputStream(new byte[0]), 0, false, null);
        rejects(() -> store.stage(TransferStore.newId(), "overflow", "out", new ByteArrayInputStream(new byte[0]), 0, false, null));
        store.delete(id, "out");
        store.stage(TransferStore.newId(), "slot reused", "out", new ByteArrayInputStream(new byte[0]), 0, false, null);

        // Restart clears only incomplete private staging directories, not completed entries.
        File stale = new File(root, "." + TransferStore.newId() + ".part"); check(stale.mkdir());
        Files.write(new File(stale, "data").toPath(), content);
        TransferStore restarted = new TransferStore(root);
        check(!stale.exists() && restarted.list(null).size() == TransferStore.MAX_FILES);
        System.out.println("PASS: streaming protocol, SHA-256, limits, cancellation, persistence, private storage and cleanup");
    }
}
