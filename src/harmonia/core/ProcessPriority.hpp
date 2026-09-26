#ifndef HARMONIA_CORE_PROCESSPRIORITY_HPP
#define HARMONIA_CORE_PROCESSPRIORITY_HPP

#include <cerrno>
#include <cstdint>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#elif defined(__unix__) || defined(__linux__) || defined(__APPLE__)
#    include <sys/resource.h>
#    include <sys/types.h>
#endif

namespace harmonia {

/// Background CPU priority for long batch jobs (headless/offscreen capture).
///
/// WHY THIS AND NOT JUST yield(): `std::this_thread::yield()` is `SwitchToThread()` on
/// Windows and `sched_yield()` on Linux -- both are HINTS that only help when a thread of
/// equal-or-higher priority is already runnable on the same core. Microsoft's docs for
/// SwitchToThread: it "will not switch execution to another processor, even if that
/// processor is idle or is running a thread of lower priority", and "if there are no other
/// threads ready to execute... the return value is zero". So a yield-only loop around GPU
/// work still burns 100% CPU. Yielding is a hint; priority is the mechanism.
///
/// Microsoft's guidance for background work (SetThreadPriority / Scheduling Priorities):
/// "For threads that perform background work... it is not sufficient to adjust the CPU
/// scheduling priority... Threads that perform background work should use
/// THREAD_MODE_BACKGROUND_BEGIN/END", and CPU-intensive background threads "can be set to
/// THREAD_PRIORITY_BELOW_NORMAL... to ensure that they can be preempted when necessary".
///
/// This drops the PROCESS below normal CPU priority so every normal process on the machine
/// (desktop, browser, editor) preempts the render automatically -- no cooperation required.
/// On an idle machine the render still gets the full CPU; under load it finishes a bit
/// later instead of freezing everything else.
///
/// PLATFORM MAP (same intent everywhere: "below normal", not "background"):
///   Windows  SetPriorityClass(BELOW_NORMAL_PRIORITY_CLASS) -> base priority 7 vs 8.
///            Fully reversible: the destructor restores the class GetPriorityClass returned.
///   POSIX    setpriority(PRIO_PROCESS, 0, previous + kNiceDelta), clamped to 19.
///            Lowering one's own priority is unprivileged and works for any user.
///   Other    no-op.
///
/// !! POSIX RESTORE IS BEST-EFFORT AND USUALLY A NO-OP !!
/// `man setpriority`: EACCES -- "attempted to set a lower nice value (i.e. a higher process
/// priority), but did not have the required capability (CAP_SYS_NICE)". Raising priority
/// back is therefore refused for an unprivileged process, so on POSIX this drop is
/// effectively ONE-WAY. The destructor still attempts the restore (it succeeds when the
/// process does hold CAP_SYS_NICE), and `restored()` reports whether it actually happened.
///
/// Consequence for callers: use this only where the process is a one-shot batch job that
/// exits immediately afterwards -- which is exactly `App::renderOffscreen()`. Do NOT wrap
/// it around a long-lived interactive loop, or the process stays deprioritised forever.
///
/// Deliberately NOT the heavier "background" modes: `PROCESS_MODE_BACKGROUND_BEGIN` on
/// Windows and `SCHED_BATCH`/`ioprio_set` on Linux also cut I/O and memory priority, which
/// makes a capture many times slower on a busy machine (see "Why does
/// THREAD_MODE_BACKGROUND_BEGIN cause my code to run 20x slower"). CPU-only deprioritising
/// is the right balance for a render batch job: it writes its images at the end and shares
/// no per-frame I/O with the rest of the system.
class ScopedBackgroundPriority {
public:
    ScopedBackgroundPriority() noexcept {
#if defined(_WIN32)
        m_process = GetCurrentProcess();
        m_previous = GetPriorityClass(m_process);
        if (m_previous != 0U) {
            SetPriorityClass(m_process, BELOW_NORMAL_PRIORITY_CLASS);
        }
        m_dropped = true;
#elif defined(__unix__) || defined(__linux__) || defined(__APPLE__)
        errno = 0;
        const int previous = getpriority(PRIO_PROCESS, 0);
        // getpriority returns -1 as a legitimate value, so only trust it when errno is clear.
        if (errno == 0) {
            m_previous = previous;
            const int raised = previous + kNiceDelta;
            const int target = raised > kNiceMax ? kNiceMax : raised;
            m_dropped = (setpriority(PRIO_PROCESS, 0, target) == 0);
        }
#endif
    }

    ~ScopedBackgroundPriority() noexcept {
#if defined(_WIN32)
        if (m_dropped && m_previous != 0U) {
            SetPriorityClass(m_process, m_previous);
        }
#elif defined(__unix__) || defined(__linux__) || defined(__APPLE__)
        if (m_dropped) {
            // Best-effort: fails with EACCES for an unprivileged process (see class docs).
            m_restored = (setpriority(PRIO_PROCESS, 0, m_previous) == 0);
        }
#endif
    }

    /// True when the priority was actually lowered at construction.
    [[nodiscard]] bool dropped() const noexcept { return m_dropped; }

    /// Only meaningful after destruction on POSIX (where restore may be refused); on
    /// Windows the restore always succeeds when the class was lowered. Kept for the test
    /// harness and for future callers that need to know.
    [[nodiscard]] bool restored() const noexcept { return m_restored; }

    ScopedBackgroundPriority(const ScopedBackgroundPriority&) = delete;
    ScopedBackgroundPriority& operator=(const ScopedBackgroundPriority&) = delete;
    ScopedBackgroundPriority(ScopedBackgroundPriority&&) = delete;
    ScopedBackgroundPriority& operator=(ScopedBackgroundPriority&&) = delete;

private:
#if defined(_WIN32)
    HANDLE m_process{nullptr};
    DWORD m_previous{0U};
#elif defined(__unix__) || defined(__linux__) || defined(__APPLE__)
    // Nice range is [-20, 19]; +10 puts us clearly behind normal (nice 0) work while still
    // taking the whole CPU when the machine is otherwise idle. CFS weight at nice 10 is
    // ~1/9 of nice 0, so interactive work wins without the render stalling entirely.
    static constexpr int kNiceDelta = 10;
    static constexpr int kNiceMax = 19;
    int m_previous{0};
#endif
    bool m_dropped{false};
    bool m_restored{false};
};

} // namespace harmonia

#endif // HARMONIA_CORE_PROCESSPRIORITY_HPP
