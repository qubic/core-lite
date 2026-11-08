#define NO_UEFI

#include "gtest/gtest.h"

#include "extensions/utils.h"        // wchar_to_string, wcharToNumber
#include "platform/file_io.h"        // CHAR16, createDir, getFileSize, setText (via console_logging.h)
#include "extensions/disk_shadow.h"  // gShadow, DiskShadow (must precede virtual_memory.h; not included here)
#include "extensions/swapvm_dirty_track.h"
#include "platform/concurrency.h"    // fork-eligibility census (forkCensusEnter/Leave/SumExcept/Offender)
#include "extensions/fork_census.h"  // SmartMutex / SmartSharedMutex
#include "extensions/fork_stats.h"   // ForkStats (unforkable-tick counters + durable log)
#include "extensions/tick_fork_barrier.h"
#include "extensions/tick_fork_control.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <future>
#include <chrono>
#if defined(__linux__)
#include <cerrno>
#include <cstdlib>   // posix_memalign, free
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

const char* kTestDirectory = "fork_rollback_test";

void writeFileUtf8(const std::string& path, const std::string& content)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
}
std::string readFileUtf8(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

// gShadow is a process-global; reset it between tests so writtenPages/shadow dirs/active don't leak.
class ForkRollback : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::filesystem::remove_all(kTestDirectory);
        std::filesystem::create_directories(kTestDirectory);
        gShadow.discard();   // clear active + writtenPages from any prior test
    }
    void TearDown() override
    {
        gShadow.discard();
        std::filesystem::remove_all(kTestDirectory);
    }
};

} // namespace

// arm + dirForWrite diverts the page to <dir>/s and records it; the real file is untouched.
TEST_F(ForkRollback, WriteDirDivertsToShadow)
{
    std::filesystem::create_directories(std::string(kTestDirectory) + "/divert");
    CHAR16 dir[256];
    setText(dir, L"fork_rollback_test/divert");
    CHAR16 page[64];
    setText(page, L"pg0");

    gShadow.registerDir(dir);
    gShadow.arm();
    const CHAR16* shadowDir = gShadow.dirForWrite(dir, page);

    EXPECT_EQ(wchar_to_string(shadowDir), std::string("fork_rollback_test/divert/s"));
    EXPECT_TRUE(std::filesystem::exists("fork_rollback_test/divert/s"));   // ensure() created it
    EXPECT_FALSE(std::filesystem::exists("fork_rollback_test/divert/pg0")); // real not written
}

// dirForRead serves /s ONLY for a written page whose /s file exists; otherwise real.
//    Guards the orphan-gate fix: a stale /s file alone must not divert a read.
TEST_F(ForkRollback, ReadDirWrittenGate)
{
    std::filesystem::create_directories(std::string(kTestDirectory) + "/gate");
    CHAR16 dir[256];
    setText(dir, L"fork_rollback_test/gate");
    CHAR16 pageA[64];
    setText(pageA, L"pgA");
    CHAR16 pageB[64];
    setText(pageB, L"pgB");
    CHAR16 pageC[64];
    setText(pageC, L"pgC");

    gShadow.registerDir(dir);
    gShadow.arm();
    const CHAR16* shadowDir = gShadow.dirForWrite(dir, pageA); // shadow dir registered, writtenPages={pgA}
    writeFileUtf8("fork_rollback_test/gate/s/pgA", "NEW");

    // pgA: written + /s file exists -> diverts to /s
    EXPECT_EQ(wchar_to_string(gShadow.dirForRead(dir, pageA)), wchar_to_string(shadowDir));
    // pgB: dir has a shadow dir but pgB was not written -> real (the orphan gate)
    EXPECT_EQ(wchar_to_string(gShadow.dirForRead(dir, pageB)), wchar_to_string(dir));
    // pgC: written but its /s file is absent -> real.
    gShadow.dirForWrite(dir, pageC);
    EXPECT_EQ(wchar_to_string(gShadow.dirForRead(dir, pageC)), wchar_to_string(dir));
}

TEST_F(ForkRollback, FileIoRoutesRegisteredDirectory)
{
    std::filesystem::create_directories(std::string(kTestDirectory) + "/file_io");
    CHAR16 directory[256];
    setText(directory, L"fork_rollback_test/file_io");
    CHAR16 fileName[64];
    setText(fileName, L"pg0");

    writeFileUtf8("fork_rollback_test/file_io/pg0", "OLD");
    gShadow.registerDir(directory);
    ASSERT_TRUE(gShadow.arm());

    const unsigned char newContents[] = { 'N', 'E', 'W' };
    ASSERT_EQ(save(fileName, sizeof(newContents), newContents, directory), sizeof(newContents));
    EXPECT_EQ(readFileUtf8("fork_rollback_test/file_io/pg0"), "OLD");
    EXPECT_EQ(getFileSize(fileName, directory), sizeof(newContents));

    unsigned char loadedContents[sizeof(newContents)]{};
    ASSERT_EQ(load(fileName, sizeof(loadedContents), loadedContents, directory), sizeof(loadedContents));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(loadedContents), sizeof(loadedContents)), "NEW");

    EXPECT_TRUE(removeFile(directory, fileName));
    EXPECT_EQ(getFileSize(fileName, directory), -1);
    EXPECT_EQ(readFileUtf8("fork_rollback_test/file_io/pg0"), "OLD");

    gShadow.discard();
    EXPECT_EQ(readFileUtf8("fork_rollback_test/file_io/pg0"), "OLD");
}

