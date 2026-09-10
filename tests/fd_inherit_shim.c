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

static void make_configured_fd_inheritable(void)
{
    const char *text = getenv("MGPU_INHERIT_FD");
    char *end = NULL;
    long fd;
    int flags;

    if (!text || !*text)
        return;
    fd = strtol(text, &end, 10);
    if (end == text || *end || fd < 0 || fd > INT_MAX)
        return;
    flags = fcntl((int)fd, F_GETFD);
    if (flags >= 0 && (flags & FD_CLOEXEC))
        (void)fcntl((int)fd, F_SETFD, flags & ~FD_CLOEXEC);
}

static void make_all_fds_inheritable_for_probe(void)
{
    /* Wine's __wine_unix_spawnvp() uses fork()+execvp().  Vulkan deliberately
     * marks exported opaque FDs close-on-exec, so a native helper otherwise
     * receives only the integer value, not the descriptor.  This shim is
     * loaded only by the probe process and clears CLOEXEC on the bounded set
     * of descriptors that can be inherited by that helper. */
    for (int fd = 3; fd < 4096; ++fd) {
        int flags = fcntl(fd, F_GETFD);
        if (flags >= 0 && (flags & FD_CLOEXEC))
            (void)fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
    }
}

static int spawn_common(posix_spawn_fn real_spawn, pid_t *pid, const char *path,
        const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr,
        char *const argv[], char *const envp[])
{
    make_configured_fd_inheritable();
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
    make_configured_fd_inheritable();
    make_all_fds_inheritable_for_probe();
    return real_execvp(file, argv);
}
