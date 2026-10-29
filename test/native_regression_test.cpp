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
    auto suffix = std::string("GET / HTTP/1.1\r\nRange: bytes=-500\r\n\r\n");
    auto range = parse_range(suffix.data(), int(suffix.size()));
    require(range.valid && range.is_suffix && range.end == 500, "suffix Range regression");
    lt_torrent_status empty{};
    lt::torrent_status status;
    status.state = lt::torrent_status::downloading_metadata;
    status.has_metadata = false;
    fill_status(empty, 1, status);
    require(empty.pieces_done == 0, "empty metadata status regression");
    require(empty.state == LT_STATE_DOWNLOADING_META, "metadata state mapped to downloading");
    const std::pair<lt::torrent_status::state_t, int> states[] = {
        {lt::torrent_status::checking_files, LT_STATE_CHECKING_FILES},
        {lt::torrent_status::downloading_metadata, LT_STATE_DOWNLOADING_META},
        {lt::torrent_status::downloading, LT_STATE_DOWNLOADING},
        {lt::torrent_status::finished, LT_STATE_FINISHED},
        {lt::torrent_status::seeding, LT_STATE_SEEDING},
        {lt::torrent_status::checking_resume_data, LT_STATE_CHECKING_RESUME},
    };
    status.progress = 1.0f;
    for (const auto& state : states) {
        status.state = state.first;
        fill_status(empty, 1, status);
        require(empty.state == state.second, "native state does not match the public C/Dart enum");
    }
    status.state = lt::torrent_status::finished;
    status.progress = 0.5f;
    fill_status(empty, 1, status);
    require(empty.state == LT_STATE_DOWNLOADING, "incomplete demand window reported finished");
    status.errc = lt::error_code(boost::system::errc::io_error, boost::system::generic_category());
    fill_status(empty, 1, status);
    require(empty.state == LT_STATE_ERROR, "storage error did not override torrent state");
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

static void wait_until(const std::function<bool()>& ready, const char* message) {
    for (int i = 0; i < 400; ++i) {
        if (ready()) return;
        std::this_thread::sleep_for(chr::milliseconds(25));
    }
    require(false, message);
}

// Keep an HTTP request open while driving piece arrival and other connections.
class PendingHttp {
public:
    socket_t socket = SOCKET_INVALID;
    std::string headers;

