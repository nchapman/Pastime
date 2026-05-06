package com.retroarch.browser.mainmenu;

import com.retroarch.BuildConfig;
import com.retroarch.browser.preferences.util.UserPreferences;
import com.retroarch.browser.retroactivity.RetroActivityFuture;

import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.preference.PreferenceActivity;
import android.preference.PreferenceManager;
import android.provider.Settings;

import java.io.File;
import java.util.List;
import java.util.ArrayList;
import android.content.pm.PackageManager;
import android.Manifest;
import android.content.DialogInterface;
import android.app.AlertDialog;
import android.util.Log;

/**
 * {@link PreferenceActivity} subclass that provides all of the
 * functionality of the main menu screen.
 */
public final class MainMenuActivity extends PreferenceActivity
{
	final private int REQUEST_CODE_ASK_MULTIPLE_PERMISSIONS = 124;
	public static String PACKAGE_NAME;
	boolean checkPermissions = false;

	/* PASTIME: All Files Access gate state.  pastimeWaitingForAllFiles is
	 * set when we send the user to system Settings, so onResume knows to
	 * re-check Environment.isExternalStorageManager() on return.
	 * pastimeStartupDone is the latch that prevents finalStartup() from
	 * being called twice on rapid permission-grant + lifecycle events. */
	private boolean pastimeWaitingForAllFiles = false;
	private boolean pastimeStartupDone = false;

	public void showMessageOKCancel(String message, DialogInterface.OnClickListener onClickListener)
	{
		new AlertDialog.Builder(this).setMessage(message)
			.setPositiveButton("OK", onClickListener).setCancelable(false)
			.setNegativeButton("Cancel", null).create().show();
	}

	private boolean addPermission(List<String> permissionsList, String permission)
	{
		if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.M)
		{
			if (checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED)
			{
				permissionsList.add(permission);

				// Check for Rationale Option
				if (!shouldShowRequestPermissionRationale(permission))
					return false;
			}
		}

