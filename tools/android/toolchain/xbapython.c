/* libxbapython.so: the python executable of the on-device Prepare (docs/android/toolchain.md).
 *
 * CPython's Android build is meant for embedding (libpython3.13.so); this is the few lines of a command-line
 * interpreter around it. It is shipped as a jniLib because an app targeting API 29+ may only exec files from its
 * nativeLibraryDir. The app runs it with PYTHONHOME pointing at the extracted standard library:
 *
 *     <nativeLibraryDir>/libxbapython.so -m tools.prepare ...
 *
 * sys.executable is then this file, so the pipeline's child steps ([sys.executable, "-m", ...]) run it again.
 * libpython3.13.so is found next to it (DT_RUNPATH $ORIGIN), LD_LIBRARY_PATH works too.
 */
#include <Python.h>

int main(int argc, char **argv)
{
    return Py_BytesMain(argc, argv);
}
