#pragma once

// Linux subreaper keeps a stable parent PID and restartable RPC sidecar across rollback promotions.
// Set QUBIC_NO_SUPERVISOR=1 to run without the shim or sidecar.

#if defined(__linux__) && !defined(LITE_WASM_SC)

#include <sys/prctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>

inline char gSidecarPort[16] = "41841";   // node http port -> sidecar listen + unix-socket key
inline char gAntWalkerThreads[16] = "0";  // matches the node default; 0 keeps the walker unspawned

// Forward a stop signal to the children so the container/service stops promptly.
static void shimForwardSignal(int sig)
{
    signal(sig, SIG_IGN);
    kill(0, sig);   // node + donors + sidecar share our process group
}

// Re-exec self as the stateless RPC proxy (a sibling of the node).
static pid_t shimForkSidecar()
{
#ifdef NO_RPC
    return -1;
#else
    const pid_t supervisorPid = getpid();
    pid_t sidecarPid = fork();
    if (sidecarPid != 0)
        return sidecarPid;                // shim: child pid (or -1)
    // An orphaned sidecar keeps the HTTP port bound (drogon is SO_REUSEPORT on Linux) and answers
    // 503 forever, so the next node cannot serve RPC. Die with the supervisor even on SIGKILL.
    prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
    if (getppid() != supervisorPid)
        _exit(0);                         // supervisor died inside the fork window
    char self[512];
    ssize_t pathLength = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (pathLength <= 0)
        _exit(127);
    self[pathLength] = 0;
    execl(self, "qubic-rpc-sidecar", "--rpc-proxy", "--rpc-listen", gSidecarPort, "--rpc-node", gSidecarPort, (char*)nullptr);
    _exit(127);                           // execl failed
#endif
}

// Re-exec self as the ant walker, a sibling of the node. Spawned here, not by the node, so it
// outlives a rollback promotion.
static pid_t shimForkAntWalker()
{
    if (std::atoi(gAntWalkerThreads) <= 0)
        return -1;

    const pid_t supervisorPid = getpid();
    pid_t walkerPid = fork();
    if (walkerPid != 0)
        return walkerPid;
    prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
    if (getppid() != supervisorPid)
        _exit(0);

    char self[512];
    ssize_t pathLength = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (pathLength <= 0)
        _exit(127);
    self[pathLength] = 0;

    char socketPath[128];
    snprintf(socketPath, sizeof(socketPath), "/tmp/qubic-antwalk-%s.sock", gSidecarPort);
    execl(self, "qubic-ant-walker", "--ant-walk-worker", "--socket", socketPath, "--threads", gAntWalkerThreads, (char*)nullptr);
    // Not fatal for the node: without a walker the backlog is simply paid on demand as before.
    fprintf(stderr, "[shim] could not exec the ant walker (%s), running without it\n", strerror(errno));
    fflush(stderr);
    _exit(127);
}

// True while any child other than the sidecar exists (i.e. the node lineage is still alive).
static bool shimHasNodeChild(pid_t sidecar, pid_t antWalker)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d/children", (int)getpid());
    FILE* childrenFile = fopen(path, "r");
    if (!childrenFile)
        return true;                      // can't tell -> assume yes (never exit prematurely)
    int childPid;
    bool hasNodeChild = false;
    while (fscanf(childrenFile, "%d", &childPid) == 1)
    {
        if (childPid != (int)sidecar && childPid != (int)antWalker)
        {
            hasNodeChild = true;
            break;
        }
    }
    fclose(childrenFile);
    return hasNodeChild;
}

// Returns ONLY in the node child. The supervisor parent loops here and _exit()s when the node drains.
static inline void runUnderSupervisor(int argc, const char** argv)
{
    // No shim -> no sidecar, no RPC at all (dev / screen). Node runs bare.
    if (getenv("QUBIC_NO_SUPERVISOR"))
        return;

    for (int i = 1; i < argc; i++)
    {
        if (std::string(argv[i]) == "--http-port" && i + 1 < argc)
        {
            std::strncpy(gSidecarPort, argv[i + 1], sizeof(gSidecarPort) - 1);
            gSidecarPort[sizeof(gSidecarPort) - 1] = 0;
        }
        if (std::string(argv[i]) == "--ant-walker-threads" && i + 1 < argc)
        {
            std::strncpy(gAntWalkerThreads, argv[i + 1], sizeof(gAntWalkerThreads) - 1);
            gAntWalkerThreads[sizeof(gAntWalkerThreads) - 1] = 0;
        }
    }

    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0)   // can't subreap: run node inline
        return;

    pid_t sidecar = shimForkSidecar();
    pid_t antWalker = shimForkAntWalker();
    int antWalkerRestarts = 0;

    pid_t node = fork();
    if (node < 0)
        return;                                            // fork failed: run node inline
    if (node == 0)
        return;                                            // CHILD: become the node

    // SUPERVISOR (stable PID). Reap everything + forward stop signals.
    signal(SIGTERM, shimForwardSignal);
    signal(SIGINT, shimForwardSignal);
    signal(SIGHUP, shimForwardSignal);   // session/screen teardown must not kill the shim uncleanly

    int lastStatus = 0;
    bool sawSignal = false;
    for (;;)
    {
        int status = 0;
        pid_t reapedPid = waitpid(-1, &status, 0);
        if (reapedPid < 0)
        {
            if (errno == EINTR)
                continue;
            break;                                         // ECHILD: nothing left
        }
        if (sidecar > 0 && reapedPid == sidecar)
        {
            sleep(1);                                      // a squatted port would hot-loop the respawn
            sidecar = shimForkSidecar();                   // RPC must not stay down: restart it
            continue;
        }
        if (antWalker > 0 && reapedPid == antWalker)
        {
            sleep(1);                                      // a squatted socket would hot-loop the respawn
            // An unrunnable walker would otherwise respawn once a second forever.
            if (++antWalkerRestarts > 5)
            {
                fprintf(stderr, "[shim] ant walker died %d times, leaving it down\n", antWalkerRestarts);
                fflush(stderr);
                antWalker = -1;
                continue;
            }
            antWalker = shimForkAntWalker();
            continue;
        }
        lastStatus = status;
        if (WIFSIGNALED(status))
            sawSignal = true;
        if (!shimHasNodeChild(sidecar, antWalker))
            break;                                         // node lineage drained -> shim exits
    }
    if (sidecar > 0)
        kill(sidecar, SIGTERM);
    if (antWalker > 0)
        kill(antWalker, SIGTERM);
    _exit(sawSignal ? 1 : (WIFEXITED(lastStatus) ? WEXITSTATUS(lastStatus) : 1));
}

#else
static inline void runUnderSupervisor(int, const char**) {}   // non-Linux or Wasm build: no fork rollback, no shim
#endif
