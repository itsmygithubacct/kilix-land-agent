#ifndef KILIX_LAND_AGENT_APP_LAUNCHER_H
#define KILIX_LAND_AGENT_APP_LAUNCHER_H

#include <stdbool.h>

typedef enum kilix_app_launch_result {
    KILIX_APP_LAUNCH_OK = 0,
    KILIX_APP_LAUNCH_NOT_INSTALLED,
    KILIX_APP_LAUNCH_NO_TERMINAL,
    KILIX_APP_LAUNCH_UNTRUSTED,
    KILIX_APP_LAUNCH_FAILED
} kilix_app_launch_result;

/* Open the one executable capability exposed by the Studio.  Neither the
 * executable nor any launcher argument is supplied by the model. */
kilix_app_launch_result kilix_app_launch_plant(const char *project_root);
const char *kilix_app_launch_notice(kilix_app_launch_result result);

bool kilix_app_launcher_selftest(void);

#endif