TEST_F(ForkRollback, FileIoLeavesUnregisteredDirectoryUnchanged)
{
    std::filesystem::create_directories(std::string(kTestDirectory) + "/unregistered");
    CHAR16 directory[256];
    setText(directory, L"fork_rollback_test/unregistered");
    CHAR16 fileName[64];
    setText(fileName, L"plain");

    ASSERT_TRUE(gShadow.arm());

    const unsigned char contents[] = { 'L', 'I', 'V', 'E' };
    ASSERT_EQ(save(fileName, sizeof(contents), contents, directory), sizeof(contents));
    EXPECT_EQ(readFileUtf8("fork_rollback_test/unregistered/plain"), "LIVE");
    EXPECT_FALSE(std::filesystem::exists("fork_rollback_test/unregistered/s"));

    unsigned char loadedContents[sizeof(contents)]{};
    ASSERT_EQ(load(fileName, sizeof(loadedContents), loadedContents, directory), sizeof(loadedContents));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(loadedContents), sizeof(loadedContents)), "LIVE");

    EXPECT_TRUE(removeFile(directory, fileName));
    EXPECT_FALSE(std::filesystem::exists("fork_rollback_test/unregistered/plain"));
}

// commit renames /s/<page> over an EXISTING real file (atomic replace).
TEST_F(ForkRollback, CommitOverExistingRealFile)
{
    std::filesystem::create_directories(std::string(kTestDirectory) + "/commit");
    CHAR16 dir[256];
    setText(dir, L"fork_rollback_test/commit");
    CHAR16 page[64];
    setText(page, L"pg0");

    writeFileUtf8("fork_rollback_test/commit/pg0", "OLD");
    gShadow.registerDir(dir);
    gShadow.arm();
    gShadow.dirForWrite(dir, page);
    writeFileUtf8("fork_rollback_test/commit/s/pg0", "NEW");

    gShadow.commit();

    EXPECT_EQ(readFileUtf8("fork_rollback_test/commit/pg0"), "NEW");          // replaced
    EXPECT_FALSE(std::filesystem::exists("fork_rollback_test/commit/s/pg0")); // moved out
}

// commit retries then exit(1) when a written page can't be renamed (here: /s file missing).
TEST_F(ForkRollback, CommitRetryThenFatalOnRenameFailure)
{
    EXPECT_EXIT({
        std::filesystem::create_directories("fork_rollback_test/fatal");
        CHAR16 dir[256];
        setText(dir, L"fork_rollback_test/fatal");
        CHAR16 page[64];
        setText(page, L"pg0");
        gShadow.registerDir(dir);
        gShadow.arm();
        gShadow.dirForWrite(dir, page);   // /s/pg0 is intentionally never created.
        gShadow.commit();              // rename(/s/pg0 -> /pg0) ENOENTs every retry -> exit(1)
    }, ::testing::ExitedWithCode(1), "FATAL: commit could not persist");
}

// discard drops the /s page; the real file stays pristine (pre-window).
TEST_F(ForkRollback, DiscardKeepsRealPristine)
{
    std::filesystem::create_directories(std::string(kTestDirectory) + "/discard");
    CHAR16 dir[256];
    setText(dir, L"fork_rollback_test/discard");
    CHAR16 page[64];
    setText(page, L"pg0");

    writeFileUtf8("fork_rollback_test/discard/pg0", "OLD");
    gShadow.registerDir(dir);
    gShadow.arm();
    gShadow.dirForWrite(dir, page);
    writeFileUtf8("fork_rollback_test/discard/s/pg0", "NEW");

    gShadow.discard();

    EXPECT_FALSE(std::filesystem::exists("fork_rollback_test/discard/s/pg0"));
    EXPECT_EQ(readFileUtf8("fork_rollback_test/discard/pg0"), "OLD");
}

