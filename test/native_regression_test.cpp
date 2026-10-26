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

static void wait_until(const std::function<bool()>& ready, const char* message) {
    for (int i = 0; i < 400; ++i) {
        if (ready()) return;
        std::this_thread::sleep_for(chr::milliseconds(25));
    }
    require(false, message);
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

static void test_stream_lifecycle(lt_session_t session, const std::filesystem::path& torrent_path,
                                  const std::filesystem::path& folder) {
    auto* sw = to_sw(session);
    auto id = lt_add_torrent_file(session, torrent_path.string().c_str(), folder.string().c_str(), 0);
    require(id >= 0, "on-demand fixture rejected");
    auto handle = sw->handles.at(id);
    wait_until([&] {
        auto state = handle.status().state;
        return state != lt::torrent_status::checking_files && state != lt::torrent_status::checking_resume_data;
    }, "on-demand fixture still checking");

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

    // A seek and late old-generation work must not revoke another owner.
    auto* old_stream = sw->streams.at(old).get();
    old_stream->seek_generation = 1;
    old_stream->drop_window(20);
    old_stream->request_piece(20, 0);
    require(scheduler->demand(32).deadline == next_deadline, "seek changed another stream's deadline");
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
    require(!sw->streams.count(last) && !sw->streams.count(sibling) && sw->streams.count(replacement),
            "explicit switch did not replace every old stream");
    lt_stop_stream(session, replacement);
    require(!sw->handles.count(id) && !sw->schedulers.count(id), "default stop did not remove last task");
    lt_stop_stream_ex(session, last, 0); // repeated cleanup is harmless
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
        auto handle = sw->handles.at(id);
        const int first_piece = int(episode * content.size() / piece_length);
        const int last_piece = int(((episode + 1) * content.size() - 1) / piece_length);
        const int wanted_pieces = last_piece - first_piece + 1;
        if (episode > 0) {
            require(!previous_handle.is_valid(), "old episode still has a live BT handle");
            require(sw->streams.size() == 1, "switchFile left concurrent old streams running");
            lt_stream_status stopped{};
            require(!lt_get_stream_status(session, previous, &stopped), "old episode HTTP stream survived switch");
            require(handle.status(lt::torrent_handle::query_pieces).pieces.count() == 0,
                    "new episode advertises old have-bits");
            require(disk_bytes(save_path) < uintmax_t(piece_length), "old payload/partfile survived switch");
            // A stale caller stopping the replaced stream cannot stop the new one.
            lt_stop_stream_ex(session, previous, 1);
            require(sw->streams.count(sid), "stale stop deleted the new episode");
        }
        require(lt_get_file_count(session, id) == 10, "switch lost collection metadata");
        for (int piece = first_piece; piece <= last_piece; ++piece)
            handle.add_piece(lt::piece_index_t(piece), content.data());
        wait_until([&] { return handle.status(lt::torrent_handle::query_pieces).pieces.count() == wanted_pieces; },
                   "current episode data did not verify");
        auto status = handle.status(lt::torrent_handle::query_pieces);
        for (int piece = 0; piece < total_pieces; ++piece) {
            require(status.pieces[lt::piece_index_t(piece)] == (piece >= first_piece && piece <= last_piece),
                    "torrent owns data outside the current episode");
        }
        auto* stream = sw->streams.at(sid).get();
        const auto reply = http_request(stream->server_port, "GET", "0-499");
        require(reply.substr(reply.find("\r\n\r\n") + 4) == content.substr(0, 500), "current episode playback failed");
        require_uploadable(handle, wanted_pieces);
        require(disk_bytes(save_path) < content.size() + uintmax_t(piece_length * 2),
                "disk data accumulated across episodes");
        if (episode == 0 || episode == 9)
            test_local_upload(sw, id, torrent_path, folder / ("peer-" + std::to_string(episode)));
        if (episode == 0) {
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

    // Explicit stop-before-start also clears payload but retains the same ID.
    lt_stop_stream_ex(session, previous, 1);
    require(std::string(lt_last_error()).empty(), "metadata-only stop failed");
    require(!previous_handle.is_valid(), "stopped episode can still upload");
    require(sw->streams.empty() && lt_get_file_count(session, id) == 10, "metadata-only retention lost task");
    require(disk_bytes(save_path) < uintmax_t(piece_length), "last episode data survived stop");
    auto restarted = lt_start_stream(session, id, 0, 512 * 1024);
    require(restarted >= 0, "metadata-only task cannot restart");

#ifndef _WIN32
    // A storage permission error must block the next episode, rather than
    // silently leaving the previous episode and continuing to accumulate data.
    if (geteuid() != 0) {
        auto handle = sw->handles.at(id);
        handle.add_piece(lt::piece_index_t(0), content.data());
        wait_until([&] { return handle.have_piece(lt::piece_index_t(0)); }, "failure fixture did not write data");
        const auto permissions = fs::status(save_path).permissions();
        fs::permissions(save_path, fs::perms::owner_read | fs::perms::owner_exec);
        const auto failed = lt_switch_file(session, id, 1, 512 * 1024);
        const std::string failure_message = lt_last_error();
        fs::permissions(save_path, permissions);
        require(failed < 0 && failure_message.find("cannot clear previous episode") != std::string::npos,
                "failed deletion did not block playback");
        require(lt_get_file_count(session, id) == 10 && sw->streams.empty(), "cleanup error lost metadata or left workers");
        require(bool(sw->handles.at(id).flags() & lt::torrent_flags::paused), "failed cleanup continued peer transfers");
        require(lt_start_stream(session, id, 1, 512 * 1024) < 0, "startStream implicitly retried destructive cleanup");
        restarted = lt_switch_file(session, id, 1, 512 * 1024);
        require(restarted >= 0, "cleanup cannot retry with the same torrent ID");
        require(disk_bytes(save_path) < uintmax_t(piece_length), "retry retained failed deletion data");
    }
#endif
    lt_remove_torrent(session, id, 1);
    require(!sw->handles.count(id) && sw->streams.empty(), "final collection cleanup left a task");
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
        test_stream_lifecycle(session, torrent_path, folder / "empty");
        auto cleanup_id = lt_add_torrent_file(session, torrent_path.string().c_str(), folder.string().c_str(), 0);
        auto cleanup_stream = lt_start_stream(session, cleanup_id, 0, 1024 * 1024);
        require(cleanup_stream >= 0, "cleanup stream failed");
        lt_stop_stream(session, cleanup_stream);
        require(!to_sw(session)->handles.count(cleanup_id), "normal mode ignored explicit default deletion policy");
        wait_until([&] { return !fs::exists(folder / "episodes/first.mp4")
                            && !fs::exists(folder / "episodes/second.mp4"); }, "torrent files not deleted");
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
