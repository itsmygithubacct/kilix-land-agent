#define _GNU_SOURCE

#include "app_launcher.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef enum candidate_state {
    CANDIDATE_MISSING = 0,
    CANDIDATE_TRUSTED,
    CANDIDATE_UNTRUSTED
} candidate_state;

typedef struct kilix_remote {
    char kitten[PATH_MAX];
    char listen_on[128];
    char password_file[PATH_MAX];
} kilix_remote;

static bool executable_metadata_trusted(const struct stat *metadata)
{
    mode_t mode;
    uid_t owner;
    if (!metadata || !S_ISREG(metadata->st_mode) ||
        metadata->st_nlink != 1)
        return false;
    mode = metadata->st_mode;
    owner = metadata->st_uid;
    if ((owner != getuid() && owner != (uid_t)0) ||
        (mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID)) != 0)
        return false;
    if (owner == getuid()) return (mode & S_IXUSR) != 0;
    return (mode & S_IXOTH) != 0;
}

static bool credential_metadata_trusted(const struct stat *metadata)
{
    return metadata && S_ISREG(metadata->st_mode) &&
           metadata->st_uid == getuid() && metadata->st_nlink == 1 &&
           (metadata->st_mode & (mode_t)07777) == (mode_t)0600;
}

static bool parent_directories_trusted(const char *canonical)
{
    char directory[PATH_MAX];
    char *slash;
    char *scan;
    size_t length;
    if (!canonical || canonical[0] != '/') return false;
    length = strlen(canonical);
    if (length == 0u || length >= sizeof directory) return false;
    (void)memcpy(directory, canonical, length + 1u);
    slash = strrchr(directory, '/');
    if (!slash) return false;
    if (slash == directory) return true;
    *slash = '\0';
    for (scan = directory + 1; ; ++scan) {
        char saved;
        struct stat metadata;
        if (*scan != '/' && *scan != '\0') continue;
        saved = *scan;
        *scan = '\0';
        if (lstat(directory, &metadata) != 0 ||
            !S_ISDIR(metadata.st_mode) ||
            (metadata.st_uid != getuid() &&
             metadata.st_uid != (uid_t)0) ||
            (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            *scan = saved;
            return false;
        }
        *scan = saved;
        if (saved == '\0') break;
    }
    return true;
}

static candidate_state inspect_executable(const char *candidate,
                                          char *canonical,
                                          size_t canonical_size)
{
    char resolved[PATH_MAX];
    struct stat metadata;
    size_t length;
    if (!candidate || candidate[0] != '/' || !canonical ||
        canonical_size == 0u)
        return CANDIDATE_UNTRUSTED;
    if (lstat(candidate, &metadata) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) return CANDIDATE_MISSING;
        return CANDIDATE_UNTRUSTED;
    }
    /* The named entry itself must not be an indirection.  realpath below is
     * used only to normalize trusted parent components such as "..". */
    if (!executable_metadata_trusted(&metadata) ||
        !realpath(candidate, resolved) ||
        lstat(resolved, &metadata) != 0 ||
        !executable_metadata_trusted(&metadata) ||
        !parent_directories_trusted(resolved))
        return CANDIDATE_UNTRUSTED;
    length = strlen(resolved);
    if (length >= canonical_size) return CANDIDATE_UNTRUSTED;
    (void)memcpy(canonical, resolved, length + 1u);
    return CANDIDATE_TRUSTED;
}

static bool inspect_credential(const char *candidate, char *canonical,
                               size_t canonical_size)
{
    char resolved[PATH_MAX];
    struct stat metadata;
    size_t length;
    if (!candidate || candidate[0] != '/' || !canonical ||
        canonical_size == 0u || lstat(candidate, &metadata) != 0 ||
        !credential_metadata_trusted(&metadata) ||
        !realpath(candidate, resolved) || strcmp(candidate, resolved) != 0 ||
        lstat(resolved, &metadata) != 0 ||
        !credential_metadata_trusted(&metadata) ||
        !parent_directories_trusted(resolved))
        return false;
    length = strlen(resolved);
    if (length >= canonical_size) return false;
    (void)memcpy(canonical, resolved, length + 1u);
    return true;
}