// arm() purges a prior window's diverted /s pages so the next window starts clean.
TEST_F(ForkRollback, ArmPurgesPriorWindowShadow)
{
    std::filesystem::create_directories(std::string(kTestDirectory) + "/armpurge");
    CHAR16 dir[256];
    setText(dir, L"fork_rollback_test/armpurge");
    CHAR16 page[64];
    setText(page, L"pg0");

    gShadow.registerDir(dir);
    gShadow.arm();
    gShadow.dirForWrite(dir, page);
    writeFileUtf8("fork_rollback_test/armpurge/s/pg0", "X");   // window 1 divert, no commit/discard
    EXPECT_TRUE(std::filesystem::exists("fork_rollback_test/armpurge/s/pg0"));

    gShadow.arm();   // window 2: purges shadowDir's /s

    EXPECT_FALSE(std::filesystem::exists("fork_rollback_test/armpurge/s/pg0"));
}

// purgeOrphans (child path): drop /s, real pristine, window inactive.
TEST_F(ForkRollback, PurgeOrphansDropsShadow)
{
    std::filesystem::create_directories(std::string(kTestDirectory) + "/purge");
    CHAR16 dir[256];
    setText(dir, L"fork_rollback_test/purge");
    CHAR16 page[64];
    setText(page, L"pg0");

    writeFileUtf8("fork_rollback_test/purge/pg0", "OLD");
    gShadow.registerDir(dir);
    gShadow.arm();
    gShadow.dirForWrite(dir, page);
    writeFileUtf8("fork_rollback_test/purge/s/pg0", "NEW");

    gShadow.purgeOrphans();

    EXPECT_FALSE(std::filesystem::exists("fork_rollback_test/purge/s/pg0"));
    EXPECT_EQ(readFileUtf8("fork_rollback_test/purge/pg0"), "OLD");
    EXPECT_FALSE(gShadow.active.load());
}

// The child inherits this registration at fork time. Parent-created files are not in the child's
// written-page set, so cleanup must use the retained directory list.
TEST_F(ForkRollback, RegisteredDirSurvivesArmForChildPurge)
{
    std::filesystem::create_directories(std::string(kTestDirectory) + "/registered");
    CHAR16 dir[256];
    setText(dir, L"fork_rollback_test/registered");
    gShadow.registerDir(dir);

    ASSERT_TRUE(gShadow.arm());
    std::filesystem::create_directories(std::string(kTestDirectory) + "/registered/s");
    writeFileUtf8("fork_rollback_test/registered/s/late.pg", "optimistic");

    EXPECT_TRUE(gShadow.purgeOrphans());
    EXPECT_FALSE(std::filesystem::exists("fork_rollback_test/registered/s"));
    EXPECT_FALSE(gShadow.active.load());
    EXPECT_FALSE(gForkWindowActive);
}

TEST_F(ForkRollback, CleanupFailureStaysInactive)
{
    writeFileUtf8(std::string(kTestDirectory) + "/not-a-directory", "x");
    CHAR16 dir[256];
    setText(dir, L"fork_rollback_test/not-a-directory");
    DiskShadow shadow;
    shadow.registerDir(dir);

    EXPECT_FALSE(shadow.arm());
    EXPECT_FALSE(shadow.active.load());
    EXPECT_FALSE(gForkWindowActive);
    EXPECT_FALSE(shadow.purgeOrphans());
    EXPECT_FALSE(shadow.active.load());
    EXPECT_FALSE(gForkWindowActive);

    gShadowPoisoned.store(false, std::memory_order_release);
}

#if defined(__linux__)

namespace {

bool waitForRetireState(tickForkControl::BspRetireHandoff& handoff, tickForkControl::BspRetireHandoff::State expected)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (handoff.state() == expected)
            return true;
        std::this_thread::yield();
    }
    return handoff.state() == expected;
}

} // namespace

TEST(RequestProcessorBarrierTest, FailedContenderDoesNotReleaseActiveOwner)
{
    tickFork::gRequestProcessorParkPhase.store(0, std::memory_order_release);

    tickFork::RequestProcessorBarrier owner;
    ASSERT_TRUE(owner.request());
    const unsigned long long ownerPhase = owner.phase();
    ASSERT_TRUE(ownerPhase & 1);

    {
        tickFork::RequestProcessorBarrier contender;
        EXPECT_FALSE(contender.request());
    }
    EXPECT_EQ(tickFork::gRequestProcessorParkPhase.load(std::memory_order_acquire), ownerPhase);

    owner.release();
    EXPECT_EQ(tickFork::gRequestProcessorParkPhase.load(std::memory_order_acquire), ownerPhase + 1);
    tickFork::gRequestProcessorParkPhase.store(0, std::memory_order_release);
}

