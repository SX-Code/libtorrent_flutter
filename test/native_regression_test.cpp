// Native regression tests include the implementation to exercise cache ownership
// and eviction without exposing test-only entry points in the public C ABI.
#include "../src/torrent_bridge.cpp"
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/hasher.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void init_test_stream(StreamEngine& stream, int piece_length, int count) {
    stream.file_offset = 0;
    stream.file_size = int64_t(piece_length) * count;
    stream.piece_length = piece_length;
    stream.start_piece = 0;
    stream.end_piece = count - 1;
    stream.head_end_piece = 1;
    stream.tail_start_piece = count - 2;
    stream.cache = std::make_unique<TorrCache>();
    stream.cache->init(int64_t(piece_length) * 4, piece_length, count, {});
}

static void test_cache() {
    StreamEngine stream;
    constexpr int length = 512 * 1024;
    init_test_stream(stream, length, 96);
    std::vector<char> input(length, 'x');
    for (int p = 0; p < 96; ++p) {
        stream.read_head = int64_t(p) * length;
        input[0] = char(p);
        stream.on_piece_read(p, input.data(), length, true);
        require(stream.cached_pieces.size() <= 4, "hot cache grows past its budget");
        require(stream.read_results.size() <= 8, "read results grow without bound");
    }
    input[0] = 'z';
    auto result = read_piece_data(&stream, 95);
    require(result.ok && result.data[0] == char(95), "alert data must be owned after return");
    require(stream.read_results.count(95) == 0, "cache hit retains duplicate read result");
    stream.drop_cached(95);
    require(stream.cache->get_piece(95)->buffer.empty(), "seek eviction leaves a buffer");
    require(result.data[0] == char(95), "eviction invalidates the active reader");

    // Byte limit, not only entry count, must constrain large-piece results.
    StreamEngine large;
    init_test_stream(large, 16 * 1024 * 1024, 6);
    std::vector<char> big(16 * 1024 * 1024, 'b');
    for (int p = 0; p < 6; ++p) large.on_piece_read(p, big.data(), int(big.size()), true);
    require(large.read_results.size() == 2, "large-piece read results ignore byte budget");

    // Concurrent seek eviction and alert delivery must not leave untracked RAM.
    std::thread evict([&] { for (int i = 0; i < 100; ++i) stream.drop_cached(50); });
    for (int i = 0; i < 100; ++i) stream.on_piece_read(50, input.data(), length, true);
    evict.join();
    for (const auto& entry : stream.cache->pieces) {
        require(entry.second->buffer.empty() || stream.cached_pieces.count(entry.first),
                "cache buffer escaped eviction bookkeeping");
    }
}

static void test_window_and_range() {
    StreamEngine stream;
    init_test_stream(stream, 512 * 1024, 1024);
    stream.extend_window(20, 0);
    require(stream.window_lo == 21 && stream.priority_hi > 21, "window did not advance");
    stream.seek_generation = 1;
    stream.drop_window(500);
    stream.extend_window(30, 0);
    require(stream.window_gen == -1, "old HTTP worker reinstalls stale download window");
    stream.extend_window(500, 1);
    require(stream.window_gen == 1 && stream.window_lo == 501, "new seek window not installed");
    auto suffix = std::string("GET / HTTP/1.1\r\nRange: bytes=-500\r\n\r\n");
    auto range = parse_range(suffix.data(), int(suffix.size()));
    require(range.valid && range.is_suffix && range.end == 500, "suffix Range regression");
    lt_torrent_status empty{};
    lt::torrent_status status;
    status.state = lt::torrent_status::downloading_metadata;
    status.has_metadata = false;
    fill_status(empty, 1, status);
    require(empty.pieces_done == 0, "empty metadata status regression");
}

static std::string http_request(int port, const std::string& method, const std::string& range) {
    socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
    require(sock != SOCKET_INVALID, "socket creation failed");
#ifdef _WIN32
    DWORD timeout = 10000;
#else
    timeval timeout{10, 0};
#endif
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        CLOSESOCKET(sock);
        throw std::runtime_error("HTTP connect failed");
    }
    std::string request = method + " /stream/test HTTP/1.1\r\nHost: localhost\r\nRange: bytes=" + range + "\r\n\r\n";
    send_all(sock, request.data(), int(request.size()));
    std::string response;
    char bytes[4096];
    for (;;) {
        int n = recv(sock, bytes, sizeof(bytes), 0);
        if (n <= 0) break;
        response.append(bytes, size_t(n));
        auto header = response.find("\r\n\r\n");
        if (header == std::string::npos) continue;
        auto length = response.find("Content-Length: ");
        require(length != std::string::npos, "HTTP content length missing");
        size_t expected = method == "HEAD" ? 0 : std::stoull(response.substr(length + 16));
        if (response.size() >= header + 4 + expected) break;
    }
    CLOSESOCKET(sock);
    return response;
}

