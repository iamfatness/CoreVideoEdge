#include "engine-comms.h"

#include "engine-writer.h"

#if __has_include(<meeting_service_components/meeting_audio_interface.h>)
#include <meeting_service_components/meeting_audio_interface.h>
#include <meeting_service_components/meeting_participants_ctrl_interface.h>
#include <meeting_service_components/meeting_talkback_ctrl_interface.h>
#else
#include <meeting_audio_interface.h>
#include <meeting_participants_ctrl_interface.h>
#include <meeting_talkback_ctrl_interface.h>
#endif
#if __has_include(<rawdata/zoom_rawdata_api.h>)
#include <rawdata/zoom_rawdata_api.h>
#include <rawdata/rawdata_audio_helper_interface.h>
#else
#include <zoom_rawdata_api.h>
#include <rawdata_audio_helper_interface.h>
#endif

#include "comms/comms_core.h"
#include "comms/comms_protocol.h"
#include "comms/devices.h"
#include "comms/engine.h"
#include "comms/talkback_channels.h"
#include "comms/talkback_sdk_win.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string narrow(const zchar_t *s)
{
    if (!s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// The auto-suppress: a virtual mic that is installed and never fed. The SDK
// then treats our meeting mic as open (talkback delivers, law 1) while the
// room hears nothing. Callbacks are recorded for the ops log only.
class SilentMic : public ZOOMSDK::IZoomSDKVirtualAudioMicEvent {
public:
    void onMicInitialize(ZOOMSDK::IZoomSDKAudioRawDataSender *) override { initialised.store(true); }
    void onMicStartSend() override { sending.store(true); }
    void onMicStopSend() override { sending.store(false); }
    void onMicUninitialized() override { initialised.store(false); sending.store(false); }
    std::atomic<bool> initialised{false};
    std::atomic<bool> sending{false};
};

// The mic chain on its own thread: ZComms' AudioEngine (capture -> gain ->
// limiter -> PTT envelope -> sidetone tap -> 20 ms frames -> pacer -> sink).
// Opening/closing WASAPI devices and enumerating them happens ONLY here.
class CommsAudioWin : public edgecomms::CommsAudioPort {
public:
    explicit CommsAudioWin(zc::FrameSink *sink) : sink_(sink) {}
    ~CommsAudioWin() override { shutdown(); }

    void SetRunning(bool on) override { update([&] { want_running_ = on; }); }
    void SetTalk(bool on) override
    {
        talk_.store(on);
        // Immediate when the worker is not mid-restart; otherwise the worker
        // applies talk_ on its next pass (it re-asserts every 50 ms).
        std::unique_lock<std::mutex> lock(engine_m_, std::try_to_lock);
        if (lock.owns_lock() && engine_) engine_->SetTalk(on);
    }
    void SetMic(const std::string &d) override { update([&] { want_mic_ = d; restart_ = true; }); }
    void SetSidetoneOut(const std::string &d) override { update([&] { want_out_ = d; restart_ = true; }); }
    void SetSidetone(bool on) override { update([&] { sidetone_ = on; }); }
    void SetTone(bool on) override { update([&] { tone_ = on; }); }
    void SetGain(int db) override { update([&] { gain_ = db; }); }
    edgecomms::CommsAudioStatus Status() override
    {
        std::lock_guard<std::mutex> lock(m_);
        return status_;
    }

    void ensure_started()
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!worker_.joinable() && !quit_) worker_ = std::thread([this] { run(); });
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(m_);
            quit_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

private:
    template <typename F> void update(F f)
    {
        {
            std::lock_guard<std::mutex> lock(m_);
            f();
        }
        cv_.notify_all();
    }

    void run()
    {
        int64_t next_enum = 0, next_open = 0;
        std::vector<std::string> mics, outs;
        std::string error;
        while (true) {
            bool running, restart, sidetone, tone;
            int gain;
            std::string mic, out;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait_for(lock, std::chrono::milliseconds(50));
                if (quit_) break;
                running = want_running_;
                restart = restart_;
                restart_ = false;
                sidetone = sidetone_;
                tone = tone_;
                gain = gain_;
                mic = want_mic_;
                out = want_out_;
            }
            const int64_t now = now_ms();
            if (now >= next_enum) {
                next_enum = now + 5000;
                mics.clear();
                outs.clear();
                for (const auto &d : zc::ListCaptureDevices()) mics.push_back(d.name);
                for (const auto &d : zc::ListPlaybackDevices()) outs.push_back(d.name);
            }
            if ((restart || !running) && engine_) stop_engine();
            if (restart) next_open = 0;
            if (running && !engine_ && now >= next_open) {
                error = open_engine(mic, out, gain, sidetone);
                if (!error.empty()) next_open = now + 5000;  // retry, never spin
            }
            edgecomms::CommsAudioStatus st;
            {
                std::lock_guard<std::mutex> lock(engine_m_);
                if (engine_) {
                    engine_->SetInputGainDb(gain);
                    engine_->SetSidetoneEnabled(sidetone);
                    engine_->SetTestTone(tone);
                    engine_->SetTalk(talk_.load());
                    const zc::EngineStats es = engine_->stats();
                    st.running = true;
                    st.device = engine_->capture_device_name();
                    st.sidetone_device = engine_->monitor_device_name();
                    st.peak_dbfs = 20.0 * std::log10((std::max)(es.capture_peak, 1.0e-5));
                    st.frames_sent = es.pacer.sends;
                    st.underruns = es.pacer.underruns;
                } else {
                    st.device = mic;
                    st.sidetone_device = out;
                }
            }
            st.devices = mics;
            st.sidetone_devices = outs;
            st.gain_db = gain;
            st.sidetone = sidetone;
            st.tone = tone;
            st.error = error;
            {
                std::lock_guard<std::mutex> lock(m_);
                // Counters survive an engine restart (device switch): a
                // number that drops back to zero reads as a broken TX path.
                if (!st.running) {
                    st.frames_sent = status_.frames_sent;
                    st.underruns = status_.underruns;
                } else {
                    st.frames_sent += frames_base_;
                    st.underruns += underruns_base_;
                }
                status_ = st;
            }
        }
        if (engine_) stop_engine();
    }

    std::string open_engine(const std::string &mic, const std::string &out, int gain, bool sidetone)
    {
        zc::EngineConfig cfg;
        cfg.capture_device = mic;
        cfg.monitor_device = out;
        cfg.monitor_enabled = true;
        cfg.sidetone_enabled = sidetone;
        cfg.input_gain_db = gain;
        cfg.aec = false;  // not ported in this slice
        std::string err;
        auto engine = std::make_unique<zc::AudioEngine>(cfg, sink_);
        if (!engine->Start(&err)) {
            // A missing or busy headphone output must not cost the operator
            // their talk path: retry without the sidetone monitor and say so.
            cfg.monitor_enabled = false;
            std::string err2;
            engine = std::make_unique<zc::AudioEngine>(cfg, sink_);
            if (!engine->Start(&err2)) return err2.empty() ? err : err2;
            err = "sidetone output unavailable (" + err + "); talking without sidetone";
        }
        std::lock_guard<std::mutex> lock(engine_m_);
        engine_ = std::move(engine);
        return err;
    }

    void stop_engine()
    {
        std::unique_ptr<zc::AudioEngine> old;
        {
            std::lock_guard<std::mutex> lock(engine_m_);
            old = std::move(engine_);
        }
        if (!old) return;
        old->SetTalk(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));  // let the release ramp finish
        const zc::EngineStats es = old->stats();
        old->Stop();  // joins the pacer: no Send after this returns
        std::lock_guard<std::mutex> lock(m_);
        frames_base_ += es.pacer.sends;
        underruns_base_ += es.pacer.underruns;
    }

    zc::FrameSink *sink_;
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false;
    bool want_running_ = false;
    bool restart_ = false;
    std::string want_mic_, want_out_;
    bool sidetone_ = true, tone_ = false;
    int gain_ = 0;
    uint64_t frames_base_ = 0, underruns_base_ = 0;
    edgecomms::CommsAudioStatus status_;
    std::atomic<bool> talk_{false};
    std::mutex engine_m_;
    std::unique_ptr<zc::AudioEngine> engine_;
};