    PendingHttp(int port, const std::string& range) {
        socket = ::socket(AF_INET, SOCK_STREAM, 0);
        require(socket != SOCKET_INVALID, "pending socket creation failed");
        try {
#ifdef _WIN32
            DWORD timeout = 5000;
#else
            timeval timeout{5, 0};
#endif
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons((uint16_t)port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            require(::connect(socket, (sockaddr*)&address, sizeof(address)) == 0, "pending connect failed");
            const auto request = "GET /stream/test HTTP/1.1\r\nHost: localhost\r\nRange: bytes=" + range + "\r\n\r\n";
            require(send_all(socket, request.data(), int(request.size())) > 0, "pending send failed");
            while (headers.find("\r\n\r\n") == std::string::npos) {
                char byte;
                require(recv(socket, &byte, 1, 0) == 1, "pending response header missing");
                headers += byte;
            }
            require(headers.find("HTTP/1.1 206") == 0, "pending Range did not return 206");
        } catch (...) {
            close();
            throw;
        }
    }
    PendingHttp(const PendingHttp&) = delete;
    PendingHttp& operator=(const PendingHttp&) = delete;
    ~PendingHttp() { close(); }
    void close() {
        if (socket != SOCKET_INVALID) CLOSESOCKET(socket);
        socket = SOCKET_INVALID;
    }
    bool readable() const {
        fd_set sockets;
        FD_ZERO(&sockets);
        FD_SET(socket, &sockets);
        timeval timeout{0, 0};
        return select(int(socket) + 1, &sockets, nullptr, nullptr, &timeout) > 0;
    }
    std::string body() {
        std::string result;
        char bytes[4096];
        for (;;) {
            int count = recv(socket, bytes, sizeof(bytes), 0);
            require(count >= 0, "pending body timed out");
            if (!count) return result;
            result.append(bytes, size_t(count));
        }
    }
};

static void test_http_probes(lt_session_t session, const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    constexpr int piece_length = 262144;
    constexpr int64_t file_size = 317889956;
    constexpr int64_t probe_offset = 85783644;
    constexpr int probe_piece = int(probe_offset / piece_length);
    const std::string first_piece(piece_length, '\0');
    const std::string later_piece(piece_length, 'p');
    lt::file_storage files;
    files.add_file("probe.mp4", file_size);
    lt::create_torrent torrent(files, piece_length, lt::create_torrent::v1_only);
    const int piece_count = int((file_size + piece_length - 1) / piece_length);
    for (int piece = 0; piece < piece_count; ++piece) {
        int length = int(std::min<int64_t>(piece_length, file_size - int64_t(piece) * piece_length));
        lt::hasher hash(piece == 0 ? first_piece.data() : later_piece.data(), length);
        torrent.set_hash(lt::piece_index_t(piece), hash.final());
    }
    std::vector<char> encoded;
    lt::bencode(std::back_inserter(encoded), torrent.generate());
    auto torrent_path = folder / "probe.torrent";
    std::ofstream(torrent_path, std::ios::binary).write(encoded.data(), encoded.size());
    auto* sw = to_sw(session);
    auto id = lt_add_torrent_file(session, torrent_path.string().c_str(), folder.string().c_str(), 1);
    require(id >= 0, "probe fixture rejected");
    auto sid = lt_start_stream(session, id, 0, 1024 * 1024);
    require(sid >= 0, "probe stream failed");
    auto* stream = sw->streams.at(sid).get();
    auto handle = stream->handle;
    auto scheduler = stream->scheduler;
    handle.add_piece(lt::piece_index_t(0), first_piece.data());
    wait_until([&] { return handle.have_piece(lt::piece_index_t(0)); }, "probe header piece missing");
    auto head_reply = http_request(stream->server_port, "GET", "0-262143");
    require(head_reply.substr(head_reply.find("\r\n\r\n") + 4) == first_piece, "header probe failed");
    wait_until([&] {
        std::lock_guard<std::mutex> lock(stream->http_mu);
        return stream->http_readers.empty();
    }, "header reader did not close");
    const auto original_state = stream->stream_state.load();
    const auto original_position = stream->read_head.load();
    require(original_position == 262144 && original_state == LT_STREAM_READY, "header status incorrect");
    const auto probe_range = std::to_string(probe_offset) + "-" + std::to_string(probe_offset + 499);
    auto head = http_request(stream->server_port, "HEAD", probe_range);
    require(head.size() == head.find("\r\n\r\n") + 4, "probe HEAD returned data");
    require(stream->read_head == original_position && stream->stream_state == original_state,
            "HEAD changed playback state");
    require(scheduler->demand(probe_piece).priority == 0, "HEAD scheduled missing data");

    PendingHttp high(stream->server_port, probe_range);
    wait_until([&] { return scheduler->demand(probe_piece).priority == 7; }, "high probe not scheduled");
    const auto high_deadline = scheduler->demand(probe_piece).deadline;
    require(stream->read_head == probe_offset && stream->stream_state == LT_STREAM_BUFFERING,
            "pending probe status incorrect");
    PendingHttp shared(stream->server_port, probe_range);
    PendingHttp disposable(stream->server_port, std::to_string(probe_offset) + "-" +
                           std::to_string(probe_offset + piece_length));
    wait_until([&] { return scheduler->demand(probe_piece + 1).priority > 0; }, "exclusive probe not scheduled");
    disposable.close();
    wait_until([&] { return scheduler->demand(probe_piece + 1).priority == 0; }, "disconnect retained exclusive demand");
    require(scheduler->demand(probe_piece).deadline == high_deadline, "disconnect removed shared deadline");
    for (int retry = 0; retry < 3; ++retry) {
        auto low = http_request(stream->server_port, "GET", "0-262143");
        require(low.substr(low.find("\r\n\r\n") + 4) == first_piece, "low probe interrupted");
        http_request(stream->server_port, "HEAD", "0-499");
        require(stream->read_head == probe_offset && stream->stream_state == LT_STREAM_BUFFERING,
                "secondary GET/HEAD overwrote active reader state");
        require(scheduler->demand(probe_piece).deadline == high_deadline, "low probe cancelled high deadline");
        require(!high.readable() && !shared.readable(), "low probe closed a pending high response");
    }
    handle.add_piece(lt::piece_index_t(probe_piece), later_piece.data());
    require(high.body() == later_piece.substr(0, 500), "high probe failed after piece arrival");
    require(shared.body() == later_piece.substr(0, 500), "overlapping probe lost shared piece data");
    wait_until([&] { return scheduler->demand(probe_piece).priority == 0; }, "completed probe retained demands");
    require(handle.have_piece(lt::piece_index_t(probe_piece)), "completion discarded verified piece");

    // A fast container probe can consume cached data faster than the estimated
    // playback rate. Once it reaches a missing prefetched piece, that piece
    // must become urgent instead of retaining its future playback deadline.
    wait_until([&] {
        std::lock_guard<std::mutex> lock(stream->http_mu);
        return stream->http_readers.empty();
    }, "completed probes left active readers");
    PendingHttp sequential(stream->server_port, std::to_string(probe_piece * piece_length) + "-" +
                           std::to_string((probe_piece + 2) * piece_length - 1));
    wait_until([&] {
        return stream->read_head == int64_t(probe_piece + 1) * piece_length
            && stream->stream_state == LT_STREAM_BUFFERING;
    }, "sequential probe did not reach missing prefetched piece");
    require(scheduler->demand(probe_piece + 1).deadline <= chr::steady_clock::now(),
            "blocked HTTP read retained a future prefetch deadline");
    handle.add_piece(lt::piece_index_t(probe_piece + 1), later_piece.data());
    require(sequential.body() == later_piece + later_piece, "sequential probe did not resume after piece arrival");

    // Removal must wake blocked GETs without waiting for the piece timeout.
    PendingHttp blocked(stream->server_port, std::to_string((probe_piece + 2) * piece_length) + "-");
    wait_until([&] { return scheduler->demand(probe_piece + 2).priority > 0; }, "blocked reader not scheduled");
    lt_remove_torrent(session, id, 1);
    require(blocked.body().empty(), "removed stream still served data");
    require(scheduler->demand(probe_piece + 2).priority == 0, "removal left reader demand");
    require(StreamEngine::active_streams == 0, "removal leaked HTTP workers");
}

static void require_uploadable(const lt::torrent_handle& handle, int pieces) {
    auto status = handle.status(lt::torrent_handle::query_pieces);
    require(!(status.flags & lt::torrent_flags::paused), "retained torrent paused");
    require(!(status.flags & lt::torrent_flags::stop_when_ready), "retained torrent stops when ready");
    require(status.pieces.count() == pieces, "retention lost verified pieces");
}

// Verify real peer upload, rather than inferring it from a seeding state label.
static void test_local_upload(SessionWrapper* sw, lt_torrent_id id,
                              const std::filesystem::path& torrent_path,
                              const std::filesystem::path& download_path) {
    lt::settings_pack settings;
    settings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
    settings.set_bool(lt::settings_pack::enable_dht, false);
    settings.set_bool(lt::settings_pack::enable_lsd, false);
    settings.set_bool(lt::settings_pack::enable_upnp, false);
    settings.set_bool(lt::settings_pack::enable_natpmp, false);
    lt::session peer(settings);
    lt::add_torrent_params params;
    params.ti = std::make_shared<lt::torrent_info>(torrent_path.string());
    params.save_path = download_path.string();
    params.flags &= ~lt::torrent_flags::auto_managed;
    params.flags &= ~lt::torrent_flags::paused;
    auto downloader = peer.add_torrent(params);
    wait_until([&] { return sw->session.listen_port() != 0; }, "seed did not listen");
    downloader.connect_peer(lt::tcp::endpoint(lt::make_address("127.0.0.1"), sw->session.listen_port()));
    wait_until([&] { return downloader.status().total_done >= 256 * 1024; },
               "retained torrent did not upload verified data to local peer");
    require(sw->handles.at(id).status().total_payload_upload > 0, "upload counter did not advance");
}

// No DHT, trackers or LSD: switching must retain the existing torrent and
// TCP connection to the local seed, without requiring peer rediscovery.
static void test_switch_connection_retention(lt_session_t session, const std::filesystem::path& torrent_path,
                                  const std::filesystem::path& folder) {
    lt::settings_pack settings;
    settings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
    settings.set_bool(lt::settings_pack::enable_dht, false);
    settings.set_bool(lt::settings_pack::enable_lsd, false);
    settings.set_bool(lt::settings_pack::enable_upnp, false);
    settings.set_bool(lt::settings_pack::enable_natpmp, false);
    lt::session seed(settings);
    lt::add_torrent_params params;
    params.ti = std::make_shared<lt::torrent_info>(torrent_path.string());
    params.save_path = folder.string();
    params.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused);
    auto seed_handle = seed.add_torrent(params);
    wait_until([&] { return seed_handle.status().is_seeding && seed.listen_port() != 0; }, "reconnect seed not ready");
    const auto endpoint = lt::tcp::endpoint(lt::make_address("127.0.0.1"), seed.listen_port());
    const auto cache = (folder / "reconnect-cache").string();
    const auto id = lt_add_torrent_file(session, torrent_path.string().c_str(), cache.c_str(), 1);
    require(id >= 0, "reconnect torrent add failed");
    auto* sw = to_sw(session);
    const auto first = lt_start_stream(session, id, 0, 512 * 1024);
    require(first >= 0, "reconnect initial stream failed");
    sw->handles.at(id).connect_peer(endpoint);
    wait_until([&] { return lt_check_stream_start(session, first) == 1; }, "initial peer did not supply first piece");

