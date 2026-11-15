#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <sys/signal.h>
#include <thread>
#include <wayland-client.h>
#include <vector>
#include <mutex>
#include <optional>
#include <condition_variable>
#include "protocols/hyprland-ctm-control-v1.hpp"
#include "protocols/wayland.hpp"

#include <hyprutils/math/Mat3x3.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
using namespace Hyprutils::Math;
using namespace Hyprutils::Memory;
#define UP CUniquePointer
#define SP CSharedPointer
#define WP CWeakPointer

struct SOutput {
    SP<CCWlOutput> output;
    uint32_t       id = 0;
    void           applyCTM(struct SState*);
};

struct SState {
    SP<CCWlRegistry>                  pRegistry;
    SP<CCHyprlandCtmControlManagerV1> pCTMMgr;
    wl_display*                       wlDisplay = nullptr;
    std::vector<SP<SOutput>>          outputs;
    bool                              initialized = false;
    Mat3x3                            ctm;
    int                               timerFD = -1;
};

enum eSunEvent : uint8_t {
    SUN_NONE = 0,
    SUN_RISE,
    SUN_SET,
};

struct SSunsetProfile {
    struct {
        std::chrono::hours   hour{0};
        std::chrono::minutes minute{0};
    } time;

    unsigned long temperature = 6000;
    float         gamma       = 1.0f;
    bool          identity    = false;
    std::string   onSwitch    = "";
    // Not SUN_NONE: time is worked out for each day from the location.
    eSunEvent sun = SUN_NONE;
};

struct SLocation {
    double latitude  = 0;
    double longitude = 0;
};

class CHyprsunset {
  public:
    float              MAX_GAMMA = 1.0f; // default
    float              GAMMA     = 1.0f; // default
    unsigned long long KELVIN    = 6000; // default
    std::string        ONSWITCH  = "";   //default
    bool               kelvinSet = false, identity = false;
    SState             state;
    // Written from the SIGTERM handler and read by the worker threads.
    std::atomic<bool> m_bTerminate{false};
    // Stays readable after the first wake so every waiter observes shutdown
    // without consuming the counter out from under the others.
    int m_stopFd = -1;

    CHyprsunset();
    ~CHyprsunset();

    int                           calculateMatrix();
    int                           init();
    void                          tick();
    void                          loadCurrentProfile();
    std::optional<SSunsetProfile> getCurrentProfile();
    void                          scheduleOnSwitch();
    void                          terminate();
    // Async-signal-safe. Wakes the schedule and IPC threads blocked in poll.
    void signalStop();

    struct {
        std::condition_variable loopSignal;
        std::mutex              loopMutex;
        std::mutex              loopRequestMutex;

        bool                    shouldProcess = false;
        bool                    isScheduled   = false;
        bool                    runOnSwitch   = false;
    } m_sEventLoopInternals;

  private:
    static void commitCTMs();
    void        reload();
    void        schedule();
    void        onSwitch();
    int         currentProfile();
    void        startEventLoop();
    // Join workers before dropping the CTM manager or returning from main.
    // Otherwise a detached thread calls current_zone() or reload() while
    // those objects are being destroyed.
    void shutdownWorkers();

    // Fills profiles from m_configProfiles with today's sun times. Under loopRequestMutex.
    void                        buildProfiles();

    std::thread                 m_scheduleThread;
    std::vector<SSunsetProfile> profiles;
    std::vector<SSunsetProfile> m_configProfiles;
    std::optional<SLocation>    m_location;
};

// Cleared at the start of ~CHyprsunset. See g_ipcSocketAlive.
inline std::atomic<bool>            g_hyprsunsetAlive{false};
inline std::unique_ptr<CHyprsunset> g_pHyprsunset;