		return true;
	}

	public void checkRuntimePermissions()
	{
		if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.M)
		{
			// Android 6.0+ needs runtime permission checks
			List<String> permissionsNeeded = new ArrayList<String>();
			final List<String> permissionsList = new ArrayList<String>();

			if (!addPermission(permissionsList, Manifest.permission.READ_EXTERNAL_STORAGE))
				permissionsNeeded.add("Read External Storage");
			if (!addPermission(permissionsList, Manifest.permission.WRITE_EXTERNAL_STORAGE))
				permissionsNeeded.add("Write External Storage");

			if (permissionsList.size() > 0)
			{
				checkPermissions = true;

				if (permissionsNeeded.size() > 0)
				{
					// Need Rationale
					Log.i("MainMenuActivity", "Need to request external storage permissions.");

					String message = "You need to grant access to " + permissionsNeeded.get(0);

					for (int i = 1; i < permissionsNeeded.size(); i++)
						message = message + ", " + permissionsNeeded.get(i);

					showMessageOKCancel(message,
						new DialogInterface.OnClickListener()
						{
							@Override
							public void onClick(DialogInterface dialog, int which)
							{
								if (which == AlertDialog.BUTTON_POSITIVE)
								{
									requestPermissions(permissionsList.toArray(new String[permissionsList.size()]),
											REQUEST_CODE_ASK_MULTIPLE_PERMISSIONS);

									Log.i("MainMenuActivity", "User accepted request for external storage permissions.");
								}
							}
						});
				}
				else
				{
					requestPermissions(permissionsList.toArray(new String[permissionsList.size()]),
						REQUEST_CODE_ASK_MULTIPLE_PERMISSIONS);

					Log.i("MainMenuActivity", "Requested external storage permissions.");
				}
			}
		}

		if (!checkPermissions)
		{
			finalStartup();
		}
	}

	public void finalStartup()
	{
		/* PASTIME: latch — Settings round-trips can fire onResume after
		 * we've already started RetroActivity and called finish(); without
		 * this guard we'd kick off a second startup. */
		if (pastimeStartupDone)
			return;
		pastimeStartupDone = true;

		Intent retro = new Intent(this, RetroActivityFuture.class);

		if (RetroActivityFuture.isRunning) {
			// RetroActivity is already running - just bring it to front.
			// PASTIME: hot-plug-while-running is unsupported.  The
			// REMOVABLE_STORAGE extra is only set in the "fresh start"
			// branch below; this path doesn't pass extras at all, and
			// even if it did, REORDER_TO_FRONT doesn't deliver new
			// extras to a running activity unless the receiver
			// implements onNewIntent + reads them (RetroActivityFuture
			// does not).  An SD card inserted while Pastime is in the
			// background requires a cold relaunch to be picked up.
			retro.setFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
		} else {
			// RetroActivity not running - full setup with parameters
			retro.setFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP);
			final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(this);

			startRetroActivity(
					retro,
					null,
					prefs.getString("libretro_path", getApplicationInfo().dataDir + "/cores/"),
					UserPreferences.getDefaultConfigPath(this),
					Settings.Secure.getString(getContentResolver(), Settings.Secure.DEFAULT_INPUT_METHOD),
					getApplicationInfo().dataDir,
					getApplicationInfo().sourceDir);

			/* PASTIME: removable-storage discovery.  Pass the list of
			 * mounted removable volume roots (e.g. /storage/<UUID>) so
			 * pastime_paths_get_root() can prefer an SD card with an
			 * existing Pastime/ tree on it.  Native side reads
			 * "REMOVABLE_STORAGE" as a ';'-joined path list. */
			String removable = pastimeCollectRemovableStorage();
			if (!removable.isEmpty())
				retro.putExtra("REMOVABLE_STORAGE", removable);
		}

		startActivity(retro);
		finish();
	}

	/* PASTIME: enumerate mounted removable volume roots for the native
	 * side.  Uses StorageManager.getStorageVolumes() (API 24+) and
	 * StorageVolume.getDirectory() (API 30+) — the supported, side-effect-
	 * free path.  Falls back to ContextCompat.getExternalFilesDirs-style
	 * stripping on API 24-29; on <24 returns empty.
	 *
	 * Adoptable-storage volumes don't appear here as separate entries
	 * (they're folded into primary), which is fine — this list is for
	 * "portable library" SD cards, and adopted ones aren't portable. */
	private String pastimeCollectRemovableStorage()
	{
		ArrayList<String> paths = new ArrayList<String>();

		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N)
		{
			StorageManager sm = (StorageManager) getSystemService(Context.STORAGE_SERVICE);
			if (sm != null)
			{
				List<StorageVolume> vols = sm.getStorageVolumes();
				/* getExternalFilesDirs() is constant across the loop;
				 * hoist so we don't re-query per volume. */
				File[] extDirs = (Build.VERSION.SDK_INT < Build.VERSION_CODES.R)
					? getExternalFilesDirs(null) : null;
				String primary = Environment.getExternalStorageDirectory()
					.getAbsolutePath();

				for (StorageVolume v : vols)
				{
					if (v.isPrimary())
						continue;
					if (!Environment.MEDIA_MOUNTED.equals(v.getState()))
						continue;

					File dir = null;
					if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
					{
						dir = v.getDirectory();
					}
					else if (extDirs != null)
					{
						/* API 24-29: no getDirectory().  Strip the
						 * Android/data/<pkg>/files suffix off the
						 * external-files dir for this volume — the only
						 * pre-R way to recover the volume root.  Side
						 * effect: creates Android/data/<pkg>/files on the
						 * card; unavoidable on this API range.
						 *
						 * Match by UUID when available; fall back to
						 * "any external dir whose root isn't primary"
						 * for FAT32 cards / OEM bugs that report a null
						 * UUID — that path silently dropped the card. */
						String uuid = v.getUuid();
						for (File d : extDirs)
						{
							if (d == null)
								continue;
							String p = d.getAbsolutePath();
							int idx = p.indexOf("/Android/data/");
							if (idx <= 0)
								continue;
							String root = p.substring(0, idx);
							boolean match = (uuid != null)
								? root.endsWith("/" + uuid)
								: !root.equals(primary);
							if (match)
							{
								dir = new File(root);
								break;
							}
						}
					}

					if (dir != null && dir.isDirectory())
					{
						String absPath = dir.getAbsolutePath();
						/* Defensive: the wire protocol to native is
						 * ';'-joined, so any ';' inside the path would
						 * be misparsed as a token separator.  Real
						 * Android volume paths never contain ';', but
						 * a malformed OEM getDirectory() shouldn't be
						 * able to corrupt our enumeration. */
						if (!absPath.contains(";"))
							paths.add(absPath);
					}
				}
			}
		}

		if (paths.isEmpty())
			return "";

		StringBuilder sb = new StringBuilder();
		for (int i = 0; i < paths.size(); i++)
		{
			if (i > 0)
				sb.append(';');
			sb.append(paths.get(i));
		}
		return sb.toString();
	}

	/* PASTIME: All Files Access gate.  Returns true when the app already
	 * has the permission (or the API level predates the requirement), in
	 * which case the caller proceeds straight to runtime R/W checks.
	 * Returns false after putting up the explainer dialog — caller should
	 * stop and wait for onResume to retry. */
	private boolean pastimeEnsureAllFilesAccess()
	{
		if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R)
			return true;
		if (Environment.isExternalStorageManager())
			return true;

		new AlertDialog.Builder(this)
			.setTitle("Pastime needs All Files Access")
			.setMessage("Pastime keeps your ROMs, saves, and states in a "
				+ "folder you control on internal storage or an SD card. "
				+ "Android requires \"All Files Access\" for apps that "
				+ "manage files this way.\n\n"
				+ "Tap Open Settings, then enable \"Allow access to "
				+ "manage all files\" for Pastime.")
			.setCancelable(false)
			.setPositiveButton("Open Settings",
				new DialogInterface.OnClickListener()
				{
					@Override
					public void onClick(DialogInterface dialog, int which)
					{
						pastimeOpenAllFilesSettings();
					}
				})
			.setNegativeButton("Quit",
				new DialogInterface.OnClickListener()
				{
					@Override
					public void onClick(DialogInterface dialog, int which)
					{
						finish();
					}
				})
			.show();
		return false;
	}

	/* PASTIME: deeplink fallback chain.  The OEM-removed-toggle case is
	 * rare on Android handhelds (close-to-AOSP) but we degrade gracefully:
	 * per-app screen → global list → instructions toast. */
	private void pastimeOpenAllFilesSettings()
	{
		pastimeWaitingForAllFiles = true;
		Intent intent = new Intent(
			Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
		intent.setData(Uri.parse("package:" + getPackageName()));
		try
		{
			startActivity(intent);
			return;
		}
		catch (ActivityNotFoundException e)
		{
			Log.w("MainMenuActivity",
				"Per-app All Files settings not available; trying global list.");
		}

		try
		{
			startActivity(new Intent(
				Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
			return;
		}
		catch (ActivityNotFoundException e)
		{
			Log.w("MainMenuActivity",
				"Global All Files settings not available either.");
		}

		pastimeWaitingForAllFiles = false;
		new AlertDialog.Builder(this)
			.setTitle("Settings unavailable")
			.setMessage("This device's Settings app does not expose the "
				+ "\"All Files Access\" toggle.  Please grant Pastime "
				+ "All Files Access manually in your system settings, "
				+ "then relaunch.")
			.setPositiveButton("OK", null)
			.show();
	}


	@Override
	public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults)
	{
		switch (requestCode)
		{
			case REQUEST_CODE_ASK_MULTIPLE_PERMISSIONS:
				for (int i = 0; i < permissions.length; i++)
				{
					if(grantResults[i] == PackageManager.PERMISSION_GRANTED)
					{
						Log.i("MainMenuActivity", "Permission: " + permissions[i] + " was granted.");
					}
					else
					{
						Log.i("MainMenuActivity", "Permission: " + permissions[i] + " was not granted.");
					}
				}

				break;
			default:
				super.onRequestPermissionsResult(requestCode, permissions, grantResults);
				break;
		}

		finalStartup();
	}

	public static void startRetroActivity(Intent retro, String contentPath, String corePath,
			String configFilePath, String imePath, String dataDirPath, String dataSourcePath)
	{
		if (contentPath != null) {
			retro.putExtra("ROM", contentPath);
		}
		retro.putExtra("LIBRETRO", corePath);
		retro.putExtra("CONFIGFILE", configFilePath);
		retro.putExtra("IME", imePath);
		retro.putExtra("DATADIR", dataDirPath);
		retro.putExtra("APK", dataSourcePath);
		String external = Environment.getExternalStorageDirectory().getAbsolutePath() + "/Android/data/" + PACKAGE_NAME + "/files";
		retro.putExtra("SDCARD", BuildConfig.PLAY_STORE_BUILD ? external : Environment.getExternalStorageDirectory().getAbsolutePath());
		retro.putExtra("EXTERNAL", external);
	}

	@Override
	public void onCreate(Bundle savedInstanceState)
	{
		super.onCreate(savedInstanceState);

		PACKAGE_NAME = getPackageName();

		// Bind audio stream to hardware controls.
		setVolumeControlStream(AudioManager.STREAM_MUSIC);

		UserPreferences.updateConfigFile(this);

		if (BuildConfig.PLAY_STORE_BUILD)
		{
			finalStartup();
			return;
		}

		/* PASTIME: All Files Access is the gate; everything else (legacy
		 * R/W runtime permissions on API 23-29) only matters when MES
		 * doesn't apply.  pastimeEnsureAllFilesAccess() returns false if
		 * it put up the explainer — onResume picks up the re-check. */
		if (!pastimeEnsureAllFilesAccess())
			return;

		checkRuntimePermissions();
	}

	/* PASTIME: re-check on return from system Settings.  ACTION_MANAGE_*
	 * deeplinks don't fire onActivityResult, so onResume is the only
	 * lifecycle hook we get when the user grants the permission. */
	@Override
	protected void onResume()
	{
		super.onResume();

		if (BuildConfig.PLAY_STORE_BUILD)
			return;
		if (pastimeStartupDone)
			return;
		if (!pastimeWaitingForAllFiles)
			return;

		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
				&& Environment.isExternalStorageManager())
		{
			pastimeWaitingForAllFiles = false;
			checkRuntimePermissions();
			return;
		}

		/* User returned without granting — the AlertDialog was dismissed
		 * by the positive-button click, so the screen is empty.  Re-show
		 * the explainer so they can try again or quit. */
		pastimeWaitingForAllFiles = false;
		pastimeEnsureAllFilesAccess();
	}
}