std::atomic<bool> g_owns_mic{false};

struct State {
    ZOOMSDK::IMeetingService **meeting_svc = nullptr;
    UINT_PTR timer = 0;
    bool in_tick = false;
    edgecomms::CommsBankSink sink;
    std::unique_ptr<CommsAudioWin> audio;
    std::unique_ptr<edgecomms::CommsCore> core;

    // Per meeting.
    std::unique_ptr<zc::TalkbackSdkWin> adapter;
    std::unique_ptr<zc::TalkbackChannels> bank;
    std::vector<zc::RosterMember> roster;
    int64_t next_roster_ms = 0;
    int64_t next_mic_ms = 0;
    bool silent_mic_installed = false;
    bool silent_mic_failed = false;
    bool silent_mic_ever = false;  // removed again at shutdown
    // Adapters are never destroyed while the SDK might still hold them as an
    // event sink: a new meeting's adapter replaces the registration, the old
    // one is parked here until process exit (a few hundred bytes each).
    std::vector<std::unique_ptr<zc::TalkbackSdkWin>> retired;

    SilentMic silent_mic;
    std::string last_json;
};

State &S()
{
    static State s;
    return s;
}

void emit_state(bool force)
{
    auto &s = S();
    if (!s.core) return;
    std::string json = s.core->StateJson();
    if (!force && json == s.last_json) return;  // only while changed
    s.last_json = json;
    EngineIpc::write(json);
}

