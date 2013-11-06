package cookbook.chapter7.openmaxsldemo;

import android.os.Bundle;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.app.Activity;

public class MainActivity extends Activity {
	
   
    
    private static native void naCreateEngine();
    private static native void naSetSurface(Surface surface);
    private static native boolean naCreateStreamingMediaPlayer(String filename); 
    
    private static native void naRewindStreamingMediaPlayer();
    private static native void naSetPlayingStreamingMediaPlayer(boolean isPlaying);
    private static native void naShutdown();
    
    static {
        System.loadLibrary("OpenMAXSLDemo");
    }
    
    
	private static final String TAG = "OpenMAXSLDemo";	
	private boolean mIsPlayingStreaming = false;
    private SurfaceView mSurfaceView;
    private SurfaceHolder mSurfaceHolder;
    
    private EditText editFileName;    
    private Button playPauseBtn, finishBtn, rewindBtn;
    
 
    
    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        
        mSurfaceView = (SurfaceView) findViewById(R.id.surfaceview1);
        mSurfaceHolder = mSurfaceView.getHolder();
        
        naCreateEngine();
        
        mSurfaceHolder.addCallback(new SurfaceHolder.Callback() {
			@Override
			public void surfaceChanged(SurfaceHolder holder, int format, int width, 
					int height) {
				Log.i(TAG, "surfaceChanged format=" + format + ", width=" + width + ", height="
                        + height);
			}
			@Override
			public void surfaceCreated(SurfaceHolder holder) {
				 Log.i(TAG, "surfaceCreated");
				 naSetSurface(holder.getSurface());
			}
			@Override
			public void surfaceDestroyed(SurfaceHolder arg0) {
				Log.i(TAG, "surfaceDestroyed");
			}
        });
        
        editFileName = (EditText) findViewById(R.id.filename_edit);
        
        playPauseBtn = (Button) findViewById(R.id.play_pause_button);
        playPauseBtn.setOnClickListener(new View.OnClickListener() {
        	boolean created = false;
			@Override
			public void onClick(View arg0) {
				if (!created) {
					//play
					Surface s = mSurfaceHolder.getSurface();
		            naSetSurface(s);
		            s.release();
		            String filePath = editFileName.getText().toString();
                    if (filePath != null) {
                        created = naCreateStreamingMediaPlayer(filePath);
                    }
                }
                if (created) {
                	//pause
                    mIsPlayingStreaming = !mIsPlayingStreaming;
                    naSetPlayingStreamingMediaPlayer(mIsPlayingStreaming);
                }
			}
		});
        
        finishBtn = (Button) findViewById(R.id.finish_button);
        finishBtn.setOnClickListener(new View.OnClickListener() {
			@Override
			public void onClick(View arg0) {
				finish();
			}
		});
        
        rewindBtn = (Button) findViewById(R.id.rewind_button);
        rewindBtn.setOnClickListener(new View.OnClickListener() {
			@Override
			public void onClick(View arg0) {
				naRewindStreamingMediaPlayer();
			}
		});
    }
    
    @Override
    protected void onPause() {
        super.onPause();
    }
    
    @Override
    protected void onDestroy() {
    	naShutdown();
        super.onDestroy();
    }

}
