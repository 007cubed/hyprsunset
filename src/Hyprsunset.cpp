#include "ConfigManager.hpp"
#include "helpers/Log.hpp"
#include "IPCSocket.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <chrono>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <hyprutils/os/Process.hpp>

#define TIMESPEC_NSEC_PER_SEC 1000000000L

static void registerSignalAction(int sig, void (*handler)(int), int sa_flags = 0) {
    struct sigaction sa;
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = sa_flags;
    sigaction(sig, &sa, nullptr);
}

static void handleExitSignal(int sig) {
    Debug::log(NONE, "┣ Exiting on user interrupt\n╹");
    g_pHyprsunset->terminate();
}

static void timespecAddNs(timespec* pTimespec, int64_t delta) {
    auto delta_ns_low = delta % TIMESPEC_NSEC_PER_SEC;
    auto delta_s_high = delta / TIMESPEC_NSEC_PER_SEC;

    pTimespec->tv_sec += delta_s_high;

    pTimespec->tv_nsec += (long)delta_ns_low;
    if (pTimespec->tv_nsec >= TIMESPEC_NSEC_PER_SEC) {
        pTimespec->tv_nsec -= TIMESPEC_NSEC_PER_SEC;
        ++pTimespec->tv_sec;
    }
}

// kindly borrowed from https://tannerhelland.com/2012/09/18/convert-temperature-rgb-algorithm-code.html
static Mat3x3 matrixForKelvin(unsigned long long temp) {
    float r = 1.F, g = 1.F, b = 1.F;

    temp /= 100.F;

    if (temp <= 66) {
        r = 255;
        g = std::clamp(99.4708025861 * std::log(temp) - 161.1195681661, 0.0, 255.0);
        if (temp <= 19)
            b = 0;
        else
            b = std::clamp(std::log(temp - 10) * 138.5177312231 - 305.0447927307, 0.0, 255.0);
    } else {
        r = std::clamp(329.698727446 * (std::pow(temp - 60, -0.1332047592)), 0.0, 255.0);
        g = std::clamp(288.1221695283 * (std::pow(temp - 60, -0.0755148492)), 0.0, 255.0);
        b = 255;
    }

    return std::array<float, 9>{r / 255.F, 0, 0, 0, g / 255.F, 0, 0, 0, b / 255.F};
}

static Mat3x3 matrixFor(unsigned long long kelvin, float gamma, bool identity) {
    Mat3x3 ctm = identity ? Mat3x3::identity() : matrixForKelvin(kelvin);
    ctm.multiply(std::array<float, 9>{gamma, 0, 0, 0, gamma, 0, 0, 0, gamma});
    return ctm;
}

static Mat3x3 matrixFor(const SSunsetProfile& profile) {
    return matrixFor(profile.temperature, profile.gamma, profile.identity);
}

// Linear in the channel gains, so every step changes the screen by the same amount.
static Mat3x3 blend(const Mat3x3& from, const Mat3x3& to, float progress) {
    auto       result = from.getMatrix();
    const auto target = to.getMatrix();

    for (size_t i = 0; i < result.size(); ++i)
        result[i] += (target[i] - result[i]) * progress;

    return result;
}

// One step per 8-bit level of the channel that changes most, and at most one per second.
static long long transitionSteps(const Mat3x3& from, const Mat3x3& to, std::chrono::seconds duration) {
    const auto a = from.getMatrix(), b = to.getMatrix();
    float      maxDelta = 0.F;

    for (size_t i = 0; i < a.size(); ++i)
        maxDelta = std::max(maxDelta, std::abs(b[i] - a[i]));

    return std::clamp<long long>(std::ceil(maxDelta * 255.F), 1, std::max<long long>(duration.count(), 1));
}

static LocalTime toLocal(SysTime time) {
    return std::chrono::current_zone()->to_local(time);
}

static LocalTime localNow() {
    return toLocal(std::chrono::system_clock::now());
}

