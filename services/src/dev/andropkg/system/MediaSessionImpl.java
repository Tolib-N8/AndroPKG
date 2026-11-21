package dev.andropkg.system;

import android.graphics.Bitmap;
import android.media.MediaMetadata;
import android.os.SystemClock;
import java.io.ByteArrayOutputStream;
import android.media.session.PlaybackState;
import android.os.Bundle;
import android.os.Process;
import android.util.Log;
import java.util.ArrayList;
import java.util.List;

/**
 * android.media.session.ISessionManager: media sessions inside the app. A session keeps the
 * metadata and playback state the app publishes, hands them to its controllers (and their
 * callbacks), and routes transport controls (play, pause, next, seek, ...) back to the app's
 * MediaSession.Callback. The active session is mirrored to the host's "Now Playing" (iOS lock
 * screen and Control Center, macOS Control Center), whose remote commands (play/pause, next,
 * previous, seek; headphones, media keys) come back as transport controls.
 */
final class MediaSessionImpl {
    private static final String TAG = "andropkg";
    private final List<Session> sessions = new ArrayList<>();
    static MediaSessionImpl instance;

    MediaSessionImpl() { instance = this; }

    // Host "Now Playing" (runtime/media/audio_track.mm; registered when the first window or
    // AudioTrack appears). state: PlaybackState.STATE_*; actions: PlaybackState.ACTION_* mask.
    static native void nativeNowPlaying(String title, String artist, String album, long durationMs,
            long positionMs, int state, float speed, long actions, byte[] artwork);
    static native void nativeNowPlayingClear();
    private static volatile boolean hostReady;

    /** Called by the runtime once the natives above are bound. */
    static void onHostReady() {
        hostReady = true;
        MediaSessionImpl m = instance;
        Session s = m != null ? m.active() : null;
        if (s != null) s.publish();
    }

    /** A remote command from the host (play, pause, toggle, next, previous, stop, seek). */
    static void hostCommand(String command, long arg) {
        MediaSessionImpl m = instance;
        Session s = m != null ? m.active() : null;
        if (s == null) return;
        Log.i(TAG, "media session: host command " + command);
        switch (command) {
            case "toggle":
                s.command(s.state != null && s.state.getState() == PlaybackState.STATE_PLAYING ? "onPause" : "onPlay");
                break;
            case "play": s.command("onPlay"); break;
            case "pause": s.command("onPause"); break;
            case "stop": s.command("onStop"); break;
            case "next": s.command("onNext"); break;
            case "previous": s.command("onPrevious"); break;
            case "seek": s.command("onSeekTo", arg); break;
            default: break;
        }
    }

    /** The most recently activated session, or null (target of host media commands). */
    synchronized Session active() {
        for (int i = sessions.size() - 1; i >= 0; i--) if (sessions.get(i).active) return sessions.get(i);
        return null;
    }

    // createSession(String packageName, ISessionCallback cb, String tag, Bundle sessionInfo, int userId)
    public Object createSession(String pkg, Object callback, String tag, Bundle info, int userId) {
        Session s = new Session(pkg, callback, tag, info);
        synchronized (this) { sessions.add(s); }
        return LocalService.create("android.media.session.ISession", s).iface;
    }

    public List<Object> getSessions(Object componentName, int userId) {
        List<Object> tokens = new ArrayList<>();
        synchronized (this) {
            for (Session s : sessions) if (s.active) tokens.add(s.token());
        }
        return tokens;
    }

    /** ISession (the app's side) + its ISessionController (the controllers' side). */
    final class Session {
        final String pkg, tag;
        final Object callback;  // ISessionCallback
        final Bundle info;
        final Object controller;  // ISessionController
        final List<Object> controllerCallbacks = new ArrayList<>();
        volatile boolean active;
        volatile long flags;
        volatile MediaMetadata metadata;
        volatile PlaybackState state;
        volatile Bundle extras;
        volatile Object launchIntent;
        volatile CharSequence queueTitle;
        volatile int ratingType;

        Session(String pkg, Object callback, String tag, Bundle info) {
            this.pkg = pkg;
            this.callback = callback;
            this.tag = tag;
            this.info = info;
            this.controller = LocalService.create("android.media.session.ISessionController", new Controller()).iface;
        }

        Object token() {
            return Hidden.newInstance(Hidden.cls("android.media.session.MediaSession$Token"), Process.myUid(), controller);
        }

        // ---- ISession ----------------------------------------------------------------------
        public Object getController() { return controller; }
        public void setActive(boolean on) {
            active = on;
            if (on) publish();
            else if (hostReady && active() == null) nativeNowPlayingClear();
        }
        public void setFlags(int f) { flags = f; }
        public void setExtras(Bundle b) { extras = b; }
        public void setLaunchPendingIntent(Object pi) { launchIntent = pi; }
        public void setQueueTitle(CharSequence t) { queueTitle = t; }
        public void setRatingType(int t) { ratingType = t; }

