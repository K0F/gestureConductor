package org.kof.gestureconductor;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.ImageFormat;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.media.Image;
import android.media.ImageReader;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.Log;
import android.view.Surface;
import android.view.TextureView;
import android.widget.Button;
import android.widget.EditText;
import android.widget.Toast;

import java.nio.ByteBuffer;
import java.util.Arrays;

public class MainActivity extends Activity {

    private static final String TAG = "MainActivity";
    private static final int PERMISSION_CAMERA_REQUEST = 101;

    static {
        System.loadLibrary("gesture_conductor");
    }

    private native boolean nativeInit(int w, int h, String oscHost, int oscPort, boolean audioEnabled);
    private native void nativeProcessFrame(byte[] yPlane, int width, int height, int rowStride, float[] outTelemetry);
    private native void nativeSetOscTarget(String oscHost, int oscPort);
    private native void nativeSetAudioEnabled(boolean enabled);
    private native void nativeClose();

    private TextureView previewView;
    private OverlayView overlayView;
    private EditText editIp, editPort;
    private Button btnAudio, btnFlipCam;

    private boolean audioEnabled = true;
    private int activeFacing = CameraCharacteristics.LENS_FACING_FRONT;
    private String cameraFacingId = null;

    private CameraDevice cameraDevice;
    private CameraCaptureSession captureSession;
    private ImageReader imageReader;
    private HandlerThread backgroundThread;
    private Handler backgroundHandler;

    private final float[] telemetryBuffer = new float[18];
    private byte[] yBytesBuffer = null;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        previewView = findViewById(R.id.camera_preview);
        overlayView = findViewById(R.id.overlay_view);
        editIp = findViewById(R.id.edit_ip);
        editPort = findViewById(R.id.edit_port);
        btnAudio = findViewById(R.id.btn_audio);
        btnFlipCam = findViewById(R.id.btn_flip_cam);

        // Native Engine Init
        String ip = editIp.getText().toString().trim();
        int port = 57120;
        try { port = Integer.parseInt(editPort.getText().toString().trim()); } catch (Exception ignored) {}
        nativeInit(320, 240, ip, port, audioEnabled);

        // UI Event Listeners
        btnAudio.setOnClickListener(v -> {
            audioEnabled = !audioEnabled;
            nativeSetAudioEnabled(audioEnabled);
            btnAudio.setText(audioEnabled ? "SND: ON" : "SND: OFF");
            btnAudio.setBackgroundColor(audioEnabled ? 0xFF33FF66 : 0xFF555555);
        });

        btnFlipCam.setOnClickListener(v -> {
            activeFacing = (activeFacing == CameraCharacteristics.LENS_FACING_FRONT)
                    ? CameraCharacteristics.LENS_FACING_BACK
                    : CameraCharacteristics.LENS_FACING_FRONT;
            reopenCamera();
        });