    const auto original_handle = sw->handles.at(id);
    const auto original_scheduler = sw->schedulers.at(id);
    std::vector<lt::peer_info> peers_before;
    original_handle.get_peer_info(peers_before);
    require(peers_before.size() == 1, "local seed connection missing before switch");
    const auto next = lt_switch_file(session, id, 1, 512 * 1024);
    require(next >= 0, lt_last_error());
    require(lt_check_stream_start(session, first) == -1, "reconnect retained old stream");
    require(sw->handles.at(id) == original_handle && original_handle.is_valid(), "switch rebuilt torrent");
    require(sw->schedulers.at(id) == original_scheduler, "switch replaced scheduler");
    std::vector<lt::peer_info> peers_after;
    original_handle.get_peer_info(peers_after);
    require(peers_after.size() == 1 && peers_after.front().remote_endpoint() == peers_before.front().remote_endpoint()
            && peers_after.front().local_endpoint() == peers_before.front().local_endpoint(),
            "switch replaced the established TCP connection");
    wait_until([&] { return lt_check_stream_start(session, next) == 1; }, "switch could not download from the retained connection");
    const auto reply = http_request(sw->streams.at(next)->server_port, "GET", "0-499");
    require(reply.size() - reply.find("\r\n\r\n") - 4 == 500, "reconnected episode could not serve HTTP");
    lt_remove_torrent(session, id, 1);
}

static void test_stream_lifecycle(lt_session_t session, const std::filesystem::path& torrent_path,
                                  const std::filesystem::path& folder) {
    auto* sw = to_sw(session);
    auto id = lt_add_torrent_file(session, torrent_path.string().c_str(), folder.string().c_str(), 0);
    require(id >= 0, "on-demand fixture rejected");
    auto handle = sw->handles.at(id);
    lt_pause_torrent(session, id);
    require(lt_add_torrent_file(session, torrent_path.string().c_str(), folder.string().c_str(), 0) == id,
            "duplicate ordinary task did not reuse its ID");
    require(bool(handle.flags() & lt::torrent_flags::paused), "duplicate add resumed a paused task");
    require(lt_add_torrent_file(session, torrent_path.string().c_str(), folder.string().c_str(), 1) < 0,
            "duplicate ordinary task accepted conflicting streamOnly mode");
    require(sw->stream_only_torrents.count(id) == 0, "duplicate changed ordinary task cleanup policy");
    lt_resume_torrent(session, id);
    wait_until([&] {
        auto state = handle.status().state;
        return state != lt::torrent_status::checking_files && state != lt::torrent_status::checking_resume_data;
    }, "on-demand fixture still checking");

    const int32_t skip[] = {0, 0};
    lt_set_file_priorities(session, id, skip, 2);
    require(std::string(lt_last_error()).empty(), "cannot set empty baseline");
    auto old = lt_start_stream(session, id, 0, 1024 * 1024);
    auto next = lt_start_stream(session, id, 1, 1024 * 1024);
    require(old >= 0 && next >= 0, "overlapping episodes did not start");
    require(sw->active_stream_ids.at(id).size() == 2, "stream ownership not registered");
    auto scheduler = sw->schedulers.at(id);
    auto first_deadline = scheduler->demand(0).deadline;
    scheduler->request(next, 0, 5, 20000);
    require(scheduler->demand(0).priority == 7, "overlap did not select highest priority");
    require(scheduler->demand(0).deadline == first_deadline, "new stream postponed existing deadline");
    const auto next_deadline = scheduler->demand(32).deadline;

    // Independent HTTP readers merge overlap and withdraw only their own picks.
    auto* old_stream = sw->streams.at(old).get();
    scheduler->retain(old, [](int piece) { return piece < 20 || piece > 23; });
    TorrReader first_reader, probe_reader;
    first_reader.reader_id = 100;
    probe_reader.reader_id = 101;
    require(old_stream->begin_http_reader(first_reader, 20 * 262144, 22 * 262144 - 1), "reader registration failed");
    old_stream->extend_window(first_reader, 20);
    const auto shared_deadline = scheduler->demand(21).deadline;
    require(old_stream->begin_http_reader(probe_reader, 21 * 262144, 23 * 262144 - 1), "probe registration failed");
    old_stream->extend_window(probe_reader, 21);
    require(scheduler->demand(21).deadline <= shared_deadline, "overlap postponed deadline");
    require(scheduler->demand(23).priority == 0, "short probe scheduled beyond Range end");
    old_stream->finish_http_reader(101);
    require(scheduler->demand(21).deadline == shared_deadline, "probe stop removed shared demand");
    require(scheduler->demand(22).priority == 0, "probe-exclusive piece survived close");
    old_stream->extend_window(probe_reader, 22); // closed workers cannot re-register
    require(scheduler->demand(22).priority == 0, "closed reader revived demand");
    old_stream->finish_http_reader(100);
    require(scheduler->demand(32).deadline == next_deadline, "probe changed another stream's deadline");
    lt_stop_stream_ex(session, old, 1);
    require(sw->active_stream_ids.at(id).count(next) == 1, "stop removed remaining owner");
    require(scheduler->demand(0).priority == 5, "shared demand not restored to remaining owner");
    require(scheduler->demand(0).deadline > first_deadline, "departed owner's deadline survived");
    require(scheduler->demand(1).priority == 0, "exclusive old demand survived stop");
    require(handle.piece_priority(lt::piece_index_t(1)) == lt::dont_download, "exclusive missing piece still downloading");
    require(scheduler->demand(32).deadline == next_deadline, "stop changed next episode deadline");
    require_uploadable(handle, 0);

    // A stopped HTTP worker must not revive its old demands.
    scheduler->request(old, 1, 7, 0);
    require(scheduler->demand(1).priority == 0, "stopped owner revived demand");
    lt_stop_stream_ex(session, next, 1);
    require(!sw->active_stream_ids.count(id), "empty ownership set retained");
    require(sw->handles.count(id) == 1, "keepTorrent deleted on-demand task");
    for (auto priority : handle.get_piece_priorities())
        require(priority == lt::dont_download, "retention enabled background download");
    require_uploadable(handle, 0);

    auto again = lt_start_stream(session, id, 0, 1024 * 1024);
    auto other = lt_start_stream(session, id, 1, 1024 * 1024);
    require(again >= 0 && other >= 0, "retained torrent cannot switch episodes");
    lt_stop_stream_ex(session, again, 0);
    require(sw->handles.count(id) == 1 && sw->streams.count(other), "default stop deleted live sibling");
    lt_stop_stream_ex(session, other, 1);
    require(sw->handles.count(id) == 1, "earlier delete request became sticky");

    auto last = lt_start_stream(session, id, 1, 1024 * 1024);
    auto sibling = lt_start_stream(session, id, 0, 1024 * 1024);
    auto replacement = lt_switch_file(session, id, 0, 1024 * 1024);
    require(replacement >= 0, "explicit switch failed for ordinary torrent");
    require(sw->handles.at(id) == handle && handle.is_valid(), "ordinary switch rebuilt torrent");
    require_uploadable(handle, 0);
    require(!sw->streams.count(last) && !sw->streams.count(sibling) && sw->streams.count(replacement),
            "explicit switch did not replace every old stream");
    lt_stop_stream(session, replacement);
    require(sw->handles.count(id) && sw->schedulers.count(id), "default stop removed task");
    lt_stop_stream_ex(session, last, 0); // repeated cleanup is harmless
    lt_remove_torrent(session, id, 1);
    require(lt_start_stream(session, id, 0, 0) < 0, "deleted task can still start a stream");

    auto racing_id = lt_add_torrent_file(session, torrent_path.string().c_str(), folder.string().c_str(), 0);
    auto racing_stream = lt_start_stream(session, racing_id, 0, 1024 * 1024);
    require(racing_stream >= 0, "concurrency fixture failed");
    auto racing_scheduler = sw->schedulers.at(racing_id);
    std::atomic<bool> go{false};
    std::thread starter([&] {
        while (!go.load()) std::this_thread::yield();
        lt_start_stream(session, racing_id, 1, 1024 * 1024);
    });
    std::thread worker([&] {
        while (!go.load()) std::this_thread::yield();
        for (int i = 0; i < 100; ++i) racing_scheduler->request(racing_stream, 0, 7, 0);
    });
    go = true;
    lt_remove_torrent(session, racing_id, 1);
    starter.join();
    worker.join();
    require(!sw->handles.count(racing_id) && !sw->active_stream_ids.count(racing_id),
            "concurrent start revived a removed torrent");
    require(sw->streams.empty(), "concurrent removal left an HTTP stream");
    require(racing_scheduler->demand(0).priority == 0, "late worker revived removed torrent demands");
}

