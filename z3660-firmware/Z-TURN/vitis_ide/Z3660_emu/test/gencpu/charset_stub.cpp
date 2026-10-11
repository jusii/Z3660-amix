#include <cstring>
#include <cstdlib>
/* Minimal host stub of Amiberry's charset ua(): TCHAR(=char) -> UTF-8 char*.
 * On this ASCII/UTF-8 host it is identity. gencpu only uses it for log strings. */
char *ua(const char *s){ return strdup(s ? s : ""); }