static bool listen_socket_pid(const char *listen_on, pid_t *pid_out)
{
    static const char prefix[] = "unix:@kilix-";
    const char *digits;
    char *end = NULL;
    long value;
    if (!listen_on || !pid_out ||
        strncmp(listen_on, prefix, sizeof prefix - 1u) != 0)
        return false;
    digits = listen_on + sizeof prefix - 1u;
    if (*digits == '\0') return false;
    for (const unsigned char *scan = (const unsigned char *)digits;
         *scan != '\0'; ++scan)
        if (*scan < (unsigned char)'0' || *scan > (unsigned char)'9')
            return false;
    errno = 0;
    value = strtol(digits, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value <= 1 ||
        value > INT_MAX)
        return false;
    *pid_out = (pid_t)value;
    return true;
}

static bool positive_decimal_id(const char *text)
{
    unsigned long value = 0u;
    if (!text || text[0] == '\0') return false;
    for (const unsigned char *scan = (const unsigned char *)text;
         *scan != '\0'; ++scan) {
        unsigned long digit;
        if (*scan < (unsigned char)'0' || *scan > (unsigned char)'9')
            return false;
        digit = (unsigned long)(*scan - (unsigned char)'0');
        if (value > ((unsigned long)INT_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    return value > 0u;
}

static kilix_app_launch_result resolve_kilix_remote(kilix_remote *remote)
{
    const char *listen_on = getenv("KITTY_LISTEN_ON");
    const char *password_file = getenv("KILIX_RC_PASSWORD_FILE");
    const char *window_id = getenv("KITTY_WINDOW_ID");
    char proc_path[64];
    char engine[PATH_MAX];
    char kitten_candidate[PATH_MAX];
    char *slash;
    struct stat process_metadata;
    ssize_t engine_length;
    size_t listen_length;
    pid_t terminal_pid;
    int written;
    if (!remote || !listen_on || listen_on[0] == '\0' ||
        !positive_decimal_id(window_id))
        return KILIX_APP_LAUNCH_NO_TERMINAL;
    if (!listen_socket_pid(listen_on, &terminal_pid))
        return KILIX_APP_LAUNCH_UNTRUSTED;
    written = snprintf(proc_path, sizeof proc_path, "/proc/%ld",
                       (long)terminal_pid);
    if (written < 0 || (size_t)written >= sizeof proc_path ||
        stat(proc_path, &process_metadata) != 0 ||
        !S_ISDIR(process_metadata.st_mode) ||
        process_metadata.st_uid != getuid())
        return KILIX_APP_LAUNCH_UNTRUSTED;
    written = snprintf(proc_path, sizeof proc_path, "/proc/%ld/exe",
                       (long)terminal_pid);
    if (written < 0 || (size_t)written >= sizeof proc_path)
        return KILIX_APP_LAUNCH_UNTRUSTED;
    engine_length = readlink(proc_path, engine, sizeof engine - 1u);
    if (engine_length <= 0 || (size_t)engine_length >= sizeof engine - 1u)
        return KILIX_APP_LAUNCH_UNTRUSTED;
    engine[(size_t)engine_length] = '\0';
    slash = strrchr(engine, '/');
    if (!slash || strcmp(slash + 1, "kitty") != 0)
        return KILIX_APP_LAUNCH_UNTRUSTED;
    *slash = '\0';
    written = snprintf(kitten_candidate, sizeof kitten_candidate,
                       "%s/kitten", engine);
    if (written < 0 || (size_t)written >= sizeof kitten_candidate ||
        inspect_executable(kitten_candidate, remote->kitten,
                           sizeof remote->kitten) != CANDIDATE_TRUSTED)
        return KILIX_APP_LAUNCH_UNTRUSTED;
    if (!inspect_credential(password_file, remote->password_file,
                            sizeof remote->password_file))
        return KILIX_APP_LAUNCH_UNTRUSTED;
    listen_length = strlen(listen_on);
    if (listen_length >= sizeof remote->listen_on)
        return KILIX_APP_LAUNCH_UNTRUSTED;
    (void)memcpy(remote->listen_on, listen_on, listen_length + 1u);
    return KILIX_APP_LAUNCH_OK;
}

static bool wait_for_launcher(pid_t child)
{
    struct timespec delay = {0, 10000000L};
    unsigned int step;
    int status = 0;
    for (step = 0u; step < 500u; ++step) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (waited < 0 && errno != EINTR) return false;
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
        delay.tv_sec = 0;
        delay.tv_nsec = 10000000L;
    }
    (void)kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return false;
}

static bool launch_tab_once(const kilix_remote *remote,
                            const char *application, bool authenticated)
{
    char app_directory[PATH_MAX];
    char *slash;
    pid_t child;
    size_t length;
    if (!remote || !application) return false;
    length = strlen(application);
    if (length == 0u || length >= sizeof app_directory) return false;
    (void)memcpy(app_directory, application, length + 1u);
    slash = strrchr(app_directory, '/');
    if (!slash || slash == app_directory) return false;
    *slash = '\0';
    child = fork();
    if (child < 0) return false;
    if (child == 0) {
        int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
        char *const socket_arguments[] = {
            (char *)remote->kitten,
            (char *)"@",
            (char *)"--to", (char *)remote->listen_on,
            (char *)"--use-password=never",
            (char *)"launch",
            (char *)"--type=tab",
            (char *)"--self",
            (char *)"--tab-title", (char *)"Kilix Plant",
            (char *)"--cwd", app_directory,
            (char *)"--",
            (char *)application,
            NULL
        };
        char *const authenticated_arguments[] = {
            (char *)remote->kitten,
            (char *)"@",
            (char *)"--to", (char *)remote->listen_on,
            (char *)"--password-file", (char *)remote->password_file,
            (char *)"--use-password=always",
            (char *)"launch",
            (char *)"--type=tab",
            (char *)"--self",
            (char *)"--tab-title", (char *)"Kilix Plant",
            (char *)"--cwd", app_directory,
            (char *)"--",
            (char *)application,
            NULL
        };
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) (void)close(null_fd);
        }
        execv(remote->kitten, authenticated ? authenticated_arguments :
              socket_arguments);
        _exit(127);
    }
    return wait_for_launcher(child);
}