// Switch before old pieces arrive, including a blocked HTTP GET. Missing old
// pieces must lose their deadlines while the new file starts on the same handle.
static void test_switch_missing_demands(lt_session_t session, const std::filesystem::path& torrent_path,
                                        const std::filesystem::path& folder) {
    auto* sw = to_sw(session);
    const auto cache = (folder / "switch-missing").string();
    const auto id = lt_add_torrent_file(session, torrent_path.string().c_str(), cache.c_str(), 1);
    const auto first = lt_start_stream(session, id, 0, 512 * 1024);
    const auto sibling = lt_start_stream(session, id, 0, 512 * 1024);
    require(first >= 0 && sibling >= 0, "missing switch fixture failed");
    const auto handle = sw->handles.at(id);
    const auto scheduler = sw->schedulers.at(id);
    const auto port = sw->streams.at(first)->server_port;
    PendingHttp blocked(port, "0-499");
    wait_until([&] {
        auto* stream = sw->streams.at(first).get();
        std::lock_guard<std::mutex> lock(stream->http_mu);
        return stream->primary_reader >= 0;
    }, "old HTTP reader did not register");
    const auto next = lt_switch_file(session, id, 1, 512 * 1024);
    require(next >= 0, lt_last_error());
    require(blocked.body().empty(), "stopped missing-data reader returned unexpected bytes");
    require(handle == sw->handles.at(id) && scheduler == sw->schedulers.at(id), "switch rebuilt task or scheduler");
    require(lt_check_stream_start(session, first) == -1 && lt_check_stream_start(session, sibling) == -1,
            "switch left old streams valid");
    for (int piece = 0; piece < 32; ++piece) {
        const auto demand = scheduler->demand(piece);
        require(demand.priority == 0 && demand.deadline == chr::steady_clock::time_point::max(),
                "old missing piece retained stream demand");
        require(handle.piece_priority(lt::piece_index_t(piece)) == lt::dont_download,
                "old missing piece still selected for download");
    }
    const auto startup = scheduler->demand(32);
    require(startup.priority == 7 && startup.deadline <= chr::steady_clock::now(), "new startup lacks immediate deadline");
    scheduler->request(first, 0, 7, 0);
    require(scheduler->demand(0).priority == 0, "late old worker revived demand");
    require(!(handle.flags() & (lt::torrent_flags::paused | lt::torrent_flags::stop_when_ready)),
            "switch paused the torrent");
    lt_remove_torrent(session, id, 1);
}

static uintmax_t disk_bytes(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return 0;
    uintmax_t bytes = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path))
        if (entry.is_regular_file()) bytes += entry.file_size();
    return bytes;
}