void end_meeting()
{
    auto &s = S();
    if (s.core) s.core->DetachBank();  // the sink lets go first
    if (s.adapter) {
        s.adapter->SetEvents(nullptr);
        s.retired.push_back(std::move(s.adapter));
    }
    s.bank.reset();
    s.roster.clear();
    s.next_roster_ms = s.next_mic_ms = 0;
    s.silent_mic_installed = false;
    s.silent_mic_failed = false;
    g_owns_mic.store(false);
}

void refresh_roster(ZOOMSDK::IMeetingParticipantsController *parts, unsigned int self_id)
{
    // Polled, never evented: EngineParticipants owns the participants
    // controller's single event slot, and ZComms refreshes on a timer anyway
    // (capability changes have no callback).
    auto &s = S();
    s.roster.clear();
    auto *list = parts->GetParticipantsList();
    if (!list) return;
    for (int i = 0; i < list->GetCount(); ++i) {
        const unsigned int uid = list->GetItem(i);
        if (uid == self_id) continue;
        zc::RosterMember m;
        m.user_id = uid;
        if (auto *info = parts->GetUserByUserID(uid)) {
            m.name = narrow(info->GetUserName());
            m.is_host = info->IsHost();
            m.supports_talkback = info->IsSupportTalkback();
        }
        if (m.name.empty()) m.name = "user " + std::to_string(uid);
        s.roster.push_back(std::move(m));
    }
}

void tick()
{
    auto &s = S();
    if (!s.core || s.in_tick) return;
    s.in_tick = true;
    const int64_t now = now_ms();
    edgecomms::CommsInputs in;
    in.now_ms = now;
    ZOOMSDK::IMeetingService *svc = s.meeting_svc ? *s.meeting_svc : nullptr;
    in.in_meeting = svc && svc->GetMeetingStatus() == ZOOMSDK::MEETING_STATUS_INMEETING;
    if (!in.in_meeting && (s.bank || s.adapter)) end_meeting();

    if (in.in_meeting) {
        auto *ctrl = svc->GetMeetingTalkbackController();
        if (ctrl && !s.adapter) {
            s.adapter = std::make_unique<zc::TalkbackSdkWin>(ctrl);
            s.bank = std::make_unique<zc::TalkbackChannels>(s.adapter.get());
            s.core->AttachBank(s.bank.get());
        }
        in.controller = ctrl != nullptr;
        in.supported = s.adapter && s.adapter->MeetingSupportsTalkback();

        auto *parts = svc->GetMeetingParticipantsController();
        ZOOMSDK::IUserInfo *me = parts ? parts->GetMySelfUser() : nullptr;
        if (me) {
            const auto role = me->GetUserRole();
            in.host_or_cohost = role == ZOOMSDK::USERROLE_HOST || role == ZOOMSDK::USERROLE_COHOST;
            s.core->SetStationName(narrow(me->GetUserName()));
        }
        if (parts && now >= s.next_roster_ms) {
            s.next_roster_ms = now + 1000;
            refresh_roster(parts, me ? me->GetUserID() : 0);
        }
        in.roster = s.roster;

        // Law 1: talkback is silent unless our meeting audio is OPEN. Only
        // once the station can actually run talkback, and only behind the
        // silent virtual mic.
        if (in.controller && in.supported && in.host_or_cohost && me) {
            if (!s.silent_mic_installed && !s.silent_mic_failed) {
                auto *helper = ZOOMSDK::GetAudioRawdataHelper();
                const ZOOMSDK::SDKError err =
                    helper ? helper->setExternalAudioSource(&s.silent_mic) : ZOOMSDK::SDKERR_UNINITIALIZE;
                if (err == ZOOMSDK::SDKERR_SUCCESS) {
                    s.silent_mic_installed = true;
                    s.silent_mic_ever = true;
                    g_owns_mic.store(true);  // capture-only stops muting us
                    s.core->Log("meeting mic auto-suppressed (open but silent to the room)");
                } else {
                    s.silent_mic_failed = true;
                    s.core->Log("mic auto-suppress unavailable (setExternalAudioSource " +
                                std::to_string(static_cast<int>(err)) +
                                ") -- the meeting mic stays closed so the room never hears this PC; talkback is silent");
                }
                EngineIpc::write(R"({"cmd":"debug","stage":"comms_silent_mic","code":)" +
                                 std::to_string(static_cast<int>(err)) + "}");
            }
            if (s.silent_mic_installed && now >= s.next_mic_ms) {
                s.next_mic_ms = now + 2000;
                if (me->IsAudioMuted()) {
                    auto *audio_ctrl = svc->GetMeetingAudioController();
                    const ZOOMSDK::SDKError err =
                        audio_ctrl ? audio_ctrl->UnMuteAudio(me->GetUserID()) : ZOOMSDK::SDKERR_UNINITIALIZE;
                    if (err == ZOOMSDK::SDKERR_SUCCESS)
                        s.core->Log("meeting mic opened -- talkback delivery needs it");
                    else
                        s.core->Log("meeting mic is MUTED (talkback is silent until it opens): UnMuteAudio " +
                                    std::to_string(static_cast<int>(err)) +
                                    " -- if the meeting forbids self-unmute, the host must allow it");
                }
            }
        }
        in.mic_open = me && me->GetAudioJoinType() != ZOOMSDK::AUDIOTYPE_NONE && !me->IsAudioMuted();
    }

    s.core->Tick(in);
    emit_state(false);
    s.in_tick = false;
}