// DST makes a local time missing or repeated. Take its first occurrence that is not before notBefore,
// so a wake inside the repeated hour does not land in the past.
static SysTime toSys(LocalTime time, SysTime notBefore) {
    const auto zone     = std::chrono::current_zone();
    const auto earliest = zone->to_sys(time, std::chrono::choose::earliest);
    return earliest >= notBefore ? earliest : zone->to_sys(time, std::chrono::choose::latest);
}

// Start of the occurrence of the profile that is in effect at now.
static SysTime profileStart(const SSunsetProfile& profile, SysTime now) {
    const auto localNow = toLocal(now);
    LocalTime  start    = std::chrono::floor<std::chrono::days>(localNow) + profile.time.hour + profile.time.minute;
    if (start > localNow)
        start -= std::chrono::days(1);
    return std::chrono::current_zone()->to_sys(start, std::chrono::choose::earliest);
}

struct STransitionStep {
    STransition transition;
    SysTime     nextStep;
};

// The transition into profiles[current] at now, if it is still running. Measured in system
// time, so a fade across a DST change keeps its real duration.
static std::optional<STransitionStep> transitionAt(const std::vector<SSunsetProfile>& profiles, size_t current, SysTime now) {
    const auto& profile = profiles[current];
    if (profiles.size() < 2 || profile.transition <= std::chrono::minutes::zero())
        return std::nullopt;

    const auto start   = profileStart(profile, now);
    const auto elapsed = now - start;
    if (elapsed >= profile.transition)
        return std::nullopt;

    const auto& previous = profiles[current == 0 ? profiles.size() - 1 : current - 1];
    const auto  from     = matrixFor(previous);
    const auto  steps    = transitionSteps(from, matrixFor(profile), profile.transition);
    const auto  duration = std::chrono::duration<double>(profile.transition);
    const auto  step     = static_cast<long long>(std::chrono::duration<double>(elapsed) / duration * steps);

    return STransitionStep{
        .transition = {.from = from, .progress = static_cast<float>(step) / steps},
        .nextStep   = start + std::chrono::ceil<std::chrono::seconds>(duration * (step + 1) / steps),
    };
}

void SOutput::applyCTM(struct SState* state) {
    if (!state->pCTMMgr)
        return;

    auto arr = state->ctm.getMatrix();
    state->pCTMMgr->sendSetCtmForOutput(output->resource(), wl_fixed_from_double(arr[0]), wl_fixed_from_double(arr[1]), wl_fixed_from_double(arr[2]), wl_fixed_from_double(arr[3]),
                                        wl_fixed_from_double(arr[4]), wl_fixed_from_double(arr[5]), wl_fixed_from_double(arr[6]), wl_fixed_from_double(arr[7]),
                                        wl_fixed_from_double(arr[8]));
}

void CHyprsunset::scheduleOnSwitch() {
    m_sEventLoopInternals.runOnSwitch = true;
}

void CHyprsunset::commitCTMs() {
    if (!g_pHyprsunset || !g_pHyprsunset->state.pCTMMgr)
        return;

    g_pHyprsunset->state.pCTMMgr->sendCommit();
}

int CHyprsunset::calculateMatrix() {
    if (KELVIN < 1000 || KELVIN > 20000) {
        Debug::log(NONE, "✖ Temperature invalid: {}. The temperature has to be between 1000 and 20000K", KELVIN);
        return 0;
    }

    if (GAMMA < 0 || GAMMA > MAX_GAMMA) {
        Debug::log(NONE, "✖ Gamma invalid: {}%. The gamma has to be between 0% and {}%", GAMMA * 100, MAX_GAMMA * 100);
        return 0;
    }

    if (m_transition) {
        state.ctm = blend(m_transition->from, matrixFor(KELVIN, GAMMA, identity), m_transition->progress);
        Debug::log(TRACE, "┣ Transition at {:.0f}%, CTM {}", m_transition->progress * 100, state.ctm.toString());
        return 1;
    }

    if (!identity)
        Debug::log(NONE, "┣ Setting the temperature to {}K{}\n┃", KELVIN, kelvinSet ? "" : " (default)");
    else
        Debug::log(NONE, "┣ Resetting the matrix (--identity passed)\n┃", KELVIN, kelvinSet ? "" : " (default)");

    state.ctm = matrixFor(KELVIN, GAMMA, identity);

    Debug::log(NONE, "┣ Calculated the CTM to be {}\n┃", state.ctm.toString());

    return 1;
}