// Real disk writes (including libtorrent's partfile), ten episode switches,
// and real upload from the currently playing episode. No public swarm needed.
static void test_current_episode(lt_session_t session, const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    fs::create_directories(folder);
    constexpr int piece_length = 256 * 1024;
    // Deliberately unaligned files exercise pieces shared by adjacent episodes.
    const std::string content(piece_length * 4 + 17, 'e');
    lt::file_storage files;
    for (int episode = 0; episode < 10; ++episode)
        files.add_file("episodes/episode-" + std::to_string(episode) + ".mp4", int64_t(content.size()));
    lt::create_torrent torrent(files, piece_length, lt::create_torrent::v1_only);
    const int total_pieces = int((content.size() * 10 + piece_length - 1) / piece_length);
    for (int piece = 0; piece < total_pieces; ++piece) {
        const int length = int(std::min<int64_t>(piece_length, content.size() * 10 - int64_t(piece) * piece_length));
        lt::hasher hash(content.data(), length);
        torrent.set_hash(lt::piece_index_t(piece), hash.final());
    }
    std::vector<char> encoded;
    lt::bencode(std::back_inserter(encoded), torrent.generate());
    auto torrent_path = folder / "collection.torrent";
    std::ofstream(torrent_path, std::ios::binary).write(encoded.data(), encoded.size());
    auto save_path = folder / "cache";
    fs::create_directories(save_path);
    auto* sw = to_sw(session);
    auto id = lt_add_torrent_file(session, torrent_path.string().c_str(), save_path.string().c_str(), 1);
    require(id >= 0, "single-episode fixture rejected");
    lt_stream_id previous = -1;
    lt::torrent_handle previous_handle;
    for (int episode = 0; episode < 10; ++episode) {
        auto sid = episode == 0
            ? lt_start_stream(session, id, episode, 512 * 1024)
            : lt_switch_file(session, id, episode, 512 * 1024);
        require(sid >= 0, lt_last_error());
        if (episode == 0)
            require(lt_check_stream_start(session, sid) == 0, "empty episode reported startup ready");
        auto handle = sw->handles.at(id);
        const int first_piece = int(episode * content.size() / piece_length);
        const int last_piece = int(((episode + 1) * content.size() - 1) / piece_length);
        const int wanted_pieces = last_piece - first_piece + 1;
        if (episode > 0) {
            require(lt_check_stream_start(session, previous) == -1, "replaced stream remains startup-valid");
            require(previous_handle.is_valid() && previous_handle == handle, "switch rebuilt torrent handle");
            require(sw->streams.size() == 1, "switchFile left concurrent old streams running");
            lt_stream_status stopped{};
            require(!lt_get_stream_status(session, previous, &stopped), "old episode HTTP stream survived switch");
            require(handle.have_piece(lt::piece_index_t(0)), "switch lost verified old pieces");
            require(disk_bytes(save_path) >= content.size(), "switch deleted old payload/partfile");
            require(sw->active_stream_ids.at(id).size() == 1 && sw->active_stream_ids.at(id).count(sid),
                    "switch retained stale owners");
            const auto withdrawn = sw->schedulers.at(id)->demand(0);
            require(withdrawn.priority == 0 && withdrawn.deadline == chr::steady_clock::time_point::max(),
                    "old exclusive startup demand survived switch");
            // A stale caller stopping the replaced stream cannot stop the new one.
            lt_stop_stream_ex(session, previous, 1);
            require(sw->streams.count(sid), "stale stop deleted the new episode");
        }
        require(lt_get_file_count(session, id) == 10, "switch lost collection metadata");
        const auto startup_demand = sw->streams.at(sid)->scheduler->demand(first_piece);
        require(startup_demand.priority == 7 && startup_demand.deadline <= chr::steady_clock::now(),
                "new episode first piece lacks immediate startup priority/deadline");
        handle.add_piece(lt::piece_index_t(last_piece), content.data());
        wait_until([&] { return handle.have_piece(lt::piece_index_t(last_piece)); }, "tail fixture did not verify");
        if (episode == 0)
            require(lt_check_stream_start(session, sid) == 0, "tail completion incorrectly satisfied startup wait");
        for (int piece = first_piece; piece <= last_piece; ++piece)
            handle.add_piece(lt::piece_index_t(piece), content.data());
        wait_until([&] { return handle.status(lt::torrent_handle::query_pieces).pieces.count() == last_piece + 1; },
                   "current episode data did not verify");
        wait_until([&] { return lt_check_stream_start(session, sid) == 1; }, "verified first piece was not readable");
        require(lt_check_stream_start(session, sid) == 1, "repeat startup check consumed readiness");
        require(sw->streams.at(sid)->read_head == 0, "startup wait advanced the HTTP cursor");
        auto status = handle.status(lt::torrent_handle::query_pieces);
        for (int piece = 0; piece < total_pieces; ++piece) {
            require(status.pieces[lt::piece_index_t(piece)] == (piece <= last_piece),
                    "switch lost downloaded episode data or downloaded unwanted pieces");
        }
        auto* stream = sw->streams.at(sid).get();
        const auto reply = http_request(stream->server_port, "GET", "0-499");
        require(reply.substr(reply.find("\r\n\r\n") + 4) == content.substr(0, 500), "current episode playback failed");
        require_uploadable(handle, last_piece + 1);
        require(disk_bytes(save_path) >= (episode + 1) * content.size(),
                "downloaded episode data was not retained on disk");
        if (episode == 0 || episode == 9)
            test_local_upload(sw, id, torrent_path, folder / ("peer-" + std::to_string(episode)));
        if (episode == 0) {
            // Both add entry points must resolve to one lifecycle owner, even
            // when a caller changes the URI's trackers or requested save path.
            const auto task_count = sw->handles.size();
            const auto handle_before_add = sw->handles.at(id);
            const auto magnet = lt::make_magnet_uri(handle_before_add);
            const auto other_path = (folder / "unused-cache").string();
            lt_torrent_id concurrent_file_id = -1;
            lt_torrent_id concurrent_magnet_id = -1;
            std::thread file_add([&] {
                concurrent_file_id = lt_add_torrent_file(session, torrent_path.string().c_str(), other_path.c_str(), 1);
            });
            std::thread magnet_add([&] {
                concurrent_magnet_id = lt_add_magnet(session, magnet.c_str(), other_path.c_str(), 1);
            });
            file_add.join();
            magnet_add.join();
            require(concurrent_file_id == id && concurrent_magnet_id == id,
                    "concurrent duplicate adds allocated another task ID");
            require(lt_add_torrent_file(session, torrent_path.string().c_str(), other_path.c_str(), 1) == id,
                    "duplicate torrent file allocated another task ID");
            require(lt_add_magnet(session, magnet.c_str(), other_path.c_str(), 1) == id,
                    "duplicate magnet allocated another task ID");
            require(lt_add_torrent_file(session, torrent_path.string().c_str(), save_path.string().c_str(), 0) < 0,
                    "duplicate torrent file changed on-demand mode");
            require(lt_add_magnet(session, magnet.c_str(), save_path.string().c_str(), 0) < 0,
                    "duplicate magnet changed on-demand mode");
            require(std::string(lt_last_error()).find("streamOnly") != std::string::npos,
                    "conflicting duplicate omitted its mode mismatch");
            require(sw->handles.size() == task_count && sw->handles.at(id) == handle_before_add,
                    "duplicate registration changed the task owner");
            require(sw->stream_only_torrents.count(id) && sw->streams.count(sid),
                    "duplicate registration changed the stream lifecycle");
            require(fs::path(handle_before_add.status().save_path) == save_path,
                    "duplicate registration moved existing data");
            require(lt_start_stream(session, id, 99, 0) < 0, "invalid episode accepted");
            require(lt_switch_file(session, id, 99, 0) < 0, "switch accepted an invalid episode");
            require(lt_switch_file(session, id, -1, 0) < 0, "switch silently auto-selected an episode");
            require(lt_switch_file(session, -1, 1, 0) < 0, "switch accepted an unknown torrent");
            const auto previous_bytes = disk_bytes(save_path);
            auto concurrent = lt_start_stream(session, id, 1, 512 * 1024);
            require(concurrent >= 0, "startStream rejected a different file in on-demand mode");
            require(sw->streams.count(sid) && sw->streams.count(concurrent), "startStream replaced an existing stream");
            require(sw->handles.at(id) == handle, "startStream replaced the underlying torrent");
            require(disk_bytes(save_path) == previous_bytes, "startStream deleted or changed current data");
            require(sw->streams.count(sid) && handle.is_valid(), "invalid selection destroyed the current episode");
            require_uploadable(handle, wanted_pieces);
            auto alias = lt_start_stream(session, id, 0, 512 * 1024);
            require(alias >= 0, "second reader of current episode failed");
            lt_stop_stream_ex(session, alias, 1);
            require_uploadable(handle, wanted_pieces); // same-episode reader must not clear data
            // Independent files can download together and retain their data.
            handle.add_piece(lt::piece_index_t(last_piece + 1), content.data());
            wait_until([&] { return handle.have_piece(lt::piece_index_t(last_piece + 1)); },
                       "additional stream could not receive its own piece");
            require_uploadable(handle, wanted_pieces + 1);
            const auto first_reply = http_request(stream->server_port, "GET", "0-499");
            require(first_reply.substr(first_reply.find("\r\n\r\n") + 4) == content.substr(0, 500),
                    "additional stream broke existing playback");
            const auto concurrent_port = sw->streams.at(concurrent)->server_port;
            const auto other_reply = http_request(concurrent_port, "GET", "0-499");
            require(other_reply.substr(other_reply.find("\r\n\r\n") + 4) == content.substr(0, 500),
                    "different file could not play concurrently");
        }
        previous = sid;
        previous_handle = handle;
    }

    // Re-selecting the current file replaces only the stream, reusing bytes.
    const auto retained_bytes = disk_bytes(save_path);
    const auto same_file = lt_switch_file(session, id, 9, 512 * 1024);
    require(same_file >= 0 && same_file != previous, "same-file switch did not create a new stream");
    require(lt_check_stream_start(session, previous) == -1, "same-file switch retained old stream");
    require(sw->handles.at(id) == previous_handle && disk_bytes(save_path) == retained_bytes,
            "same-file switch reset torrent or disk data");
    wait_until([&] { return lt_check_stream_start(session, same_file) == 1; }, "same-file switch lost ready data");
    previous = same_file;

    // Stopping is non-destructive even with the legacy keep_torrent=false flag.
    lt_stop_stream_ex(session, previous, 0);
    require(lt_check_stream_start(session, previous) == -1, "stopped stream remains startup-valid");
    require(previous_handle.is_valid(), "stop removed torrent");
    require(sw->streams.empty() && lt_get_file_count(session, id) == 10, "stop lost task");
    require(disk_bytes(save_path) == retained_bytes, "stop deleted disk data");
    require(lt_has_downloads(session, id) == 0, "playback became persistent download");
    require_uploadable(previous_handle, total_pieces);
    auto restarted = lt_start_stream(session, id, 0, 512 * 1024);
    require(restarted >= 0, "retained task cannot restart");

    lt_remove_torrent(session, id, 1);
    require(lt_check_stream_start(session, restarted) == -1, "removed stream remains startup-valid");
    require(!sw->handles.count(id) && sw->streams.empty(), "final collection cleanup left a task");
}

