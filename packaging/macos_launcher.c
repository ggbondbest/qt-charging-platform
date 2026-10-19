/* Native Finder entry point. All children use runtimes shipped inside this app. */
#include <mach-o/dyld.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char executable[PATH_MAX], resolved[PATH_MAX], python[PATH_MAX], script[PATH_MAX];
    uint32_t length = sizeof(executable);
    if (_NSGetExecutablePath(executable, &length) != 0 || !realpath(executable, resolved))
        return 1;
    char *separator = strrchr(resolved, '/');
    if (!separator) return 1;
    *separator = '\0';
    int a = snprintf(python, sizeof(python), "%s/../Resources/python/bin/python3", resolved);
    int b = snprintf(script, sizeof(script), "%s/../Resources/project/packaging/run_review.py", resolved);
    if (a < 0 || b < 0 || (size_t)a >= sizeof(python) || (size_t)b >= sizeof(script)) return 1;
    char **arguments = calloc((size_t)argc + 4, sizeof(char *));
    if (!arguments) return 1;
    arguments[0] = python;
    arguments[1] = "-I";
    arguments[2] = "-B"; /* Keep the signed app immutable, including stdlib imports. */
    arguments[3] = script;
    for (int i = 1; i < argc; ++i) arguments[i + 3] = argv[i];
    execv(python, arguments);
    fputs("Bundled Python could not be started. Please extract the complete review package.\n", stderr);
    free(arguments);
    return 1;
}