static bool launch_tab(const kilix_remote *remote, const char *application)
{
    /* Current Kilix uses socket-scoped authorization, where adding password
     * encryption can fail on older live engines.  Password-only engines reject
     * the first request and are retried with the already validated credential.
     * Both variants target the same owned process and carry the same fixed app
     * argv. */
    return launch_tab_once(remote, application, false) ||
           launch_tab_once(remote, application, true);
}

kilix_app_launch_result kilix_app_launch_plant(const char *project_root)
{
    static const char *const installed[] = {
        "/usr/local/bin/pleb-plant-grower",
        "/usr/bin/pleb-plant-grower"
    };
    char sibling[PATH_MAX];
    char application[PATH_MAX];
    kilix_remote remote;
    candidate_state state;
    kilix_app_launch_result remote_result;
    size_t index;
    int written;
    if (!project_root || project_root[0] != '/')
        return KILIX_APP_LAUNCH_UNTRUSTED;
    written = snprintf(sibling, sizeof sibling,
                       "%s/../../games/pleb-plant-grower/"
                       "pleb-plant-grower", project_root);
    if (written < 0 || (size_t)written >= sizeof sibling)
        return KILIX_APP_LAUNCH_UNTRUSTED;
    state = inspect_executable(sibling, application, sizeof application);
    if (state == CANDIDATE_UNTRUSTED)
        return KILIX_APP_LAUNCH_UNTRUSTED;
    for (index = 0u; state == CANDIDATE_MISSING &&
         index < sizeof installed / sizeof installed[0]; ++index) {
        state = inspect_executable(installed[index], application,
                                   sizeof application);
        if (state == CANDIDATE_UNTRUSTED)
            return KILIX_APP_LAUNCH_UNTRUSTED;
    }
    if (state == CANDIDATE_MISSING)
        return KILIX_APP_LAUNCH_NOT_INSTALLED;
    remote_result = resolve_kilix_remote(&remote);
    if (remote_result != KILIX_APP_LAUNCH_OK) return remote_result;
    return launch_tab(&remote, application) ? KILIX_APP_LAUNCH_OK :
           KILIX_APP_LAUNCH_FAILED;
}

