// Small audio manager: cached sound buffers, a pool of sf::Sound voices and a
// looping background track. Everything degrades to a silent no-op when the
// audio device or files are missing. SOUND_DEBUG=1 logs each played sound.
#pragma once

#include <SFML/Audio.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <cmath>
#include <string>
#include <vector>

enum class SfxId {
    Shoot, Hit, Teleport, Pickup, Deliver, Win, Lose, Join, Chat, Step, Empty,
    UiClick, UiHover, UiBack, ChatPop, Whisper,
    Alarm, Drop, Recover, Return, Leap, Land, Secret, Count
};

class Sound {
public:
    static Sound& get() { static Sound s; return s; }

    // root = directory containing "Ninja Adventure - Asset Pack"
    // (as returned by findAssetRoot(); the pack folder is searched under it).
    void init(const std::string& root) {
        if (inited_) return;
        inited_ = true;
        debug_ = std::getenv("SOUND_DEBUG") != nullptr;
        dir_ = root;
        if (!dir_.empty() && dir_.back() != '/' && dir_.back() != '\\') dir_ += '/';
        std::string a = dir_ + "Audio/";
        std::string s = a + "Sounds/";
        auto set = [&](SfxId id, const std::string& rel) { files_[(size_t)id] = rel; };
        set(SfxId::Shoot,    s + "Elemental/Fireball.wav");
        set(SfxId::Hit,      s + "Hit & Impact/Hit1.wav");
        set(SfxId::Teleport, s + "Magic & Skill/Magic1.wav");
        set(SfxId::Pickup,   s + "Bonus/Gold1.wav");
        set(SfxId::Deliver,  s + "Bonus/PowerUp1.wav");
        set(SfxId::Win,      a + "Jingles/LevelUp1.wav");
        set(SfxId::Lose,     a + "Jingles/GameOver.wav");
        set(SfxId::Join,     s + "Menu/Menu10.wav");
        set(SfxId::Chat,     s + "Alert/Alert.wav");
        set(SfxId::ChatPop,  s + "Menu/Accept5.wav");    // short bright "pop": a message from a teammate
        set(SfxId::Whisper,  s + "Alert/Alert2.wav");    // slightly longer two-tone alert: a private message
        set(SfxId::Step,     s + "Elemental/Grass.wav");
        set(SfxId::Empty,    s + "Menu/Cancel.wav");
        set(SfxId::UiClick,  s + "Menu/Menu5.wav");
        set(SfxId::UiHover,  s + "Menu/Move1.wav");
        set(SfxId::UiBack,   s + "Menu/Cancel.wav");
        set(SfxId::Alarm,    s + "Alert/Alert3.wav");            // a trophy was stolen (global)
        set(SfxId::Drop,     s + "Alert/Alert5.wav");
        set(SfxId::Recover,  s + "Bonus/PowerUp2.wav");
        set(SfxId::Return,   s + "Magic & Skill/Heal.wav");
        set(SfxId::Leap,     s + "Jump & Bounce/Jump.wav");
        set(SfxId::Land,     s + "Jump & Bounce/Bounce3.wav");
        set(SfxId::Secret,   s + "Bonus/Bonus3.wav");
        sfxOk_ = true;
        if (debug_) std::cerr << "[sound] init root=" << dir_ << " audio=" << (sfxOk_ ? "yes" : "no") << "\n";
    }

    // Background music: a shuffled playlist (each track plays once, then the next one) that crossfades to a
    // tense "chase" track while the local player carries a treasure. Missing files are skipped silently.
    // Extra downloaded tracks live in Audio/MusicExtra/ (see CREDITS in that folder).
    void startMusic() {
        if (!inited_ || !sfxOk_) return;
        const std::string M = "Audio/Musics/", X = "Audio/MusicExtra/";
        calmList_  = { X + "Sneaky Snitch.mp3", M + "31 - Sunny.ogg", X + "Pixelland.mp3", M + "35 - Adventure.ogg",
                       M + "20 - Good Time.ogg", X + "Carefree.mp3", M + "23 - Road.ogg",
                       X + "Monkeys Spinning Monkeys.mp3", M + "36 - Village.ogg", X + "Wallpaper.mp3", M + "27 - Chill.ogg" };
        chaseList_ = { X + "Black Vortex.mp3", X + "Hot Pursuit.mp3", X + "Volatile Reaction.mp3",
                       X + "Prelude and Action.mp3", M + "28 - Tension.ogg", M + "17 - Fight.ogg" };
        std::mt19937 rng{ std::random_device{}() };
        std::shuffle(calmList_.begin(), calmList_.end(), rng);
        std::shuffle(chaseList_.begin(), chaseList_.end(), rng);
        musicOk_ = true;
        nextCalm();
    }

    // Call every frame (menus included): advances playlists and crossfades.
    void tick(float dt) {
        if (!musicOk_) return;
        chaseT_ -= dt;
        if (forceChase_) { forceT_ += dt; if (forceT_ > 4.f && forceT_ < 12.f) setChase(true); }   // SOUND_FORCE_CHASE=1 debug
        const bool wantChase = chaseT_ > 0.f;
        const float step = dt * 1.6f;
        calmGain_  = wantChase ? std::max(0.f, calmGain_ - step)  : std::min(1.f, calmGain_ + step);
        chaseGain_ = wantChase ? std::min(1.f, chaseGain_ + step) : std::max(0.f, chaseGain_ - step);
        if (wantChase) {
            if (chase_.getStatus() == sf::SoundSource::Status::Stopped) nextChase();
            if (calmGain_ <= 0.f && calm_.getStatus() == sf::SoundSource::Status::Playing) calm_.pause();
        } else {
            if (calmGain_ > 0.f && calm_.getStatus() == sf::SoundSource::Status::Paused) calm_.play();
            if (chaseGain_ <= 0.f && chase_.getStatus() != sf::SoundSource::Status::Stopped) chase_.stop();
        }
        if (calmAvail_ && calm_.getStatus() == sf::SoundSource::Status::Stopped) nextCalm();   // track finished -> next song
        applyMusicVolume();
    }