        TextWatcher oscWatcher = new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void afterTextChanged(Editable s) {
                String targetIp = editIp.getText().toString().trim();
                int targetPort = 57120;
                try { targetPort = Integer.parseInt(editPort.getText().toString().trim()); } catch (Exception ignored) {}
                nativeSetOscTarget(targetIp, targetPort);
            }
        };
        editIp.addTextChangedListener(oscWatcher);
        editPort.addTextChangedListener(oscWatcher);

        previewView.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            @Override public void onSurfaceTextureAvailable(SurfaceTexture surface, int width, int height) {
                openCamera();
            }
            @Override public void onSurfaceTextureSizeChanged(SurfaceTexture surface, int width, int height) {}
            @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture surface) { return true; }
            @Override public void onSurfaceTextureUpdated(SurfaceTexture surface) {}
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        startBackgroundThread();
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.CAMERA}, PERMISSION_CAMERA_REQUEST);
        } else if (previewView.isAvailable()) {
            openCamera();
        }
    }

    @Override
    protected void onPause() {
        closeCamera();
        stopBackgroundThread();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        nativeClose();
        super.onDestroy();
    }

    private void startBackgroundThread() {
        backgroundThread = new HandlerThread("CameraBackground");
        backgroundThread.start();
        backgroundHandler = new Handler(backgroundThread.getLooper());
    }

    private void stopBackgroundThread() {
        if (backgroundThread != null) {
            backgroundThread.quitSafely();
            try {
                backgroundThread.join();
                backgroundThread = null;
                backgroundHandler = null;
            } catch (InterruptedException e) {
                Log.e(TAG, "stopBackgroundThread error", e);
            }
        }
    }

    private void reopenCamera() {
        closeCamera();
        openCamera();
    }

    private void openCamera() {
        CameraManager manager = (CameraManager) getSystemService(Context.CAMERA_SERVICE);
        try {
            cameraFacingId = null;
            for (String id : manager.getCameraIdList()) {
                CameraCharacteristics chars = manager.getCameraCharacteristics(id);
                Integer facing = chars.get(CameraCharacteristics.LENS_FACING);
                if (facing != null && facing == activeFacing) {
                    cameraFacingId = id;
                    break;
                }
            }
            if (cameraFacingId == null && manager.getCameraIdList().length > 0) {
                cameraFacingId = manager.getCameraIdList()[0];
            }

            if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
                return;
            }

            imageReader = ImageReader.newInstance(320, 240, ImageFormat.YUV_420_888, 2);
            imageReader.setOnImageAvailableListener(reader -> {
                Image image = reader.acquireLatestImage();
                if (image == null) return;

                Image.Plane[] planes = image.getPlanes();
                if (planes != null && planes.length > 0) {
                    ByteBuffer yBuffer = planes[0].getBuffer();
                    int rowStride = planes[0].getRowStride();
                    int length = yBuffer.remaining();

                    if (yBytesBuffer == null || yBytesBuffer.length != length) {
                        yBytesBuffer = new byte[length];
                    }
                    yBuffer.get(yBytesBuffer);

                    // Native Processing in pure C (Tracker + Sound + OSC)
                    nativeProcessFrame(yBytesBuffer, 320, 240, rowStride, telemetryBuffer);

                    // Update UI Overlay
                    overlayView.updateTelemetry(telemetryBuffer);
                }
                image.close();
            }, backgroundHandler);

            manager.openCamera(cameraFacingId, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice camera) {
                    cameraDevice = camera;
                    startCaptureSession();
                }
                @Override public void onDisconnected(CameraDevice camera) {
                    camera.close();
                    cameraDevice = null;
                }
                @Override public void onError(CameraDevice camera, int error) {
                    camera.close();
                    cameraDevice = null;
                }
            }, backgroundHandler);

        } catch (CameraAccessException e) {
            Log.e(TAG, "openCamera error", e);
        }
    }

    private void startCaptureSession() {
        if (cameraDevice == null || !previewView.isAvailable() || imageReader == null) return;
        try {
            SurfaceTexture texture = previewView.getSurfaceTexture();
            texture.setDefaultBufferSize(320, 240);
            Surface previewSurface = new Surface(texture);
            Surface readerSurface = imageReader.getSurface();

            final CaptureRequest.Builder builder = cameraDevice.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
            builder.addTarget(previewSurface);
            builder.addTarget(readerSurface);

            cameraDevice.createCaptureSession(Arrays.asList(previewSurface, readerSurface),
                    new CameraCaptureSession.StateCallback() {
                        @Override public void onConfigured(CameraCaptureSession session) {
                            captureSession = session;
                            try {
                                session.setRepeatingRequest(builder.build(), null, backgroundHandler);
                            } catch (CameraAccessException e) {
                                Log.e(TAG, "setRepeatingRequest error", e);
                            }
                        }
                        @Override public void onConfigureFailed(CameraCaptureSession session) {
                            Log.e(TAG, "createCaptureSession configure failed");
                        }
                    }, backgroundHandler);

        } catch (CameraAccessException e) {
            Log.e(TAG, "startCaptureSession error", e);
        }
    }

    private void closeCamera() {
        if (captureSession != null) {
            captureSession.close();
            captureSession = null;
        }
        if (cameraDevice != null) {
            cameraDevice.close();
            cameraDevice = null;
        }
        if (imageReader != null) {
            imageReader.close();
            imageReader = null;
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode == PERMISSION_CAMERA_REQUEST) {
            if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                openCamera();
            } else {
                Toast.makeText(this, "Camera permission required for gesture conductor", Toast.LENGTH_LONG).show();
            }
        }
    }
}