const char *kilix_app_launch_notice(kilix_app_launch_result result)
{
    switch (result) {
    case KILIX_APP_LAUNCH_OK:
        return "Plant simulator opened in a separate Kilix tab.";
    case KILIX_APP_LAUNCH_NOT_INSTALLED:
        return "Plant simulator is not installed at an allowlisted path.";
    case KILIX_APP_LAUNCH_NO_TERMINAL:
        return "Plant simulator needs a live Kilix tab.";
    case KILIX_APP_LAUNCH_UNTRUSTED:
        return "Plant launch was blocked by the executable trust check.";
    case KILIX_APP_LAUNCH_FAILED:
        return "Kilix could not open the plant simulator tab.";
    }
    return "Kilix could not open the plant simulator tab.";
}

bool kilix_app_launcher_selftest(void)
{
    char directory[] = "/tmp/kilix-agent-launch.XXXXXX";
    char executable[PATH_MAX];
    char hard_link[PATH_MAX];
    char symbolic_link[PATH_MAX];
    struct stat metadata;
    pid_t pid = 0;
    int descriptor = -1;
    int written;
    bool passed = false;
    if (!listen_socket_pid("unix:@kilix-12345", &pid) ||
        pid != (pid_t)12345 ||
        listen_socket_pid("unix:@kilix-0", &pid) ||
        listen_socket_pid("unix:@kilix-12x", &pid) ||
        listen_socket_pid("unix:/tmp/kilix-12", &pid) ||
        !positive_decimal_id("362") || positive_decimal_id("0") ||
        positive_decimal_id("12x") ||
        !mkdtemp(directory))
        return false;
    written = snprintf(executable, sizeof executable, "%s/app", directory);
    if (written < 0 || (size_t)written >= sizeof executable) goto done;
    written = snprintf(hard_link, sizeof hard_link, "%s/hard", directory);
    if (written < 0 || (size_t)written >= sizeof hard_link) goto done;
    written = snprintf(symbolic_link, sizeof symbolic_link, "%s/link",
                       directory);
    if (written < 0 || (size_t)written >= sizeof symbolic_link) goto done;
    descriptor = open(executable, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                      (mode_t)0700);
    if (descriptor < 0 || close(descriptor) != 0) goto done;
    descriptor = -1;
    if (lstat(executable, &metadata) != 0 ||
        !executable_metadata_trusted(&metadata) ||
        chmod(executable, (mode_t)0722) != 0 ||
        lstat(executable, &metadata) != 0 ||
        executable_metadata_trusted(&metadata) ||
        chmod(executable, (mode_t)0600) != 0 ||
        lstat(executable, &metadata) != 0 ||
        !credential_metadata_trusted(&metadata) ||
        link(executable, hard_link) != 0 ||
        lstat(executable, &metadata) != 0 ||
        credential_metadata_trusted(&metadata) ||
        unlink(hard_link) != 0 ||
        symlink(executable, symbolic_link) != 0 ||
        lstat(symbolic_link, &metadata) != 0 ||
        executable_metadata_trusted(&metadata))
        goto done;
    passed = true;

done:
    if (descriptor >= 0) (void)close(descriptor);
    (void)unlink(symbolic_link);
    (void)unlink(hard_link);
    (void)unlink(executable);
    (void)rmdir(directory);
    return passed;
}