CHyprsunset::CHyprsunset() {
    g_hyprsunsetAlive.store(true, std::memory_order_release);
}

int CHyprsunset::init() {
    // connect to the wayland server
    if (const auto SERVER = getenv("XDG_CURRENT_DESKTOP"); SERVER)
        Debug::log(NONE, "┣ Running on {}", SERVER);

    state.wlDisplay = wl_display_connect(nullptr);

    if (!state.wlDisplay) {
        Debug::log(NONE, "✖ Couldn't connect to a wayland compositor");
        return 0;
    }

    state.pRegistry = makeShared<CCWlRegistry>((wl_proxy*)wl_display_get_registry(state.wlDisplay));
    state.pRegistry->setGlobal([this](CCWlRegistry* r, uint32_t name, const char* interface, uint32_t version) {
        const std::string IFACE = interface;

        if (IFACE == hyprland_ctm_control_manager_v1_interface.name) {
            auto targetVersion = std::min(version, 2u);

            Debug::log(NONE, "┣ Found hyprland-ctm-control-v1 supported with version {}, binding to v{}", version, targetVersion);
            state.pCTMMgr = makeShared<CCHyprlandCtmControlManagerV1>(
                (wl_proxy*)wl_registry_bind((wl_registry*)state.pRegistry->resource(), name, &hyprland_ctm_control_manager_v1_interface, targetVersion));

            if (targetVersion >= 2) {
                state.pCTMMgr->setBlocked([this](CCHyprlandCtmControlManagerV1*) {
                    Debug::log(NONE, "✖ A CTM manager is already running on the current compositor.");
                    terminate();
                });
            }
        } else if (IFACE == wl_output_interface.name) {
            if (std::find_if(state.outputs.begin(), state.outputs.end(), [name](const auto& el) { return el->id == name; }) != state.outputs.end())
                return;

            Debug::log(NONE, "┣ Found new output with ID {}, binding", name);
            auto o = state.outputs.emplace_back(
                makeShared<SOutput>(makeShared<CCWlOutput>((wl_proxy*)wl_registry_bind((wl_registry*)state.pRegistry->resource(), name, &wl_output_interface, 3)), name));

            if (state.initialized) {
                Debug::log(NONE, "┣ already initialized, applying CTM instantly");
                o->applyCTM(&state);
                commitCTMs();
            }
        }
    });

    state.pRegistry->setGlobalRemove([this](CCWlRegistry* r, uint32_t name) { std::erase_if(state.outputs, [name](const auto& e) { return e->id == name; }); });

    wl_display_roundtrip(state.wlDisplay);

    if (!state.pCTMMgr) {
        Debug::log(NONE, "✖ Compositor doesn't support hyprland-ctm-control-v1, are you running on Hyprland?");
        return 0;
    }

    if (m_bTerminate.load())
        return 0;

    m_stopFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_stopFd < 0) {
        Debug::log(NONE, "✖ Couldn't create the shutdown eventfd: {}", std::strerror(errno));
        return 0;
    }

    Debug::log(NONE, "┣ Found {} output(s), applying CTMs", state.outputs.size());

    reload();

    state.initialized = true;

    g_pIPCSocket = std::make_unique<CIPCSocket>();
    g_ipcSocketAlive.store(true, std::memory_order_release);
    g_pIPCSocket->initialize();

    registerSignalAction(SIGTERM, ::handleExitSignal);
    registerSignalAction(SIGINT, ::handleExitSignal);

    state.timerFD = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);

    schedule();
    startEventLoop();

    return 1;
}