TEST(RequestProcessorBarrierTest, UnacknowledgedOwnerReleasesOwnedPhase)
{
    constexpr unsigned long long processorNumber = 0;
    const unsigned long long processorIDs[] = { processorNumber };
    tickFork::gRequestProcessorParkPhase.store(0, std::memory_order_release);
    tickFork::gRequestProcessorParkAcknowledgement[processorNumber].store(0, std::memory_order_release);

    unsigned long long ownerPhase;
    {
        tickFork::RequestProcessorBarrier owner;
        ASSERT_TRUE(owner.request());
        ownerPhase = owner.phase();
        EXPECT_FALSE(owner.allAcknowledged(processorIDs, 1));
    }
    EXPECT_EQ(tickFork::gRequestProcessorParkPhase.load(std::memory_order_acquire), ownerPhase + 1);

    tickFork::gRequestProcessorParkPhase.store(0, std::memory_order_release);
    tickFork::gRequestProcessorParkAcknowledgement[processorNumber].store(0, std::memory_order_release);
}

TEST(ForkRollbackControl, ParkWorkerAcknowledgesNextPhaseWithoutReleaseObservation)
{
    constexpr unsigned long long processorNumber = 0;
    constexpr unsigned long long firstParkPhase = 1;
    constexpr unsigned long long secondParkPhase = 3;
    tickFork::gRequestProcessorParkAcknowledgement[processorNumber].store(0, std::memory_order_release);
    tickFork::gRequestProcessorParkPhase.store(firstParkPhase, std::memory_order_release);

    std::thread worker([processorNumber] { tickFork::requestProcessorParkPoint(processorNumber); });
    const auto waitForPark = [processorNumber](unsigned long long expectedPhase) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (tickFork::gRequestProcessorParkAcknowledgement[processorNumber].load(std::memory_order_acquire) == expectedPhase)
                return true;
            std::this_thread::yield();
        }
        return false;
    };

    const bool firstParked = waitForPark(firstParkPhase);
    EXPECT_TRUE(firstParked);
    if (firstParked)
    {
        // Model a worker missing the even release phase between consecutive BSP park requests.
        tickFork::gRequestProcessorParkPhase.store(secondParkPhase, std::memory_order_release);
        EXPECT_TRUE(waitForPark(secondParkPhase));
    }

    tickFork::gRequestProcessorParkPhase.store(secondParkPhase + 1, std::memory_order_release);
    worker.join();
    tickFork::gRequestProcessorParkPhase.store(0, std::memory_order_release);
    tickFork::gRequestProcessorParkAcknowledgement[processorNumber].store(0, std::memory_order_release);
}

TEST(ForkRollbackControl, RetireRequestWaitsForBspCompletion)
{
    tickForkControl::BspRetireHandoff handoff;
    auto result = std::async(std::launch::async, [&] {
        return handoff.requestAndWait(1000);
    });

    ASSERT_TRUE(waitForRetireState(handoff, tickForkControl::BspRetireHandoff::State::Requested));
    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    ASSERT_TRUE(handoff.tryStart());
    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);

    ASSERT_TRUE(handoff.finish(true));
    ASSERT_EQ(result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_TRUE(result.get());
    EXPECT_EQ(handoff.state(), tickForkControl::BspRetireHandoff::State::Idle);
}

