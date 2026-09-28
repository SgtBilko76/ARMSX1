package com.nanodata.armsx;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicInteger;

public class EmulatorActivity extends SDLActivity {
    static final String EXTRA_NATIVE_ARGS = "com.nanodata.armsx.EXTRA_NATIVE_ARGS";
    private static final String TAG = "EmulatorActivity";
    private static final int REQUEST_LIBRARY_DIRECTORY = 4301;
    private static final String PREFS_NAME = "armsx_mobile_library";
    private static final String PREF_LIBRARY_ROOTS = "library_roots";
    private static final String PREF_ASKED_ALL_FILES_ACCESS = "asked_all_files_access";
    private static final String VIRTUAL_PREFIX = "armsx-android:///";
    // Quest build: the resolution of the virtual screen in the headset. See createSDLSurface().
    private static final int VR_SCREEN_WIDTH = 1920;
    private static final int VR_SCREEN_HEIGHT = 1080;

    private static native void nativeEnqueueLaunchArgument(String argument);
    private static native void nativeSetPlatformLibrarySnapshot(
        String[] rootPaths,
        String[] rootLabels,
        boolean[] rootRecursive,
        String[] gamePaths,
        String[] gameLabels,
        long[] gameSizes,
        long[] gameModified
    );

    private final Object libraryLock = new Object();
    private final Map<String, String> virtualFileUris = new HashMap<>();
    private final AtomicInteger libraryScanGeneration = new AtomicInteger();
    private boolean pendingLibraryRecursive = true;

    private static final class RootRecord {
        Uri uri;
        String virtualPath;
        String label;
        boolean recursive;
    }

    private static final class GameRecord {
        String virtualPath;
        String label;
        long size;
        long modified;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (BuildConfig.QUEST_VR) {
            // Read by the native frontend (armsx::vr::Requested) before it creates its window, so
            // this has to happen before SDL starts its main thread.
            try {
                Os.setenv("ARMSX_VR", "1", true);
            } catch (ErrnoException e) {
                Log.e(TAG, "Unable to enable VR mode", e);
            }
        }
        super.onCreate(savedInstanceState);
        if (!BuildConfig.QUEST_VR) {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        }
        applyImmersiveMode();
        scanPersistedLibrariesAsync();
        if (BuildConfig.QUEST_VR) {
            requestAllFilesAccessOnce();
        }
    }

    // Quest build: BIOS files and games copied onto the headset over USB land in shared storage
    // (Download and friends), which the FSUI file browser can only reach with all-files access.
    // Asked once automatically; Settings > VR Screen > Storage Access asks again.
    private void requestAllFilesAccessOnce() {
        if (hasAllFilesAccess()) {
            return;
        }
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        if (prefs.getBoolean(PREF_ASKED_ALL_FILES_ACCESS, false)) {
            return;
        }
        prefs.edit().putBoolean(PREF_ASKED_ALL_FILES_ACCESS, true).apply();
        requestAllFilesAccess();
    }

    private static boolean hasAllFilesAccess() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager();
    }

    // Called from native code (Settings > VR Screen > Storage Access).
    public void requestAllFilesAccess() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return;
        }
        runOnUiThread(() -> {
            try {
                startActivity(new Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())
                ));
            } catch (Exception appPageMissing) {
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                } catch (Exception e) {
                    Log.e(TAG, "Unable to open the all-files access settings", e);
                }
            }
        });
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        SDLSurface surface = super.createSDLSurface(context);
        if (BuildConfig.QUEST_VR) {
            // In the headset this surface is never shown: the frontend draws into offscreen
            // textures of the window's size, which OpenXR puts on the virtual screen. A fixed
            // buffer size makes that the screen's resolution, whatever the activity window is.
            surface.getHolder().setFixedSize(VR_SCREEN_WIDTH, VR_SCREEN_HEIGHT);
        }
        return surface;
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyImmersiveMode();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyImmersiveMode();
        }
    }

    private void applyImmersiveMode() {
        final Window window = getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            WindowManager.LayoutParams params = window.getAttributes();
            params.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            window.setAttributes(params);
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                );
            }
        } else {
            window.getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            );
        }
    }

    public void openLibraryDirectoryPicker(boolean recursive) {
        runOnUiThread(() -> {
            pendingLibraryRecursive = recursive;
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            intent.addFlags(
                Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                    | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION
            );
            startActivityForResult(intent, REQUEST_LIBRARY_DIRECTORY);
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_LIBRARY_DIRECTORY || resultCode != RESULT_OK || data == null) {
            return;
        }

        Uri treeUri = data.getData();
        if (treeUri == null) {
            return;
        }

        int takeFlags = data.getFlags()
            & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        if ((takeFlags & Intent.FLAG_GRANT_READ_URI_PERMISSION) == 0) {
            takeFlags |= Intent.FLAG_GRANT_READ_URI_PERMISSION;
        }
        try {
            getContentResolver().takePersistableUriPermission(treeUri, takeFlags);
        } catch (SecurityException error) {
            Log.e(TAG, "The document provider did not grant persistent access to " + treeUri, error);
            return;
        }

        Set<String> roots = new HashSet<>(
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .getStringSet(PREF_LIBRARY_ROOTS, new HashSet<>())
        );
        String uriText = treeUri.toString();
        roots.removeIf(value -> persistedUri(value).equals(uriText));
        roots.add((pendingLibraryRecursive ? "1|" : "0|") + uriText);
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .edit()
            .putStringSet(PREF_LIBRARY_ROOTS, roots)
            .apply();
        scanPersistedLibrariesAsync();
    }

    public void removeLibraryDirectory(String virtualRoot) {
        if (virtualRoot == null || virtualRoot.isEmpty()) {
            return;
        }

        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        Set<String> roots = new HashSet<>(prefs.getStringSet(PREF_LIBRARY_ROOTS, new HashSet<>()));
        List<Uri> removedUris = new ArrayList<>();
        roots.removeIf(value -> {
            Uri uri = Uri.parse(persistedUri(value));
            boolean matches = virtualRootForUri(uri).equals(virtualRoot);
            if (matches) {
                removedUris.add(uri);
            }
            return matches;
        });
        prefs.edit().putStringSet(PREF_LIBRARY_ROOTS, roots).apply();

        for (Uri uri : removedUris) {
            try {
                getContentResolver().releasePersistableUriPermission(
                    uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION
                );
            } catch (SecurityException ignored) {
                Log.w(TAG, "Persistent access was already unavailable for " + uri);
            }
        }
        scanPersistedLibrariesAsync();
    }

    public int openVirtualFileDescriptor(String virtualPath) {
        String mapped;
        synchronized (libraryLock) {
            mapped = virtualFileUris.get(virtualPath);
            if (mapped == null) {
                // Cue sheets often name their tracks in another case ("GAME.BIN" for "Game.bin").
                for (Map.Entry<String, String> entry : virtualFileUris.entrySet()) {
                    if (entry.getKey().equalsIgnoreCase(virtualPath)) {
                        mapped = entry.getValue();
                        break;
                    }
                }
            }
        }
        final String uriText = mapped;
        if (uriText == null) {
            Log.w(TAG, "No document URI mapped for " + virtualPath);
            return -1;
        }

        try {
            ParcelFileDescriptor descriptor =
                getContentResolver().openFileDescriptor(Uri.parse(uriText), "r");
            return descriptor != null ? descriptor.detachFd() : -1;
        } catch (IOException | SecurityException error) {
            Log.e(TAG, "Unable to open " + virtualPath, error);
            return -1;
        }
    }

    private void scanPersistedLibrariesAsync() {
        final int generation = libraryScanGeneration.incrementAndGet();
        new Thread(() -> {
            List<RootRecord> roots = new ArrayList<>();
            List<GameRecord> games = new ArrayList<>();
            Map<String, String> fileUris = new HashMap<>();
            Set<String> persisted = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .getStringSet(PREF_LIBRARY_ROOTS, new HashSet<>());

            for (String value : new HashSet<>(persisted)) {
                Uri uri = Uri.parse(persistedUri(value));
                RootRecord root = new RootRecord();
                root.uri = uri;
                root.virtualPath = virtualRootForUri(uri);
                root.label = queryDisplayName(uri);
                root.recursive = persistedRecursive(value);
                if (root.label == null || root.label.isEmpty()) {
                    root.label = "Game Directory";
                }
                roots.add(root);

                List<GameRecord> rootGames = new ArrayList<>();
                Set<String> visitedDocumentIds = new HashSet<>();
                try {
                    String rootDocumentId = DocumentsContract.getTreeDocumentId(uri);
                    scanDirectory(
                        uri,
                        rootDocumentId,
                        root.virtualPath,
                        "",
                        root.recursive,
                        visitedDocumentIds,
                        rootGames,
                        fileUris
                    );
                } catch (RuntimeException error) {
                    Log.e(TAG, "Unable to enumerate persisted game directory " + uri, error);
                }
                suppressCueCompanionBins(rootGames);
                games.addAll(rootGames);
            }

            if (generation != libraryScanGeneration.get()) {
                return;
            }
            synchronized (libraryLock) {
                virtualFileUris.clear();
                virtualFileUris.putAll(fileUris);
            }
            publishLibrarySnapshot(roots, games);
        }, "ARMSX-library-scan").start();
    }

    private void scanDirectory(
        Uri treeUri,
        String documentId,
        String virtualRoot,
        String relativeDirectory,
        boolean recursive,
        Set<String> visitedDocumentIds,
        List<GameRecord> games,
        Map<String, String> fileUris
    ) {
        if (!visitedDocumentIds.add(documentId)) {
            return;
        }

        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, documentId);
        String[] projection = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED,
        };

        try (Cursor cursor = getContentResolver().query(childrenUri, projection, null, null, null)) {
            if (cursor == null) {
                return;
            }
            while (cursor.moveToNext()) {
                String childId = cursor.getString(0);
                String name = cursor.getString(1);
                String mimeType = cursor.getString(2);
                long size = cursor.isNull(3) ? 0 : cursor.getLong(3);
                long modified = cursor.isNull(4) ? 0 : cursor.getLong(4);
                if (childId == null || name == null || name.isEmpty()) {
                    continue;
                }

                String relativePath = relativeDirectory.isEmpty()
                    ? name
                    : relativeDirectory + "/" + name;
                Uri childUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, childId);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mimeType)) {
                    if (recursive) {
                        scanDirectory(
                            treeUri,
                            childId,
                            virtualRoot,
                            relativePath,
                            true,
                            visitedDocumentIds,
                            games,
                            fileUris
                        );
                    }
                    continue;
                }

                String virtualPath = virtualRoot + "/" + relativePath;
                fileUris.put(virtualPath, childUri.toString());
                if (!isSupportedGameFile(name)) {
                    continue;
                }

                GameRecord game = new GameRecord();
                game.virtualPath = virtualPath;
                game.label = titleFromFileName(name);
                game.size = size;
                game.modified = modified;
                games.add(game);
            }
        } catch (SecurityException error) {
            Log.e(TAG, "Permission lost while enumerating " + childrenUri, error);
        }
    }

    private static void suppressCueCompanionBins(List<GameRecord> games) {
        Set<String> cueDirectories = new HashSet<>();
        for (GameRecord game : games) {
            if (game.virtualPath.toLowerCase(Locale.ROOT).endsWith(".cue")) {
                cueDirectories.add(parentVirtualPath(game.virtualPath));
            }
        }
        games.removeIf(game ->
            game.virtualPath.toLowerCase(Locale.ROOT).endsWith(".bin")
                && cueDirectories.contains(parentVirtualPath(game.virtualPath))
        );
    }

    private static String parentVirtualPath(String path) {
        int slash = path.lastIndexOf('/');
        return slash >= 0 ? path.substring(0, slash) : "";
    }

    private void publishLibrarySnapshot(List<RootRecord> roots, List<GameRecord> games) {
        String[] rootPaths = new String[roots.size()];
        String[] rootLabels = new String[roots.size()];
        boolean[] rootRecursive = new boolean[roots.size()];
        for (int index = 0; index < roots.size(); index++) {
            RootRecord root = roots.get(index);
            rootPaths[index] = root.virtualPath;
            rootLabels[index] = root.label;
            rootRecursive[index] = root.recursive;
        }

        String[] gamePaths = new String[games.size()];
        String[] gameLabels = new String[games.size()];
        long[] gameSizes = new long[games.size()];
        long[] gameModified = new long[games.size()];
        for (int index = 0; index < games.size(); index++) {
            GameRecord game = games.get(index);
            gamePaths[index] = game.virtualPath;
            gameLabels[index] = game.label;
            gameSizes[index] = game.size;
            gameModified[index] = game.modified;
        }
        nativeSetPlatformLibrarySnapshot(
            rootPaths,
            rootLabels,
            rootRecursive,
            gamePaths,
            gameLabels,
            gameSizes,
            gameModified
        );
    }

    private String queryDisplayName(Uri treeUri) {
        try {
            String documentId = DocumentsContract.getTreeDocumentId(treeUri);
            Uri documentUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId);
            try (Cursor cursor = getContentResolver().query(
                documentUri,
                new String[]{DocumentsContract.Document.COLUMN_DISPLAY_NAME},
                null,
                null,
                null
            )) {
                if (cursor != null && cursor.moveToFirst()) {
                    return cursor.getString(0);
                }
            }
        } catch (RuntimeException error) {
            Log.w(TAG, "Unable to read directory name for " + treeUri, error);
        }
        return null;
    }

    private static boolean isSupportedGameFile(String name) {
        String lower = name.toLowerCase(Locale.ROOT);
        return lower.endsWith(".cue")
            || lower.endsWith(".bin")
            || lower.endsWith(".iso")
            || lower.endsWith(".img")
            || lower.endsWith(".chd")
            || lower.endsWith(".zip")
            || lower.endsWith(".exe")
            || lower.endsWith(".ps-exe")
            || lower.endsWith(".psexe");
    }

    private static String titleFromFileName(String name) {
        int dot = name.lastIndexOf('.');
        String title = dot > 0 ? name.substring(0, dot) : name;
        return title.replace('_', ' ').trim();
    }

    private static boolean persistedRecursive(String value) {
        return value != null && value.startsWith("1|");
    }

    private static String persistedUri(String value) {
        if (value == null) {
            return "";
        }
        int separator = value.indexOf('|');
        return separator >= 0 ? value.substring(separator + 1) : value;
    }

    private static String virtualRootForUri(Uri uri) {
        return VIRTUAL_PREFIX + stableKey(uri.toString());
    }

    private static String stableKey(String value) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] bytes = digest.digest(value.getBytes(StandardCharsets.UTF_8));
            StringBuilder result = new StringBuilder(bytes.length * 2);
            for (byte item : bytes) {
                result.append(String.format(Locale.ROOT, "%02x", item & 0xff));
            }
            return result.toString();
        } catch (NoSuchAlgorithmException impossible) {
            return Integer.toHexString(value.hashCode());
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);

        String launchArgument = launchArgumentFromIntent(intent);
        if (launchArgument != null && !launchArgument.isEmpty()) {
            Log.i(TAG, "Runtime protocol launch: " + launchArgument);
            nativeEnqueueLaunchArgument(launchArgument);
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL2", "armsx"};
    }

    @Override
    protected String getMainFunction() {
        return "armsx_android_main";
    }

    @Override
    protected String[] getArguments() {
        // SDL supplies argv[0] ("app_process") itself; everything here is a real argument, and a
        // bare word would be taken as a game to boot.
        List<String> args = new ArrayList<>();

        String biosPath = copyBundledBios();
        if (biosPath != null) {
            args.add("--bios");
            args.add(biosPath);
        }

        Intent intent = getIntent();
        String[] extras = intent != null ? intent.getStringArrayExtra(EXTRA_NATIVE_ARGS) : null;
        if (extras != null) {
            for (String extra : extras) {
                if (extra != null && !extra.isEmpty()) {
                    args.add(extra);
                }
            }
        }

        String launchArgument = launchArgumentFromIntent(intent);
        if (launchArgument != null && !launchArgument.isEmpty()) {
            args.add(launchArgument);
        }

        return args.toArray(new String[0]);
    }

    private String launchArgumentFromIntent(Intent intent) {
        if (intent == null) {
            return null;
        }

        String dataString = intent.getDataString();
        if (dataString != null && !dataString.isEmpty()) {
            return dataString;
        }

        return null;
    }

    private String copyBundledBios() {
        AssetManager assets = getAssets();
        try (InputStream input = assets.open("bios.bin")) {
            File output = new File(getFilesDir(), "bios.bin");
            if (output.exists() && !output.delete()) {
                Log.w(TAG, "Unable to delete stale bios.bin");
            }
            try (FileOutputStream fos = new FileOutputStream(output)) {
                byte[] buffer = new byte[16 * 1024];
                int read;
                while ((read = input.read(buffer)) != -1) {
                    fos.write(buffer, 0, read);
                }
                fos.flush();
            }
            return output.getAbsolutePath();
        } catch (IOException missing) {
            Log.i(TAG, "Bundled bios.bin not present, continuing without it");
            return null;
        }
    }
}