void CHyprsunset::startEventLoop() {
    pollfd pollfds[] = {
        {
            .fd     = wl_display_get_fd(state.wlDisplay),
            .events = POLLIN,
        },
        {
            .fd     = state.timerFD,
            .events = POLLIN,
        },
    };

    bool        wlEventsPending = false;

    std::thread pollThread([&]() {
        while (1) {
            bool preparedToRead = wl_display_prepare_read(state.wlDisplay) == 0;
            int  ret            = 0;

            if (m_bTerminate.load())
                break;

            if (preparedToRead) {
                ret = poll(pollfds, 2, 5000);

                if (ret < 0) {
                    RASSERT(errno == EINTR, "[core] Polling fds failed with {}", errno);
                    wl_display_cancel_read(state.wlDisplay);
                    continue;
                }

                for (size_t i = 0; i < 2; ++i) {
                    RASSERT(!(pollfds[i].revents & POLLHUP), "[core] Disconnected from pollfd id {}", i);
                }

                wl_display_read_events(state.wlDisplay);
            }

            if (ret > 0 || !preparedToRead) {
                Debug::log(TRACE, "[core] got poll event");
                std::lock_guard<std::mutex> lg(m_sEventLoopInternals.loopRequestMutex);
                m_sEventLoopInternals.shouldProcess = true;
                if (!preparedToRead || (pollfds[0].revents & POLLIN))
                    wlEventsPending = true;
                m_sEventLoopInternals.loopSignal.notify_one();
            }
        }
    });

    while (1) {
        std::unique_lock<std::mutex> lk(m_sEventLoopInternals.loopMutex);

        if (!m_sEventLoopInternals.shouldProcess)
            m_sEventLoopInternals.loopSignal.wait_for(lk, std::chrono::milliseconds(5000), [this] { return m_sEventLoopInternals.shouldProcess; });

        if (m_bTerminate.load())
            break;

        std::lock_guard<std::mutex> lg(m_sEventLoopInternals.loopRequestMutex);

        m_sEventLoopInternals.shouldProcess = false;

        if (wlEventsPending) {
            wlEventsPending = false;
            wl_display_dispatch_pending(state.wlDisplay);
            wl_display_flush(state.wlDisplay);
        }

        if (m_sEventLoopInternals.isScheduled)
            reload();
        else
            tick();

        if (m_sEventLoopInternals.runOnSwitch) {
            onSwitch();
            m_sEventLoopInternals.runOnSwitch = false;
        }

        m_sEventLoopInternals.isScheduled = false;
    }

    Debug::log(TRACE, "Exiting loop");

    // Workers call current_zone() and reload(). Join them before the CTM
    // manager is released and before main() returns into static destruction.
    shutdownWorkers();

    // cleanup wl resources
    state.outputs.clear();
    state.pRegistry.reset();
    state.pCTMMgr.reset();

    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    timespecAddNs(&now, 1000L * 1000L * 100L);

    itimerspec ts = {.it_value = now};

    timerfd_settime(state.timerFD, TFD_TIMER_ABSTIME, &ts, nullptr);

    pollThread.join();

    wl_display_disconnect(state.wlDisplay);
    close(state.timerFD);
}

void CHyprsunset::tick() {
    if (g_pIPCSocket && g_pIPCSocket->mainThreadParseRequest())
        reload();
}

void CHyprsunset::reload() {
    calculateMatrix();

    for (auto& o : state.outputs) {
        o->applyCTM(&state);
    }

    commitCTMs();

    wl_display_flush(state.wlDisplay);
}

