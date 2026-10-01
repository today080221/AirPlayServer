#include <winsock2.h>
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <stdint.h>
// This vendored Bonjour header predates MSVC's stdint.h.
#define _MSL_STDINT_H
#include <dns_sd.h>
#undef _MSL_STDINT_H
#include "Airplay2Head.h"
extern "C" {
#include "netutils.h"
#include "http_request.h"
#include "fairplay.h"
#include "logger.h"
#include "plist/include/plist.h"
}
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>

static std::map<std::wstring, unsigned> traceHandleTypes() {
    struct Entry { HANDLE handle; ULONG_PTR refs, pointers; ULONG access, type, attributes, reserved; };
    struct Snapshot { ULONG_PTR count, reserved; Entry entries[1]; };
    struct UnicodeString { USHORT length, maximum; PWSTR buffer; };
    using QueryProcess = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    using QueryObject = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    using QueryThread = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    auto ntdll = GetModuleHandleA("ntdll.dll");
    auto processQuery = reinterpret_cast<QueryProcess>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
    auto objectQuery = reinterpret_cast<QueryObject>(GetProcAddress(ntdll, "NtQueryObject"));
    auto threadQuery = reinterpret_cast<QueryThread>(GetProcAddress(ntdll, "NtQueryInformationThread"));
    std::vector<unsigned char> bytes(65536);
    ULONG size = 0;
    if (!processQuery || !objectQuery || processQuery(GetCurrentProcess(), 51, bytes.data(), static_cast<ULONG>(bytes.size()), &size) < 0) return {};
    const auto snapshot = reinterpret_cast<Snapshot*>(bytes.data());
    std::map<std::wstring, unsigned> counts;
    for (ULONG_PTR i = 0; i < snapshot->count; ++i) {
        unsigned char type[2048] = {};
        if (objectQuery(snapshot->entries[i].handle, 2, type, sizeof(type), &size) < 0) continue;
        const auto name = reinterpret_cast<UnicodeString*>(type);
        const auto typeName = std::wstring(name->buffer, name->length / sizeof(wchar_t));
        ++counts[typeName];
        if (typeName == L"Thread" && threadQuery) {
            void* start = nullptr; HMODULE module = nullptr; DWORD exitCode = 0;
            const auto thread = snapshot->entries[i].handle;
            threadQuery(thread, 9, &start, sizeof(start), nullptr);
            GetExitCodeThread(thread, &exitCode);
            char path[MAX_PATH] = {};
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(start), &module)) GetModuleFileNameA(module, path, MAX_PATH);
            const auto base = std::strrchr(path, '\\');
            if (base && _stricmp(base + 1, "airplay2dll.dll") == 0)
                throw std::runtime_error("Stopped receiver still owns a network worker thread handle");
            std::printf("  thread id=%lu exit=%lu start=%s+%llu\n", GetThreadId(thread), exitCode,
                base ? base + 1 : path, reinterpret_cast<uintptr_t>(start) - reinterpret_cast<uintptr_t>(module));
        }
    }
    for (const auto& count : counts) std::printf(" %ls=%u", count.first.c_str(), count.second);
    std::puts("");
    return counts;
}

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(std::string(#expr) + " line=" + std::to_string(__LINE__) + " WSA=" + std::to_string(WSAGetLastError())); } while (0)

struct Socket {
    SOCKET fd = INVALID_SOCKET;
    Socket() = default;
    explicit Socket(SOCKET value) : fd(value) {}
    Socket(const Socket&) = delete;
    Socket(Socket&& other) noexcept : fd(other.fd) { other.fd = INVALID_SOCKET; }
    ~Socket() { if (fd != INVALID_SOCKET) closesocket(fd); }
};

static unsigned short preferredRaop = 5001, preferredAirplay = 7001;

static sockaddr_in address(unsigned short port, bool wildcard = false) {
    sockaddr_in result = {};
    result.sin_family = AF_INET;
    result.sin_addr.s_addr = htonl(wildcard ? INADDR_ANY : INADDR_LOOPBACK);
    result.sin_port = htons(port);
    return result;
}
static unsigned short portOf(SOCKET fd) {
    sockaddr_in a = {}; int len = sizeof(a);
    CHECK(getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0);
    return ntohs(a.sin_port);
}
static Socket bindSocket(unsigned short port, bool wildcard, bool reuse = false) {
    Socket s(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    CHECK(s.fd != INVALID_SOCKET);
    const int yes = 1;
    CHECK(setsockopt(s.fd, SOL_SOCKET, reuse ? SO_REUSEADDR : SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&yes), sizeof(yes)) == 0);
    auto a = address(port, wildcard);
    CHECK(bind(s.fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    return s;
}
static Socket connectTo(unsigned short port) {
    Socket s(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    CHECK(s.fd != INVALID_SOCKET);
    DWORD timeout = 3000;
    CHECK(setsockopt(s.fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0);
    auto a = address(port);
    CHECK(connect(s.fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    return s;
}
static void checkClosed(unsigned short port) {
    Socket s(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    auto a = address(port);
    CHECK(connect(s.fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAECONNREFUSED);
}

enum class Occupancy { None, Bound, Listen, Outbound, ReuseListen };
struct Occupant {
    std::unique_ptr<Socket> occupied, listener, peer;
    Occupant(unsigned short port, Occupancy kind) {
        if (kind == Occupancy::None) return;
        occupied = std::make_unique<Socket>(bindSocket(port, kind != Occupancy::Outbound, kind == Occupancy::ReuseListen));
        if (kind == Occupancy::Listen || kind == Occupancy::ReuseListen) CHECK(listen(occupied->fd, 1) == 0);
        if (kind == Occupancy::Outbound) {
            listener = std::make_unique<Socket>(bindSocket(0, false));
            CHECK(listen(listener->fd, 1) == 0);
            auto a = address(portOf(listener->fd));
            CHECK(connect(occupied->fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
            peer = std::make_unique<Socket>(accept(listener->fd, nullptr, nullptr));
            CHECK(peer->fd != INVALID_SOCKET);
            CHECK(portOf(occupied->fd) == port);
            int accepting = -1; int length = sizeof(accepting);
            CHECK(getsockopt(occupied->fd, SOL_SOCKET, SO_ACCEPTCONN, reinterpret_cast<char*>(&accepting), &length) == 0);
            CHECK(accepting == 0); // This is the reported outbound case, not a listener.
        }
    }
    void checkIntact() {
        if (!peer) return;
        char data = 0;
        CHECK(send(occupied->fd, "X", 1, 0) == 1);
        CHECK(recv(peer->fd, &data, 1, 0) == 1 && data == 'X');
        CHECK(send(peer->fd, "Y", 1, 0) == 1);
        CHECK(recv(occupied->fd, &data, 1, 0) == 1 && data == 'Y');
    }
};

struct Callback : IAirServerCallback {
    std::atomic<bool> stopped{false};
    std::atomic<unsigned> calls{0};
    void connected(const char*, const char*) override {}
    void disconnected(const char*, const char*) override {}
    void outputAudio(SFgAudioFrame*, const char*, const char*) override {}
    void outputH264AccessUnit(SFgH264AccessUnit*, const char*, const char*) override {}
    void outputVideo(SFgVideoFrame*, const char*, const char*) override {}
    void videoGeometryChanged(float, float, float, float, const char*, const char*) override {}
    void videoSenderPausedChanged(bool, const char*, const char*) override {}
    void videoPlay(char*, double, double) override {}
    void videoGetPlayInfo(double*, double*, double*) override {}
    void setVolume(float, const char*, const char*) override {}
    bool approvePairingRequest(const char*, const char*, const char*, const char*, const char*, const char*, const char*, const char*) override { return true; }
    bool requestPinApproval(const char*, const char*) override { return false; }
    void log(int level, const char* text) override {
        if (stopped.load()) std::abort();
        ++calls;
        if (level <= 3 || strstr(text, "TCP listening:")) std::printf("  %s\n", text);
    }
};
struct Server {
    void* handle = nullptr;
    ~Server() { if (handle) fgServerStop(handle); }
    void stop() { fgServerStop(handle); handle = nullptr; }
};

struct Resolved { unsigned short port = 0; DNSServiceErrorType error = 0; bool done = false; };
static void DNSSD_API resolved(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType error,
    const char*, const char*, uint16_t port, uint16_t, const unsigned char*, void* context) {
    auto& result = *static_cast<Resolved*>(context);
    result = {ntohs(port), error, true};
}
static void checkDiscovery(const std::string& name, const char* type, unsigned short expected) {
    Resolved result;
    DNSServiceRef service = nullptr;
    CHECK(DNSServiceResolve(&service, 0, 0, name.c_str(), type, "local.", resolved, &result) == 0);
    const auto deadline = GetTickCount64() + 15000;
    while (!result.done && GetTickCount64() < deadline) {
        fd_set fds; FD_ZERO(&fds); FD_SET(DNSServiceRefSockFD(service), &fds);
        timeval timeout = {0, 100000};
        if (select(0, &fds, nullptr, nullptr, &timeout) > 0) {
            if (DNSServiceProcessResult(service) != 0) break;
        }
    }
    DNSServiceRefDeallocate(service);
    CHECK(result.done && result.error == 0 && result.port == expected);
}

static std::string request(SOCKET fd, const std::string& method, const std::string& path, const std::string& body = "") {
    std::string message = method + " " + path + " RTSP/1.0\r\nCSeq: 1\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < message.size()) {
        int count = send(fd, message.data() + sent, static_cast<int>(message.size() - sent), 0);
        CHECK(count > 0); sent += count;
    }
    std::string response;
    for (;;) {
        char bytes[4096]; int count = recv(fd, bytes, sizeof(bytes), 0);
        CHECK(count > 0); response.append(bytes, count);
        CHECK(response.size() < 1024 * 1024);
        auto end = response.find("\r\n\r\n");
        if (end == std::string::npos) continue;
        auto length = response.find("Content-Length:");
        size_t size = length < end ? std::stoul(response.substr(length + 15)) : 0;
        if (response.size() >= end + 4 + size) return response;
    }
}
static std::string plistBytes(plist_t root) {
    char* bytes = nullptr; uint32_t length = 0;
    plist_to_bin(root, &bytes, &length);
    CHECK(bytes && length);
    std::string result(bytes, length); free(bytes); plist_free(root);
    return result;
}
static void checkProtocol(unsigned short port) {
    auto client = connectTo(port);
    CHECK(request(client.fd, "GET", "/info").find("200 OK") != std::string::npos);
    // Synthetic interoperability handshake; this checks port plumbing, not
    // iPhone authentication, encrypted video correctness, or playback.
    std::string handshake(164, '\0'); handshake[4] = 3;
    request(client.fd, "POST", "/fp-setup", handshake);
    auto setup = plist_new_dict();
    std::string iv(16, '\0'), key(72, '\0');
    plist_dict_set_item(setup, "eiv", plist_new_data(iv.data(), iv.size()));
    plist_dict_set_item(setup, "ekey", plist_new_data(key.data(), key.size()));
    plist_dict_set_item(setup, "timingPort", plist_new_uint(9));
    plist_dict_set_item(setup, "name", plist_new_string("Regression sender"));
    plist_dict_set_item(setup, "deviceID", plist_new_string("02:00:00:00:00:01"));
    request(client.fd, "SETUP", "/stream", plistBytes(setup));
    auto root = plist_new_dict(), streams = plist_new_array(), stream = plist_new_dict();
    plist_dict_set_item(stream, "type", plist_new_uint(110));
    plist_dict_set_item(stream, "streamConnectionID", plist_new_uint(1));
    plist_array_append_item(streams, stream); plist_dict_set_item(root, "streams", streams);
    const auto response = request(client.fd, "SETUP", "/stream", plistBytes(root));
    const auto start = response.find("\r\n\r\n") + 4;
    plist_t reply = nullptr;
    plist_from_bin(response.data() + start, static_cast<uint32_t>(response.size() - start), &reply);
    CHECK(reply != nullptr);
    uint64_t event = 0, data = 0;
    plist_get_uint_val(plist_dict_get_item(reply, "eventPort"), &event);
    auto streamReply = plist_array_get_item(plist_dict_get_item(reply, "streams"), 0);
    plist_get_uint_val(plist_dict_get_item(streamReply, "dataPort"), &data);
    plist_free(reply);
    CHECK(event == port && data > 0 && data <= 65535);
    auto mirror = connectTo(static_cast<unsigned short>(data));
}

static void scenario(int index, Occupancy raop, Occupancy airplay) {
    Occupant r(preferredRaop, raop), a(preferredAirplay, airplay);
    Callback callback; Server server;
    const auto name = "AirPlay-port-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(index);
    server.handle = fgServerStart(name.c_str(), preferredRaop, preferredAirplay, &callback, nullptr);
    CHECK(server.handle != nullptr && fgServerIsRunning(server.handle));
    unsigned short rp = 0, ap = 0;
    CHECK(fgServerGetPorts(server.handle, &rp, &ap));
    CHECK(rp && ap && rp != ap);
    CHECK((rp == preferredRaop) == (raop == Occupancy::None));
    CHECK((ap == preferredAirplay) == (airplay == Occupancy::None));
    checkDiscovery(name, "_airplay._tcp", ap);
    checkDiscovery("0210ABCDEF09@" + name, "_raop._tcp", rp);
    auto airClient = connectTo(ap);
    CHECK(request(airClient.fd, "GET", "/server-info").find("200 OK") != std::string::npos);
    checkProtocol(rp);
    r.checkIntact(); a.checkIntact();
    // Let the client initiate FIN so controlled fixed-port occupiers in the
    // next case do not inherit the previous server's TIME_WAIT ownership.
    CHECK(shutdown(airClient.fd, SD_SEND) == 0);
    char eof; CHECK(recv(airClient.fd, &eof, 1, 0) == 0);
    const auto start = GetTickCount64();
    server.stop();
    CHECK(GetTickCount64() - start < 8000);
    callback.stopped = true;
    auto count = callback.calls.load(); Sleep(25); CHECK(callback.calls == count);
    checkClosed(rp); checkClosed(ap); checkClosed(7100);
    r.checkIntact(); a.checkIntact();
    std::printf("PASS scenario=%d RAOP=%u AirPlay=%u discovery/protocol/stop/occupants\n", index, rp, ap);
}

static void checkFailureAndRestart() {
    Callback cb;
    std::map<std::wstring, unsigned> failureResources;
    auto traceHandles = [&](const char* phase) {
        DWORD count = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &count));
        std::printf("  handles %s=%lu\n", phase, count);
        auto resources = traceHandleTypes();
        CHECK(!resources.empty());
        if (failureResources.empty()) failureResources = resources;
        else {
            CHECK(resources[L"File"] <= failureResources[L"File"]);
            CHECK(resources[L"Mutant"] <= failureResources[L"Mutant"]);
        }
    };
    traceHandles("before failure cases");
    {
        Occupant conflict(7100, Occupancy::Bound);
        CHECK(fgServerStart("AirPlay-failure-test", preferredRaop, preferredAirplay, &cb, nullptr) == nullptr);
        checkClosed(preferredRaop); checkClosed(preferredAirplay);
    }
    traceHandles("after fixed-port failure");
    // Both listeners start, then registration rejects this oversized name.
    std::string longName(300, 'x');
    CHECK(fgServerStart(longName.c_str(), preferredRaop, preferredAirplay, &cb, nullptr) == nullptr);
    checkClosed(preferredRaop); checkClosed(preferredAirplay); checkClosed(7100);
    traceHandles("after registration failure");
    CHECK(fgServerStart("invalid", 65536, 7001, &cb, nullptr) == nullptr);
    traceHandles("after invalid arguments");
    {
        Occupant conflict(preferredRaop, Occupancy::Bound);
        SetEnvironmentVariableA("MIRRORSIM_EXTERNAL_DNSSD", "1");
        auto result = fgServerStart("external-fixed-ports", preferredRaop, preferredAirplay, &cb, nullptr);
        SetEnvironmentVariableA("MIRRORSIM_EXTERNAL_DNSSD", nullptr);
        CHECK(result == nullptr);
        checkClosed(preferredAirplay); checkClosed(7100);
    }
    traceHandles("after external-discovery conflict");
    std::map<std::wstring, unsigned> restartResources;
    for (int i = 0; i != 4; ++i) {
        Server server;
        server.handle = fgServerStart("AirPlay-restart-test", preferredRaop, preferredAirplay, &cb, nullptr);
        CHECK(server.handle != nullptr);
        unsigned short rp, ap; CHECK(fgServerGetPorts(server.handle, &rp, &ap));
        auto client = connectTo(rp);
        request(client.fd, "GET", "/info");
        // Stop with an accepted client still alive: callback join and socket
        // release must work even when the following start needs a new port.
        server.stop(); checkClosed(rp); checkClosed(ap); checkClosed(7100);
        DWORD handles = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles));
        std::printf("  handles after restart %d (one test client alive)=%lu\n", i, handles);
        auto resources = traceHandleTypes();
        CHECK(!resources.empty());
        // Compare identical warmed workloads, with the same test client alive.
        // Whole-suite totals mix different lazy initialization/cache lifetimes.
        if (i == 0) restartResources = resources;
        else {
            CHECK(resources[L"File"] <= restartResources[L"File"]);
            CHECK(resources[L"Mutant"] <= restartResources[L"Mutant"]);
        }
    }
    std::puts("PASS startup rollback, invalid arguments, external discovery fail-closed, repeated restart");
}

static void checkInputBounds() {
    auto parser = http_request_init(); CHECK(parser);
    std::string header = "POST /x HTTP/1.1\r\nContent-Length: 8388609\r\n\r\n";
    http_request_add_data(parser, header.data(), static_cast<int>(header.size()));
    std::string chunk(65536, 'x');
    for (int i = 0; i < 128; ++i) http_request_add_data(parser, chunk.data(), static_cast<int>(chunk.size()));
    CHECK(!http_request_has_error(parser));
    http_request_add_data(parser, "x", 1); CHECK(http_request_has_error(parser));
    http_request_destroy(parser);
    auto logger = logger_init(); auto fp = fairplay_init(logger); CHECK(fp);
    unsigned char req[16] = {}, res[142] = {}; req[4] = 3;
    req[14] = 255; CHECK(fairplay_setup(fp, req, res) == -1);
    req[14] = 0; CHECK(fairplay_setup(fp, req, res) == 0);
    fairplay_destroy(fp); logger_destroy(logger);
    Callback cb; Server server;
    server.handle = fgServerStart("AirPlay-input-test", preferredRaop, preferredAirplay, &cb, nullptr); CHECK(server.handle);
    unsigned short rp, ap; CHECK(fgServerGetPorts(server.handle, &rp, &ap));
    for (int size : {0, 1, 15, 17}) {
        auto client = connectTo(rp);
        auto setup = plist_new_dict(); std::string bytes(size, 'x');
        plist_dict_set_item(setup, "eiv", plist_new_data(bytes.data(), bytes.size()));
        request(client.fd, "SETUP", "/stream", plistBytes(setup));
        char byte; CHECK(recv(client.fd, &byte, 1, 0) == 0);
    }
    CHECK(fgServerIsRunning(server.handle));
    std::puts("PASS HTTP body cap, FairPlay mode bounds, malformed SETUP isolation");
}

static void checkLiveDefaults() {
    Callback cb; Server server;
    auto name = "AirPlay-live-" + std::to_string(GetCurrentProcessId());
    server.handle = fgServerStart(name.c_str(), 5001, 7001, &cb, nullptr);
    CHECK(server.handle);
    unsigned short rp, ap; CHECK(fgServerGetPorts(server.handle, &rp, &ap));
    checkDiscovery(name, "_airplay._tcp", ap);
    checkDiscovery("0210ABCDEF09@" + name, "_raop._tcp", rp);
    checkProtocol(rp);
    {
        auto client = connectTo(ap);
        CHECK(request(client.fd, "GET", "/server-info").find("200 OK") != std::string::npos);
    }
    server.stop(); checkClosed(rp); checkClosed(ap);
    std::printf("PASS live defaults requested=5001/7001 actual=%u/%u DNS-SD/protocol/shutdown\n", rp, ap);
}

static LONG WINAPI crashDiagnostic(EXCEPTION_POINTERS* exception) {
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(exception->ExceptionRecord->ExceptionAddress), &displacement, symbol);
    std::fprintf(stderr, "Native exception 0x%lx in %s+0x%llx\n", exception->ExceptionRecord->ExceptionCode, symbol->Name, displacement);
    CONTEXT context = *exception->ContextRecord;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 16; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &context,
            nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        if (SymFromAddr(GetCurrentProcess(), frame.AddrPC.Offset, &displacement, symbol))
            std::fprintf(stderr, "  %s+0x%llx\n", symbol->Name, displacement);
        IMAGEHLP_LINE64 line = {}; line.SizeOfStruct = sizeof(line); DWORD delta = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), frame.AddrPC.Offset, &delta, &line))
            std::fprintf(stderr, "    %s:%lu\n", line.FileName, line.LineNumber);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(crashDiagnostic);
    try {
        // Before Bonjour's process-wide Winsock initialization, a socket()
        // failure must propagate unchanged and must never trigger fallback.
        unsigned short invalidPort = 5001; int fallback = -1;
        CHECK(netutils_init_socket_fallback(&invalidPort, 0, 0, 1, &fallback) == -1);
        CHECK(WSAGetLastError() == WSANOTINITIALISED && invalidPort == 5001 && fallback == 0);
        if (argc >= 3) {
            const auto rp = std::stoul(argv[1]), ap = std::stoul(argv[2]);
            CHECK(rp > 0 && rp <= 65535 && ap > 0 && ap <= 65535 && rp != ap && rp != 7100 && ap != 7100);
            preferredRaop = static_cast<unsigned short>(rp);
            preferredAirplay = static_cast<unsigned short>(ap);
        }
        WSADATA data; CHECK(WSAStartup(MAKEWORD(2,2), &data) == 0);
        SetEnvironmentVariableA("MIRRORSIM_EXTERNAL_DNSSD", nullptr);
        SetEnvironmentVariableA("MIRRORSIM_HARDWARE_ADDRESS", "0210ABCDEF09");
        if (argc == 4 && std::strcmp(argv[3], "lifetime") == 0) {
            checkFailureAndRestart();
            checkFailureAndRestart();
            WSACleanup();
            return 0;
        }
        scenario(0, Occupancy::None, Occupancy::None);
        DWORD before = 0, after = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
        scenario(1, Occupancy::Listen, Occupancy::None);
        scenario(2, Occupancy::None, Occupancy::Listen);
        scenario(3, Occupancy::Listen, Occupancy::Listen);
        scenario(4, Occupancy::Bound, Occupancy::Bound);
        scenario(5, Occupancy::Outbound, Occupancy::None);
        scenario(6, Occupancy::None, Occupancy::Outbound);
        scenario(7, Occupancy::Outbound, Occupancy::Outbound);
        scenario(8, Occupancy::ReuseListen, Occupancy::ReuseListen);
        checkFailureAndRestart();
        checkInputBounds();
        if (preferredRaop != 5001 || preferredAirplay != 7001) checkLiveDefaults();
        CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
        std::printf("  total handles before=%lu after=%lu\n", before, after);
        CHECK(WSACleanup() == 0);
        std::printf("INFO total process handles %lu -> %lu (different workloads; diagnostic only)\n", before, after);
        std::puts("PASS socket/mutex handle stability and receiver worker cleanup; non-bind errors do not fall back");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what()); return 1;
    }
}