void CALLBACK timer_proc(HWND, UINT, UINT_PTR, DWORD)
{
    // Comms must never take the engine down with it.
    try {
        tick();
    } catch (const std::exception &e) {
        S().in_tick = false;
        EngineIpc::write(std::string(R"({"cmd":"debug","stage":"comms_tick_exception","what":")") +
                         e.what() + "\"}");
    } catch (...) {
        S().in_tick = false;
        EngineIpc::write(R"({"cmd":"debug","stage":"comms_tick_exception"})");
    }
}

}  // namespace

EngineComms &EngineComms::instance()
{
    static EngineComms inst;
    return inst;
}

bool EngineComms::owns_mic() { return g_owns_mic.load(); }

void EngineComms::start(ZOOMSDK::IMeetingService **meeting_svc)
{
    auto &s = S();
    if (s.core) return;
    s.meeting_svc = meeting_svc;
    s.audio = std::make_unique<CommsAudioWin>(&s.sink);
    s.audio->ensure_started();
    s.core = std::make_unique<edgecomms::CommsCore>(s.audio.get(), &s.sink);
    s.timer = SetTimer(nullptr, 0, 100, timer_proc);
    if (!s.timer)
        EngineIpc::write(R"({"cmd":"debug","stage":"comms_timer","code":)" +
                         std::to_string(static_cast<int>(GetLastError())) + "}");
}

bool EngineComms::handle_line(const std::string &line)
{
    if (!edgecomms::IsCommsCommand(line)) return false;
    auto &s = S();
    if (!s.core) return true;
    try {
        const edgecomms::CommsCommand c = edgecomms::ParseCommsCommand(line);
        const std::string refusal = s.core->Apply(c, now_ms());
        if (!refusal.empty()) s.core->Log("refused: " + refusal);
        emit_state(false);
    } catch (...) {
        EngineIpc::write(R"({"cmd":"debug","stage":"comms_command_exception"})");
    }
    return true;
}

void EngineComms::shutdown()
{
    auto &s = S();
    if (!s.core) return;
    if (s.timer) KillTimer(nullptr, s.timer);
    s.timer = 0;
    end_meeting();
    if (s.audio) s.audio->shutdown();  // joins the worker, which stops the pacer
    if (s.silent_mic_ever) {
        if (auto *helper = ZOOMSDK::GetAudioRawdataHelper()) helper->setExternalAudioSource(nullptr);
    }
    s.core.reset();
}
