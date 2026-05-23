package com.retroarch.browser.retroactivity;

import android.annotation.TargetApi;
import android.util.Log;
import android.view.PointerIcon;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.content.Intent;
import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import com.retroarch.browser.preferences.util.ConfigFile;
import com.retroarch.browser.preferences.util.UserPreferences;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

public final class RetroActivityFuture extends RetroActivityCamera {

  // Tracks activity lifecycle state for MainMenuActivity resume detection
  public static volatile boolean isRunning = false;

  // If set to true then RetroArch will completely exit when it loses focus
  private boolean quitfocus = false;

  // Top-level window decor view
  private View mDecorView;

  /* PASTIME: when true, attemptToggleImmersiveMode keeps the Android
   * status bar visible (drops SYSTEM_UI_FLAG_FULLSCREEN / LOW_PROFILE).
   * Flipped from native via pastimeSetLauncherImmersive() based on
   * whether a libretro core is currently loaded. */
  private volatile boolean pastimeLauncherMode = false;

  // Constants used for Handler messages
  private static final int HANDLER_WHAT_TOGGLE_IMMERSIVE = 1;
  private static final int HANDLER_WHAT_TOGGLE_POINTER_CAPTURE = 2;
  private static final int HANDLER_WHAT_TOGGLE_POINTER_NVIDIA = 3;
  private static final int HANDLER_WHAT_TOGGLE_POINTER_ICON = 4;
  private static final int HANDLER_ARG_TRUE = 1;
  private static final int HANDLER_ARG_FALSE = 0;
  private static final int HANDLER_MESSAGE_DELAY_DEFAULT_MS = 300;

  // Handler used for UI events
  private final Handler mHandler = new Handler(Looper.getMainLooper()) {
    @Override
    public void handleMessage(Message msg) {
      boolean state = (msg.arg1 == HANDLER_ARG_TRUE) ? true : false;

      if (msg.what == HANDLER_WHAT_TOGGLE_IMMERSIVE) {
        attemptToggleImmersiveMode(state);
      } else if (msg.what == HANDLER_WHAT_TOGGLE_POINTER_CAPTURE) {
        attemptTogglePointerCapture(state);
      } else if (msg.what == HANDLER_WHAT_TOGGLE_POINTER_NVIDIA) {
        attemptToggleNvidiaCursorVisibility(state);
      } else if (msg.what == HANDLER_WHAT_TOGGLE_POINTER_ICON) {
        attemptTogglePointerIcon(state);
      }
    }
  };

  @Override
  public void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    
    isRunning = true;
    mDecorView = getWindow().getDecorView();

    /* PASTIME: re-assert game-mode immersive whenever the system reveals
     * any bar (surface re-creation, transient system events).  In
     * launcher mode we *want* the status bar visible, so do nothing. */
    mDecorView.setOnSystemUiVisibilityChangeListener(
        new View.OnSystemUiVisibilityChangeListener() {
          @Override
          public void onSystemUiVisibilityChange(int visibility) {
            if (pastimeLauncherMode)
              return;
            if ((visibility & View.SYSTEM_UI_FLAG_FULLSCREEN) == 0
             || (visibility & View.SYSTEM_UI_FLAG_HIDE_NAVIGATION) == 0) {
              pastimeApplyImmersive();
            }
          }
        });

