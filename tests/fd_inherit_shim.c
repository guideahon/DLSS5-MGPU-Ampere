#define _GNU_SOURCE

#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdlib.h>
#include <unistd.h>

extern char **environ;

typedef int (*posix_spawn_fn)(pid_t *, const char *, const posix_spawn_file_actions_t *,
        const posix_spawnattr_t *, char *const[], char *const[]);
typedef int (*execvp_fn)(const char *, char *const[]);

static void make_configured_fds_inheritable(void)
{
    const char *text = getenv("MGPU_INHERIT_FD");
    if (!text || !*text)
        return;

    /* Accept one descriptor for existing probes or a comma/space-separated
     * list for the persistent worker.  Keep Wine's private spawn pipe
     * close-on-exec: making every FD inheritable prevents __wine_unix_spawnvp
     * from observing exec completion when the child daemon stays alive. */
    while (*text) {
        char *end = NULL;
        long fd;
        int flags;
        while (*text == ',' || *text == ';' || *text == ':' || *text == ' ' || *text == '\t')
            ++text;
        if (!*text)
            break;
        fd = strtol(text, &end, 10);
        if (end == text) {
            while (*text && *text != ',' && *text != ';' && *text != ':' &&
                   *text != ' ' && *text != '\t')
                ++text;
            continue;
        }
        text = end;
        if (fd < 0 || fd > INT_MAX)
            continue;
        flags = fcntl((int)fd, F_GETFD);
        if (flags >= 0 && (flags & FD_CLOEXEC))
            (void)fcntl((int)fd, F_SETFD, flags & ~FD_CLOEXEC);
    }
}

static void make_argv_fds_inheritable(char *const argv[])
{
    if (!argv)
        return;
    for (int index = 1; argv[index]; ++index) {
        char *end = NULL;
        long fd = strtol(argv[index], &end, 10);
        int flags;
        if (end == argv[index] || *end || fd < 0 || fd > INT_MAX)
            continue;
        flags = fcntl((int)fd, F_GETFD);
        if (flags >= 0 && (flags & FD_CLOEXEC))
            (void)fcntl((int)fd, F_SETFD, flags & ~FD_CLOEXEC);
    }
}

static int spawn_common(posix_spawn_fn real_spawn, pid_t *pid, const char *path,
        const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr,
        char *const argv[], char *const envp[])
{
    make_configured_fds_inheritable();
    return real_spawn(pid, path, actions, attr, argv, envp);
}

int posix_spawn(pid_t *pid, const char *path,
        const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr,
        char *const argv[], char *const envp[])
{
    static posix_spawn_fn real_spawn;
    if (!real_spawn)
        real_spawn = (posix_spawn_fn)dlsym(RTLD_NEXT, "posix_spawn");
    return spawn_common(real_spawn, pid, path, actions, attr, argv,
            envp ? envp : environ);
}

int posix_spawnp(pid_t *pid, const char *file,
        const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr,
        char *const argv[], char *const envp[])
{
    static posix_spawn_fn real_spawnp;
    if (!real_spawnp)
        real_spawnp = (posix_spawn_fn)dlsym(RTLD_NEXT, "posix_spawnp");
    return spawn_common(real_spawnp, pid, file, actions, attr, argv,
            envp ? envp : environ);
}

int execvp(const char *file, char *const argv[])
{
    static execvp_fn real_execvp;
    if (!real_execvp)
        real_execvp = (execvp_fn)dlsym(RTLD_NEXT, "execvp");
    make_configured_fds_inheritable();
    make_argv_fds_inheritable(argv);
    return real_execvp(file, argv);
}