// Mid-file gaps survive seeks/switches as baseline downloads; shared boundary
// pieces count only the file's own bytes and every required piece must verify.
static void test_file_downloads(lt_session_t session, const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    fs::create_directories(folder);
    constexpr int piece_length = 256 * 1024;
    constexpr int first_size = 20 * 1024 * 1024 + 17;
    const std::string data(piece_length, 'd');
    lt::file_storage files;
    files.add_file("downloads/first.mp4", first_size);
    files.add_file("downloads/second.mp4", first_size);
    lt::create_torrent torrent(files, piece_length, lt::create_torrent::v1_only);
    for (int piece = 0; piece < torrent.num_pieces(); ++piece) {
        const int bytes = int(std::min<int64_t>(piece_length, 2LL * first_size - int64_t(piece) * piece_length));
        torrent.set_hash(lt::piece_index_t(piece), lt::hasher(data.data(), bytes).final());
    }
    std::vector<char> encoded;
    lt::bencode(std::back_inserter(encoded), torrent.generate());
    const auto torrent_path = folder / "downloads.torrent";
    std::ofstream(torrent_path, std::ios::binary).write(encoded.data(), encoded.size());
    const auto id = lt_add_torrent_file(session, torrent_path.string().c_str(), folder.string().c_str(), 1);
    auto* sw = to_sw(session);
    const auto first = lt_start_stream(session, id, 0, 512 * 1024);
    require(first >= 0 && lt_has_downloads(session, id) == 0, "temporary task owns downloads");
    const auto handle = sw->handles.at(id);
    const auto scheduler = sw->schedulers.at(id);
    require(scheduler->demand(40).priority == 0, "test gap was prefetched");
    require(lt_download_file(session, id, -1) == 0 && lt_download_file(session, id, 2) == 0,
            "invalid download file accepted");
    require(lt_download_file(session, -1, 0) == 0, "unknown task accepted download");
    require(lt_download_file(session, id, 0) == 1 && lt_download_file(session, id, 0) == 1,
            "download registration is not idempotent");
    require(sw->handles.at(id) == handle && sw->streams.count(first), "download interrupted playback");
    require(lt_has_downloads(session, id) == 1 && scheduler->demand(40).priority == 4,
            "download omitted skipped middle pieces");
    require(scheduler->add_reader(first, 42), "seek reader registration failed");
    scheduler->request(first, 40, 7, 0, 42);
    require(scheduler->demand(40).priority == 7, "playback did not override baseline");
    scheduler->remove_reader(first, 42);
    require(scheduler->demand(40).priority == 4
            && scheduler->demand(40).deadline == chr::steady_clock::time_point::max(),
            "seek withdrawal erased baseline or retained deadline");
    const auto next = lt_switch_file(session, id, 1, 512 * 1024);
    require(next >= 0 && sw->handles.at(id) == handle, "download switch rebuilt task");
    require(scheduler->demand(0).priority == 4 && scheduler->demand(40).priority == 4,
            "switch erased original file download");
    const int boundary = first_size / piece_length;
    require(scheduler->demand(boundary).priority == 7, "shared boundary lost new playback demand");
    lt_stop_stream_ex(session, next, 0);
    require(scheduler->demand(boundary).priority == 4 && scheduler->demand(boundary + 1).priority == 0,
            "stop did not preserve only selected file baseline");
    scheduler->reapply(); // async file priority completion must preserve the same selection
    require(handle.piece_priority(lt::piece_index_t(40)) == lt::default_priority,
            "file-priority completion lost download baseline");
    lt_file_download_status status[2]{};
    require(lt_get_file_downloads(session, id, status, 2) == 1 && !status[0].is_complete
            && status[0].verified_bytes == 0, "empty file was reported complete");
    for (int piece = 0; piece <= boundary; ++piece) {
        if (piece == 40) continue; // leave a seek gap despite complete head and tail
        handle.add_piece(lt::piece_index_t(piece), data.data());
    }
    wait_until([&] { return handle.status(lt::torrent_handle::query_pieces).pieces.count() == boundary; },
               "download fixture pieces did not verify");
    require(lt_get_file_downloads(session, id, status, 2) == 1 && !status[0].is_complete
            && status[0].verified_bytes == first_size - piece_length, "head/tail incorrectly completed a gapped file");
    handle.add_piece(lt::piece_index_t(40), data.data());
    wait_until([&] { return lt_get_file_downloads(session, id, status, 2) == 1 && status[0].is_complete; },
               "filling middle gap did not complete file");
    require(status[0].verified_bytes == first_size && status[0].file_index == 0 && status[0].priority == 4,
            "download progress counted adjacent file bytes");
    require(lt_has_downloads(session, id) == 1, "completed download lost retention policy");
    require_uploadable(handle, boundary + 1);
    require(lt_download_file(session, id, 1) == 1, "cannot add second download");
    require(lt_get_file_downloads(session, id, status, 2) == 2 && !status[1].is_complete
            && status[1].verified_bytes == piece_length - 17, "shared boundary progress is wrong");
    lt_remove_torrent(session, id, 0);
    require(fs::exists(folder / "downloads/first.mp4") && lt_has_downloads(session, id) == 0,
            "remove without delete lost files or retained demands");
}

