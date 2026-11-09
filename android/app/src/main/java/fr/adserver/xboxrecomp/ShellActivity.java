package fr.adserver.xboxrecomp;

import org.libsdl.app.SDLActivity;

/** The app shell every title uses: SDL loads libSDL2.so, then libmain.so, and calls its SDL_main. */
public class ShellActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }
}