        public void setMetadata(MediaMetadata m, long duration, String description) {
            metadata = m;
            if (m != null) {
                Log.i(TAG, "media session: now playing \"" + m.getString(MediaMetadata.METADATA_KEY_TITLE) + "\" by "
                        + m.getString(MediaMetadata.METADATA_KEY_ARTIST));
            }
            for (Object cb : callbacks()) Hidden.call(cb, "onMetadataChanged", m);
            publish();
        }

        public void setPlaybackState(PlaybackState s) {
            state = s;
            for (Object cb : callbacks()) Hidden.call(cb, "onPlaybackStateChanged", s);
            publish();
        }

        private Bitmap lastArt;
        private byte[] lastArtBytes;

        /** Mirrors this session to the host's Now Playing if it is the active one. */
        void publish() {
            if (!hostReady || !active || active() != this) return;
            MediaMetadata m = metadata;
            PlaybackState st = state;
            String title = null, artist = null, album = null;
            long duration = 0;
            byte[] art = null;
            if (m != null) {
                title = m.getString(MediaMetadata.METADATA_KEY_TITLE);
                if (title == null) title = m.getString(MediaMetadata.METADATA_KEY_DISPLAY_TITLE);
                artist = m.getString(MediaMetadata.METADATA_KEY_ARTIST);
                album = m.getString(MediaMetadata.METADATA_KEY_ALBUM);
                duration = m.getLong(MediaMetadata.METADATA_KEY_DURATION);
                Bitmap b = m.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART);
                if (b == null) b = m.getBitmap(MediaMetadata.METADATA_KEY_ART);
                if (b != null && b != lastArt) {
                    ByteArrayOutputStream out = new ByteArrayOutputStream();
                    b.compress(Bitmap.CompressFormat.PNG, 100, out);
                    lastArt = b;
                    lastArtBytes = out.toByteArray();
                }
                art = b != null ? lastArtBytes : null;
            }
            int s = st != null ? st.getState() : PlaybackState.STATE_NONE;
            float speed = st != null ? st.getPlaybackSpeed() : 0f;
            long pos = 0;
            if (st != null && st.getPosition() >= 0) {
                pos = st.getPosition();
                if (s == PlaybackState.STATE_PLAYING && st.getLastPositionUpdateTime() > 0) {
                    pos += (long) ((SystemClock.elapsedRealtime() - st.getLastPositionUpdateTime()) * speed);
                }
            }
            long actions = st != null ? st.getActions() : 0;
            nativeNowPlaying(title, artist, album, duration, pos, s, speed, actions, art);
        }

        public void destroySession() {
            active = false;
            synchronized (MediaSessionImpl.this) { sessions.remove(this); }
            if (hostReady && active() == null) nativeNowPlayingClear();
            for (Object cb : callbacks()) Hidden.call(cb, "onSessionDestroyed");
        }

        private List<Object> callbacks() {
            synchronized (controllerCallbacks) { return new ArrayList<>(controllerCallbacks); }
        }

        /** A transport command to the app's MediaSession.Callback, as from a controller. */
        void command(String method, Object... extra) {
            Object[] args = new Object[3 + extra.length];
            args[0] = pkg;
            args[1] = Process.myPid();
            args[2] = Process.myUid();
            System.arraycopy(extra, 0, args, 3, extra.length);
            Hidden.call(callback, method, args);
        }

        // ---- ISessionController ------------------------------------------------------------
        final class Controller {
            public String getTag() { return tag; }
            public String getPackageName() { return pkg; }
            public Bundle getSessionInfo() { return info; }
            public long getFlags() { return flags; }
            public Bundle getExtras() { return extras; }
            public Object getLaunchPendingIntent() { return launchIntent; }
            public MediaMetadata getMetadata() { return metadata; }
            public PlaybackState getPlaybackState() { return state; }
            public CharSequence getQueueTitle() { return queueTitle; }
            public int getRatingType() { return ratingType; }

            public void registerCallback(String pkg, Object cb) {
                synchronized (controllerCallbacks) { controllerCallbacks.add(cb); }
            }

            public void unregisterCallback(Object cb) {
                synchronized (controllerCallbacks) {
                    controllerCallbacks.removeIf(c -> ((android.os.IInterface) c).asBinder() == ((android.os.IInterface) cb).asBinder());
                }
            }

            public void play(String caller) { command("onPlay"); }
            public void pause(String caller) { command("onPause"); }
            public void stop(String caller) { command("onStop"); }
            public void next(String caller) { command("onNext"); }
            public void previous(String caller) { command("onPrevious"); }
            public void fastForward(String caller) { command("onFastForward"); }
            public void rewind(String caller) { command("onRewind"); }
            public void seekTo(String caller, long pos) { command("onSeekTo", pos); }
        }
    }
}