TEST(ForkRollbackControl, ShutdownIntentIsDeliveredToBsp)
{
    tickForkControl::BspRetireHandoff handoff;
    auto result = std::async(std::launch::async, [&] {
        return handoff.requestAndWait(1000, true);
    });

    ASSERT_TRUE(waitForRetireState(handoff, tickForkControl::BspRetireHandoff::State::ShutdownRequested));
    bool shutDownAfterCommit = false;
    ASSERT_TRUE(handoff.tryStart(shutDownAfterCommit));
    EXPECT_TRUE(shutDownAfterCommit);
    ASSERT_TRUE(handoff.finish(true));
    ASSERT_EQ(result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_TRUE(result.get());
    EXPECT_EQ(handoff.state(), tickForkControl::BspRetireHandoff::State::Idle);
}

TEST(ForkRollbackControl, RetireRequestTimeoutCancels)
{
    tickForkControl::BspRetireHandoff handoff;

    EXPECT_FALSE(handoff.requestAndWait(1));
    EXPECT_EQ(handoff.state(), tickForkControl::BspRetireHandoff::State::Idle);
    EXPECT_FALSE(handoff.tryStart());
}

TEST(ForkRollbackControl, RunningRetireIgnoresRequestTimeout)
{
    tickForkControl::BspRetireHandoff handoff;
    auto result = std::async(std::launch::async, [&] {
        return handoff.requestAndWait(10);
    });

    ASSERT_TRUE(waitForRetireState(handoff, tickForkControl::BspRetireHandoff::State::Requested));
    ASSERT_TRUE(handoff.tryStart());
    const auto afterTimeout = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    while (std::chrono::steady_clock::now() < afterTimeout)
        std::this_thread::yield();

    EXPECT_EQ(handoff.state(), tickForkControl::BspRetireHandoff::State::Running);
    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    ASSERT_TRUE(handoff.finish(true));
    ASSERT_EQ(result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_TRUE(result.get());
    EXPECT_EQ(handoff.state(), tickForkControl::BspRetireHandoff::State::Idle);
}

TEST(ForkRollbackControl, FailedRetireReturnsFailure)
{
    tickForkControl::BspRetireHandoff handoff;
    auto result = std::async(std::launch::async, [&] {
        return handoff.requestAndWait(1000);
    });

    ASSERT_TRUE(waitForRetireState(handoff, tickForkControl::BspRetireHandoff::State::Requested));
    ASSERT_TRUE(handoff.tryStart());
    ASSERT_TRUE(handoff.finish(false));
    ASSERT_EQ(result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_FALSE(result.get());
    EXPECT_EQ(handoff.state(), tickForkControl::BspRetireHandoff::State::Idle);
}

TEST(ForkRollbackControl, ExplicitRetireDoesNotPromote)
{
    int pipeFds[2];
    ASSERT_EQ(pipe(pipeFds), 0);

    ASSERT_TRUE(tickForkControl::writeRetireCommand(pipeFds[1]));
    close(pipeFds[1]);

    const auto command = tickForkControl::readChildCommand(pipeFds[0], 1234);
    close(pipeFds[0]);

    EXPECT_EQ(command.action, tickForkControl::ChildAction::Retire);
    EXPECT_EQ(command.targetTick, 0U);
}

TEST(ForkRollbackControl, RetiredChildIsCollectedWithoutBlockingTheRetire)
{
    int pipeFds[2];
    ASSERT_EQ(pipe(pipeFds), 0);
    pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        close(pipeFds[1]);
        const auto command = tickForkControl::readChildCommand(pipeFds[0], 1234);
        // Stands in for the kernel freeing a large address space.
        usleep(300000);
        _exit(command.action == tickForkControl::ChildAction::Retire ? 0 : 1);
    }
    close(pipeFds[0]);

    ASSERT_TRUE(tickForkControl::writeRetireCommand(pipeFds[1]));
    close(pipeFds[1]);

    pid_t retired = child;
    EXPECT_FALSE(tickForkControl::collectRetiredChild(retired, false));
    EXPECT_EQ(retired, child);
    EXPECT_TRUE(tickForkControl::collectRetiredChild(retired, true));
    EXPECT_EQ(retired, -1);
    EXPECT_TRUE(tickForkControl::collectRetiredChild(retired, true));

    // Not our child, as in a promoted child that inherited its sibling's pid.
    pid_t notOurChild = getppid();
    EXPECT_TRUE(tickForkControl::collectRetiredChild(notOurChild, true));
    EXPECT_EQ(notOurChild, -1);
}

TEST(ForkRollbackControl, ParentCrashEofStillPromotes)
{
    int pipeFds[2];
    ASSERT_EQ(pipe(pipeFds), 0);
    close(pipeFds[1]);

    const auto command = tickForkControl::readChildCommand(pipeFds[0], 1234);
    close(pipeFds[0]);

    EXPECT_EQ(command.action, tickForkControl::ChildAction::Promote);
    EXPECT_EQ(command.targetTick, 1234U);
}

TEST(ForkRollbackControl, PromoteCommandCarriesTargetTick)
{
    int pipeFds[2];
    ASSERT_EQ(pipe(pipeFds), 0);

    const char tag = tickForkControl::promoteTag;
    const unsigned int targetTick = 5678;
    const char sentinel = 'x';
    ASSERT_EQ(write(pipeFds[1], &tag, 1), 1);
    ASSERT_EQ(write(pipeFds[1], &targetTick, sizeof(targetTick)), (ssize_t)sizeof(targetTick));
    ASSERT_EQ(write(pipeFds[1], &sentinel, 1), 1);
    close(pipeFds[1]);

    const auto command = tickForkControl::readChildCommand(pipeFds[0], 1234);
    close(pipeFds[0]);

    EXPECT_EQ(command.action, tickForkControl::ChildAction::Promote);
    EXPECT_EQ(command.targetTick, targetTick);
}

TEST(ForkRollbackControl, PromoteWaitsForParentEof)
{
    int pipeFds[2];
    ASSERT_EQ(pipe(pipeFds), 0);

    const char tag = tickForkControl::promoteTag;
    const unsigned int targetTick = 5678;
    ASSERT_EQ(write(pipeFds[1], &tag, 1), 1);
    ASSERT_EQ(write(pipeFds[1], &targetTick, sizeof(targetTick)), (ssize_t)sizeof(targetTick));

    std::promise<tickForkControl::ChildCommand> commandPromise;
    std::future<tickForkControl::ChildCommand> commandFuture = commandPromise.get_future();
    std::thread reader([&] {
        commandPromise.set_value(tickForkControl::readChildCommand(pipeFds[0], 1234));
        close(pipeFds[0]);
    });

    bool drained = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline && commandFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
    {
        int bytesAvailable = -1;
        if (ioctl(pipeFds[0], FIONREAD, &bytesAvailable) == 0 && bytesAvailable == 0)
        {
            drained = true;
            break;
        }
        std::this_thread::yield();
    }
    EXPECT_TRUE(drained);
    EXPECT_EQ(commandFuture.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);

    close(pipeFds[1]);
    const bool ready = commandFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    EXPECT_TRUE(ready);
    reader.join();
    if (!ready)
        return;
    const auto command = commandFuture.get();

    EXPECT_EQ(command.action, tickForkControl::ChildAction::Promote);
    EXPECT_EQ(command.targetTick, targetTick);
}

TEST(ForkRollbackControl, PromoteClosesOnlyInheritedRpcUnixSockets)
{
    EXPECT_EXIT(
        {
            const std::string rpcPath = "/tmp/qubic-rpc-promote-" + std::to_string(getpid()) + ".sock";
            unlink(rpcPath.c_str());

            const int listenerFd = socket(AF_UNIX, SOCK_STREAM, 0);
            sockaddr_un rpcAddress{};
            rpcAddress.sun_family = AF_UNIX;
            std::strncpy(rpcAddress.sun_path, rpcPath.c_str(), sizeof(rpcAddress.sun_path) - 1);
            if (listenerFd < 0 || bind(listenerFd, (sockaddr*)&rpcAddress, sizeof(rpcAddress)) != 0 || listen(listenerFd, 1) != 0)
            {
                unlink(rpcPath.c_str());
                _exit(1);
            }

            const int clientFd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (clientFd < 0 || connect(clientFd, (sockaddr*)&rpcAddress, sizeof(rpcAddress)) != 0)
            {
                unlink(rpcPath.c_str());
                _exit(1);
            }

            const int acceptedFd = accept(listenerFd, nullptr, nullptr);
            int unrelatedUnixFds[2];
            int pipeFds[2];
            if (acceptedFd < 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, unrelatedUnixFds) != 0 || pipe(pipeFds) != 0)
            {
                unlink(rpcPath.c_str());
                _exit(1);
            }

            const int inetFd = socket(AF_INET, SOCK_STREAM, 0);
            if (inetFd < 0)
            {
                unlink(rpcPath.c_str());
                _exit(1);
            }

            const unsigned int closedCount = tickForkControl::closeInheritedRpcUnixSocketsForPromote(listenerFd, rpcPath.c_str());
            unlink(rpcPath.c_str());

            errno = 0;
            const bool listenerClosed = fcntl(listenerFd, F_GETFD) == -1 && errno == EBADF;
            errno = 0;
            const bool acceptedConnectionClosed = fcntl(acceptedFd, F_GETFD) == -1 && errno == EBADF;
            const bool clientOpen = fcntl(clientFd, F_GETFD) != -1;
            const bool unrelatedUnixOpen = fcntl(unrelatedUnixFds[0], F_GETFD) != -1 && fcntl(unrelatedUnixFds[1], F_GETFD) != -1;
            const bool inetOpen = fcntl(inetFd, F_GETFD) != -1;
            const bool pipeOpen = fcntl(pipeFds[0], F_GETFD) != -1 && fcntl(pipeFds[1], F_GETFD) != -1;

            _exit(closedCount == 2 && listenerClosed && acceptedConnectionClosed && clientOpen && unrelatedUnixOpen && inetOpen && pipeOpen ? 0
                  : 2);
        }, ::testing::ExitedWithCode(0), "");
}

// registerPool + tryMarkDirty marks in-range slots dirty and ignores out-of-range addresses.
TEST(ForkRollbackDirtyTrack, MarkDirtyInRange)
{
    const int slotCount = 4;
    const unsigned long long stride = 4096;
    unsigned char* buffer = nullptr;
    ASSERT_EQ(posix_memalign((void**)&buffer, 4096, stride * slotCount), 0);
    unsigned char* poolBase = buffer;
    volatile unsigned char dirty[slotCount] = {0};

    gSwapDirtyTrackEnabled = true;
    SwapDirtyTrack::registerPool(&poolBase, stride, slotCount, dirty);

    EXPECT_TRUE(SwapDirtyTrack::tryMarkDirty(buffer + 2 * stride + 100));
    EXPECT_EQ(dirty[2], 1);
    EXPECT_FALSE(SwapDirtyTrack::tryMarkDirty(buffer + slotCount * stride + 10)); // past the pool

    SwapDirtyTrack::unregisterPool(&poolBase);
    gSwapDirtyTrackEnabled = false;
    free(buffer);
}

// unregisterPool stops the fault path for that pool; a later register reuses the dead slot.
TEST(ForkRollbackDirtyTrack, UnregisterSkipsAndReusesSlot)
{
    const int slotCount = 4;
    const unsigned long long stride = 4096;
    unsigned char* buffer = nullptr;
    ASSERT_EQ(posix_memalign((void**)&buffer, 4096, stride * slotCount), 0);
    gSwapDirtyTrackEnabled = true;

    unsigned char* poolBase = buffer;
    volatile unsigned char dirty1[slotCount] = {0};
    SwapDirtyTrack::registerPool(&poolBase, stride, slotCount, dirty1);
    SwapDirtyTrack::unregisterPool(&poolBase);
    EXPECT_FALSE(SwapDirtyTrack::tryMarkDirty(buffer + 100)); // basePtr -> gDeadBase -> skipped

    const int countBeforeReuse = SwapDirtyTrack::gPoolCount.load();
    unsigned char* poolBase2 = buffer;
    volatile unsigned char dirty2[slotCount] = {0};
    SwapDirtyTrack::registerPool(&poolBase2, stride, slotCount, dirty2);
    EXPECT_EQ(SwapDirtyTrack::gPoolCount.load(), countBeforeReuse);   // reused the dead slot, no growth
    EXPECT_TRUE(SwapDirtyTrack::tryMarkDirty(buffer + 2 * stride + 1));
    EXPECT_EQ(dirty2[2], 1);

    SwapDirtyTrack::unregisterPool(&poolBase2);
    gSwapDirtyTrackEnabled = false;
    free(buffer);
}

// The registry never grows past MAX_POOLS no matter how many pools are registered.
TEST(ForkRollbackDirtyTrack, OverflowGuardCapsPoolCount)
{
    const int maxPools = SwapDirtyTrack::MAX_POOLS;
    const int slotCount = 4;
    const unsigned long long stride = 4096;
    unsigned char* buffer = nullptr;
    ASSERT_EQ(posix_memalign((void**)&buffer, 4096, stride * slotCount), 0);
    gSwapDirtyTrackEnabled = true;

    const int registrationCount = maxPools + 4;
    std::vector<unsigned char*> poolBases(registrationCount);
    std::vector<std::vector<unsigned char>> dirty(registrationCount, std::vector<unsigned char>(slotCount, 0));
    for (int i = 0; i < registrationCount; i++)
    {
        poolBases[i] = buffer;
        SwapDirtyTrack::registerPool(&poolBases[i], stride, slotCount, (volatile unsigned char*)dirty[i].data());
        EXPECT_LE(SwapDirtyTrack::gPoolCount.load(), maxPools); // guard holds at every step
    }
    for (int i = 0; i < registrationCount; i++)
        SwapDirtyTrack::unregisterPool(&poolBases[i]);

    gSwapDirtyTrackEnabled = false;
    free(buffer);
}

#endif // __linux__

// The caller's lock is excluded; another thread's lock is counted and released on thread exit.
TEST(ForkCensus, SelfExcludedOtherCounted)
{
    forkCensusEnter("selfHeld");
    EXPECT_EQ(forkCensusSumExcept(), 0);   // only this thread holds -> excluded from its own gate view
    forkCensusLeave();
    EXPECT_EQ(forkCensusSumExcept(), 0);

    std::atomic<bool> held{ false };
    std::atomic<bool> release{ false };
    std::thread holder([&] {
        forkCensusEnter("otherThreadLock");
        held.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
        forkCensusLeave();
    });
    while (!held.load(std::memory_order_acquire))
        std::this_thread::yield();

    EXPECT_GE(forkCensusSumExcept(), 1);                 // the other thread's held lock is visible
    const char* offender = forkCensusOffender();
    ASSERT_NE(offender, nullptr);
    EXPECT_NE(std::string(offender).find("otherThreadLock"), std::string::npos);

    release.store(true, std::memory_order_release);
    holder.join();
    EXPECT_EQ(forkCensusSumExcept(), 0);                 // slot freed at thread exit; no dangling read
    EXPECT_EQ(forkCensusOffender(), nullptr);
}

// SmartMutex and SmartSharedMutex feed the same census, including the shared RPC path.
TEST(ForkCensus, SmartMutexCounted)
{
    SmartMutex mutex{ "smTest" };
    SmartSharedMutex sharedMutex{ "ssTest" };

    // exclusive SmartMutex held by another thread is counted
    {
        std::atomic<bool> held{ false };
        std::atomic<bool> release{ false };
        std::thread holder([&] {
            std::lock_guard<SmartMutex> guard(mutex);
            held.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
        });
        while (!held.load(std::memory_order_acquire))
        std::this_thread::yield();
        EXPECT_GE(forkCensusSumExcept(), 1);
        release.store(true, std::memory_order_release);
        holder.join();
        EXPECT_EQ(forkCensusSumExcept(), 0);
    }
    // shared SmartSharedMutex held by another thread is counted (the gRpcDispatchLock shared path)
    {
        std::atomic<bool> held{ false };
        std::atomic<bool> release{ false };
        std::thread holder([&] {
            std::shared_lock<SmartSharedMutex> guard(sharedMutex);
            held.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
        });
        while (!held.load(std::memory_order_acquire))
        std::this_thread::yield();
        EXPECT_GE(forkCensusSumExcept(), 1);
        release.store(true, std::memory_order_release);
        holder.join();
        EXPECT_EQ(forkCensusSumExcept(), 0);
    }
}

// ForkStats records every unforkable tick as a durable log line, not a bounded ring.
TEST(ForkStatsTest, CountersAndDurableLog)
{
    std::filesystem::remove(ForkStats::kLogPath);

    unsigned long long initialSkippedTotal = ForkStats::forksSkippedTotal.load();
    unsigned long long initialCensusSkips = ForkStats::skipByReason[ForkStats::CENSUS].load();
    unsigned long long initialForksOk = ForkStats::forksOk.load();
    unsigned long long initialMismatches = ForkStats::mismatches.load();

    ForkStats::onForkOk();
    ForkStats::onVerdict(true);
    ForkStats::onForkSkipped(ForkStats::CENSUS, 1001, "spectrumLock @ x");
    ForkStats::onForkSkipped(ForkStats::CENSUS, 1002, "tickDataLock @ y");
    ForkStats::onForkSkipped(ForkStats::PARK_TIMEOUT, 1003, "");

    EXPECT_EQ(ForkStats::forksOk.load() - initialForksOk, 1u);
    EXPECT_EQ(ForkStats::mismatches.load() - initialMismatches, 1u);
    EXPECT_EQ(ForkStats::forksSkippedTotal.load() - initialSkippedTotal, 3u);
    EXPECT_EQ(ForkStats::skipByReason[ForkStats::CENSUS].load() - initialCensusSkips, 2u);
    EXPECT_EQ(ForkStats::lastSkipTick.load(), 1003u);
    EXPECT_EQ(ForkStats::lastSkipReason.load(), (int)ForkStats::PARK_TIMEOUT);

    // durable log holds every skipped tick (3 lines), with tick + reason
    std::string logContents = ForkStats::readLogAll();
    int lineCount = 0;
    for (char character : logContents)
    {
        if (character == '\n')
            lineCount++;
    }
    EXPECT_EQ(lineCount, 3);
    EXPECT_NE(logContents.find("tick=1001"), std::string::npos);
    EXPECT_NE(logContents.find("tick=1002"), std::string::npos);
    EXPECT_NE(logContents.find("tick=1003"), std::string::npos);
    EXPECT_NE(logContents.find("reason=census"), std::string::npos);
    EXPECT_NE(logContents.find("reason=park_timeout"), std::string::npos);

    // summary JSON reflects the counters
    std::string summary = ForkStats::summaryJson();
    EXPECT_NE(summary.find("\"forksSkippedTotal\""), std::string::npos);
    EXPECT_NE(summary.find("\"quiesceTimeout\""), std::string::npos);
    EXPECT_NE(summary.find("\"lastUnforkable\""), std::string::npos);

    std::filesystem::remove(ForkStats::kLogPath);
}

#if defined(__linux__)
TEST(ForkStatsTest, ParentUpdatesAreVisibleToForkChild)
{
    const unsigned long long initialForksOk = ForkStats::forksOk.load();
    const unsigned long long initialMismatches = ForkStats::mismatches.load();
    int releasePipe[2];
    ASSERT_EQ(pipe(releasePipe), 0);

    const pid_t child = fork();
    if (child == 0)
    {
        close(releasePipe[1]);
        char release = 0;
        ssize_t readSize;
        do
        {
            readSize = read(releasePipe[0], &release, 1);
        }
        while (readSize < 0 && errno == EINTR);
        close(releasePipe[0]);

        const bool countersVisible = ForkStats::forksOk.load() == initialForksOk + 1 && ForkStats::mismatches.load() == initialMismatches + 1;
        _exit(readSize == 1 && countersVisible ? 0 : 1);
    }

    if (child < 0)
    {
        close(releasePipe[0]);
        close(releasePipe[1]);
        FAIL() << "fork failed: " << strerror(errno);
        return;
    }
    close(releasePipe[0]);
    ForkStats::onForkOk();
    ForkStats::onVerdict(true);
    const char release = 1;
    EXPECT_EQ(write(releasePipe[1], &release, 1), 1);
    close(releasePipe[1]);

    int status = 0;
    pid_t waited = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;)
    {
        do
        {
            waited = waitpid(child, &status, WNOHANG);
        }
        while (waited < 0 && errno == EINTR);
        if (waited != 0 || std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (waited == 0)
    {
        kill(child, SIGKILL);
        do
        {
            waited = waitpid(child, &status, 0);
        }
        while (waited < 0 && errno == EINTR);
        FAIL() << "fork stats child timed out";
        return;
    }

    ASSERT_EQ(waited, child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif
