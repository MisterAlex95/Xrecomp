package fr.adserver.xboxrecomp;

import android.content.Intent;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/**
 * The app shell every title uses: SDL loads libSDL2.so, then libmain.so, and calls its SDL_main.
 * Started by LauncherActivity (the game picker, XBA-54) once a game is set up.
 */
public class ShellActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        /* The chosen game folder, as RECOMP_GAME_DIR, before SDLActivity starts anything: SDL_main runs later on
         * SDLThread, and recomp_app_start reads it in recomp_storage_resolve. The process environment is libc's, so
         * native getenv sees it. Set here, it wins over a RECOMP_GAME_DIR line in recomp.cfg. */
        String dir = GameStore.chosenDir(this);
        if (dir != null) {
            try {
                Os.setenv("RECOMP_GAME_DIR", dir, true);
            } catch (ErrnoException e) {
                Log.w(LauncherActivity.TAG, "shell: cannot set RECOMP_GAME_DIR", e);
            }
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    /**
     * Called by shell_main.c (SDLThread, through JNI) when default.xbe is not in the game folder (NO_GAME_DATA): the
     * picker opens naming that folder, and this activity finishes (SDL_main then gets SDL_QUIT and returns).
     */
    @SuppressWarnings("unused")
    public void openGamePicker(String gameDir) {
        runOnUiThread(() -> {
            Intent i = new Intent(this, LauncherActivity.class)
                    .putExtra(LauncherActivity.EXTRA_NO_GAME_DATA, gameDir)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(i);
            finish();
        });
    }
}