    // If QUITFOCUS parameter is provided then enable that Retroarch quits when focus is lost
    quitfocus = getIntent().hasExtra("QUITFOCUS");
  }

  @Override
  public void onNewIntent(Intent intent) {
    super.onNewIntent(intent);
    
    // Extract game parameters from new intent
    String newRom = intent.getStringExtra("ROM");
    String newCore = intent.getStringExtra("LIBRETRO");
    
    // Get current intent parameters for comparison
    Intent currentIntent = getIntent();
    String currentRom = currentIntent != null ? currentIntent.getStringExtra("ROM") : null;
    String currentCore = currentIntent != null ? currentIntent.getStringExtra("LIBRETRO") : null;
    
    
    // Check if we're trying to launch different content
    if ((newRom != null && !newRom.equals(currentRom)) ||
        (newCore != null && !newCore.equals(currentCore))) {
      // Different game content - start fresh instance then exit
      Intent restartIntent = new Intent(intent);
      restartIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
      startActivity(restartIntent);
      System.exit(0);
    } else {
      // Same content, just update intent
      setIntent(intent);
    }
  }

  @Override
  public void onResume() {
    super.onResume();

    setSustainedPerformanceMode(sustainedPerformanceMode);

    // Check for Android UI specific parameters
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
      String refresh = getIntent().getStringExtra("REFRESH");

      // If REFRESH parameter is provided then try to set refreshrate accordingly
      if (refresh != null) {
        WindowManager.LayoutParams params = getWindow().getAttributes();
        params.preferredRefreshRate = Integer.parseInt(refresh);
        getWindow().setAttributes(params);
      }
    }

    // Checks if Android versions is above 9.0 (28) and enable the screen to write over notch if the user desires
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
      ConfigFile configFile = new ConfigFile(UserPreferences.getDefaultConfigPath(this));
      try {
        if (configFile.getBoolean("video_notch_write_over_enable")) {
          getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
      } catch (Exception e) {
        Log.w("RetroActivityFuture", "Key doesn't exist yet: " + e.getMessage());
      }
    }
  }

  @Override
  public void onStop() {
    super.onStop();

    // If QUITFOCUS parameter was set then completely exit RetroArch when focus is lost
    if (quitfocus) System.exit(0);
  }

  @Override
  public void onDestroy() {
    super.onDestroy();
    isRunning = false;
  }

  @Override
  public void onWindowFocusChanged(boolean hasFocus) {
    super.onWindowFocusChanged(hasFocus);

    mHandlerSendUiMessage(HANDLER_WHAT_TOGGLE_IMMERSIVE, hasFocus);

    try {
      ConfigFile configFile = new ConfigFile(UserPreferences.getDefaultConfigPath(this));
      if (configFile.getBoolean("input_auto_mouse_grab")) {
        inputGrabMouse(hasFocus);
      }
    } catch (Exception e) {
      Log.w("RetroActivityFuture", "[onWindowFocusChanged] exception thrown: " + e.getMessage());
    }
  }

  private void mHandlerSendUiMessage(int what, boolean state) {
    int arg1 = (state ? HANDLER_ARG_TRUE : HANDLER_ARG_FALSE);
    int arg2 = -1;

    Message message = mHandler.obtainMessage(what, arg1, arg2);
    mHandler.sendMessageDelayed(message, HANDLER_MESSAGE_DELAY_DEFAULT_MS);
  }

  public void inputGrabMouse(boolean state) {
    mHandlerSendUiMessage(HANDLER_WHAT_TOGGLE_POINTER_CAPTURE, state);
    mHandlerSendUiMessage(HANDLER_WHAT_TOGGLE_POINTER_NVIDIA, state);
    mHandlerSendUiMessage(HANDLER_WHAT_TOGGLE_POINTER_ICON, state);
  }

  private void attemptToggleImmersiveMode(boolean state) {
    /* PASTIME: routes through pastimeApplyImmersive so the focus-driven
     * re-apply and the launcher↔game JNI setter share one code path.
     * `state` from onWindowFocusChanged: only act when we have focus —
     * a focus-loss re-apply would just fight whatever's stealing focus. */
    if (state)
      pastimeApplyImmersive();
  }

  /* PASTIME: single source of truth for status-bar / nav-bar visibility.
   * Launcher mode: status bar visible, nav hidden.  Game mode: both
   * hidden, sticky so transient system events don't bring them back.
   * The C menu driver queries pastimeGetStatusBarHeight() to inset its
   * launcher chrome below the bar — the surface itself stays edge-to-
   * edge in both modes (NativeActivity doesn't reliably shrink the
   * native window when WindowInsets are applied). */
  private void pastimeApplyImmersive() {
    try {
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
        pastimeApplyImmersiveR(getWindow());
        return;
      }
      /* Legacy path (pre-API-30) */
      Window window = getWindow();
      int base = View.SYSTEM_UI_FLAG_LAYOUT_STABLE
               | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
               | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
               | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
               | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
      if (pastimeLauncherMode) {
        window.clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        mDecorView.setSystemUiVisibility(base);
      } else {
        window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        mDecorView.setSystemUiVisibility(base
              | View.SYSTEM_UI_FLAG_FULLSCREEN
              | View.SYSTEM_UI_FLAG_LOW_PROFILE);
      }
    } catch (Exception e) {
      Log.w("RetroActivityFuture", "[pastimeApplyImmersive] exception: " + e.getMessage());
    }
  }

  /* PASTIME: API 30+ path is split into its own method so the Dalvik /
   * ART class verifier doesn't try to resolve WindowInsetsController /
   * WindowInsets.Type at class-load time on older devices.  The SDK_INT
   * guard at the call site keeps this method off the verifier's radar
   * unless we're actually on R+. */
  @TargetApi(Build.VERSION_CODES.R)
  private void pastimeApplyImmersiveR(Window window) {
    WindowInsetsController ic = window.getInsetsController();
    if (ic == null)
      return;
    window.setDecorFitsSystemWindows(false);
    ic.setSystemBarsBehavior(
        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    if (pastimeLauncherMode) {
      ic.hide(WindowInsets.Type.navigationBars());
      ic.show(WindowInsets.Type.statusBars());
    } else {
      ic.hide(WindowInsets.Type.statusBars()
            | WindowInsets.Type.navigationBars());
    }
  }

  /* PASTIME: returns the system status-bar height in pixels.  Called
   * from native (pastime_external_android.c) to inset launcher chrome
   * below the bar.  Resources.getIdentifier hack is the canonical way
   * to read this on Android — works on all API levels. */
  public int pastimeGetStatusBarHeight() {
    try {
      int id = getResources().getIdentifier("status_bar_height", "dimen", "android");
      if (id > 0)
        return getResources().getDimensionPixelSize(id);
    } catch (Exception e) {
      Log.w("RetroActivityFuture", "[pastimeGetStatusBarHeight] exception: " + e.getMessage());
    }
    return 0;
  }

  /* PASTIME: called from native (pastime_external_android.c) when the
   * launcher↔game transition fires.  Stores the flag and re-applies on
   * the UI thread — Window.setFlags / setSystemUiVisibility must run
   * there or they coalesce with the next relayout and effectively
   * no-op. */
  public void pastimeSetLauncherImmersive(boolean launcherMode) {
    pastimeLauncherMode = launcherMode;
    runOnUiThread(new Runnable() {
      @Override
      public void run() {
        pastimeApplyImmersive();
      }
    });
  }

  private void attemptTogglePointerCapture(boolean state) {
    // Attempt requestPointerCapture for Android 8.0 (26) and up
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      try {
        if (state) {
          mDecorView.requestPointerCapture();
        } else {
          mDecorView.releasePointerCapture();
        }
      } catch (Exception e) {
        Log.w("RetroActivityFuture", "[attemptTogglePointerCapture] exception thrown: " + e.getMessage());
      }
    }
  }

  private void attemptToggleNvidiaCursorVisibility(boolean state) {
    // Attempt setCursorVisibility for Android 4.1 (16) and up
    // only works if NVIDIA Android extensions for NVIDIA Shield are available
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN) {
      try {
        boolean cursorVisibility = !state;
        Method mInputManager_setCursorVisibility = InputManager.class.getMethod("setCursorVisibility", boolean.class);
        InputManager inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        mInputManager_setCursorVisibility.invoke(inputManager, cursorVisibility);
      } catch (NoSuchMethodException e) {
        // Extensions were not available so do nothing
      } catch (Exception e) {
        Log.w("RetroActivityFuture", "[attemptToggleNvidiaCursorVisibility] exception thrown: " + e.getMessage());
      }
    }
  }

  private void attemptTogglePointerIcon(boolean state) {
    // Attempt setPointerIcon for Android 7.x (24, 25) only
    // For Android 8.0+, requestPointerCapture is used
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N && Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
      try {
        if (state) {
          PointerIcon nullPointerIcon = PointerIcon.getSystemIcon(this, PointerIcon.TYPE_NULL);
          mDecorView.setPointerIcon(nullPointerIcon);
        } else {
          // Restore the pointer icon to it's default value
          mDecorView.setPointerIcon(null);
        }
      } catch (Exception e) {
        Log.w("RetroActivityFuture", "[attemptTogglePointerIcon] exception thrown: " + e.getMessage());
      }
    }
  }
}
