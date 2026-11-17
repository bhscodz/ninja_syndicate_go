// Client network layer: non-blocking connect on a worker thread, encrypted channel with
// trust-on-first-use server key pinning, background receive thread, ping replies, reconnect support.
#pragma once
#include <SFML/Network.hpp>
#include <SFML/System.hpp>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../game/Constants.hpp"
#include "../game/Protocol.hpp"
#include "../game/SecureChannel.hpp"

namespace netc {

// ---- known_hosts (trust on first use) ----
inline std::string& dataDir() { static std::string d; return d; }   // directory for the known_hosts file (with trailing slash)

inline std::string pinnedKey(const std::string& hostport) {
    std::ifstream f(dataDir() + "known_hosts");
    std::string line;
    while (std::getline(f, line)) {
        size_t sp = line.find(' ');
        if (sp != std::string::npos && line.substr(0, sp) == hostport) return line.substr(sp + 1);
    }
    return {};
}
inline void savePin(const std::string& hostport, const std::string& hex) {
    std::vector<std::string> keep;
    { std::ifstream f(dataDir() + "known_hosts"); std::string line;
      while (std::getline(f, line)) { size_t sp = line.find(' '); if (!line.empty() && (sp == std::string::npos || line.substr(0, sp) != hostport)) keep.push_back(line); } }
    std::ofstream o(dataDir() + "known_hosts", std::ios::trunc);
    for (auto& l : keep) o << l << "\n";
    o << hostport << " " << hex << "\n";
}

struct Link {
    sf::TcpSocket sock;
    sec::Channel chan;
};

struct ConnectParams {
    std::string host; unsigned short port = gc::SERVER_PORT;
    std::string name, code; JoinMode mode = JoinMode::Create; uint64_t token = 0;
    bool trustNewKey = false;
};

struct ConnectJob {
    ConnectParams p;
    std::shared_ptr<Link> link = std::make_shared<Link>();
    std::atomic<int> state{ 0 };         // 0 working, 1 ok, 2 failed
    std::atomic<bool> cancel{ false };
    std::string err;                     // valid once state == 2
    bool keyChanged = false;
    std::string newFp, oldFp;            // fingerprints for the warning screen

    static bool waitPacket(sf::TcpSocket& s, sf::Packet& pkt, float seconds, std::atomic<bool>& cancel) {
        sf::SocketSelector sel; sel.add(s);
        float waited = 0.f;
        while (waited < seconds && !cancel) {
            if (sel.wait(sf::milliseconds(100))) {
                auto st = s.receive(pkt);
                if (st == sf::Socket::Status::Done) return true;
                if (st != sf::Socket::Status::NotReady && st != sf::Socket::Status::Partial) return false;
            } else waited += 0.1f;
        }
        return false;
    }
    void fail(const std::string& e) { err = e; state = 2; }

    void run() {
        auto addr = sf::IpAddress::resolve(p.host);
        if (!addr) { fail("Unknown host '" + p.host + "'"); return; }
        sf::TcpSocket& s = link->sock;
        s.setBlocking(true);
        if (s.connect(*addr, p.port, sf::seconds(3)) != sf::Socket::Status::Done) { fail("Could not connect to " + p.host + ":" + std::to_string(p.port)); return; }
        if (cancel) { fail("Cancelled"); return; }
        s.setBlocking(false);

        // 1) key exchange
        sec::KeyPair e = sec::generateKeyPair();
        if (!e.ok) { fail("Crypto error"); return; }
        { sf::Packet hello; hello << sec::MAGIC << std::string((const char*)e.pub, 32);
          if (sec::sendPacket(s, hello) != sf::Socket::Status::Done) { fail("Connection lost during handshake"); return; } }
        sf::Packet reply;
        if (!waitPacket(s, reply, 4.f, cancel)) { fail(cancel ? "Cancelled" : "Server did not answer the handshake (is it a game server?)"); return; }
        uint32_t magic = 0; std::string qs, spub;
        reply >> magic >> qs >> spub;
        if (!reply || magic != sec::MAGIC || qs.size() != 32 || spub.size() != 32) { fail("Not a compatible server (bad handshake)"); return; }

        // 2) TOFU pin check
        const uint8_t* S = (const uint8_t*)spub.data();
        std::string hp = p.host + ":" + std::to_string(p.port);
        std::string hex = sec::toHex(S, 32), pinned = pinnedKey(hp);
        if (!pinned.empty() && pinned != hex && !p.trustNewKey) {
            uint8_t old[32] = {}; sec::fromHex(pinned, old, 32);
            keyChanged = true; newFp = sec::fingerprint(S); oldFp = sec::fingerprint(old);
            fail("SERVER IDENTITY CHANGED for " + hp);
            return;
        }
        uint8_t dh1[32], dh2[32];
        sec::SessionKeys k;
        if (!sec::dh(e.priv, (const uint8_t*)qs.data(), dh1) || !sec::dh(e.priv, S, dh2) ||
            !sec::deriveKeys(dh1, dh2, e.pub, (const uint8_t*)qs.data(), S, k)) { fail("Crypto error"); return; }
        link->chan.start(k.c2s, k.s2c);

        // 3) the server must prove it holds the pinned long-term key: first AEAD packet decrypts
        sf::Packet conf, plain;
        if (!waitPacket(s, conf, 4.f, cancel)) { fail(cancel ? "Cancelled" : "Server did not complete the handshake"); return; }
        uint32_t m = 0;
        if (!link->chan.open(conf, plain) || !(plain >> m) || static_cast<Msg>(m) != Msg::S2C_Secure) {
            fail("Server authentication failed (possible man-in-the-middle)"); return;
        }
        if (pinned != hex) savePin(hp, hex);

        // 4) Hello
        sf::Packet j; j << static_cast<uint32_t>(Msg::C2S_Join) << p.name << static_cast<uint8_t>(p.mode) << p.code << p.token;
        sf::Packet w;
        if (!link->chan.seal(j, w) || sec::sendPacket(s, w) != sf::Socket::Status::Done) { fail("Connection lost"); return; }
        state = 1;
    }
};

enum class ConnectResult { Idle, Working, Ok, Failed };

struct Net {
    std::mutex mtx;
    std::shared_ptr<Link> link;
    Snapshot snap;
    bool hasSnap = false;
    uint8_t myId = 255;
    std::string roomCode;
    uint64_t token = 0;
    LobbyInfo lobby;
    std::string errText;          // server rejected us / kicked / shutdown (no reconnect after this)
    bool matchStarted = false, toLobby = false, shutdown = false;
    int welcomes = 0;
    std::atomic<bool> running{ false };
    std::atomic<bool> connected{ false };
    std::thread thr;
    std::vector<GameEvent> evQueue;
    std::vector<ChatLine> chatQueue;

