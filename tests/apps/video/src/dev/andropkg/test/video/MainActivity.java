package dev.andropkg.test.video;

import android.app.Activity;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Bundle;
import android.util.Log;
import android.graphics.SurfaceTexture;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.TextureView;
import android.view.SurfaceView;
import android.widget.LinearLayout;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

/**
 * Video decoding through MediaCodec onto SurfaceViews: an H.264 stream (green frames, with
 * B-frames) on top, an HEVC stream (red frames) below. Both are Annex-B elementary streams with
 * access unit delimiters and in-band parameter sets, fed one access unit at a time.
 */
public class MainActivity extends Activity {
    static final String TAG = "VideoTest";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.addView(player("video/avc", "green_avc.bin", 9), new LinearLayout.LayoutParams(-1, 0, 1));
        root.addView(player("video/hevc", "red_hevc.bin", 35), new LinearLayout.LayoutParams(-1, 0, 1));
        root.addView(texturePlayer("video/avc", "blue_avc.bin", 9), new LinearLayout.LayoutParams(-1, 0, 1));
        root.addView(extractorPlayer("yellow.mp4.bin"), new LinearLayout.LayoutParams(-1, 0, 1));
        root.addView(videoView("magenta.mp4.bin"), new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(root);
        // What players (ExoPlayer's MediaCodecUtil) read before choosing a decoder.
        for (android.media.MediaCodecInfo info : new android.media.MediaCodecList(android.media.MediaCodecList.ALL_CODECS).getCodecInfos()) {
            for (String type : info.getSupportedTypes()) {
                if (!type.startsWith("video/")) continue;
                android.media.MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(type);
                Log.i(TAG, "[video] codec " + info.getName() + " " + type + " 1080p="
                        + caps.getVideoCapabilities().isSizeSupported(1920, 1080) + " profiles=" + caps.profileLevels.length
                        + " adaptive=" + caps.isFeatureSupported(android.media.MediaCodecInfo.CodecCapabilities.FEATURE_AdaptivePlayback));
            }
        }
    }

