// M1 smoke test: runs on ART (interpreter) on Darwin with Android 14's libcore.
public class Hello {
    public static void main(String[] args) {
        System.out.println("Hello from ART on " + System.getProperty("os.name") + " " + System.getProperty("os.arch"));
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 5; i++) sb.append(i * i).append(' ');
        System.out.println("squares: " + sb.toString().trim());
        java.util.List<String> l = new java.util.ArrayList<>(java.util.Arrays.asList("c", "a", "b"));
        java.util.Collections.sort(l);
        System.out.println("sorted: " + l + ", hash=" + "andropkg".hashCode());
        Object lock = new Object();
        Thread t = new Thread(() -> { synchronized (lock) { lock.notifyAll(); } });
        synchronized (lock) {
            t.start();
            try { lock.wait(2000); } catch (InterruptedException e) { }
        }
        System.out.println("thread+monitor ok; gc...");
        for (int i = 0; i < 200000; i++) { byte[] b = new byte[64]; }
        System.gc();
        System.out.println("done");
    }
}
