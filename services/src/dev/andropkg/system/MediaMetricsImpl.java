package dev.andropkg.system;

import android.util.Base64;
import java.security.SecureRandom;

/**
 * android.media.metrics.IMediaMetricsManager: session ids for MediaMetricsManager's playback,
 * recording, editing and transcoding sessions (ExoPlayer creates a PlaybackSession per player; a
 * null id fails its @NonNull check). Reported metrics go nowhere.
 */
final class MediaMetricsImpl {
    private final SecureRandom random = new SecureRandom();

    private String newId() {
        byte[] b = new byte[12];
        random.nextBytes(b);
        return Base64.encodeToString(b, Base64.URL_SAFE | Base64.NO_WRAP | Base64.NO_PADDING);
    }

    public String getPlaybackSessionId(int userId) { return newId(); }
    public String getRecordingSessionId(int userId) { return newId(); }
    public String getTranscodingSessionId(int userId) { return newId(); }
    public String getEditingSessionId(int userId) { return newId(); }
    public String getBundleSessionId(int userId) { return newId(); }
}