void CHyprsunset::onSwitch() {
    Debug::log(LOG, "Executing {}", ONSWITCH);

    Hyprutils::OS::CProcess proc("/bin/sh", {"-c", ONSWITCH});
    if (!proc.runAsync()) {
        Debug::log(ERR, "Failed run \"{}\"", ONSWITCH);
        return;
    }

    Debug::log(LOG, "Process Created with pid {}", proc.pid());
}

void CHyprsunset::loadCurrentProfile() {
    profiles  = g_pConfigManager->getSunsetProfiles();
    MAX_GAMMA = g_pConfigManager->getMaxGamma();

    Debug::log(NONE, "┣ Loaded {} profiles", profiles.size());

    std::sort(profiles.begin(), profiles.end(), [](const auto& a, const auto& b) {
        if (a.time.hour < b.time.hour)
            return true;
        else if (a.time.hour > b.time.hour)
            return false;
        else
            return a.time.minute < b.time.minute;
    });

    // A transition has to finish before the next profile starts.
    for (size_t i = 0; i < profiles.size(); ++i) {
        auto&       profile = profiles[i];
        const auto& next    = profiles[(i + 1) % profiles.size()];
        auto        gap     = (next.time.hour + next.time.minute) - (profile.time.hour + profile.time.minute);
        if (gap <= std::chrono::minutes::zero())
            gap += std::chrono::days(1);

        if (profile.transition > gap) {
            Debug::log(WARN, "Transition of profile {}:{} is longer than the {} minutes until the next profile, shortening it", profile.time.hour.count(),
                       profile.time.minute.count(), gap.count());
            profile.transition = gap;
        }
    }

    const int current = currentProfile(localNow());

    if (current == -1)
        return;

    const auto& profile = profiles[current];
    applyProfile(profile);
    resumeTransition();

    Debug::log(NONE, "┣ Applying profile from: {}:{}", profile.time.hour.count(), profile.time.minute.count());
}

void CHyprsunset::setSchedule(bool enabled) {
    m_scheduleEnabled = enabled;

    if (!enabled) {
        m_transition.reset();
        return;
    }

    // Back on: apply what the schedule says now, mid-fade included.
    if (const int current = currentProfile(localNow()); current != -1) {
        applyProfile(profiles[current]);
        resumeTransition();
    }
}

void CHyprsunset::resumeTransition() {
    m_transitionPaused = false;

    const auto now     = std::chrono::system_clock::now();
    const int  current = currentProfile(toLocal(now));
    const auto step    = current == -1 ? std::nullopt : transitionAt(profiles, current, now);
    m_transition       = step ? std::optional(step->transition) : std::nullopt;
}

void CHyprsunset::applyProfile(const SSunsetProfile& profile) {
    KELVIN   = profile.temperature;
    GAMMA    = profile.gamma;
    ONSWITCH = profile.onSwitch;
    identity = profile.identity;
}

void CHyprsunset::pauseTransition() {
    m_transition.reset();
    m_transitionPaused = true;
}

int CHyprsunset::currentProfile(LocalTime now) {
    if (profiles.empty())
        return -1;
    else if (profiles.size() == 1)
        return 0;

    for (size_t i = 0; i < profiles.size(); ++i) {
        const auto& p = profiles[i];

        auto        time = std::chrono::floor<std::chrono::days>(now) + p.time.hour + p.time.minute;

        if (time >= now) {
            if (i == 0)
                return profiles.size() - 1;
            return i - 1;
        }
    }

    return profiles.size() - 1;
}

std::optional<SSunsetProfile> CHyprsunset::getCurrentProfile() {
    int current = currentProfile(localNow());
    if (current < 0)
        return std::nullopt;

    return profiles[current];
}

static std::string profileJson(const SSunsetProfile& profile) {
    const auto sun = profile.sunEvent == NSun::EVENT_NONE ? std::string{} : std::format("{}{:+}", NSun::eventName(profile.sunEvent), profile.sunOffset.count());
    return std::format(R"({{"time":"{:0>2}:{:0>2}","sun":"{}","temperature":{},"identity":{},"gamma":{:.0f},"transition":{}}})", profile.time.hour.count(),
                       profile.time.minute.count(), sun, profile.temperature, profile.identity, profile.gamma * 100, profile.transition.count());
}

