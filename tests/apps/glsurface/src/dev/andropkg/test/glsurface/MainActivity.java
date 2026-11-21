package dev.andropkg.test.glsurface;

import android.app.Activity;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.util.Log;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/** GLSurfaceView (EGL10 via gles_jni, GLES20 via android.opengl): a triangle over a green clear. */
public class MainActivity extends Activity {
    static final String TAG = "GlSurfaceTest";
    GLSurfaceView view;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        view = new GLSurfaceView(this);
        view.setEGLContextClientVersion(2);
        view.setRenderer(new Renderer());
        if (System.getProperty("andropkg.test.autofinish") != null) {  // after the pixel checks
            new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(this::finish, 6000);
        }
        setContentView(view);
    }

    @Override protected void onPause() { super.onPause(); view.onPause(); }
    @Override protected void onResume() { super.onResume(); view.onResume(); }

    static class Renderer implements GLSurfaceView.Renderer {
        int program, aPos, frames, w, h;
        long t1;
        FloatBuffer tri;

        static int shader(int type, String src) {
            int s = GLES20.glCreateShader(type);
            GLES20.glShaderSource(s, src);
            GLES20.glCompileShader(s);
            int[] ok = new int[1];
            GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0);
            if (ok[0] == 0) Log.i(TAG, "[glsv] shader error: " + GLES20.glGetShaderInfoLog(s));
            return s;
        }

        @Override public void onSurfaceCreated(GL10 gl, EGLConfig config) {
            Log.i(TAG, "[glsv] created " + GLES20.glGetString(GLES20.GL_VERSION));
            program = GLES20.glCreateProgram();
            GLES20.glAttachShader(program, shader(GLES20.GL_VERTEX_SHADER,
                "attribute vec2 aPos; void main() { gl_Position = vec4(aPos, 0.0, 1.0); }"));
            GLES20.glAttachShader(program, shader(GLES20.GL_FRAGMENT_SHADER,
                "precision mediump float; void main() { gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0); }"));
            GLES20.glLinkProgram(program);
            aPos = GLES20.glGetAttribLocation(program, "aPos");
            float[] v = {0f, 0.8f, -0.7f, -0.6f, 0.7f, -0.6f};
            tri = ByteBuffer.allocateDirect(v.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
            tri.put(v).position(0);
        }

        @Override public void onSurfaceChanged(GL10 gl, int width, int height) {
            w = width; h = height;
            GLES20.glViewport(0, 0, width, height);
            Log.i(TAG, "[glsv] changed " + width + "x" + height);
        }

        @Override public void onDrawFrame(GL10 gl) {
            GLES20.glClearColor(0f, 1f, 0f, 1f);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            GLES20.glUseProgram(program);
            GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, tri);
            GLES20.glEnableVertexAttribArray(aPos);
            GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 3);
            if (frames == 0) {
                ByteBuffer px = ByteBuffer.allocateDirect(4);
                GLES20.glReadPixels(w / 2, h / 2, 1, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, px);
                Log.i(TAG, String.format("[glsv] center=%02x%02x%02x error=%d", px.get(0) & 0xff, px.get(1) & 0xff,
                        px.get(2) & 0xff, GLES20.glGetError()));
            }
            if (++frames == 1) t1 = System.nanoTime();
            if (frames == 61) Log.i(TAG, "[glsv] fps=" + Math.round(60e9 / (System.nanoTime() - t1)));
        }
    }
}
