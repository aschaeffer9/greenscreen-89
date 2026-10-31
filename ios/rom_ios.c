/* ROM discovery on iOS.
 *
 * android/rom.c already knows how to scan a directory tree and how to pull a
 * .gb out of a .zip; it is compiled here with rom_scan renamed, and this
 * wrapper points it at the app's Documents folder (which the Files app can
 * see, see UIFileSharingEnabled). The Android download paths main.c passes in
 * are ignored unless they happen to exist. */

#include <stdint.h>
#include <unistd.h>

#include "rom.h"
#include "ios_glue.h"

extern int rom_scan_android(const char *const *dirs, int ndirs, uint8_t *out);

int rom_scan(const char *const *dirs, int ndirs, uint8_t *out) {
    const char *mine[4];
    int n = 0;
    const char *docs = ios_data_path();
    if (docs) mine[n++] = docs;
    for (int i = 0; i < ndirs && n < 4; i++) {
        if (dirs[i] && access(dirs[i], R_OK) == 0) mine[n++] = dirs[i];
    }
    if (n == 0) return ROM_NOT_FOUND;
    return rom_scan_android(mine, n, out);
}