static void test_resume_state(lt_session_t session, const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    fs::create_directories(folder);
    lt::file_storage files;
    files.add_file("resume/first.mp4", 256 * 1024);
    files.add_file("resume/second.mp4", 256 * 1024);
    lt::create_torrent torrent(files, 256 * 1024, lt::create_torrent::v1_only);
    const std::string content(256 * 1024, 'r');
    for (int i = 0; i < 2; ++i)
        torrent.set_hash(lt::piece_index_t(i), lt::hasher(content.data(), content.size()).final());
    std::vector<char> encoded;
    lt::bencode(std::back_inserter(encoded), torrent.generate());
    const auto path = folder / "resume.torrent";
    std::ofstream(path, std::ios::binary).write(encoded.data(), encoded.size());
    const auto id = lt_add_torrent_file(session, path.string().c_str(), folder.string().c_str(), 1);
    require(id > 0 && lt_download_file(session, id, 0) == 1, "resume fixture failed");
    const auto sid = lt_start_stream(session, id, 1, 512 * 1024);
    require(sid > 0, "resume fixture playback failed");
    auto* sw = to_sw(session);
    const auto handle = sw->handles.at(id);
    lt_pause_torrent(session, id);
    wait_until([&] { return bool(handle.status().flags & lt::torrent_flags::paused); }, "pause not applied");
    require(lt_request_resume_data(session, id) == 1, "resume request failed");
    int size = 0;
    wait_until([&] {
        size = lt_read_resume_data(session, id, nullptr, 0);
        require(size >= 0, lt_last_error());
        return size > 0;
    }, "resume alert missing");
    std::vector<char> bytes(size);
    require(lt_read_resume_data(session, id, bytes.data(), size) == size, "resume buffer copy failed");
    lt::error_code ec;
    auto node = lt::bdecode(lt::span<char const>(bytes.data(), bytes.size()), ec);
    auto params = lt::read_resume_data(node, ec);
    require(!ec && params.ti && params.file_priorities.size() == 2,
        "resume metadata/selection missing");
    require(params.file_priorities[0] == lt::default_priority && params.file_priorities[1] == lt::dont_download,
        "playback demand leaked into resume file selection");
    require(params.piece_priorities.empty(), "playback piece priorities persisted");
    require(bool(params.flags & lt::torrent_flags::paused), "resume lost paused state");
    const std::string magnet = lt_get_torrent_magnet(session, id);
    require(lt_find_torrent_by_magnet(session, magnet.c_str()) == id, "resume lookup failed");
    require(lt_download_file(session, id, 1) == 1, "second selection failed");
    require(lt_restore_resume_data(session, bytes.data(), size) == id, "restore duplicated live task");
    require(sw->schedulers.at(id)->download_priorities()[1] == lt::default_priority,
        "restore overwrote live task selection");
    lt_remove_torrent(session, id, 0);
    const auto restored = lt_restore_resume_data(session, bytes.data(), size);
    require(restored > 0 && restored != id, "restore did not register fresh task");
    require(!sw->active_stream_ids.count(restored), "restore created a stream");
    require(sw->schedulers.at(restored)->demand(0).priority == 4
        && sw->schedulers.at(restored)->demand(1).priority == 0,
        "restored baseline includes old playback demand");
    require(bool(sw->handles.at(restored).status().flags & lt::torrent_flags::paused),
        "restored paused task was resumed");
    require(lt_restore_resume_data(session, "le", 2) == -1, "non-dictionary resume accepted");
    auto malformed = lt::write_resume_data(params);
    malformed["ltf-version"] = 1;
    malformed["file_priority"].list().pop_back();
    std::vector<char> malformed_bytes;
    lt::bencode(std::back_inserter(malformed_bytes), malformed);
    require(lt_restore_resume_data(session, malformed_bytes.data(), int(malformed_bytes.size())) == -1,
        "malformed selection length accepted");
    lt_remove_torrent(session, restored, 1);

    // v2 magnets use btmh multihashes and must also deduplicate by hash.
    const std::string v2 = "magnet:?xt=urn:btmh:1220" + std::string(64, 'a');
    const auto v2id = lt_add_magnet(session, v2.c_str(), folder.string().c_str(), 0);
    require(v2id > 0, "v2 magnet rejected");
    require(lt_find_torrent_by_magnet(session, (v2 + "&dn=other").c_str()) == v2id,
        "v2 lookup failed");
    lt_remove_torrent(session, v2id, 1);
}