    // Call every frame with "local player is carrying a treasure"; fades back out half a second after it stops.
    void setChase(bool on) {
        if (on) { if (chaseT_ <= 0.f && debug_) std::cerr << "[sound] chase music ON\n"; chaseT_ = 0.5f; }
    }

    // vol: extra 0..1 scale (e.g. distance attenuation), pitch: playback speed.
    void play(SfxId id, float vol = 1.f, float pitch = 1.f) {
        if (!inited_ || !sfxOk_ || vol <= 0.01f) return;
        const std::string& f = files_[(size_t)id];
        if (f.empty()) return;
        if (debug_) std::cerr << "[sound] play " << name(id) << " vol=" << vol << " " << f << "\n";
        if (muted_) return;
        const sf::SoundBuffer* buf = load(f);
        if (!buf) return;
        sf::Sound* snd = nullptr;
        for (auto& p : pool_) if (p->getStatus() == sf::SoundSource::Status::Stopped) { snd = p.get(); break; }
        if (!snd) {
            if (pool_.size() >= 24) return;
            pool_.push_back(std::make_unique<sf::Sound>(*buf));
            snd = pool_.back().get();
        }
        snd->setBuffer(*buf);
        snd->setVolume(std::clamp(master_ * vol, 0.f, 1.f) * 100.f);
        snd->setPitch(pitch);
        snd->play();
    }

    // Footsteps: call every frame while moving; rate-limited internally.
    void step(float dt, bool moving) {
        stepT_ -= dt;
        if (!moving) { stepT_ = std::min(stepT_, 0.f); return; }
        if (stepT_ <= 0.f) {
            stepT_ = 0.32f;
            stepAlt_ = !stepAlt_;
            play(SfxId::Step, 0.35f, stepAlt_ ? 1.0f : 0.9f);
        }
    }

    // Distance-attenuated volume factor (pixels).
    static float distVol(float dx, float dy, float full = 160.f, float maxD = 700.f) {
        float d = std::sqrt(dx * dx + dy * dy);
        if (d <= full) return 1.f;
        if (d >= maxD) return 0.f;
        return 1.f - (d - full) / (maxD - full);
    }

    void toggleMute() { muted_ = !muted_; applyMusicVolume(); }
    void adjustVolume(float delta) {
        master_ = std::clamp(master_ + delta, 0.f, 1.f);
        if (delta != 0.f) muted_ = false;
        applyMusicVolume();
    }
    bool muted() const { return muted_; }
    float volume() const { return master_; }
    std::string status() const {
        return muted_ ? "Sound: muted" : "Volume: " + std::to_string((int)(master_ * 100.f + 0.5f)) + "%";
    }

private:
    Sound() = default;

    void applyMusicVolume() {
        if (!musicOk_) return;
        calm_.setVolume(muted_ ? 0.f : master_ * 30.f * calmGain_);
        chase_.setVolume(muted_ ? 0.f : master_ * 50.f * chaseGain_);
    }
    bool openNext(sf::Music& m, std::vector<std::string>& list, size_t& idx, bool paused) {
        for (size_t tries = 0; tries < list.size(); ++tries) {
            const std::string& rel = list[idx++ % list.size()];
            if (!m.openFromFile(dir_ + rel)) { if (debug_) std::cerr << "[sound] music missing: " << rel << "\n"; continue; }
            m.setLooping(false);
            if (!paused) m.play();
            if (debug_) std::cerr << "[sound] music start " << rel << "\n";
            return true;
        }
        return false;
    }
    void nextCalm()  { calmAvail_ = openNext(calm_, calmList_, calmIdx_, false); applyMusicVolume(); }
    void nextChase() { openNext(chase_, chaseList_, chaseIdx_, false); applyMusicVolume(); }
    const sf::SoundBuffer* load(const std::string& f) {
        auto it = cache_.find(f);
        if (it != cache_.end()) return it->second.get();
        auto b = std::make_unique<sf::SoundBuffer>();
        if (!b->loadFromFile(f)) {
            if (debug_) std::cerr << "[sound] missing file " << f << "\n";
            b.reset();
        }
        const sf::SoundBuffer* r = b.get();
        cache_[f] = std::move(b);
        return r;
    }
    static const char* name(SfxId id) {
        static const char* n[] = {"Shoot","Hit","Teleport","Pickup","Deliver","Win","Lose","Join","Chat","Step",
                                  "Empty","UiClick","UiHover","UiBack","ChatPop","Whisper",
                                  "Alarm","Drop","Recover","Return","Leap","Land","Secret"};
        return n[(int)id];
    }

    bool inited_ = false, debug_ = false, sfxOk_ = false, musicOk_ = false, muted_ = false, stepAlt_ = false;
    float master_ = 0.7f, stepT_ = 0.f;
    std::string dir_;
    std::array<std::string, (size_t)SfxId::Count> files_{};
    std::map<std::string, std::unique_ptr<sf::SoundBuffer>> cache_;
    std::vector<std::unique_ptr<sf::Sound>> pool_;
    sf::Music calm_, chase_;
    std::vector<std::string> calmList_, chaseList_;
    size_t calmIdx_ = 0, chaseIdx_ = 0;
    bool calmAvail_ = false, forceChase_ = std::getenv("SOUND_FORCE_CHASE") != nullptr;
    float forceT_ = 0.f;
    float calmGain_ = 1.f, chaseGain_ = 0.f, chaseT_ = 0.f;
};