std::string CHyprsunset::statusJson() {
    const auto  now     = std::chrono::system_clock::now();
    const int   current = currentProfile(toLocal(now));
    std::string profile = "null", next = "null", fade = "null";

    if (current != -1) {
        profile = profileJson(profiles[current]);
        next    = profileJson(profiles[(current + 1) % profiles.size()]);

        if (m_transition) {
            const auto end = std::chrono::floor<std::chrono::minutes>(toLocal(profileStart(profiles[current], now) + profiles[current].transition));
            fade           = std::format(R"({{"progress":{:.2f},"end":"{:%H:%M}"}})", m_transition->progress, end);
        }
    }

    return std::format(R"({{"temperature":{},"identity":{},"gamma":{:.0f},"schedule":{},"paused":{},"fade":{},"profile":{},"next":{}}})", KELVIN, identity, GAMMA * 100,
                       m_scheduleEnabled, m_transitionPaused, fade, profile, next);
}

// Relative sleeps pause during suspend. An absolute CLOCK_REALTIME timer is
// already expired when the machine wakes after the deadline. The stop fd is
// polled with it so shutdown does not wait out that deadline.
static bool waitUntilRealtime(int stopFd, const std::atomic<bool>& terminate, timespec deadline) {
    if (terminate.load())
        return false;

    int timerFd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timerFd < 0) {
        Debug::log(ERR, "Failed to create profile timer: {}", std::strerror(errno));
        return false;
    }

    itimerspec its{};
    its.it_value = deadline;
    // A zero value disarms a timerfd. One nanosecond past the epoch is still
    // in the past, so the timer expires immediately instead.
    if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0)
        its.it_value.tv_nsec = 1;

    if (timerfd_settime(timerFd, TFD_TIMER_ABSTIME, &its, nullptr) != 0) {
        Debug::log(ERR, "Failed to wait for profile switch: {}", std::strerror(errno));
        close(timerFd);
        return false;
    }

    pollfd pfds[2] = {};
    pfds[0]        = {.fd = timerFd, .events = POLLIN};
    nfds_t nfds    = 1;
    if (stopFd >= 0) {
        pfds[1] = {.fd = stopFd, .events = POLLIN};
        nfds    = 2;
    }

    bool reached = false;
    while (!terminate.load()) {
        int rc = poll(pfds, nfds, -1);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            Debug::log(ERR, "Failed to wait for profile switch: {}", std::strerror(errno));
            break;
        }

        if (terminate.load() || (nfds == 2 && (pfds[1].revents & POLLIN)))
            break;

        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;

        if (pfds[0].revents & POLLIN) {
            reached = true;
            break;
        }
    }

    close(timerFd);
    return reached;
}