    SurfaceView player(String mime, String asset, int audType) {
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {
                new Thread(() -> play(mime, asset, audType, holder), mime).start();
            }
            @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) {}
            @Override public void surfaceDestroyed(SurfaceHolder h) {}
        });
        return view;
    }

    /** The same through a TextureView: its SurfaceTexture, wrapped in a Surface (as ExoPlayer does). */
    TextureView texturePlayer(String mime, String asset, int audType) {
        TextureView view = new TextureView(this);
        view.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            @Override public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) {
                Log.i(TAG, "[video] texture available " + w + "x" + h);
                Surface surface = new Surface(st);
                new Thread(() -> play(mime + " (texture)", mime, asset, audType, surface), "texture").start();
            }
            @Override public void onSurfaceTextureSizeChanged(SurfaceTexture st, int w, int h) {}
            @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture st) { return true; }
            @Override public void onSurfaceTextureUpdated(SurfaceTexture st) {}
        });
        return view;
    }

    /** An MP4 (H.264 with B-frames + AAC) demuxed by MediaExtractor. */
    SurfaceView extractorPlayer(String asset) {
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {
                new Thread(() -> extract(asset, holder.getSurface()), "extractor").start();
            }
            @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) {}
            @Override public void surfaceDestroyed(SurfaceHolder h) {}
        });
        return view;
    }

    void extract(String asset, Surface surface) {
        try (android.content.res.AssetFileDescriptor fd = getAssets().openFd(asset)) {
            android.media.MediaExtractor ex = new android.media.MediaExtractor();
            ex.setDataSource(fd.getFileDescriptor(), fd.getStartOffset(), fd.getLength());
            StringBuilder tracks = new StringBuilder();
            int video = -1;
            MediaFormat vf = null;
            for (int i = 0; i < ex.getTrackCount(); i++) {
                MediaFormat f = ex.getTrackFormat(i);
                String mime = f.getString(MediaFormat.KEY_MIME);
                tracks.append(mime).append(' ');
                if (mime.startsWith("video/")) {
                    video = i;
                    vf = f;
                }
            }
            ex.selectTrack(video);
            MediaCodec codec = MediaCodec.createDecoderByType(vf.getString(MediaFormat.KEY_MIME));
            codec.configure(vf, surface, null, 0);
            codec.start();
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            int samples = 0, sync = 0, rendered = 0;
            boolean inEos = false, eos = false;
            for (int spins = 0; !eos && spins < 2000; spins++) {
                if (!inEos) {
                    int in = codec.dequeueInputBuffer(10000);
                    if (in >= 0) {
                        int n = ex.readSampleData(codec.getInputBuffer(in), 0);
                        if (n < 0) {
                            codec.queueInputBuffer(in, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inEos = true;
                        } else {
                            if ((ex.getSampleFlags() & android.media.MediaExtractor.SAMPLE_FLAG_SYNC) != 0) sync++;
                            codec.queueInputBuffer(in, 0, n, ex.getSampleTime(), 0);
                            samples++;
                            ex.advance();
                        }
                    }
                }
                int out = codec.dequeueOutputBuffer(info, 10000);
                if (out >= 0) {
                    codec.releaseOutputBuffer(out, info.size > 0);
                    if (info.size > 0) rendered++;
                    eos = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                }
            }
            codec.release();
            ex.release();
            Log.i(TAG, "[video] extractor tracks=" + tracks.toString().trim() + " size=" + vf.getInteger(MediaFormat.KEY_WIDTH)
                    + "x" + vf.getInteger(MediaFormat.KEY_HEIGHT) + " csd=" + vf.containsKey("csd-0") + " samples=" + samples
                    + " sync=" + sync + " rendered=" + rendered);
        } catch (Exception e) {
            Log.e(TAG, "[video] extractor failed", e);
        }
    }

    /** VideoView: MediaPlayer onto its SurfaceView. */
    android.widget.VideoView videoView(String asset) {
        android.widget.VideoView view = new android.widget.VideoView(this);
        try {
            java.io.File f = new java.io.File(getCacheDir(), asset.replace(".bin", ""));
            try (InputStream in = getAssets().open(asset); java.io.FileOutputStream out = new java.io.FileOutputStream(f)) {
                byte[] b = new byte[8192];
                for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
            }
            view.setOnPreparedListener(mp -> Log.i(TAG, "[video] videoview prepared " + mp.getVideoWidth() + "x"
                    + mp.getVideoHeight() + " duration=" + (mp.getDuration() / 100) * 100));
            view.setOnCompletionListener(mp -> Log.i(TAG, "[video] videoview completed"));
            view.setVideoPath(f.getPath());
            view.start();
        } catch (Exception e) {
            Log.e(TAG, "[video] videoview failed", e);
        }
        return view;
    }

    void play(String mime, String asset, int audType, SurfaceHolder holder) {
        play(mime, mime, asset, audType, holder.getSurface());
    }

    void play(String label, String mime, String asset, int audType, Surface surface) {
        try {
            List<byte[]> units = accessUnits(read(asset), mime.equals("video/hevc"), audType);
            MediaFormat format = MediaFormat.createVideoFormat(mime, 320, 240);
            MediaCodec codec = MediaCodec.createDecoderByType(mime);
            codec.configure(format, surface, null, 0);
            codec.start();
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            int next = 0, rendered = 0;
            String outFormat = "";
            boolean eos = false;
            for (int spins = 0; !eos && spins < 2000; spins++) {
                if (next <= units.size()) {
                    int in = codec.dequeueInputBuffer(10000);
                    if (in >= 0) {
                        ByteBuffer buf = codec.getInputBuffer(in);
                        buf.clear();
                        if (next < units.size()) {
                            buf.put(units.get(next));
                            codec.queueInputBuffer(in, 0, units.get(next).length, next * 33333L, 0);
                        } else {
                            codec.queueInputBuffer(in, 0, 0, next * 33333L, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        }
                        next++;
                    }
                }
                int out = codec.dequeueOutputBuffer(info, 10000);
                if (out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat f = codec.getOutputFormat();
                    outFormat = f.getInteger(MediaFormat.KEY_WIDTH) + "x" + f.getInteger(MediaFormat.KEY_HEIGHT);
                } else if (out >= 0) {
                    boolean last = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                    codec.releaseOutputBuffer(out, info.size > 0);
                    if (info.size > 0) rendered++;
                    eos = last;
                }
            }
            codec.stop();
            codec.release();
            Log.i(TAG, "[video] " + label + " units=" + units.size() + " rendered=" + rendered + " format=" + outFormat);
        } catch (Exception e) {
            Log.e(TAG, "[video] " + label + " failed", e);
        }
    }

    byte[] read(String asset) throws Exception {
        try (InputStream in = getAssets().open(asset)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] b = new byte[8192];
            for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
            return out.toByteArray();
        }
    }

    /** Splits an Annex-B stream at access unit delimiters (NAL type audType). */
    static List<byte[]> accessUnits(byte[] s, boolean hevc, int audType) {
        List<Integer> starts = new ArrayList<>();
        for (int i = 0; i + 4 < s.length; i++) {
            if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
                int type = hevc ? (s[i + 3] >> 1) & 0x3f : s[i + 3] & 0x1f;
                if (type == audType) starts.add(i > 0 && s[i - 1] == 0 ? i - 1 : i);
            }
        }
        List<byte[]> units = new ArrayList<>();
        for (int k = 0; k < starts.size(); k++) {
            int a = starts.get(k), b = k + 1 < starts.size() ? starts.get(k + 1) : s.length;
            byte[] u = new byte[b - a];
            System.arraycopy(s, a, u, 0, u.length);
            units.add(u);
        }
        return units;
    }
}
