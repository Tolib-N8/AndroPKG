package dev.andropkg.test.media;

import android.app.Activity;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTimestamp;
import android.media.AudioTrack;
import android.media.MediaMetadataRetriever;
import android.media.MediaPlayer;
import android.media.SoundPool;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

/**
 * Sound: MediaPlayer (a raw resource, then a file with prepareAsync), a streaming AudioTrack
 * (playback head + timestamp), a static AudioTrack with a marker, and audio focus. Everything
 * plays at volume 0. Logs "[media] ..." lines for scripts/test-mac.sh.
 */
public class MainActivity extends Activity {
    static final String TAG = "MediaTest";
    private final Handler handler = new Handler(Looper.getMainLooper());
    // Held in fields: an unreferenced player or track is finalized (and stops), as on Android.
    private MediaPlayer rawPlayer, filePlayer;
    private AudioTrack staticTrack;
    private MediaSession session;
    private SoundPool pool;
    private MediaPlayer oggPlayer;
    private int stepsLeft = 6;  // raw+file chain, streaming, static, session command, soundpool, ogg

    private void stepDone() {
        if (--stepsLeft == 0 && System.getProperty("andropkg.test.autofinish") != null) finish();
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        TextView text = new TextView(this);
        text.setText("media");
        setContentView(text);

        AudioManager am = getSystemService(AudioManager.class);
        int focus = am.requestAudioFocus(new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN).build());
        Log.i(TAG, "[media] focus=" + focus + " max=" + am.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
                + " rate=" + am.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE));

        session = new MediaSession(this, "media-test");
        session.setCallback(new MediaSession.Callback() {
            @Override
            public void onPause() {
                Log.i(TAG, "[media] session onPause");
                stepDone();
            }
        });
        session.setMetadata(new MediaMetadata.Builder().putString(MediaMetadata.METADATA_KEY_TITLE, "Tone")
                .putString(MediaMetadata.METADATA_KEY_ARTIST, "andropkg").putLong(MediaMetadata.METADATA_KEY_DURATION, 600).build());
        session.setPlaybackState(new PlaybackState.Builder().setState(PlaybackState.STATE_PLAYING, 0, 1f)
                .setActions(PlaybackState.ACTION_PLAY_PAUSE | PlaybackState.ACTION_PAUSE).build());
        session.setActive(true);
        // MediaRouter / MediaRouter2: the system route (apps create these at startup)
        android.media.MediaRouter router = (android.media.MediaRouter) getSystemService(MEDIA_ROUTER_SERVICE);
        android.media.MediaRouter2 router2 = android.media.MediaRouter2.getInstance(this);
        Log.i(TAG, "[media] router selected=" + (router.getSelectedRoute(android.media.MediaRouter.ROUTE_TYPE_LIVE_AUDIO) != null)
                + " router2 routes=" + router2.getSystemController().getSelectedRoutes().size()
                + " system=" + router2.getSystemController().getSelectedRoutes().get(0).isSystemRoute());
        Log.i(TAG, "[media] session active controller=" + session.getController().getMetadata().getString(MediaMetadata.METADATA_KEY_TITLE));

        rawResource();
        soundPool();
        oggMediaPlayer();
        streamingTrack();
        staticTrack();
    }

    private void rawResource() {
        MediaPlayer mp = rawPlayer = MediaPlayer.create(this, R.raw.tone);
        if (mp == null) {
            Log.e(TAG, "[media] MediaPlayer.create failed");
            return;
        }
        Log.i(TAG, "[media] raw duration=" + mp.getDuration());
        mp.setVolume(0, 0);
        mp.setOnCompletionListener(p -> {
            Log.i(TAG, "[media] raw completed");
            p.release();
            fileSource();
        });
        mp.start();
        Log.i(TAG, "[media] raw playing=" + mp.isPlaying());
    }

    private void soundPool() {
        pool = new SoundPool.Builder().setMaxStreams(4)
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).build()).build();
        int[] loaded = {0};
        pool.setOnLoadCompleteListener((sp, sampleId, status) -> {
            Log.i(TAG, "[media] soundpool loaded " + sampleId + " status=" + status);
            if (++loaded[0] == 2) {
                int a = sp.play(1, 0f, 0f, 1, 0, 1f);
                int c = sp.play(2, 0f, 0f, 1, 2, 1.5f);
                Log.i(TAG, "[media] soundpool playing " + (a > 0) + " " + (c > 0));
                stepDone();
            }
        });
        pool.load(this, R.raw.tone, 1);
        pool.load(this, R.raw.tone_vorbis, 1);
    }

    private void oggMediaPlayer() {
        MediaPlayer mp = oggPlayer = MediaPlayer.create(this, R.raw.tone_opus);
        if (mp == null) {
            Log.e(TAG, "[media] opus MediaPlayer.create failed");
            return;
        }
        Log.i(TAG, "[media] opus duration=" + (mp.getDuration() / 100) * 100);
        mp.setVolume(0, 0);
        mp.setOnCompletionListener(p -> {
            Log.i(TAG, "[media] opus completed at " + (p.getCurrentPosition() / 100) * 100);
            p.release();
            stepDone();
        });
        mp.start();
        try (android.content.res.AssetFileDescriptor afd = getResources().openRawResourceFd(R.raw.tone_vorbis)) {
            MediaMetadataRetriever r = new MediaMetadataRetriever();
            r.setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getLength());
            long d = Long.parseLong(r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION));
            Log.i(TAG, "[media] vorbis retriever duration=" + (d / 100) * 100 + " mime="
                    + r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_MIMETYPE));
            r.release();
        } catch (Exception e) {
            Log.e(TAG, "[media] vorbis retriever failed", e);
        }
    }

    private void fileSource() {
        try {
            File f = new File(getFilesDir(), "tone-copy");
            try (InputStream in = getResources().openRawResource(R.raw.tone); FileOutputStream out = new FileOutputStream(f)) {
                byte[] buf = new byte[8192];
                for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
            }
            MediaMetadataRetriever r = new MediaMetadataRetriever();
            r.setDataSource(f.getPath());
            Log.i(TAG, "[media] retriever duration=" + r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)
                    + " mime=" + r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_MIMETYPE)
                    + " audio=" + r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_HAS_AUDIO));
            r.release();
            MediaPlayer mp = filePlayer = new MediaPlayer();
            mp.setDataSource(f.getPath());
            mp.setVolume(0, 0);
            mp.setOnPreparedListener(p -> {
                Log.i(TAG, "[media] file prepared");
                p.seekTo(300);
            });
            mp.setOnSeekCompleteListener(p -> {
                Log.i(TAG, "[media] file seek done at " + (p.getCurrentPosition() / 100) * 100);
                p.start();
            });
            mp.setOnCompletionListener(p -> {
                Log.i(TAG, "[media] file completed");
                p.release();
                stepDone();
            });
            mp.prepareAsync();
        } catch (Exception e) {
            Log.e(TAG, "[media] file source failed", e);
        }
    }

    private void streamingTrack() {
        int rate = 48000;
        int min = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        AudioTrack t = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(rate)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setBufferSizeInBytes(Math.max(min, rate)).setTransferMode(AudioTrack.MODE_STREAM).build();
        Log.i(TAG, "[media] track state=" + t.getState() + " rate=" + t.getSampleRate() + " min=" + (min > 0));
        t.setVolume(0);
        t.play();
        int frames = rate * 3 / 10;  // 0.3 s
        short[] pcm = new short[frames * 2];
        for (int i = 0; i < frames; i++) pcm[2 * i] = pcm[2 * i + 1] = (short) (3000 * Math.sin(2 * Math.PI * 440 * i / rate));
        new Thread(() -> {
            int written = t.write(pcm, 0, pcm.length);
            Log.i(TAG, "[media] track wrote " + written);
            long deadline = System.currentTimeMillis() + 3000;
            while (t.getPlaybackHeadPosition() < frames && System.currentTimeMillis() < deadline) {
                try { Thread.sleep(20); } catch (InterruptedException e) { return; }
            }
            AudioTimestamp ts = new AudioTimestamp();
            Log.i(TAG, "[media] track played " + (t.getPlaybackHeadPosition() >= frames) + " timestamp=" + t.getTimestamp(ts));
            t.stop();
            t.release();
            handler.post(this::stepDone);
        }).start();
    }

    private void staticTrack() {
        int rate = 22050, frames = rate / 10;
        short[] pcm = new short[frames];
        AudioTrack t = staticTrack = new AudioTrack(AudioManager.STREAM_MUSIC, rate, AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT, frames * 2, AudioTrack.MODE_STATIC);
        Log.i(TAG, "[media] static wrote " + t.write(pcm, 0, pcm.length) + " state=" + t.getState());
        t.setVolume(0);
        t.setNotificationMarkerPosition(frames);
        t.setPlaybackPositionUpdateListener(new AudioTrack.OnPlaybackPositionUpdateListener() {
            @Override
            public void onMarkerReached(AudioTrack track) {
                Log.i(TAG, "[media] static marker reached");
                track.release();
                stepDone();
            }

            @Override
            public void onPeriodicNotification(AudioTrack track) {}
        }, handler);
        t.play();
    }

    @Override
    protected void onDestroy() {
        if (session != null) session.release();
        if (pool != null) pool.release();
        Log.i(TAG, "[media] done");
        super.onDestroy();
    }
}