    // last connection parameters (for reconnecting)
    ConnectParams last;
    // result of the last finished connect job
    std::string failErr; bool failKeyChanged = false; std::string failNewFp, failOldFp;

    // ---- connect (never blocks the caller) ----
    void beginConnect(const ConnectParams& p) {
        cancelConnect();
        dropLink();
        last = p;
        job_ = std::make_shared<ConnectJob>();
        job_->p = p;
        auto job = job_;
        std::thread([job] { job->run(); }).detach();
    }
    void cancelConnect() { if (job_) { job_->cancel = true; job_.reset(); } }
    bool connecting() const { return job_ != nullptr; }

    // Call every frame. Returns Ok / Failed once when the running job finishes.
    ConnectResult pollConnect() {
        if (!job_) return ConnectResult::Idle;
        int st = job_->state;
        if (st == 0) return ConnectResult::Working;
        auto job = job_; job_.reset();
        if (st == 2) {
            failErr = job->err; failKeyChanged = job->keyChanged; failNewFp = job->newFp; failOldFp = job->oldFp;
            return ConnectResult::Failed;
        }
        link = job->link;
        link->sock.setBlocking(false);
        connected = true; running = true;
        lastRecv_.restart();
        thr = std::thread([this] { recvLoop(); });
        return ConnectResult::Ok;
    }

    // Close the socket/thread but keep room + match state (used while reconnecting).
    void dropLink() {
        running = false;
        if (thr.joinable()) thr.join();
        std::lock_guard<std::mutex> lk(mtx);
        if (link) { link->sock.disconnect(); link.reset(); }
        connected = false;
    }

    // Leave cleanly and forget everything.
    void disconnect() {
        cancelConnect();
        if (connected) sendSimple(Msg::C2S_Leave);
        dropLink();
        std::lock_guard<std::mutex> lk(mtx);
        snap = Snapshot{}; hasSnap = false; myId = 255; roomCode.clear(); lobby = LobbyInfo{}; token = 0;
        errText.clear(); matchStarted = false; toLobby = false; shutdown = false; welcomes = 0;
        evQueue.clear(); chatQueue.clear();
    }

    bool canReconnect() {
        std::lock_guard<std::mutex> lk(mtx);
        return errText.empty() && !shutdown && token != 0 && !roomCode.empty();
    }