static void test_local_stream() {
    namespace fs = std::filesystem;
    auto folder = fs::temp_directory_path() / ("ltf-regression-" + std::to_string(chr::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(folder);
    lt_session_t session = nullptr;
    try {
        std::string content(8 * 1024 * 1024, '\0');
        for (size_t i = 0; i < content.size(); ++i) content[i] = char((i * 31 + i / 257) % 251);
        fs::create_directories(folder / "episodes");
        std::ofstream(folder / "episodes/first.mp4", std::ios::binary).write(content.data(), content.size());
        std::ofstream(folder / "episodes/second.mp4", std::ios::binary).write(content.data(), content.size());
        lt::file_storage files;
        files.add_file("episodes/first.mp4", int64_t(content.size()));
        files.add_file("episodes/second.mp4", int64_t(content.size()));
        lt::create_torrent torrent(files, 256 * 1024, lt::create_torrent::v1_only);
        for (int piece = 0; piece < 64; ++piece) {
            lt::hasher hash(content.data() + (piece % 32) * 256 * 1024, 256 * 1024);
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
            if (status.pieces_done == 64) break;
            std::this_thread::sleep_for(chr::milliseconds(50));
        }
        auto checked = to_sw(session)->handles.at(id).status(lt::torrent_handle::query_pieces);
        if (checked.pieces.count() != 64) {
            std::lock_guard<std::mutex> lock(to_sw(session)->dart_queue_mu);
            for (const auto& alert : to_sw(session)->dart_queue) std::cerr << alert.message << '\n';
        }
        require(checked.pieces.count() == 64, ("fixture not verified: state=" + std::to_string(int(checked.state)) +
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
        auto second = lt_start_stream(session, id, 1, 1024 * 1024);
        auto overlap = lt_start_stream(session, id, 0, 1024 * 1024);
        require(second >= 0 && overlap >= 0, "concurrent episode stream failed");
        const auto second_port = to_sw(session)->streams.at(second)->server_port;
        const auto overlap_port = to_sw(session)->streams.at(overlap)->server_port;
        // Explicit read results must fan out to all streams sharing the piece.
        auto* overlap_stream = to_sw(session)->streams.at(overlap).get();
        stream->read_head = int64_t(15) * stream->piece_length;
        overlap_stream->read_head = int64_t(15) * overlap_stream->piece_length;
        stream->drop_cached(15);
        overlap_stream->drop_cached(15);
        stream->handle.read_piece(lt::piece_index_t(15));
        wait_until([&] { return stream->has_cached_piece(15) && overlap_stream->has_cached_piece(15); },
                   "read alert reached only one matching stream");
        lt_stop_stream_ex(session, sid, 1);
        auto reply = http_request(second_port, "GET", "0-499");
        require(reply.substr(reply.find("\r\n\r\n") + 4) == content.substr(0, 500), "old stop broke next episode");
        reply = http_request(overlap_port, "GET", "0-499");
        require(reply.substr(reply.find("\r\n\r\n") + 4) == content.substr(0, 500), "old stop broke overlapping file");
        lt_stop_stream_ex(session, second, 1);
        lt_stop_stream_ex(session, overlap, 1);
        require_uploadable(to_sw(session)->handles.at(id), 64);
        test_local_upload(to_sw(session), id, torrent_path, folder / "peer");

        // removeTorrent owns teardown even while HTTP and preload are active.
        auto restarted = lt_start_stream(session, id, 0, 1024 * 1024);
        auto sibling = lt_start_stream(session, id, 1, 1024 * 1024);
        require(restarted >= 0 && sibling >= 0, "retained task cannot restart");
        lt_preload_stream(session, restarted, 512 * 1024);
        lt_remove_torrent(session, id, 0);
        require(to_sw(session)->streams.empty() && to_sw(session)->active_stream_ids.empty(),
                "direct removal left active streams");
        require(fs::exists(folder / "episodes/first.mp4"), "deleteFiles=false deleted data");
        lt_remove_torrent(session, id, 1);

        // streamOnly controls selection, while both modes share stop policy.
        test_switch_connection_retention(session, torrent_path, folder);
        test_switch_missing_demands(session, torrent_path, folder);
        test_file_downloads(session, folder / "file-downloads");
        test_resume_state(session, folder / "resume-state");
        test_stream_lifecycle(session, torrent_path, folder / "empty");
        auto cleanup_id = lt_add_torrent_file(session, torrent_path.string().c_str(), folder.string().c_str(), 0);
        auto cleanup_stream = lt_start_stream(session, cleanup_id, 0, 1024 * 1024);
        require(cleanup_stream >= 0, "cleanup stream failed");
        lt_stop_stream(session, cleanup_stream);
        require(to_sw(session)->handles.count(cleanup_id), "normal mode stop deleted task");
        require(lt_has_downloads(session, cleanup_id) == 1, "ordinary download lost baseline");
        lt_remove_torrent(session, cleanup_id, 1);
        wait_until([&] { return !fs::exists(folder / "episodes/first.mp4")
                            && !fs::exists(folder / "episodes/second.mp4"); }, "torrent files not deleted");
        test_http_probes(session, folder / "http-probes");
        test_current_episode(session, folder / "single-episode");
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
