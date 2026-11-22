// iOS: stand-ins for runtime/prepare.c, which recompiles the game on the player's Mac (spawns clang,
// osascript, dlopen). On iOS the game code is compiled into the app and the data comes from Documents.
#include <stddef.h>
int prep_choose_game(const char *exe, char *root, size_t n) { (void)exe; (void)root; (void)n; return 0; }
int prep_from_installer(const char *exe, const char *setup, char *root, size_t n) { (void)exe; (void)setup; (void)root; (void)n; return 0; }
int prep_load_game(const char *root, const char *exe) { (void)root; (void)exe; return 0; }