    void sendRaw(sf::Packet& p) {   // mtx held by caller
        if (!connected || !link) return;
        sf::Packet w;
        if (!link->chan.seal(p, w)) { connected = false; return; }
        auto st = sec::sendPacket(link->sock, w);
        if (st == sf::Socket::Status::Disconnected || st == sf::Socket::Status::Error) connected = false;
    }
    void sendInput(const ClientInput& in) {
        if (!connected) return;
        std::lock_guard<std::mutex> lk(mtx);
        sf::Packet p;
        p << static_cast<uint32_t>(Msg::C2S_Input) << in.dirX << in.dirY << in.facing << in.shoot;
        sendRaw(p);
    }
    void sendChat(const std::string& text) {
        if (!connected) return;
        std::lock_guard<std::mutex> lk(mtx);
        sf::Packet p; p << static_cast<uint32_t>(Msg::C2S_Chat) << text;
        sendRaw(p);
    }
    void sendSimple(Msg m) {
        if (!connected) return;
        std::lock_guard<std::mutex> lk(mtx);
        sf::Packet p; p << static_cast<uint32_t>(m);
        sendRaw(p);
    }
    void sendSettings(const RoomSettings& rs) {
        if (!connected) return;
        std::lock_guard<std::mutex> lk(mtx);
        sf::Packet p; p << static_cast<uint32_t>(Msg::C2S_Settings) << rs;
        sendRaw(p);
    }
    void sendKick(const std::string& who) {
        if (!connected) return;
        std::lock_guard<std::mutex> lk(mtx);
        sf::Packet p; p << static_cast<uint32_t>(Msg::C2S_Kick) << who;
        sendRaw(p);
    }

    Snapshot copySnap() { std::lock_guard<std::mutex> lk(mtx); return snap; }
    uint8_t copyId() { std::lock_guard<std::mutex> lk(mtx); return myId; }
    bool hasSnapshot() { std::lock_guard<std::mutex> lk(mtx); return hasSnap; }
    int welcomeCount() { std::lock_guard<std::mutex> lk(mtx); return welcomes; }
    void roomState(std::string& code, LobbyInfo& li, std::string& err, bool& started) {
        std::lock_guard<std::mutex> lk(mtx);
        code = roomCode; li = lobby; err = errText; started = matchStarted;
    }
    // true once after the server sent the room back to the lobby
    bool takeToLobby() { std::lock_guard<std::mutex> lk(mtx); bool t = toLobby; toLobby = false; return t; }
    void clearMatchStarted() { std::lock_guard<std::mutex> lk(mtx); matchStarted = false; hasSnap = false; }
    int myPing() {
        std::lock_guard<std::mutex> lk(mtx);
        if (myId >= gc::NUM_PLAYERS) return 0;
        return hasSnap ? snap.players[myId].ping : lobby.slots[myId].ping;
    }

    std::vector<GameEvent> drainEvents() {
        std::lock_guard<std::mutex> lk(mtx);
        std::vector<GameEvent> out; out.swap(evQueue); return out;
    }
    std::vector<ChatLine> drainChat() {
        std::lock_guard<std::mutex> lk(mtx);
        std::vector<ChatLine> out; out.swap(chatQueue); return out;
    }

    ~Net() { cancelConnect(); dropLink(); }

private:
    std::shared_ptr<ConnectJob> job_;
    sf::Clock lastRecv_;

    void recvLoop() {
        while (running) {
            bool got = false;
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (!link) return;
                for (int i = 0; i < 32; ++i) {      // drain a burst per wake-up
                    sf::Packet pkt;
                    auto st = link->sock.receive(pkt);
                    if (st == sf::Socket::Status::Done) {
                        got = true; lastRecv_.restart();
                        sf::Packet plain;
                        if (!link->chan.open(pkt, plain)) { connected = false; return; }   // tampered / replayed
                        handle(plain);
                    } else if (st == sf::Socket::Status::Disconnected || st == sf::Socket::Status::Error) { connected = false; return; }
                    else break;
                }
                if (lastRecv_.getElapsedTime().asSeconds() > 9.f) { connected = false; return; }   // heartbeat timeout
            }
            if (!got) sf::sleep(sf::milliseconds(4));
        }
    }

    void handle(sf::Packet& pkt) {
        uint32_t m; pkt >> m;
        switch (static_cast<Msg>(m)) {
            case Msg::S2C_Welcome: pkt >> myId >> roomCode >> token; ++welcomes; break;
            case Msg::S2C_Lobby: pkt >> lobby; break;
            case Msg::S2C_GameStart: matchStarted = true; break;
            case Msg::S2C_ToLobby: toLobby = true; matchStarted = false; break;
            case Msg::S2C_Error: { uint8_t c; std::string t; pkt >> c >> t; errText = t.empty() ? "Server error" : t; break; }
            case Msg::S2C_Disconnect: shutdown = true; errText = "The server was shut down"; break;
            case Msg::S2C_Ping: { uint32_t ts = 0; pkt >> ts; sf::Packet p; p << static_cast<uint32_t>(Msg::C2S_Pong) << ts; sendRaw(p); break; }
            case Msg::S2C_Snapshot: pkt >> snap; hasSnap = true; break;
            case Msg::S2C_Chat: { ChatLine c; pkt >> c; chatQueue.push_back(c); if (chatQueue.size() > 200) chatQueue.erase(chatQueue.begin()); break; }
            case Msg::S2C_Event: { GameEvent e; pkt >> e; evQueue.push_back(e); break; }
            default: break;
        }
    }
};

} // namespace netc