void CHyprsunset::schedule() {
    m_scheduleThread = std::thread([this] {
        // profiles is rebuilt by an IPC reset on the main thread, so it is only read under loopRequestMutex.
        int last = -1;
        {
            std::lock_guard<std::mutex> lg(m_sEventLoopInternals.loopRequestMutex);
            last = currentProfile(localNow());
        }

        while (!m_bTerminate.load()) {
            SysTime wake;
            // A wake at a profile's time applies it and runs on-switch, even if it is the same profile.
            bool forProfile = false;
            {
                std::lock_guard<std::mutex> lg(m_sEventLoopInternals.loopRequestMutex);
                const auto                  now     = std::chrono::system_clock::now();
                const int                   current = currentProfile(toLocal(now));
                if (current == -1)
                    break;

                // Wake for the next transition step, or else for the next profile. Steps keep
                // waking while a manual change pauses the transition, so a reset can resume it.
                if (const auto step = transitionAt(profiles, current, now); step)
                    wake = step->nextStep;
                else {
                    const auto& next     = profiles[(current + 1) % profiles.size()];
                    const auto  localNow = toLocal(now);
                    LocalTime   local    = std::chrono::floor<std::chrono::days>(localNow) + next.time.hour + next.time.minute;
                    if (localNow >= local)
                        local += std::chrono::days(1);
                    wake       = toSys(local, now);
                    forProfile = true;
                }
            }

            const auto seconds  = std::chrono::ceil<std::chrono::seconds>(wake.time_since_epoch());
            timespec   deadline = {.tv_sec = static_cast<time_t>(seconds.count()), .tv_nsec = 0};

            if (!waitUntilRealtime(m_stopFd, m_bTerminate, deadline) || m_bTerminate.load())
                break;

            std::lock_guard<std::mutex> lg(m_sEventLoopInternals.loopRequestMutex);
            if (m_bTerminate.load())
                break;

            const auto now        = std::chrono::system_clock::now();
            const int  newcurrent = currentProfile(toLocal(now));
            if (newcurrent == -1)
                break;

            const bool switched = forProfile || newcurrent != last;

            // With the schedule off nothing is applied, but the profile is tracked, so
            // turning it back on does not count as a switch.
            if (!m_scheduleEnabled) {
                last = newProfile.id;
                continue;
            }

            // A manual change holds until the next profile.
            if (!switched && m_transitionPaused)
                continue;

            if (switched) {
                const auto& newProfile = profiles[newcurrent];
                last                   = newcurrent;
                applyProfile(newProfile);
                m_transitionPaused                = false;
                m_sEventLoopInternals.runOnSwitch = true;
                Debug::log(NONE, "┣ Switched to new profile from: {}:{}{}", newProfile.time.hour.count(), newProfile.time.minute.count(),
                           newProfile.transition.count() > 0 ? std::format(", fading in over {} minutes", newProfile.transition.count()) : "");
            }

            const auto step = transitionAt(profiles, newcurrent, now);
            m_transition    = step ? std::optional(step->transition) : std::nullopt;

            m_sEventLoopInternals.shouldProcess = true;
            m_sEventLoopInternals.isScheduled   = true;
            m_sEventLoopInternals.loopSignal.notify_all();
        }
    });
}

static_assert(std::atomic<bool>::is_always_lock_free, "SIGTERM handler stores m_bTerminate");

void CHyprsunset::signalStop() {
    m_bTerminate.store(true);

    if (m_stopFd < 0)
        return;

    // write() is async-signal-safe. EAGAIN means an earlier wake already saturated the counter.
    const uint64_t value      = 1;
    const int      savedErrno = errno;
    (void)write(m_stopFd, &value, sizeof(value));
    errno = savedErrno;
}

void CHyprsunset::shutdownWorkers() {
    signalStop();

    if (m_scheduleThread.joinable())
        m_scheduleThread.join();

    if (g_ipcSocketAlive.load(std::memory_order_acquire))
        g_pIPCSocket->stop();
}

CHyprsunset::~CHyprsunset() {
    // Wake any worker that is still blocked, then drop the liveness flag
    // before either thread is joined. After this, ~CIPCSocket must not
    // touch this object. The socket global is left alone: exit may already
    // have freed it, and its destructor joins the IPC thread.
    signalStop();
    g_hyprsunsetAlive.store(false, std::memory_order_release);

    if (m_scheduleThread.joinable())
        m_scheduleThread.join();

    if (g_ipcSocketAlive.load(std::memory_order_acquire))
        g_pIPCSocket->stop();

    if (m_stopFd >= 0) {
        close(m_stopFd);
        m_stopFd = -1;
    }
}

void CHyprsunset::terminate() {
    signalStop();

    m_sEventLoopInternals.shouldProcess = true;
    m_sEventLoopInternals.loopSignal.notify_all();
}