static void test_local_stream() {
    namespace fs = std::filesystem;
    auto folder = fs::temp_directory_path() / ("ltf-regression-" + std::to_string(chr::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(folder);
    lt_session_t session = nullptr;
    try {
        std::string content(8 * 1024 * 1024, '\0');
        for (size_t i = 0; i < content.size(); ++i) content[i] = char((i * 31 + i / 257) % 251);
        std::ofstream(folder / "movie.mp4", std::ios::binary).write(content.data(), content.size());
        lt::file_storage files;
        files.add_file("movie.mp4", int64_t(content.size()));
        lt::create_torrent torrent(files, 256 * 1024, lt::create_torrent::v1_only);
        for (int piece = 0; piece < 32; ++piece) {
            lt::hasher hash(content.data() + piece * 256 * 1024, 256 * 1024);
            torrent.set_hash(lt::piece_index_t(piece), hash.final());
        }
        std::vector<char> encoded;
        lt::bencode(std::back_inserter(encoded), torrent.generate());
        auto torrent_path = folder / "movie.torrent";
        std::ofstream(torrent_path, std::ios::binary).write(encoded.data(), encoded.size());
        session = lt_create_session("127.0.0.1:0", 0, 0);
        require(session, "native session failed");
        lt::settings_pack offline;
        offline.set_bool(lt::settings_pack::enable_dht, false);
        offline.set_bool(lt::settings_pack::enable_lsd, false);
        offline.set_bool(lt::settings_pack::enable_upnp, false);
        offline.set_bool(lt::settings_pack::enable_natpmp, false);
        to_sw(session)->session.apply_settings(offline);
        auto id = lt_add_torrent_file(session, torrent_path.string().c_str(), folder.string().c_str(), 0);
        require(id >= 0, "local torrent rejected");
        lt_file_info info{};
        require(lt_get_files(session, id, &info, 1) == 1 && info.size == int64_t(content.size()), "file size lost at FFI boundary");
        for (int i = 0; i < 200; ++i) {
            lt_torrent_status status{};
            lt_get_status(session, id, &status);
            if (status.pieces_done == 32) break;
            std::this_thread::sleep_for(chr::milliseconds(50));
        }
        auto checked = to_sw(session)->handles.at(id).status(lt::torrent_handle::query_pieces);
        if (checked.pieces.count() != 32) {
            std::lock_guard<std::mutex> lock(to_sw(session)->dart_queue_mu);
            for (const auto& alert : to_sw(session)->dart_queue) std::cerr << alert.message << '\n';
        }
        require(checked.pieces.count() == 32, ("fixture not verified: state=" + std::to_string(int(checked.state)) +
            " pieces=" + std::to_string(checked.pieces.count()) + " error=" + checked.errc.message()).c_str());
        auto sid = lt_start_stream(session, id, 0, 2 * 1024 * 1024);
        require(sid >= 0, "local stream did not start");
        auto* stream = to_sw(session)->streams.at(sid).get();
        for (const auto& spec : std::vector<std::pair<std::string, size_t>>{{"0-499", 0}, {"-500", content.size() - 500}, {"1048576-1049075", 1048576}, {"262000-262499", 262000}, {"1000-1499", 1000},
            {std::to_string(content.size() - 500) + "-", content.size() - 500}}) {
            auto reply = http_request(stream->server_port, "GET", spec.first);
            auto body = reply.find("\r\n\r\n");
            require(reply.find("HTTP/1.1 206") == 0, "Range request did not return 206");
            if (body == std::string::npos || reply.substr(body + 4) != content.substr(spec.second, 500)) {
                throw std::runtime_error("HTTP bytes differ for Range " + spec.first + ": received " +
                    std::to_string(body == std::string::npos ? 0 : reply.size() - body - 4) + " body bytes");
            }
        }
        lt_set_cache_settings(session, sid, 256 * 1024, 95, 25);
        require(stream->cached_pieces.size() <= 4, "live cache resizing leaves stale buffers");
        auto head = http_request(stream->server_port, "HEAD", "0-499");
        require(head.size() == head.find("\r\n\r\n") + 4, "HEAD returned a body");
        lt_stop_stream(session, sid);
        auto second = lt_start_stream(session, id, 0, 1024 * 1024);
        require(second >= 0, "stream cannot restart after stop");
        lt_stop_stream(session, second);
        lt_destroy_session(session);
        session = nullptr;
        fs::remove_all(folder);
    } catch (...) {
        if (session) lt_destroy_session(session);
        fs::remove_all(folder);
        throw;
    }
}

int main() {
    try {
        test_cache();
        test_window_and_range();
        test_local_stream();
        std::cout << "Native regression tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
