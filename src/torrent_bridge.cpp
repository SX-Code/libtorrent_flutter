// torrent_bridge.cpp — libtorrent 2.0 streaming engine
// TorrServer-grade streaming: function-by-function C++ port of TorrServer's Go code
// Cross-platform: Windows, Linux, macOS, Android, iOS
//
// Go source mapping:
//   torrstor/ranges.go   → PieceRange, in_ranges(), merge_ranges()
//   torrstor/piece.go    → CachePiece
//   torrstor/mempiece.go → CachePiece (inline memory storage)
//   torrstor/cache.go    → TorrCache
//   torrstor/reader.go   → TorrReader
//   torrstor/storage.go  → integrated into SessionWrapper
//   torr/stream.go       → handle_connection() / serve_range()
//   torr/preload.go      → preload_stream()
//   torr/torrent.go      → StreamEngine lifecycle

#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601
  #endif
#endif

#include "torrent_bridge.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <cstdlib>
#include <sys/stat.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/bdecode.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/version.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/mmap_disk_io.hpp>
#include <libtorrent/posix_disk_io.hpp>

// ── cross-platform sockets ──────────────────────────────────────────────────────
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET socket_t;
  #define SOCKET_INVALID  INVALID_SOCKET
  #define CLOSESOCKET(s)  ::closesocket(s)
  #define INIT_SOCKETS()  { WSADATA _w; ::WSAStartup(MAKEWORD(2,2), &_w); }
  typedef int socklen_t_;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <poll.h>
  #include <signal.h>
  typedef int socket_t;
  #define SOCKET_INVALID  (-1)
  #define CLOSESOCKET(s)  ::close(s)
  #define INIT_SOCKETS()  { signal(SIGPIPE, SIG_IGN); }
  typedef socklen_t socklen_t_;
#endif

#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <map>
#include <cerrno>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <deque>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cinttypes>
#include <functional>
#include <cstdio>
#include <stdexcept>
#include <cstdlib>

namespace lt  = libtorrent;
namespace chr = std::chrono;

// ── debug logging ───────────────────────────────────────────────────────────────
// Off unless TB_LOG_FILE names a file or "stderr". It used to fopen() a hard-coded
// desktop path on every call, under a global lock, on every platform.
static FILE* g_logfile = nullptr;
static std::mutex g_log_mu;
static std::once_flag g_log_once;

static bool tb_log_enabled() {
    std::call_once(g_log_once, [] {
        if (const char* path = std::getenv("TB_LOG_FILE")) {
            if (std::strcmp(path, "stderr") == 0) g_logfile = stderr;
            else {
                g_logfile = fopen(path, "w");
                if (g_logfile) setvbuf(g_logfile, nullptr, _IONBF, 0);
            }
        }
    });
    return g_logfile != nullptr;
}

#define TB_LOG(fmt, ...) do { \
    if (tb_log_enabled()) { \
        std::lock_guard<std::mutex> _lk(g_log_mu); \
        auto _now = chr::steady_clock::now(); \
        auto _ms = chr::duration_cast<chr::milliseconds>(_now.time_since_epoch()).count() % 100000; \
        fprintf(g_logfile, "[%05lld] " fmt "\n", (long long)_ms, ##__VA_ARGS__); \
    } \
} while(0)

// ── error handling ──────────────────────────────────────────────────────────────
static thread_local std::string g_last_error;
static void set_err(const std::string& s) { g_last_error = s; }

#ifdef __ANDROID__
static std::string android_openssl_error() {
    unsigned long code = ERR_get_error();
    if (code == 0) return "unknown OpenSSL error";
    char buf[256] = {0};
    ERR_error_string_n(code, buf, sizeof(buf));
    return std::string(buf);
}

static void validate_android_ca_bundle() {
    const char* path = std::getenv("SSL_CERT_FILE");
    if (!path || !*path) {
        throw std::runtime_error(
            "SSL_CERT_FILE was not set before creating the libtorrent session");
    }

    struct stat st {};
    if (stat(path, &st) != 0 || st.st_size < 100 * 1024) {
        throw std::runtime_error(
            std::string("Android CA bundle is missing or too small: ") + path);
    }

    ERR_clear_error();
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        throw std::runtime_error(
            std::string("SSL_CTX_new failed: ") + android_openssl_error());
    }

    const int loaded = SSL_CTX_load_verify_locations(ctx, path, nullptr);
    SSL_CTX_free(ctx);
    if (loaded != 1) {
        throw std::runtime_error(
            std::string("OpenSSL could not load Android CA bundle '") + path
            + "': " + android_openssl_error());
    }

    __android_log_print(
        ANDROID_LOG_INFO,
        "libtorrent_native",
        "Validated OpenSSL CA bundle: %s (%lld bytes)",
        path,
        static_cast<long long>(st.st_size));
}
#endif

// ── file extension checks ───────────────────────────────────────────────────────
static bool is_streamable(const std::string& name) {
    static const char* exts[] = {
        ".mkv",".mp4",".avi",".mov",".wmv",".flv",".webm",
        ".m4v",".ts",".m2ts",".mpg",".mpeg",".mp3",".aac",
        ".flac",".opus",".ogg",".wav", nullptr
    };
    auto d = name.rfind('.');
    if (d == std::string::npos) return false;
    std::string e = name.substr(d);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    for (int i = 0; exts[i]; ++i) if (e == exts[i]) return true;
    return false;
}

// ── port of server/mimetype — MimeTypeByPath ────────────────────────────────────
static std::string get_mime(const std::string& name) {
    auto d = name.rfind('.');
    if (d == std::string::npos) return "video/mp4";
    std::string e = name.substr(d);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    if (e == ".mkv")  return "video/x-matroska";
    if (e == ".mp4" || e == ".m4v") return "video/mp4";
    if (e == ".avi")  return "video/x-msvideo";
    if (e == ".mov")  return "video/quicktime";
    if (e == ".webm") return "video/webm";
    if (e == ".ts" || e == ".m2ts") return "video/mp2t";
    if (e == ".flv")  return "video/x-flv";
    if (e == ".wmv")  return "video/x-ms-wmv";
    if (e == ".mpg" || e == ".mpeg") return "video/mpeg";
    if (e == ".mp3")  return "audio/mpeg";
    if (e == ".flac") return "audio/flac";
    if (e == ".aac")  return "audio/aac";
    if (e == ".ogg" || e == ".opus") return "audio/ogg";
    if (e == ".wav")  return "audio/wav";
    if (e == ".m4a" || e == ".m4b" || e == ".m4p") return "audio/mp4";
    return "application/octet-stream";
}

// ── fill torrent status struct ──────────────────────────────────────────────────
static void fill_status(lt_torrent_status& out, int64_t id,
                        const lt::torrent_status& st)
{
    out.id    = id;
    // The public C/Dart enum is not libtorrent's enum: libtorrent reserves
    // value 0 and uses 1..7. Translate explicitly across the ABI boundary.
    switch (st.state) {
        case lt::torrent_status::checking_files: out.state = LT_STATE_CHECKING_FILES; break;
        case lt::torrent_status::downloading_metadata: out.state = LT_STATE_DOWNLOADING_META; break;
        case lt::torrent_status::downloading: out.state = LT_STATE_DOWNLOADING; break;
        case lt::torrent_status::finished: out.state = LT_STATE_FINISHED; break;
        case lt::torrent_status::seeding: out.state = LT_STATE_SEEDING; break;
#if TORRENT_ABI_VERSION == 1
        case lt::torrent_status::allocating: out.state = LT_STATE_ALLOCATING; break;
#endif
        case lt::torrent_status::checking_resume_data: out.state = LT_STATE_CHECKING_RESUME; break;
        default: out.state = LT_STATE_UNKNOWN; break;
    }

    if ((st.state == lt::torrent_status::finished ||
         st.state == lt::torrent_status::seeding) && st.progress < 0.999f)
        out.state = LT_STATE_DOWNLOADING;

    out.progress      = st.progress;
    out.download_rate = st.download_rate;
    out.upload_rate   = st.upload_rate;
    out.total_done    = st.total_done;
    out.total_wanted  = st.total_wanted;
    out.total_uploaded = st.total_payload_upload;
    out.num_peers     = st.num_peers;
    out.num_seeds     = st.num_seeds;
    out.num_pieces    = (int32_t)st.num_pieces;

    int have = 0;
    if (st.has_metadata && !st.pieces.empty() && st.pieces.size() > 0) {
        int sz = static_cast<int>(st.pieces.size());
        for (int i = 0; i < sz; ++i) {
            try {
                if (st.pieces.get_bit(lt::piece_index_t(i))) have++;
            } catch (...) { break; }
        }
    }
    out.pieces_done = (int32_t)have;

    out.is_paused   = (st.flags & lt::torrent_flags::paused) ? 1 : 0;
    out.is_finished = (st.progress >= 0.999f && st.is_finished) ? 1 : 0;
    out.has_metadata = st.has_metadata ? 1 : 0;

    int qp = static_cast<int>(st.queue_position);
    out.queue_position = (qp < 0) ? -1 : qp;

    std::string name = st.name;
    if (st.has_metadata) {
        auto ti = st.handle.torrent_file();
        if (ti) name = ti->name();
    }
    std::strncpy(out.name, name.c_str(), sizeof(out.name) - 1);
    out.name[sizeof(out.name) - 1] = 0;

    std::strncpy(out.save_path, st.save_path.c_str(), sizeof(out.save_path) - 1);
    out.save_path[sizeof(out.save_path) - 1] = 0;

    if (st.errc) {
        std::string e = st.errc.message();
        std::strncpy(out.error_msg, e.c_str(), sizeof(out.error_msg) - 1);
        out.error_msg[sizeof(out.error_msg) - 1] = 0;
        out.state = LT_STATE_ERROR;
    } else {
        out.error_msg[0] = 0;
    }
}

// ── read result for async piece reads ───────────────────────────────────────────
struct ReadResult {
    std::vector<char> data;
    bool ok = false;
};

// ── alert record for dart queue ─────────────────────────────────────────────────
struct AlertRecord {
    int           type;
    lt_torrent_id torrent_id;
    std::string   message;
};

// ============================================================================
// PORT OF torrstor/ranges.go
// ============================================================================

// Range — port of torrstor.Range
struct PieceRange {
    int     start = 0;
    int     end_  = 0;
    int     file_index = -1;     // which file this range is for
    int64_t file_offset = 0;     // file's byte offset in torrent
    int64_t file_length = 0;     // file's total byte length
};

// inRanges — port of torrstor.inRanges
static bool in_ranges(const std::vector<PieceRange>& ranges, int ind) {
    for (auto& r : ranges) {
        if (ind >= r.start && ind <= r.end_)
            return true;
    }
    return false;
}

// mergeRange — port of torrstor.mergeRange
static std::vector<PieceRange> merge_ranges(std::vector<PieceRange> ranges) {
    if (ranges.size() <= 1) return ranges;

    std::sort(ranges.begin(), ranges.end(), [](const PieceRange& a, const PieceRange& b) {
        if (a.start < b.start) return true;
        if (a.start == b.start && a.end_ < b.end_) return true;
        return false;
    });

    int j = 0;
    for (int i = 1; i < (int)ranges.size(); ++i) {
        if (ranges[j].end_ >= ranges[i].start) {
            if (ranges[j].end_ < ranges[i].end_)
                ranges[j].end_ = ranges[i].end_;
        } else {
            ++j;
            ranges[j] = ranges[i];
        }
    }
    ranges.resize(j + 1);
    return ranges;
}

// ============================================================================
// PORT OF torrstor/piece.go + torrstor/mempiece.go (merged)
// ============================================================================

struct TorrCache; // forward decl

// CachePiece — port of torrstor.Piece + torrstor.MemPiece (memory-only mode)
// Each piece has its own buffer, size tracking, completion flag, access timestamp
struct CachePiece {
    int     id = 0;
    int64_t size = 0;
    bool    complete = false;
    int64_t accessed = 0;   // unix timestamp — port of Piece.Accessed

    std::vector<char> buffer;  // port of MemPiece.buffer
    std::shared_mutex mu;      // port of MemPiece.mu (RWMutex)

    TorrCache* cache = nullptr;

    CachePiece() = default;
    CachePiece(int piece_id, TorrCache* c) : id(piece_id), cache(c) {}

    // port of MemPiece.WriteAt
    int write_at(const char* b, int len, int64_t off);

    // port of MemPiece.ReadAt
    int read_at(char* b, int len, int64_t off);

    // port of Piece.MarkComplete
    void mark_complete() { complete = true; }

    // port of Piece.MarkNotComplete
    void mark_not_complete() { complete = false; }

    // port of MemPiece.Release + Piece.Release
    void release();
};

// ============================================================================
// PORT OF torrstor/reader.go
// ============================================================================

// TorrReader — port of torrstor.Reader
// Tracks a single reader's position, readahead, and active state
struct TorrReader {
    int     reader_id = 0;
    int     request_first_piece = 0;
    int     request_last_piece = 0;
    int     priority_hi = -1;
    int     deadline_hi = -1;
    int64_t offset = 0;          // port of Reader.offset
    int64_t readahead = 0;       // port of Reader.readahead
    bool    is_closed = false;   // port of Reader.isClosed
    int64_t last_access = 0;     // port of Reader.lastAccess (unix timestamp)
    bool    is_use = true;       // port of Reader.isUse

    // file info for this reader
    int     file_index = -1;
    int64_t file_offset = 0;     // byte offset of file in torrent
    int64_t file_length = 0;     // file length

    TorrCache* cache = nullptr;
    std::mutex mu;               // port of Reader.mu

    TorrReader() = default;

    // port of Reader.getPieceNum
    int get_piece_num(int64_t off) const;

    // port of Reader.getReaderPiece
    int get_reader_piece() const { return get_piece_num(offset); }

    // port of Reader.getReaderRAHPiece
    int get_reader_rah_piece() const { return get_piece_num(offset + readahead); }

    // port of Reader.getOffsetRange
    void get_offset_range(int64_t& begin_off, int64_t& end_off) const;

    // port of Reader.getPiecesRange
    PieceRange get_pieces_range() const;

    // port of Reader.checkReader — auto-disable idle readers
    void check_reader();

    // port of Reader.readerOn
    void reader_on();

    // port of Reader.readerOff
    void reader_off();

    // port of Reader.getUseReaders
    int get_use_readers() const;

    // port of Reader.SetReadahead
    void set_readahead(int64_t length);

    // port of Reader.Close
    void close();

    static int64_t now_unix() {
        return (int64_t)chr::duration_cast<chr::seconds>(
            chr::system_clock::now().time_since_epoch()).count();
    }
};

// ============================================================================
// PORT OF torrstor/cache.go — the core streaming cache
// ============================================================================

struct TorrCache {
    std::atomic<int64_t> capacity{0};        // port of Cache.capacity (bytes)
    int64_t filled = 0;          // port of Cache.filled
    int64_t piece_length = 0;    // port of Cache.pieceLength
    int     piece_count = 0;     // port of Cache.pieceCount

    std::unordered_map<int, std::unique_ptr<CachePiece>> pieces; // port of Cache.pieces

    std::unordered_map<int, TorrReader*> readers;  // port of Cache.readers (reader_id → reader)
    mutable std::mutex mu_readers;                 // port of Cache.muReaders

    std::atomic<bool> is_remove{false};  // port of Cache.isRemove
    std::atomic<bool> is_closed{false};  // port of Cache.isClosed
    std::mutex mu_remove;                // port of Cache.muRemove

    lt::torrent_handle handle;  // replaces Cache.torrent (anacrolix *torrent.Torrent)

    // TorrServer settings — port of settings.BTsets fields
    int reader_read_ahead_pct = 95;  // port of BTsets.ReaderReadAHead (5-100%)
    int connections_limit = 25;      // port of BTsets.ConnectionsLimit

    // ── port of Cache.Init ──
    void init(int64_t cap, int64_t pl, int pc, const lt::torrent_handle& h) {
        capacity = cap;
        if (capacity == 0) capacity = pl * 4;
        piece_length = pl;
        piece_count = pc;
        handle = h;

        for (int i = 0; i < pc; ++i)
            pieces[i] = std::make_unique<CachePiece>(i, this);
    }

    // ── port of Cache.Piece — get piece by index ──
    CachePiece* get_piece(int index) {
        auto it = pieces.find(index);
        return it != pieces.end() ? it->second.get() : nullptr;
    }

    // ── port of Cache.removePiece ──
    void remove_piece(CachePiece* piece) {
        if (!is_closed.load())
            piece->release();
    }

    // ── port of Cache.AdjustRA ──
    void adjust_readahead(int64_t ra) {
        if (capacity == 0) capacity = ra * 3;
        std::lock_guard<std::mutex> lk(mu_readers);
        for (auto& kv : readers) {
            if (kv.second) kv.second->set_readahead(ra);
        }
    }

    // ── port of Cache.cleanPieces ──
    // DISABLED: serve_range handles piece lifecycle directly via
    // libtorrent priorities. Cache eviction would override serve_range's
    // focused priorities and stop downloads.
    void clean_pieces() { return; }

    // ── port of Cache.getRemPieces ──
    std::vector<CachePiece*> get_removable_pieces() {
        std::vector<CachePiece*> pieces_remove;
        int64_t fill = 0;

        // collect ranges from all active readers
        std::vector<PieceRange> ranges;
        {
            std::lock_guard<std::mutex> lk(mu_readers);
            for (auto& kv : readers) {
                auto* r = kv.second;
                if (!r) continue;
                r->check_reader();
                if (r->is_use)
                    ranges.push_back(r->get_pieces_range());
            }
        }
        ranges = merge_ranges(ranges);

        // collect the exact pieces each reader is currently on — NEVER evict these.
        // Also protect the NEXT piece (reader_piece + 1) to prevent a tight
        // evict-download-evict loop at piece boundaries where the reader is
        // about to cross into the next piece.
        std::unordered_set<int> current_reader_pieces;
        {
            std::lock_guard<std::mutex> lk2(mu_readers);
            for (auto& kv2 : readers) {
                auto* r2 = kv2.second;
                if (r2 && r2->is_use) {
                    int rp = r2->get_reader_piece();
                    current_reader_pieces.insert(rp);
                    current_reader_pieces.insert(rp + 1); // protect next piece too
                }
            }
        }

        // find removable pieces — port of the Go loop in getRemPieces
        for (auto& kv : pieces) {
            int id = kv.first;
            auto* p = kv.second.get();
            if (p->size > 0)
                fill += p->size;

            // NEVER evict the piece the reader is currently sitting on
            if (current_reader_pieces.count(id)) continue;

            if (!ranges.empty()) {
                if (!in_ranges(ranges, id)) {
                    if (p->size > 0 && !is_in_file_begin_end(ranges, id))
                        pieces_remove.push_back(p);
                }
            } else {
                // no readers (preload clean mode)
                if (p->size > 0 && !is_in_file_begin_end(ranges, id))
                    pieces_remove.push_back(p);
            }
        }

        clear_priority_impl();
        set_load_priority(ranges);

        // sort by access time — oldest first (LRU). Port of sort.Slice in Go
        std::sort(pieces_remove.begin(), pieces_remove.end(),
            [](const CachePiece* a, const CachePiece* b) {
                return a->accessed < b->accessed;
            });

        filled = fill;
        return pieces_remove;
    }

    // ── port of Cache.setLoadPriority ──
    // DISABLED: serve_range handles all piece prioritization directly
    // with a focused 2-piece-ahead window. The old TorrServer priority
    // gradient (Now/Next/Readahead/High/Normal for 25 pieces) spread
    // bandwidth too thin and conflicted with serve_range's priorities.
    void set_load_priority(const std::vector<PieceRange>& /*ranges*/) { return; }

    // ── port of Cache.isIdInFileBE ──
    // Protects first/last 8-16MB of each file from eviction
    bool is_in_file_begin_end(const std::vector<PieceRange>& ranges, int id) const {
        // keep 8/16 MB — port of FileRangeNotDelete
        int64_t file_range_not_delete = piece_length;
        if (file_range_not_delete < 8 * 1024 * 1024)
            file_range_not_delete = 8 * 1024 * 1024;

        for (auto& rng : ranges) {
            int ss = (int)(rng.file_offset / piece_length);
            int se = (int)((rng.file_offset + file_range_not_delete) / piece_length);
            int es = (int)((rng.file_offset + rng.file_length - file_range_not_delete) / piece_length);
            int ee = (int)((rng.file_offset + rng.file_length) / piece_length);

            if ((id >= ss && id < se) || (id > es && id <= ee))
                return true;
        }
        return false;
    }

    // ── port of Cache.NewReader ──
    TorrReader* new_reader(int reader_id, int file_idx, int64_t file_off, int64_t file_len) {
        auto* r = new TorrReader();
        r->reader_id = reader_id;
        r->file_index = file_idx;
        r->file_offset = file_off;
        r->file_length = file_len;
        r->cache = this;
        r->is_use = true;
        r->set_readahead(0);
        r->last_access = TorrReader::now_unix();

        std::lock_guard<std::mutex> lk(mu_readers);
        readers[reader_id] = r;
        return r;
    }

    // ── port of Cache.GetUseReaders ──
    int get_use_readers() const {
        std::lock_guard<std::mutex> lk(mu_readers);
        int count = 0;
        for (auto& kv : readers)
            if (kv.second && kv.second->is_use) count++;
        return count;
    }

    // ── port of Cache.Readers ──
    int reader_count() const {
        std::lock_guard<std::mutex> lk(mu_readers);
        return (int)readers.size();
    }

    // ── port of Cache.CloseReader ──
    void close_reader(TorrReader* r) {
        if (!r) return;
        if (is_closed.load()) return;  // cache already closed, reader already freed
        try {
            {
                std::lock_guard<std::mutex> lk(mu_readers);
                r->close();
                readers.erase(r->reader_id);
            }
            delete r;
        } catch (...) {
            // Never crash on reader cleanup — reader may already be freed
        }
        // REMOVED: detached thread that called clear_priority_impl() after
        // 1-second delay. When no readers exist (gap between player
        // connections), this set ALL pieces to dont_download, killing
        // all downloads. serve_range handles priorities directly.
    }

    // ── port of Cache.clearPriority ──
    // DISABLED: serve_range handles all piece prioritization directly.
    // The old code set ALL pieces to dont_download when no readers exist,
    // which killed downloads during gaps between player HTTP requests.
    void clear_priority_impl() { return; }

    // ── port of Cache.Close ──
    void close() {
        if (is_closed.load()) return;  // already closed
        is_closed.store(true);
        try {
            std::lock_guard<std::mutex> lk(mu_readers);
            for (auto& kv : readers) {
                try { delete kv.second; } catch (...) {}
            }
            readers.clear();
            pieces.clear();
        } catch (...) {
            // Never crash during cache teardown
        }
    }

    // ── port of Cache.GetCapacity ──
    int64_t get_capacity() const { return capacity; }

    // ── port of Cache.GetState — cache telemetry ──
    void get_state(int64_t& out_capacity, int64_t& out_filled, int& out_pieces_count,
                   int& out_readers) const {
        int64_t fill = 0;
        for (auto& kv : pieces)
            if (kv.second->size > 0) fill += kv.second->size;
        out_capacity = capacity;
        out_filled = fill;
        out_pieces_count = piece_count;
        out_readers = reader_count();
    }
};

// ── CachePiece method implementations (need TorrCache to be defined) ──────────

// port of MemPiece.WriteAt
int CachePiece::write_at(const char* b, int len, int64_t off) {
    std::unique_lock<std::shared_mutex> lk(mu);

    if (buffer.empty()) {
        // clean_pieces call removed — serve_range handles piece lifecycle
        buffer.resize((size_t)cache->piece_length, 0);
    }

    if (off < 0 || (size_t)off >= buffer.size()) return 0;
    int n = std::min(len, (int)(buffer.size() - (size_t)off));
    std::memcpy(buffer.data() + off, b, (size_t)n);
    size += (int64_t)n;
    if (size > cache->piece_length) size = cache->piece_length;
    accessed = TorrReader::now_unix();
    return n;
}

// port of MemPiece.ReadAt
int CachePiece::read_at(char* b, int len, int64_t off) {
    std::shared_lock<std::shared_mutex> lk(mu);

    if (buffer.empty()) return -1; // EOF equivalent

    int avail = (int)buffer.size() - (int)off;
    if (avail <= 0) return -1;
    int n = std::min(len, avail);
    std::memcpy(b, buffer.data() + off, (size_t)n);
    accessed = TorrReader::now_unix();

    // clean_pieces spawn removed — serve_range handles piece lifecycle
    return n;
}

// port of Piece.Release + MemPiece.Release
void CachePiece::release() {
    {
        std::unique_lock<std::shared_mutex> lk(mu);
        buffer.clear();
        buffer.shrink_to_fit();
        size = 0;
        complete = false;
    }
    // REMOVED: setting piece to dont_download. serve_range handles
    // piece lifecycle and priority directly. The old code could nuke
    // pieces that serve_range was actively waiting for.
}

// ── TorrReader method implementations (need TorrCache to be defined) ──────────

// port of Reader.getPieceNum
int TorrReader::get_piece_num(int64_t off) const {
    if (!cache || cache->piece_length <= 0) return 0;
    return (int)((off + file_offset) / cache->piece_length);
}

// port of Reader.getOffsetRange
// 100% forward cache — everything ahead of playback, nothing behind.
// This means even a 64 MB cache can stream a 60 GB file: we only
// keep upcoming data and aggressively evict what's already been played.
void TorrReader::get_offset_range(int64_t& begin_off, int64_t& end_off) const {
    int64_t num_readers = (int64_t)get_use_readers();
    if (num_readers == 0) num_readers = 1;

    begin_off = offset;  // nothing kept behind playback position
    end_off   = offset + (cache->capacity / num_readers);

    if (begin_off < 0) begin_off = 0;
    if (end_off > file_length) end_off = file_length;
}

// port of Reader.getPiecesRange
PieceRange TorrReader::get_pieces_range() const {
    int64_t start_off, end_off;
    get_offset_range(start_off, end_off);
    PieceRange r;
    r.start = get_piece_num(start_off);
    r.end_  = get_piece_num(end_off);
    r.file_index = file_index;
    r.file_offset = file_offset;
    r.file_length = file_length;
    return r;
}

// port of Reader.checkReader — disable idle readers when others exist
void TorrReader::check_reader() {
    if (!cache) return;
    int64_t now = now_unix();
    if (now > last_access + 60 && cache->readers.size() > 1) {
        reader_off();
    } else {
        reader_on();
    }
}

// port of Reader.readerOn
void TorrReader::reader_on() {
    std::lock_guard<std::mutex> lk(mu);
    if (!is_use) {
        is_use = true;
        // readahead is restored in the cache's priority recalculation
    }
}

// port of Reader.readerOff
void TorrReader::reader_off() {
    std::lock_guard<std::mutex> lk(mu);
    if (is_use) {
        readahead = 0;
        is_use = false;
    }
}

// port of Reader.getUseReaders
int TorrReader::get_use_readers() const {
    if (!cache) return 0;
    int count = 0;
    for (auto& kv : cache->readers)
        if (kv.second && kv.second->is_use) count++;
    return count;
}

// port of Reader.SetReadahead
void TorrReader::set_readahead(int64_t length) {
    if (cache && length > cache->capacity)
        length = cache->capacity;
    readahead = length;
}

// port of Reader.Close
void TorrReader::close() {
    is_closed = true;
    // REMOVED: detached thread that called get_removable_pieces()
    // which reset priorities and could nuke pieces serve_range needs.
}

// ============================================================================
// PORT OF torr/stream.go — StreamEngine with TorrCache
// ============================================================================

// A torrent has one libtorrent picker, but each HTTP stream owns its demands.
// All picker writes are serialized here; withdrawing one owner cannot cancel
// another owner's requests. Deadlines are absolute so unrelated updates never
// push an existing playback deadline into the future.
struct PieceDemand {
    int priority = 0;
    chr::steady_clock::time_point deadline = chr::steady_clock::time_point::max();
};

class StreamScheduler {
public:
    explicit StreamScheduler(lt::torrent_handle handle, bool ordinary_download = false) : handle_(handle) {
        const auto ti = handle_.torrent_file();
        file_priorities_.assign(size_t(ti->num_files()), lt::dont_download);
        if (ordinary_download) {
            file_priorities_ = handle_.get_file_priorities();
            file_priorities_.resize(size_t(ti->num_files()), lt::default_priority);
        }
        rebuild_base_locked();
    }

    // A file selection owns a baseline independently of HTTP readers/streams.
    void download_file(int index) {
        std::lock_guard<std::mutex> lock(mu_);
        auto& priority = file_priorities_.at(size_t(index));
        if (priority >= lt::default_priority) return;
        priority = lt::default_priority;
        rebuild_base_locked();
        // Materialize selected files instead of leaving their bytes in partfile.
        // file_prio_alert reapplies the aggregate after this async disk operation.
        handle_.prioritize_files(file_priorities_);
        reapply_locked();
    }

    void select_files(const std::vector<lt::download_priority_t>& priorities,
                      const std::set<int>& paused = {}) {
        std::lock_guard<std::mutex> lock(mu_);
        file_priorities_ = priorities;
        paused_files_ = paused;
        rebuild_base_locked();
        handle_.prioritize_files(file_priorities_);
        reapply_locked();
    }

    std::vector<lt::download_priority_t> download_priorities() {
        std::lock_guard<std::mutex> lock(mu_);
        return file_priorities_;
    }

    // Keep disk/file selection intact: pause only withdraws the piece baseline.
    // Public callers serialize this with stream creation through lifecycle_mu.
    void set_download_paused(int index, bool paused) {
        std::lock_guard<std::mutex> lock(mu_);
        if (index < 0 || size_t(index) >= file_priorities_.size()
            || file_priorities_[index] == lt::dont_download)
            throw std::invalid_argument("file is not selected for download");
        const bool changed = paused ? paused_files_.insert(index).second : paused_files_.erase(index) != 0;
        if (!changed) return;
        rebuild_base_locked();
        reapply_locked();
    }

    // Deselect without removing streams or deleting their on-disk data.
    void cancel_download(int index) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto ti = handle_.torrent_file();
        if (index < 0 || size_t(index) >= file_priorities_.size()
            || ti->files().pad_file_at(lt::file_index_t(index)))
            throw std::invalid_argument("invalid download file index");
        if (file_priorities_[index] == lt::dont_download) return;
        file_priorities_[index] = lt::dont_download;
        paused_files_.erase(index);
        rebuild_base_locked();
        // Async file_prio_alert must reapply the remaining playback/other-file
        // demands after libtorrent updates storage and resets piece priorities.
        handle_.prioritize_files(file_priorities_);
        reapply_locked();
    }

    std::set<int> paused_downloads() {
        std::lock_guard<std::mutex> lock(mu_);
        return paused_files_;
    }

    bool has_downloads() {
        std::lock_guard<std::mutex> lock(mu_);
        return std::any_of(file_priorities_.begin(), file_priorities_.end(),
            [](auto priority) { return priority != lt::dont_download; });
    }

    void add_stream(lt_stream_id id) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!initialized_) reapply_locked();
        demands_.emplace(std::make_pair(id, 0), std::unordered_map<int, PieceDemand>{});
    }

    bool add_reader(lt_stream_id id, int reader) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!demands_.count({id, 0})) return false;
        demands_.emplace(std::make_pair(id, reader), std::unordered_map<int, PieceDemand>{});
        return true;
    }

    void request(lt_stream_id id, int piece, int priority, int deadline_ms = -1, int reader = 0) {
        std::lock_guard<std::mutex> lock(mu_);
        auto owner = demands_.find({id, reader});
        if (owner == demands_.end()) return; // stopped workers cannot re-register
        auto& demand = owner->second[piece];
        demand.priority = std::max(demand.priority, priority);
        if (deadline_ms >= 0) {
            demand.deadline = std::min(demand.deadline,
                chr::steady_clock::now() + chr::milliseconds(deadline_ms));
        }
        apply_locked(piece);
    }

    void release(lt_stream_id id, int piece, int reader = 0) {
        std::lock_guard<std::mutex> lock(mu_);
        auto owner = demands_.find({id, reader});
        if (owner != demands_.end() && owner->second.erase(piece)) apply_locked(piece);
    }

    void retain(lt_stream_id id, const std::function<bool(int)>& keep, int reader = 0) {
        std::lock_guard<std::mutex> lock(mu_);
        auto owner = demands_.find({id, reader});
        if (owner == demands_.end()) return;
        std::vector<int> removed;
        for (auto it = owner->second.begin(); it != owner->second.end();) {
            if (keep(it->first)) { ++it; continue; }
            removed.push_back(it->first);
            it = owner->second.erase(it);
        }
        for (int p : removed) apply_locked(p);
    }

    void remove_reader(lt_stream_id id, int reader) {
        std::lock_guard<std::mutex> lock(mu_);
        auto owner = demands_.find({id, reader});
        if (owner == demands_.end()) return;
        auto removed = std::move(owner->second);
        demands_.erase(owner);
        for (const auto& entry : removed) apply_locked(entry.first);
    }

    void remove_stream(lt_stream_id id) {
        std::lock_guard<std::mutex> lock(mu_);
        std::unordered_set<int> removed;
        for (auto it = demands_.begin(); it != demands_.end();) {
            if (it->first.first != id) { ++it; continue; }
            for (const auto& entry : it->second) removed.insert(entry.first);
            it = demands_.erase(it);
        }
        for (int piece : removed) apply_locked(piece);
    }

    bool owns(const lt::torrent_handle& handle) const { return handle_ == handle; }

    // File selection is asynchronous in libtorrent. If a selection submitted
    // before streaming completes later, restore the active streams' aggregate.
    void reapply() {
        std::lock_guard<std::mutex> lock(mu_);
        reapply_locked();
    }

private:
    void reapply_locked() {
        initialized_ = true;
        auto ti = handle_.torrent_file();
        auto status = handle_.status(lt::torrent_handle::query_pieces);
        std::vector<std::pair<lt::piece_index_t, lt::download_priority_t>> priorities;
        for (int p = 0; ti && p < ti->num_pieces(); ++p) {
            if (p < status.pieces.size() && status.pieces[lt::piece_index_t(p)]) continue;
            priorities.emplace_back(lt::piece_index_t(p), lt::download_priority_t(merged_locked(p).priority));
        }
        handle_.prioritize_pieces(priorities);
        std::unordered_set<int> pieces;
        for (const auto& owner : demands_)
            for (const auto& entry : owner.second) pieces.insert(entry.first);
        for (int piece : pieces) apply_locked(piece);
    }

public:
    void shutdown() {
        std::lock_guard<std::mutex> lock(mu_);
        std::unordered_set<int> pieces;
        for (const auto& owner : demands_)
            for (const auto& entry : owner.second) pieces.insert(entry.first);
        demands_.clear(); // late worker updates can no longer find an owner
        file_priorities_.assign(file_priorities_.size(), lt::dont_download);
        base_priorities_.assign(base_priorities_.size(), 0);
        for (int piece : pieces) apply_locked(piece);
    }

    void refresh(int piece) {
        std::lock_guard<std::mutex> lock(mu_);
        apply_locked(piece);
    }

    // Also used by regression tests to inspect the merged scheduling contract.
    PieceDemand demand(int piece) {
        std::lock_guard<std::mutex> lock(mu_);
        return merged_locked(piece);
    }

private:
    void rebuild_base_locked() {
        const auto ti = handle_.torrent_file();
        const auto& files = ti->files();
        base_priorities_.assign(size_t(ti->num_pieces()), 0);
        for (int file = 0; file < files.num_files(); ++file) {
            const auto index = lt::file_index_t(file);
            if (files.pad_file_at(index)) { file_priorities_[file] = lt::dont_download; continue; }
            const int priority = static_cast<std::uint8_t>(file_priorities_[file]);
            const auto size = files.file_size(index);
            if (!priority || size <= 0 || paused_files_.count(file)) continue;
            const auto offset = files.file_offset(index);
            const int first = int(offset / ti->piece_length());
            const int last = int((offset + size - 1) / ti->piece_length());
            for (int piece = first; piece <= last; ++piece)
                base_priorities_[piece] = std::max(base_priorities_[piece], priority);
        }
    }

    PieceDemand merged_locked(int piece) const {
        PieceDemand merged;
        if (piece >= 0 && size_t(piece) < base_priorities_.size()) merged.priority = base_priorities_[piece];
        for (const auto& owner : demands_) {
            auto it = owner.second.find(piece);
            if (it == owner.second.end()) continue;
            merged.priority = std::max(merged.priority, it->second.priority);
            merged.deadline = std::min(merged.deadline, it->second.deadline);
        }
        return merged;
    }

    void apply_locked(int piece) {
        const auto merged = merged_locked(piece);
        const auto index = lt::piece_index_t(piece);
        // Download selection never changes ownership of verified disk pieces.
        // Leave their priorities alone and remove obsolete time-critical picks.
        if (handle_.have_piece(index)) {
            handle_.reset_piece_deadline(index);
            return;
        }
        if (merged.deadline == chr::steady_clock::time_point::max()) {
            handle_.reset_piece_deadline(index);
        } else {
            const auto remaining = chr::duration_cast<chr::milliseconds>(
                merged.deadline - chr::steady_clock::now()).count();
            handle_.set_piece_deadline(index, int(std::max<int64_t>(0, remaining)));
        }
        handle_.piece_priority(index, lt::download_priority_t(merged.priority));
    }

    std::mutex mu_;
    lt::torrent_handle handle_;
    bool initialized_ = false;
    std::vector<lt::download_priority_t> file_priorities_;
    std::vector<int> base_priorities_;
    std::set<int> paused_files_;
    // Reader 0 owns startup/preload requests. Positive IDs own HTTP GETs.
    std::map<std::pair<lt_stream_id, int>, std::unordered_map<int, PieceDemand>> demands_;
};

struct StreamEngine {
    std::shared_ptr<StreamScheduler> scheduler;
    lt_stream_id  id;
    lt_torrent_id torrent_id;
    int           file_index;

    lt::torrent_handle                      handle;
    std::shared_ptr<const lt::torrent_info> ti;

    int64_t file_offset;
    int64_t file_size;
    int     piece_length;
    int     start_piece;
    int     end_piece;
    int     total_pieces;

    // ── TorrServer cache (replaces old piece_cache) ──
    std::unique_ptr<TorrCache> cache;

    // playback
    std::atomic<int64_t> read_head{0};
    std::atomic<int32_t> stream_state{LT_STREAM_BUFFERING};
    // HTTP offsets are not user seek signals. Only one active reader supplies
    // telemetry; independent probes must not change another reader's status.
    struct HttpPosition { int64_t cursor; bool waiting; };
    std::mutex http_mu;
    std::map<int, HttpPosition> http_readers;
    int primary_reader = -1;

    // readahead fixed at 16MB like TorrServer — port of torrent.go updateRA()
    static constexpr int64_t FIXED_READAHEAD = 16 * 1024 * 1024;

    // 8MB head/tail for container metadata — port of isIdInFileBE's FileRangeNotDelete
    static constexpr int64_t PROTECT_BYTES = 8 * 1024 * 1024;
    int head_end_piece;
    int tail_start_piece;

    // HTTP server
    std::thread       server_thread;
    std::atomic<bool> running{false};
    socket_t          listen_sock = SOCKET_INVALID;
    int               server_port = 0;

    // concurrent stream counter — port of stream.go activeStreams
    static std::atomic<int32_t> active_streams;

    // active clients — supports multiple concurrent connections
    // port of TorrServer's multi-reader design
    std::mutex                         clients_mu;
    std::unordered_map<int, std::thread> client_threads;
    std::unordered_map<int, socket_t>    client_sockets;   // track sockets for forced shutdown
    std::unordered_set<int>              finished_clients;  // signal thread completion
    std::atomic<int> next_reader_id{1};

    // piece availability — signaled by alert thread on piece_finished
    std::mutex              piece_mu;
    std::condition_variable piece_cv;
    std::set<int>           pieces_have;

    // piece data reads — signaled by alert thread on read_piece_alert
    std::mutex              read_mu;
    std::condition_variable read_cv;
    std::unordered_map<int, ReadResult> read_results;
    // Guarded by SessionWrapper::streams_mu. Startup probes schedule disk I/O
    // asynchronously and share a throttle across all Dart waiters.
    chr::steady_clock::time_point next_start_read{};
    std::atomic<bool> start_piece_readable{false};

    // preload state — port of torrent.go PreloadSize/PreloadedBytes
    std::atomic<int64_t> preload_size{0};
    std::atomic<int64_t> preloaded_bytes{0};
    std::atomic<bool>    preloading{false};
    std::thread          preload_thread;

    // speed tracking — port of torrent.go progressEvent
    std::atomic<double> download_speed{0};
    std::atomic<double> upload_speed{0};

    // trailing retention — keep last N played pieces for re-reads/rewinds
    static constexpr int TRAILING_WINDOW = 3;

    // Hot-cache bookkeeping. Every read_piece_alert copies a whole piece into
    // RAM (cache buffer + read_results). Without a hard bound, prefetched,
    // tail and seek-orphaned pieces were never freed and a full movie slowly
    // accumulated in RAM until the low-memory killer took the app mid-movie.
    std::mutex    cached_mu;
    std::set<int> cached_pieces;

    int head_piece() const {
        return std::clamp(byte_to_piece(read_head.load()), start_piece, end_piece);
    }

    void release_cached_piece(int p) {
        if (!cache) return;
        if (auto* cp = cache->get_piece(p)) {
            std::unique_lock<std::shared_mutex> lk(cp->mu);
            cp->buffer.clear();
            cp->buffer.shrink_to_fit();
            cp->size = 0;
            cp->complete = false;
        }
    }

    bool has_cached_piece(int p) {
        if (!cache) return false;
        auto* cp = cache->get_piece(p);
        if (!cp) return false;
        std::shared_lock<std::shared_mutex> lk(cp->mu);
        return !cp->buffer.empty() && cp->complete;
    }

    void drop_cached(int p) {
        std::lock_guard<std::mutex> lk(cached_mu);
        cached_pieces.erase(p);
        release_cached_piece(p);
    }

    // Keep at most max(4, capacity / piece_length) pieces in RAM. Evicts the
    // piece farthest from the playhead, preferring pieces already played.
    void enforce_cache_budget() {
        if (!cache || piece_length <= 0) return;
        const size_t max_pieces = (size_t)std::max<int64_t>(
            4, cache->capacity / piece_length);
        {
            std::lock_guard<std::mutex> lk(cached_mu);
            const int head = head_piece();
            while (cached_pieces.size() > max_pieces) {
                int victim = -1;
                int best = -1;
                for (int p : cached_pieces) {
                    if (p >= head && p <= head + 2) continue; // about to be served
                    // behind-the-head distance counts double so played data
                    // goes first
                    int d = (p < head) ? (head - p) * 2 : (p - head);
                    if (d > best) { best = d; victim = p; }
                }
                if (victim < 0) break;
                cached_pieces.erase(victim);
                release_cached_piece(victim);
            }
        }
    }

    // Caller holds read_mu. Preserve the arriving result; other readers can
    // retry from the hot cache or disk if an older result was evicted.
    void trim_read_results_locked(int keep) {
        constexpr size_t MAX_ENTRIES = 8;
        constexpr size_t MAX_BYTES   = 24u * 1024 * 1024;
        const int head = head_piece();
        for (;;) {
            size_t bytes = 0;
            for (auto& kv : read_results) bytes += kv.second.data.size();
            if (read_results.size() <= 2) break;
            if (read_results.size() <= MAX_ENTRIES && bytes <= MAX_BYTES) break;
            auto victim = read_results.end();
            int best = -1;
            for (auto it = read_results.begin(); it != read_results.end(); ++it) {
                int p = it->first;
                if (p == keep) continue;
                int d = (p < head) ? (head - p) * 2 : (p - head);
                if (d > best) { best = d; victim = it; }
            }
            if (victim == read_results.end()) break;
            read_results.erase(victim);
        }
    }

    // adaptive bitrate estimate (bytes/sec) — derived from file size
    float estimated_bitrate_bps = 625000.0f;
    int   critical_startup_pieces = 2;  // computed at init

    // Each HTTP reader advances its own bounded window. Demand merging in
    // StreamScheduler preserves overlapping probes and other playback streams.
    // ~60 s of video ahead of the cursor (on top of the player's buffer).
    int window_pieces() const {
        int64_t bytes = std::clamp<int64_t>((int64_t)(estimated_bitrate_bps * 60.0f),
                                            32LL << 20, 192LL << 20);
        return std::clamp((int)((bytes + piece_length - 1) / piece_length), 6, 256);
    }

    // The nearest ~20 s get deadlines spaced by their playback time.
    int deadline_pieces() const {
        int64_t bytes = (int64_t)(estimated_bitrate_bps * 20.0f);
        return std::clamp((int)((bytes + piece_length - 1) / piece_length), 3, 16);
    }

    int piece_play_ms() const {
        return std::clamp((int)(piece_length / estimated_bitrate_bps * 1000.0f), 100, 10000);
    }

    bool begin_http_reader(TorrReader& reader, int64_t first, int64_t last) {
        std::lock_guard<std::mutex> lock(http_mu);
        if (!active.load() || (scheduler && !scheduler->add_reader(id, reader.reader_id))) return false;
        reader.request_first_piece = byte_to_piece(first);
        reader.request_last_piece = byte_to_piece(last);
        reader.priority_hi = reader.deadline_hi = reader.request_first_piece - 1;
        http_readers.emplace(reader.reader_id, HttpPosition{first, true});
        if (primary_reader < 0) primary_reader = reader.reader_id;
        publish_http_position();
        return true;
    }

    // Caller holds http_mu. This is network read progress, not decoded playback.
    void publish_http_position() {
        auto current = http_readers.find(primary_reader);
        if (current == http_readers.end()) return;
        read_head.store(current->second.cursor);
        stream_state.store(current->second.waiting ? LT_STREAM_BUFFERING : LT_STREAM_READY);
    }

    void update_http_reader(int reader, int64_t cursor, bool waiting) {
        std::lock_guard<std::mutex> lock(http_mu);
        auto current = http_readers.find(reader);
        if (current == http_readers.end()) return;
        current->second = {cursor, waiting};
        if (primary_reader == reader) publish_http_position();
    }

    void finish_http_reader(int reader) {
        std::lock_guard<std::mutex> lock(http_mu);
        if (scheduler) scheduler->remove_reader(id, reader);
        http_readers.erase(reader);
        if (primary_reader == reader) {
            primary_reader = http_readers.empty() ? -1 : http_readers.begin()->first;
            publish_http_position();
        }
    }

    void extend_window(TorrReader& reader, int from) {
        if (!active.load() || !scheduler) return;
        const int last = std::min(reader.request_last_piece, from + window_pieces());
        const int last_deadline = std::min(last, from + deadline_pieces());
        const int keep_from = std::max(reader.request_first_piece, from - TRAILING_WINDOW);
        scheduler->retain(id, [&](int p) { return p >= keep_from && p <= last; }, reader.reader_id);
        const int span = std::max(1, last - from);
        // Only advance this connection's window. A short metadata probe must
        // not start a full readahead window beyond the requested Range.
        for (int p = std::max(from, reader.priority_hi + 1); p <= last; ++p)
            scheduler->request(id, p, std::max(1, 7 - 6 * (p - from) / span), -1, reader.reader_id);
        for (int p = std::max(from, reader.deadline_hi + 1); p <= last_deadline; ++p)
            scheduler->request(id, p, 7, (p - from) * piece_play_ms(), reader.reader_id);
        reader.priority_hi = std::max(reader.priority_hi, last);
        reader.deadline_hi = std::max(reader.deadline_hi, last_deadline);
    }

    std::atomic<bool> active{true};

    int byte_to_piece(int64_t off) const {
        if (piece_length <= 0) return start_piece;
        return (int)((file_offset + off) / piece_length);
    }

    void piece_file_range(int p, int64_t& beg, int64_t& end_) const {
        int64_t ps = (int64_t)p * piece_length;
        int64_t pe = ps + piece_length;
        beg  = std::max(ps, file_offset) - file_offset;
        end_ = std::min(pe, file_offset + file_size) - file_offset;
    }

    std::string make_url() const {
        std::string hash = "0000000000000000000000000000000000000000";
        try {
            if (handle.is_valid()) {
                auto ih = handle.info_hashes();
                std::stringstream ss;
                if (ih.has_v1()) {
                    for (auto b : ih.v1)
                        ss << std::hex << std::setw(2) << std::setfill('0') << (int)(uint8_t)b;
                } else if (ih.has_v2()) {
                    for (auto b : ih.v2)
                        ss << std::hex << std::setw(2) << std::setfill('0') << (int)(uint8_t)b;
                }
                hash = ss.str().substr(0, 40);
            }
        } catch (...) {}
        return "http://127.0.0.1:" + std::to_string(server_port)
             + "/stream/" + hash + "/" + std::to_string(file_index);
    }

    // signal piece_finished from alert thread
    void on_piece_finished(int p) {
        TB_LOG("on_piece_finished: piece=%d", p);
        {
            std::lock_guard<std::mutex> lk(piece_mu);
            pieces_have.insert(p);
        }
        // Wake serve_range — it waits on piece_cv for pieces_have
        piece_cv.notify_all();
    }

    // signal read_piece_alert from alert thread
    void on_piece_read(int p, const char* data, int size, bool ok) {
        TB_LOG("on_piece_read: piece=%d ok=%d size=%d", p, (int)ok, size);

        // Copy from the alert's data pointer exactly ONCE into a local vector.
        // On Android ARM64 with MTE, the alert buffer can be re-tagged by
        // libtorrent's internal pool between two separate memcpy passes,
        // causing SEGV_ACCERR on the second access.
        std::vector<char> safe_copy;
        if (ok && data && size > 0) {
            safe_copy.assign(data, data + size);
        }
        if (p == start_piece) start_piece_readable = !safe_copy.empty();

        // Store result for read_piece_data() consumers
        {
            std::lock_guard<std::mutex> lk(read_mu);
            ReadResult r;
            if (!safe_copy.empty()) {
                r.data = safe_copy;  // copy from our safe local
                r.ok = true;
            }
            read_results[p] = std::move(r);
            trim_read_results_locked(p);
        }

        // Populate hot piece cache from our safe copy — instant re-reads for
        // player probes, overlapping range requests, and small backward seeks
        if (!safe_copy.empty() && cache) {
            auto* cp = cache->get_piece(p);
            if (cp) {
                std::lock_guard<std::mutex> cache_lock(cached_mu);
                std::unique_lock<std::shared_mutex> lk(cp->mu);
                cached_pieces.insert(p);
                if (cp->buffer.empty()) {
                    cp->buffer = std::move(safe_copy);  // move into cache (zero-copy)
                    cp->size = (int64_t)cp->buffer.size();
                    cp->complete = true;
                    cp->accessed = TorrReader::now_unix();
                }
            }
            if (cp) {
                enforce_cache_budget();
            }
        }

        read_cv.notify_all();
        piece_cv.notify_all();
    }

    void on_hash_failed(int p) {
        if (p == start_piece) start_piece_readable = false;
        {
            std::lock_guard<std::mutex> lk(piece_mu);
            pieces_have.erase(p);
        }
        drop_cached(p);
        {
            std::lock_guard<std::mutex> lk(read_mu);
            read_results.erase(p);
        }
        if (scheduler) scheduler->refresh(p);
    }

    void wake_all() {
        piece_cv.notify_all();
        read_cv.notify_all();
    }
};

std::atomic<int32_t> StreamEngine::active_streams{0};

// ── forward declarations ────────────────────────────────────────────────────────
struct SessionWrapper;
static void run_http_server(SessionWrapper* sw, StreamEngine* stream);

// ── session wrapper ─────────────────────────────────────────────────────────────
struct ResumeRequest {
    lt::torrent_handle handle;
    std::vector<lt::download_priority_t> selection;
    bool stream_only = false;
    std::set<int> paused_files;
    bool pending = true;
    std::vector<char> bytes;
    std::string error;
};

struct SessionWrapper {
    lt::session session;

    std::mutex mu;
    std::unordered_map<int64_t, lt::torrent_handle> handles;
    std::unordered_set<int64_t> stream_only_torrents;
    std::atomic<int64_t> next_id{1};

    // Serializes public start/stop/remove through thread teardown. Alert and
    // HTTP workers never acquire this mutex, so joins cannot deadlock on it.
    std::mutex lifecycle_mu;
    std::unordered_set<lt_torrent_id> removing_torrents;
    std::unordered_map<lt_torrent_id, std::unordered_set<lt_stream_id>> active_stream_ids;
    std::mutex streams_mu;
    // Scheduler map mutations hold both lifecycle_mu and streams_mu; alert
    // delivery only needs streams_mu and never blocks teardown's thread joins.
    std::unordered_map<lt_torrent_id, std::shared_ptr<StreamScheduler>> schedulers;
    std::unordered_map<int64_t, std::unique_ptr<StreamEngine>> streams;
    std::atomic<int64_t> next_stream_id{1};

    // Resume callbacks never acquire lifecycle_mu: stream teardown may be joining
    // readers which need this same alert thread to finish their disk reads.
    std::mutex resume_mu;
    std::unordered_map<lt_torrent_id, ResumeRequest> resume_requests;

    void receive_resume(const lt::torrent_handle& handle,
                        const lt::add_torrent_params* params, const std::string& error) {
        std::lock_guard<std::mutex> lock(resume_mu);
        for (auto& item : resume_requests) {
            auto& request = item.second;
            if (!request.pending || request.handle != handle) continue;
            try {
                if (!params) throw std::runtime_error(error);
                auto saved = *params;
                saved.file_priorities = request.selection;
                saved.piece_priorities.clear(); // never persist playback windows
                saved.flags &= ~lt::torrent_flags::stop_when_ready;
                auto entry = lt::write_resume_data(saved);
                entry["ltf-version"] = 2;
                auto& paused = entry["ltf-paused-files"].list();
                for (int file : request.paused_files) paused.emplace_back(file);
                entry["ltf-stream-only"] = request.stream_only ? 1 : 0;
                lt::bencode(std::back_inserter(request.bytes), entry);
            } catch (const std::exception& e) { request.error = e.what(); }
            request.pending = false;
        }
    }

    // alert thread — sole consumer of session.pop_alerts()
    std::thread       alert_thread;
    std::atomic<bool> alert_running{false};

    // push callback (called from alert thread)
    lt_alert_callback  dart_callback  = nullptr;
    void*              dart_user_data = nullptr;
    std::mutex         cb_mu;

    // pull queue (for lt_poll_alerts)
    std::mutex              dart_queue_mu;
    std::deque<AlertRecord> dart_queue;

    explicit SessionWrapper(lt::settings_pack sp) : session(std::move(sp)) {}
    explicit SessionWrapper(lt::session_params params) : session(std::move(params)) {}

    // ── TorrServer config — port of settings.BTSets (session-level defaults) ──
    lt_bt_config bt_config;

    void init_default_config() {
        // port of settings.SetDefaultConfig()
        bt_config.cache_size = 64 * 1024 * 1024;       // 64 MB
        bt_config.reader_read_ahead = 95;               // 95%
        bt_config.preload_cache = 50;                   // 50%
        bt_config.connections_limit = 25;
        bt_config.torrent_disconnect_timeout = 30;      // 30 seconds
        bt_config.force_encrypt = 0;                    // pe_enabled
        bt_config.disable_tcp = 0;
        bt_config.disable_utp = 0;
        bt_config.disable_upload = 0;
        bt_config.disable_dht = 0;
        bt_config.disable_upnp = 0;
        bt_config.enable_ipv6 = 0;
        bt_config.download_rate_limit = 0;              // unlimited
        bt_config.upload_rate_limit = 0;                // unlimited
        bt_config.peers_listen_port = 0;                // random
        bt_config.responsive_mode = 1;                  // enabled by default
    }

    int64_t id_for_handle(const lt::torrent_handle& h) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& kv : handles)
            if (kv.second == h) return kv.first;
        return -1;
    }

    // Caller holds mu.
    std::shared_ptr<const lt::torrent_info> metadata(lt_torrent_id id) {
        auto handle = handles.find(id);
        return handle != handles.end() && handle->second.is_valid()
            ? handle->second.torrent_file() : nullptr;
    }

    void start_alert_thread() {
        alert_running = true;
        alert_thread = std::thread([this]() { process_alerts(); });
    }

    void process_alerts() {
        while (alert_running.load()) {
            try {
                if (!session.wait_for_alert(lt::milliseconds(100)))
                    continue;
                std::vector<lt::alert*> alerts;
                session.pop_alerts(&alerts);

                for (auto* a : alerts) {
                    if (!a) continue;
                    try {
                        if (auto* saved = lt::alert_cast<lt::save_resume_data_alert>(a)) {
                            receive_resume(saved->handle, &saved->params, "");
                            continue;
                        }
                        if (auto* failed = lt::alert_cast<lt::save_resume_data_failed_alert>(a)) {
                            receive_resume(failed->handle, nullptr, failed->error.message());
                            continue;
                        }
                        // read_piece_alert → route to stream's cache
                        if (auto* rpa = lt::alert_cast<lt::read_piece_alert>(a)) {
                            int p = static_cast<int>(rpa->piece);
                            std::lock_guard<std::mutex> slk(streams_mu);
                            for (auto& kv : streams) {
                                auto& s = kv.second;
                                if (!s->active || s->handle != rpa->handle) continue;
                                if (p < s->start_piece || p > s->end_piece) continue;
                                s->on_piece_read(p,
                                    rpa->error ? nullptr : rpa->buffer.get(),
                                    rpa->error ? 0 : rpa->size,
                                    !rpa->error);
                            }
                            continue;
                        }

                        // piece_finished_alert → signal waiters + update cache
                        if (auto* pfa = lt::alert_cast<lt::piece_finished_alert>(a)) {
                            int p = static_cast<int>(pfa->piece_index);
                            std::lock_guard<std::mutex> slk(streams_mu);
                            for (auto& kv : streams) {
                                auto& s = kv.second;
                                if (!s->active || s->handle != pfa->handle) continue;
                                if (p >= s->start_piece && p <= s->end_piece)
                                    s->on_piece_finished(p);
                            }
                        }

                        // hash_failed_alert → remove from available, re-request
                        else if (auto* hf = lt::alert_cast<lt::hash_failed_alert>(a)) {
                            int p = static_cast<int>(hf->piece_index);
                            std::lock_guard<std::mutex> slk(streams_mu);
                            for (auto& entry : schedulers)
                                if (entry.second->owns(hf->handle)) entry.second->refresh(p);
                            for (auto& kv : streams) {
                                auto& s = kv.second;
                                if (!s->active || s->handle != hf->handle) continue;
                                if (p >= s->start_piece && p <= s->end_piece)
                                    s->on_hash_failed(p);
                            }
                        }

                        else if (auto* priority_alert = lt::alert_cast<lt::file_prio_alert>(a)) {
                            std::lock_guard<std::mutex> lock(streams_mu);
                            for (auto& entry : schedulers) {
                                if (!entry.second->owns(priority_alert->handle)) continue;
                                entry.second->reapply();
                                break; // also enforce no background download after retention
                            }
                        }

                        // metadata_received → zero-out missing-piece priorities (stream-only)
                        // so nothing downloads until a stream asks for pieces.
                        // Not paused: pausing dropped every peer right before
                        // the stream started and made it reconnect from scratch.
                        else if (auto* mra = lt::alert_cast<lt::metadata_received_alert>(a)) {
                            int64_t mid = id_for_handle(mra->handle);
                            bool is_stream_only = false;
                            {
                                std::lock_guard<std::mutex> lk(mu);
                                is_stream_only = stream_only_torrents.count(mid) > 0;
                            }
                            std::lock_guard<std::mutex> slk(streams_mu);
                            auto scheduler = schedulers.find(mid);
                            if (scheduler != schedulers.end()) {
                                scheduler->second->reapply();
                                is_stream_only = false;
                            }
                            if (is_stream_only) {
                                try {
                                    auto ti = mra->handle.torrent_file();
                                    if (ti) {
                                        auto status = mra->handle.status(lt::torrent_handle::query_pieces);
                                        std::vector<std::pair<lt::piece_index_t, lt::download_priority_t>> priorities;
                                        for (int p = 0; p < ti->num_pieces(); ++p) {
                                            if (p < status.pieces.size() && status.pieces[lt::piece_index_t(p)]) continue;
                                            priorities.emplace_back(lt::piece_index_t(p), lt::dont_download);
                                        }
                                        mra->handle.prioritize_pieces(priorities);
                                    }
                                } catch (...) {}
                            }
                        }

                        // queue alert for dart
                        lt_torrent_id tid = -1;
                        if (auto* ta = dynamic_cast<lt::torrent_alert*>(a))
                            tid = id_for_handle(ta->handle);

                        {
                            std::lock_guard<std::mutex> ql(dart_queue_mu);
                            if (dart_queue.size() < 2048)
                                dart_queue.push_back({a->type(), tid, a->message()});
                        }

                        {
                            std::lock_guard<std::mutex> cl(cb_mu);
                            if (dart_callback)
                                dart_callback(a->type(), tid, a->message().c_str(), dart_user_data);
                        }
                    } catch (...) {}
                }

                // TorrServer recalculates priorities on every piece event via
                // cleanPieces → getRemPieces → setLoadPriority. Our alert thread
                // already marks pieces complete which triggers cleanPieces via
                // the cache write path.

            } catch (...) {}
        }
    }
};

static SessionWrapper* to_sw(lt_session_t h) {
    return reinterpret_cast<SessionWrapper*>(h);
}

// Caller holds lifecycle_mu; map changes also synchronize with alert delivery.
static std::shared_ptr<StreamScheduler> download_scheduler(SessionWrapper* sw, lt_torrent_id tid) {
    std::lock_guard<std::mutex> streams_lock(sw->streams_mu);
    std::lock_guard<std::mutex> lock(sw->mu);
    auto found = sw->handles.find(tid);
    if (found == sw->handles.end() || !found->second.is_valid())
        throw std::runtime_error("torrent not found");
    if (!found->second.torrent_file()) throw std::runtime_error("torrent metadata is not ready");
    auto& scheduler = sw->schedulers[tid];
    if (!scheduler) scheduler = std::make_shared<StreamScheduler>(found->second,
        !sw->stream_only_torrents.count(tid));
    return scheduler;
}

// Caller holds lifecycle_mu. This path never removes the torrent, and is
// shared by explicit stop, failed startup and whole-torrent removal.
static lt_torrent_id stop_stream_internal(SessionWrapper* sw, lt_stream_id sid) {
    std::unique_ptr<StreamEngine> stream;
    {
        std::lock_guard<std::mutex> lock(sw->streams_mu);
        auto it = sw->streams.find(sid);
        if (it == sw->streams.end()) return -1;
        stream = std::move(it->second);
        sw->streams.erase(it);
        stream->active = false;
        stream->preloading = false;
    }
    const auto tid = stream->torrent_id;
    auto owners = sw->active_stream_ids.find(tid);
    if (owners != sw->active_stream_ids.end()) {
        owners->second.erase(sid);
        if (owners->second.empty()) sw->active_stream_ids.erase(owners);
    }
    // Reject late worker requests before joining. No pause, file-priority
    // reset or stop_when_ready: verified pieces remain available for upload.
    if (stream->scheduler) stream->scheduler->remove_stream(sid);
    stream->wake_all();
    // The HTTP thread owns its listening socket and wakes within 200 ms.
    // Let it close the socket to avoid racing close/select and fd reuse.
    if (stream->preload_thread.joinable()) stream->preload_thread.join();
    if (stream->server_thread.joinable()) stream->server_thread.join();
    if (stream->cache) stream->cache->close();
    return tid;
}

// Caller holds lifecycle_mu. Workers never invoke this function, so deleting
// an entire torrent cannot recurse through public stopStream cleanup policy.
static void remove_torrent_internal(SessionWrapper* sw, lt_torrent_id tid, bool delete_files) {
    {
        std::lock_guard<std::mutex> lock(sw->resume_mu);
        sw->resume_requests.erase(tid);
    }
    sw->removing_torrents.insert(tid);
    auto scheduler = sw->schedulers.find(tid);
    if (scheduler != sw->schedulers.end()) scheduler->second->shutdown();
    auto owners = sw->active_stream_ids.find(tid);
    if (owners != sw->active_stream_ids.end()) {
        const auto ids = owners->second;
        for (auto sid : ids) stop_stream_internal(sw, sid);
    }
    {
        std::lock_guard<std::mutex> lock(sw->streams_mu);
        sw->schedulers.erase(tid);
    }
    {
        std::lock_guard<std::mutex> lock(sw->mu);
        auto it = sw->handles.find(tid);
        if (it != sw->handles.end()) {
            if (it->second.is_valid()) sw->session.remove_torrent(it->second,
                delete_files ? lt::session::delete_files : lt::remove_flags_t{});
            sw->handles.erase(it);
        }
        sw->stream_only_torrents.erase(tid);
    }
    sw->removing_torrents.erase(tid);
}

// ── piece waiting — port of blocking read in TorrServer's Reader.Read ────────
// blocks until piece is downloaded and verified — zero CPU polling

static bool wait_for_piece(StreamEngine* s, int piece, int timeout_ms) {
    std::unique_lock<std::mutex> lk(s->piece_mu);
    if (s->pieces_have.count(piece)) return true;

    // deadline + top priority — triggers time-critical download from multiple peers
    lk.unlock();
    try { if (s->scheduler) s->scheduler->request(s->id, piece, 7, 0); } catch (...) {}
    lk.lock();

    return s->piece_cv.wait_for(lk, chr::milliseconds(timeout_ms),
        [&]{ return !s->active.load()
                 || s->pieces_have.count(piece) > 0; })
        && s->pieces_have.count(piece) > 0;
}

// Read piece data directly from libtorrent disk storage.
// Like lt2http: storage()->readv() — reads downloaded piece data.
static ReadResult read_piece_data(StreamEngine* s, int piece,
                                  int timeout_ms = 5000,
                                  const std::function<bool()>& cancelled = {}) {

    // Check hot piece cache first — instant for re-reads
    if (s->cache) {
        auto* cp = s->cache->get_piece(piece);
        if (cp) {
            std::shared_lock<std::shared_mutex> lk(cp->mu);
            if (!cp->buffer.empty() && cp->complete) {
                ReadResult r;
                r.data.assign(cp->buffer.begin(), cp->buffer.end());
                r.ok = true;
                lk.unlock();
                // The prefetch put a second copy in read_results — without
                // this erase it lived until the next seek (i.e. forever).
                std::lock_guard<std::mutex> rlk(s->read_mu);
                s->read_results.erase(piece);
                return r;
            }
        }
    }

    // Check if we already have a read result buffered
    {
        std::lock_guard<std::mutex> lk(s->read_mu);
        auto it = s->read_results.find(piece);
        if (it != s->read_results.end() && it->second.ok) {
            ReadResult r = std::move(it->second);
            s->read_results.erase(it);
            return r;
        }
        s->read_results.erase(piece);
    }

    // Request libtorrent to read piece from disk
    try { s->handle.read_piece(lt::piece_index_t(piece)); } catch (...) {
        return {};
    }

    std::unique_lock<std::mutex> lk(s->read_mu);
    const auto deadline = chr::steady_clock::now() + chr::milliseconds(timeout_ms);
    while (s->active.load() && !(cancelled && cancelled()) && chr::steady_clock::now() < deadline) {
        if (s->read_results.count(piece) || s->has_cached_piece(piece)) break;
        s->read_cv.wait_until(lk, std::min(deadline, chr::steady_clock::now() + chr::milliseconds(200)));
    }
    if (!s->active.load() || (cancelled && cancelled())) return {};
    auto result = s->read_results.find(piece);
    if (result != s->read_results.end()) {
        ReadResult data = std::move(result->second);
        s->read_results.erase(result);
        return data;
    }
    lk.unlock();
    if (s->cache) {
        if (auto* cp = s->cache->get_piece(piece)) {
            std::shared_lock<std::shared_mutex> clk(cp->mu);
            if (!cp->buffer.empty() && cp->complete) {
                ReadResult r;
                r.data.assign(cp->buffer.begin(), cp->buffer.end());
                r.ok = true;
                return r;
            }
        }
    }
    return {};
}

// ── HTTP server — port of torr/stream.go Stream() ───────────────────────────

struct RangeReq {
    int64_t start = -1;
    int64_t end   = -1;
    bool    valid = false;
    bool    is_suffix = false;
    std::string header;
};

static RangeReq parse_range(const char* buf, int len) {
    RangeReq r;
    std::string s(buf, (size_t)len);
    std::string sl = s;
    for (auto& c : sl) c = (char)tolower((unsigned char)c);

    auto pos = sl.find("range: bytes=");
    if (pos == std::string::npos) return r;

    r.valid = true;
    pos += 13;
    auto end = s.find('\r', pos);
    if (end == std::string::npos) end = s.size();
    std::string rs = s.substr(pos, end - pos);
    r.header = rs.substr(0, 256);
    for (auto& ch : r.header)
        if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) > 126) ch = '?';
    auto dash = rs.find('-');
    if (dash != std::string::npos) {
        try {
            if (dash > 0) {
                r.start = std::stoll(rs.substr(0, dash));
            } else {
                r.is_suffix = true;
            }
            if (dash + 1 < rs.size()) {
                r.end = std::stoll(rs.substr(dash + 1));
            }
        } catch (...) {}
    }
    return r;
}

static int send_all(socket_t sock, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = ::send(sock, data + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

static bool http_client_closed(socket_t client) {
#ifdef _WIN32
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(client, &readable);
    timeval timeout{0, 0};
    const int selected = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
    // File descriptors in a Flutter app may exceed select's FD_SETSIZE.
    pollfd descriptor{client, POLLIN, 0};
    const int selected = ::poll(&descriptor, 1, 0);
    if (selected > 0 && (descriptor.revents & (POLLERR | POLLNVAL))) return true;
#endif
    if (selected == 0) return false;
    if (selected > 0) {
        char byte;
        const int received = ::recv(client, &byte, 1, MSG_PEEK);
        if (received >= 0) return received == 0;
    }
#ifdef _WIN32
    const int error = WSAGetLastError();
    return error != WSAEINTR && error != WSAEWOULDBLOCK;
#else
    return errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK;
#endif
}

static bool wait_for_http_piece(StreamEngine* s, int piece, socket_t client) {
    const auto deadline = chr::steady_clock::now() + chr::seconds(180);
    std::unique_lock<std::mutex> lock(s->piece_mu);
    while (s->active.load() && !http_client_closed(client)) {
        if (s->pieces_have.count(piece)) return true;
        if (chr::steady_clock::now() >= deadline) break;
        s->piece_cv.wait_until(lock, std::min(deadline, chr::steady_clock::now() + chr::milliseconds(200)));
    }
    return false;
}

// serve_range — based on lt2http's Reader::read() pattern
//
// lt2http reads directly from libtorrent storage — no intermediate cache.
// For each piece: wait until downloaded (have_piece), read via read_piece,
// send to socket. The sliding download window follows the read cursor;
// disk-read prefetch is separately limited to the next two available pieces.
static bool serve_range(StreamEngine* s, TorrReader* reader, socket_t cli,
                        int64_t range_start, int64_t range_end) {
    int64_t cursor = range_start;
    if (!s->begin_http_reader(*reader, range_start, range_end)) return false;
    reader->offset = cursor;
    reader->last_access = TorrReader::now_unix();

    while (cursor <= range_end && s->active.load()) {
        if (http_client_closed(cli)) return false;
        int p = std::clamp(s->byte_to_piece(cursor), s->start_piece, s->end_piece);

        int64_t pfbeg, pfend;
        s->piece_file_range(p, pfbeg, pfend);
        pfend -= 1;

        int64_t sbeg = std::max(cursor, pfbeg);
        int64_t send_end = std::min(range_end, pfend);
        if (send_end < sbeg) { cursor = pfend + 1; continue; }

        // Extend the download window even when the current piece is already
        // present, so fast playback does not repeatedly exhaust its requests.
        bool have_it = false;
        {
            std::lock_guard<std::mutex> lk(s->piece_mu);
            have_it = s->pieces_have.count(p) > 0;
        }

        s->extend_window(*reader, p);

        if (!have_it) {
            // Container probes and player buffering can outrun the estimated
            // playback rate. A piece blocking this read is urgent now, even
            // if this reader already scheduled it with a later deadline.
            if (s->scheduler) s->scheduler->request(s->id, p, 7, 0, reader->reader_id);
            s->update_http_reader(reader->reader_id, cursor, true);
            TB_LOG("HTTP wait stream=%lld reader=%d piece=%d", (long long)s->id, reader->reader_id, p);
            if (!wait_for_http_piece(s, p, cli)) return false;
        }

        // ── piece is downloaded — read data directly via read_piece ──
        // Like lt2http: storage()->readv(b, p, offset, ...)
        // We use read_piece_data which does read_piece → waits for data
        ReadResult rd = read_piece_data(s, p, 10000, [&] { return http_client_closed(cli); });
        if (!rd.ok || rd.data.empty()) {
            TB_LOG("serve_range: read_piece_data FAILED piece=%d", p);
            return false;
        }

        // Extract the slice we need from the piece
        int64_t abs_start = s->file_offset + sbeg;
        int64_t piece_start = (int64_t)p * s->piece_length;
        int64_t off = abs_start - piece_start;
        int64_t nb  = send_end - sbeg + 1;

        if (off < 0 || (size_t)off >= rd.data.size()) { cursor = send_end + 1; continue; }
        if ((size_t)(off + nb) > rd.data.size()) nb = (int64_t)rd.data.size() - off;
        if (nb <= 0) { cursor = send_end + 1; continue; }

        // ── disk-read prefetch ──
        // While we're about to block on the socket send for piece p, kick off
        // libtorrent disk reads for the next 1-2 pieces if they're already
        // downloaded. By the time this send finishes and the loop advances,
        // the read_piece_alert for p+1 is already in s->read_results, so
        // read_piece_data(p+1) returns instantly instead of blocking another
        // ~5-15 ms for the disk round trip. Classic socket-I/O ↔ disk-I/O
        // overlap — free throughput on slow disks and during player buffering.
        for (int i = 1; i <= 2; ++i) {
            int np = p + i;
            if (np > s->end_piece) break;
            bool have_np = false;
            { std::lock_guard<std::mutex> lk(s->piece_mu); have_np = s->pieces_have.count(np) > 0; }
            if (!have_np) break;  // not downloaded yet → no point prefetching
            // skip if already cached or result already buffered
            bool already = false;
            if (s->cache) {
                if (auto* cp = s->cache->get_piece(np)) {
                    std::shared_lock<std::shared_mutex> clk(cp->mu);
                    if (!cp->buffer.empty() && cp->complete) already = true;
                }
            }
            if (!already) {
                std::lock_guard<std::mutex> rlk(s->read_mu);
                if (s->read_results.count(np)) already = true;
            }
            if (already) continue;
            try { s->handle.read_piece(lt::piece_index_t(np)); } catch (...) {}
        }

        if (send_all(cli, rd.data.data() + off, (int)nb) < 0)
            return false;

        cursor = sbeg + nb;

        reader->offset = cursor;
        reader->last_access = TorrReader::now_unix();
        s->update_http_reader(reader->reader_id, cursor, false);
    }
    return cursor > range_end;
}

// handle_connection — port of torr/stream.go Stream()
// Each connection creates a TorrReader (like TorrServer's t.NewReader(file))
// and uses it for the lifetime of the connection
static void handle_connection(StreamEngine* s, socket_t cli, int reader_id) {
    // port of stream.go: atomic.AddInt32(&activeStreams, 1)
    StreamEngine::active_streams.fetch_add(1);

    TorrReader* reader = nullptr;
    try {

    int opt = 1;
    ::setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt));
    int sndbuf = 2 * 1024 * 1024;
    ::setsockopt(cli, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));

#ifdef _WIN32
    DWORD tv = 36000000;
    ::setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    ::setsockopt(cli, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = 36000; tv.tv_usec = 0;
    ::setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(cli, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    while (s->active.load()) {
        char buf[8192] = {};
        int total = 0;
        bool header_complete = false;
        while (total < (int)sizeof(buf) - 1 && s->active.load()) {
            int n = ::recv(cli, buf + total, (int)sizeof(buf) - 1 - total, 0);
            if (n <= 0) goto cleanup;
            total += n;
            for (int i = std::max(0, total - 4); i <= total - 4; ++i) {
                if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n') {
                    header_complete = true; break;
                }
            }
            if (header_complete) break;
        }
        if (!header_complete || !s->active.load()) goto cleanup;

        {
            std::string req(buf, (size_t)total);

            bool is_options = req.find("OPTIONS ") != std::string::npos;
            bool is_head    = req.find("HEAD ") != std::string::npos;
            bool is_get     = req.find("GET ")  != std::string::npos;

            if (is_options) {
                const char* cors =
                    "HTTP/1.1 204 No Content\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
                    "Access-Control-Allow-Headers: Range\r\n"
                    "Access-Control-Max-Age: 1728000\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: keep-alive\r\n\r\n";
                if (send_all(cli, cors, (int)strlen(cors)) < 0) goto cleanup;
                continue;
            }

            if (!is_get && !is_head) goto cleanup;

            int64_t fsz = s->file_size;
            if (fsz <= 0) goto cleanup;

            RangeReq rr = parse_range(buf, total);
            int64_t rstart = (rr.valid && rr.start >= 0) ? rr.start : 0;
            int64_t rend   = (rr.valid && rr.end >= 0)   ? rr.end   : fsz - 1;

            if (rr.valid && rr.is_suffix) {
                rstart = fsz - rr.end; // rr.end holds the suffix length
                rend = fsz - 1;
            }

            rstart = std::clamp(rstart, (int64_t)0, fsz - 1);
            rend   = std::clamp(rend,   rstart,     fsz - 1);
            int64_t clen = rend - rstart + 1;
            bool is_partial = (rr.valid && (rr.start >= 0 || rr.is_suffix));

            TB_LOG("HTTP request stream=%lld reader=%d method=%s Range=%s resolved=%lld-%lld",
                   (long long)s->id, reader_id, is_head ? "HEAD" : "GET",
                   rr.valid ? rr.header.c_str() : "(none)", (long long)rstart, (long long)rend);
            // Each connection serves one GET and closes. A different connection
            // may be a container/index probe, a retry, or a real user seek.
            // Its Range cannot be used to cancel another live connection.

            // get filename — port of stream.go MIME detection
            std::string filename = "video.mp4";
            if (s->ti) {
                try { filename = std::string(s->ti->files().file_name(lt::file_index_t{s->file_index})); }
                catch (...) {}
            }

            // build response headers — port of stream.go header setup
            std::string etag_raw = s->make_url();
            std::ostringstream hdr;
            if (is_partial) {
                hdr << "HTTP/1.1 206 Partial Content\r\n";
                hdr << "Content-Range: bytes " << rstart << "-" << rend << "/" << fsz << "\r\n";
            } else {
                hdr << "HTTP/1.1 200 OK\r\n";
            }
            hdr << "Content-Type: " << get_mime(filename) << "\r\n";
            hdr << "Content-Length: " << clen << "\r\n";
            hdr << "Accept-Ranges: bytes\r\n";
            // port of stream.go: resp.Header().Set("Connection", "close")
            hdr << "Connection: close\r\n";
            // port of stream.go: ETag header
            hdr << "ETag: \"" << std::hash<std::string>{}(etag_raw) << "\"\r\n";
            hdr << "Access-Control-Allow-Origin: *\r\n";
            hdr << "Access-Control-Allow-Headers: Range\r\n";
            // port of stream.go: DLNA headers
            hdr << "transferMode.dlna.org: Streaming\r\n";
            hdr << "contentFeatures.dlna.org: DLNA.ORG_OP=01;DLNA.ORG_CI=0;"
                   "DLNA.ORG_FLAGS=01700000000000000000000000000000\r\n";
            hdr << "Cache-Control: no-store, no-cache\r\n\r\n";

            std::string h = hdr.str();
            if (send_all(cli, h.c_str(), (int)h.size()) < 0) goto cleanup;

            if (is_get) {
                reader = s->cache->new_reader(reader_id, s->file_index, s->file_offset, s->file_size);
                reader->set_readahead(StreamEngine::FIXED_READAHEAD);
                bool sr_ok = serve_range(s, reader, cli, rstart, rend);
                TB_LOG("HTTP body stream=%lld reader=%d result=%s bytes=%lld",
                       (long long)s->id, reader_id, sr_ok ? "complete" : "interrupted",
                       (long long)std::max<int64_t>(0, reader->offset - rstart));
                if (!sr_ok) goto cleanup;
            }

            // Connection: close — break after one request like TorrServer
            goto cleanup;
        }
    }
    } catch (const std::exception& error) {
        TB_LOG("HTTP error stream=%lld reader=%d: %s", (long long)s->id, reader_id, error.what());
    } catch (...) {
        TB_LOG("HTTP error stream=%lld reader=%d: unknown exception", (long long)s->id, reader_id);
    }

cleanup:
    // Also withdraw failed/closed requests. Never revoke another reader's picks.
    if (reader) {
        s->finish_http_reader(reader_id);
        if (s->cache) s->cache->close_reader(reader);
    }
    StreamEngine::active_streams.fetch_add(-1);
}

// run_http_server — accepts connections, each gets its own TorrReader
// port of TorrServer's multi-connection model where each http.ServeContent
// call creates its own Reader
static void run_http_server(SessionWrapper* /*sw*/, StreamEngine* stream) {
    try {
        INIT_SOCKETS();

        socket_t sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == SOCKET_INVALID) return;

        int opt = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        if (::bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
            CLOSESOCKET(sock); return;
        }
        socklen_t_ al = sizeof(addr);
        ::getsockname(sock, (sockaddr*)&addr, (socklen_t*)&al);
        stream->server_port = ntohs(addr.sin_port);
        stream->listen_sock = sock;
        ::listen(sock, 16);
        stream->running = true;

        while (stream->active.load()) {
            fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
            timeval tv{0, 200000};
            if (::select((int)sock + 1, &fds, nullptr, nullptr, &tv) <= 0)
                continue;

            sockaddr_in ca{}; socklen_t_ cl = sizeof(ca);
            socket_t cli = ::accept(sock, (sockaddr*)&ca, (socklen_t*)&cl);
            if (cli == SOCKET_INVALID) continue;

            // each new connection gets its own reader — port of TorrServer model
            // TorrServer does NOT kill old connections; it supports concurrent readers
            int rid = stream->next_reader_id.fetch_add(1);

            // register client socket for forced shutdown
            {
                std::lock_guard<std::mutex> lk(stream->clients_mu);
                stream->client_sockets[rid] = cli;

                // join and erase finished client threads
                for (auto it = stream->client_threads.begin(); it != stream->client_threads.end(); ) {
                    if (stream->finished_clients.count(it->first)) {
                        if (it->second.joinable()) it->second.join();
                        stream->finished_clients.erase(it->first);
                        it = stream->client_threads.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            std::thread t([stream, cli, rid]() {
                try {
                    handle_connection(stream, cli, rid);
                } catch (...) {}
                try { CLOSESOCKET(cli); } catch (...) {}
                // signal completion so accept loop can join us
                try {
                    std::lock_guard<std::mutex> lk(stream->clients_mu);
                    stream->client_sockets.erase(rid);
                    stream->finished_clients.insert(rid);
                } catch (...) {}
            });

            {
                std::lock_guard<std::mutex> lk(stream->clients_mu);
                stream->client_threads[rid] = std::move(t);
            }
        }

        // Close listen socket if lt_stop_stream hasn't already closed it
        if (stream->listen_sock != SOCKET_INVALID) {
            CLOSESOCKET(sock);
            stream->listen_sock = SOCKET_INVALID;
        }

        // shut down all client sockets to unblock recv() in client threads
        {
            std::lock_guard<std::mutex> lk(stream->clients_mu);
            for (auto& kv : stream->client_sockets) {
#ifdef _WIN32
                ::shutdown(kv.second, SD_BOTH);
#else
                ::shutdown(kv.second, SHUT_RDWR);
#endif
            }
        }

        // join all client threads before returning — prevents use-after-free
        // on StreamEngine's mutexes when the unique_ptr is destroyed
        std::unordered_map<int, std::thread> threads_to_join;
        {
            std::lock_guard<std::mutex> lk(stream->clients_mu);
            threads_to_join = std::move(stream->client_threads);
            stream->client_threads.clear();
            stream->finished_clients.clear();
        }
        for (auto& kv : threads_to_join) {
            if (kv.second.joinable()) kv.second.join();
        }
    } catch (...) {}
}

// ── preload — port of torr/preload.go Preload() ─────────────────────────────
// Preloads head and tail of file in parallel before streaming
static void preload_stream(StreamEngine* s, int64_t preload_bytes) {
    if (preload_bytes <= 0) return;
    s->preload_size.store(preload_bytes);
    s->preloading.store(true);

    if (preload_bytes > s->file_size)
        preload_bytes = s->file_size;

    // port of preload.go: startend → 8/16 MB
    int64_t startend = s->piece_length;
    if (startend < 8 * 1024 * 1024)
        startend = 8 * 1024 * 1024;

    int64_t reader_start_end = preload_bytes - startend;
    if (reader_start_end < 0) reader_start_end = preload_bytes;
    if (reader_start_end > s->file_size) reader_start_end = s->file_size;

    int64_t reader_end_start = s->file_size - startend;

    // head preload — port of main preload section
    int head_piece = s->start_piece;
    int head_end_piece = s->byte_to_piece(reader_start_end);
    head_end_piece = std::min(head_end_piece, s->end_piece);

    for (int p = head_piece; p <= head_end_piece && s->active.load() && s->preloading.load(); ++p) {
        // wait for piece data in cache (alert thread eagerly pre-fetches)
        if (!wait_for_piece(s, p, 30000)) break;
        // small extra wait for cache buffer if needed
        for (int w = 0; w < 50 && s->active.load(); ++w) {
            auto* cp = s->cache ? s->cache->get_piece(p) : nullptr;
            if (cp) {
                std::shared_lock<std::shared_mutex> cache_lock(cp->mu);
                if (!cp->buffer.empty() && cp->complete) break;
            }
            std::this_thread::sleep_for(chr::milliseconds(100));
        }
        s->preloaded_bytes.fetch_add(s->piece_length);
    }

    // tail preload — port of end range preload goroutine
    if (reader_end_start > reader_start_end) {
        int tail_piece = s->byte_to_piece(reader_end_start);
        tail_piece = std::max(tail_piece, s->start_piece);

        for (int p = tail_piece; p <= s->end_piece && s->active.load() && s->preloading.load(); ++p) {
            if (!wait_for_piece(s, p, 30000)) break;
            for (int w = 0; w < 50 && s->active.load(); ++w) {
                auto* cp = s->cache ? s->cache->get_piece(p) : nullptr;
                if (cp) {
                    std::shared_lock<std::shared_mutex> cache_lock(cp->mu);
                    if (!cp->buffer.empty() && cp->complete) break;
                }
                std::this_thread::sleep_for(chr::milliseconds(100));
            }
            s->preloaded_bytes.fetch_add(s->piece_length);
        }
    }

    s->preloading.store(false);
}

// ── C API ───────────────────────────────────────────────────────────────────────

extern "C" {

TORRENT_API void lt_set_ssl_cert_path(const char* path) {
#ifdef __ANDROID__
    if (path && *path) {
        setenv("SSL_CERT_FILE", path, 1);
        unsetenv("SSL_CERT_DIR");
        __android_log_print(
            ANDROID_LOG_INFO,
            "libtorrent_native",
            "SSL_CERT_FILE set to %s",
            path);
    }
#else
    (void)path;
#endif
}

TORRENT_API lt_session_t lt_create_session(const char* iface, int dl, int ul) {
    try {
#ifdef __ANDROID__
        // Fail early with a useful Logcat/lt_last_error message instead of
        // allowing every wss:// tracker to time out ambiguously.
        validate_android_ca_bundle();
#endif
        lt::settings_pack sp;

        // alert categories
        sp.set_int(lt::settings_pack::alert_mask,
            lt::alert_category::status
            | lt::alert_category::error
            | lt::alert_category::storage
            | lt::alert_category::piece_progress
            | lt::alert_category::tracker
            | lt::alert_category::peer
            | lt::alert_category::dht);

        sp.set_str(lt::settings_pack::listen_interfaces,
            (iface && *iface) ? iface : "0.0.0.0:0,[::]:0");

        if (dl > 0) sp.set_int(lt::settings_pack::download_rate_limit, dl);
        if (ul > 0) sp.set_int(lt::settings_pack::upload_rate_limit,   ul);

#ifdef __ANDROID__
        // Keep TLS verification enabled — the bundled CA file makes wss:// trackers work.
        sp.set_bool(lt::settings_pack::validate_https_trackers, true);
#endif

        // ── connection speed — get peers fast ──
        sp.set_int (lt::settings_pack::connection_speed,          200);
        sp.set_int (lt::settings_pack::torrent_connect_boost,     200);
        sp.set_bool(lt::settings_pack::smooth_connects,           false);
        // Session-wide cap. Per-torrent cap (default 25) is what actually
        // governs streaming fanout — see set_max_connections() in
        // lt_start_stream. Keep this comfortably above per-torrent * active.
        sp.set_int (lt::settings_pack::connections_limit,         200);
        sp.set_int (lt::settings_pack::min_reconnect_time,        5);
        sp.set_int (lt::settings_pack::max_failcount,             3);
        sp.set_int (lt::settings_pack::peer_connect_timeout,      5);
        sp.set_int (lt::settings_pack::handshake_timeout,         5);

        // ── timeouts — detect slow/stalled peers quickly for streaming ──
        sp.set_int (lt::settings_pack::piece_timeout,             5);
        sp.set_int (lt::settings_pack::request_timeout,           5);
        sp.set_int (lt::settings_pack::peer_timeout,              60);
        sp.set_int (lt::settings_pack::inactivity_timeout,        120);

        // ── request pipeline — short queue time = fast seek response ──
        // request_queue_time is SECONDS of outstanding requests per peer.
        // At 3s, after a priority change peers take up to 3s to drain the
        // pipe before serving new pieces. 1s = 3x faster seek response.
        sp.set_int (lt::settings_pack::request_queue_time,        1);
        sp.set_int (lt::settings_pack::max_out_request_queue,     500);
        sp.set_int (lt::settings_pack::max_allowed_in_request_queue, 2000);

        // ── piece picking — WE control priorities ──
        sp.set_bool(lt::settings_pack::auto_sequential,           false);
        // piece_extent_affinity=true: keep a peer downloading the same
        // file region instead of jumping. Reduces piece-completion variance
        // (= stutter). Designed exactly for streaming.
        sp.set_bool(lt::settings_pack::piece_extent_affinity,     true);
        // strict_end_game_mode=false: enables block-level duplication on
        // the trailing edge of in-progress pieces. With strict=true and a
        // 2-piece window, end-game NEVER triggers — peers sit waiting on a
        // single slow block. false = duplicate the last blocks across peers.
        sp.set_bool(lt::settings_pack::strict_end_game_mode,      false);
        sp.set_bool(lt::settings_pack::prioritize_partial_pieces, true);
        sp.set_int (lt::settings_pack::initial_picker_threshold,  0);

        // ── disk I/O ──
        sp.set_int (lt::settings_pack::aio_threads,               4);
        sp.set_int (lt::settings_pack::hashing_threads,           2);
        // 64MB lets the disk pipeline absorb burst writes when many peers
        // deliver simultaneously (common right after a seek).
        sp.set_int (lt::settings_pack::max_queued_disk_bytes,     64 * 1024 * 1024);
        sp.set_int (lt::settings_pack::disk_io_read_mode,         lt::settings_pack::enable_os_cache);
        sp.set_int (lt::settings_pack::disk_io_write_mode,        lt::settings_pack::enable_os_cache);
        sp.set_int (lt::settings_pack::file_pool_size,            100);
        sp.set_bool(lt::settings_pack::no_atime_storage,          true);

        // ── upload — unlimited unchoke so peers reciprocate (tit-for-tat) ──
        sp.set_int (lt::settings_pack::unchoke_slots_limit,       -1);
        sp.set_int (lt::settings_pack::active_seeds,              0);
        sp.set_int (lt::settings_pack::suggest_mode,              lt::settings_pack::suggest_read_cache);

        // ── DHT + discovery ──
        sp.set_bool(lt::settings_pack::enable_dht,                true);
        sp.set_bool(lt::settings_pack::enable_lsd,                true);
        sp.set_bool(lt::settings_pack::enable_upnp,               true);
        sp.set_bool(lt::settings_pack::enable_natpmp,             true);
        sp.set_str (lt::settings_pack::dht_bootstrap_nodes,
            "dht.libtorrent.org:25401,"
            "router.bittorrent.com:6881,"
            "dht.transmissionbt.com:6881,"
            "router.utorrent.com:6881");
        sp.set_int (lt::settings_pack::dht_announce_interval,     60);
        sp.set_bool(lt::settings_pack::announce_to_all_trackers,  true);
        sp.set_bool(lt::settings_pack::announce_to_all_tiers,     true);

        // ── general ──
        sp.set_int (lt::settings_pack::active_downloads,          1);
        sp.set_int (lt::settings_pack::active_limit,              10);
        sp.set_int (lt::settings_pack::alert_queue_size,          10000);
        // Keep seeds connected when the current streaming window is complete.
        sp.set_bool(lt::settings_pack::close_redundant_connections, false);
        sp.set_int (lt::settings_pack::peer_turnover,             5);
        sp.set_int (lt::settings_pack::peer_turnover_interval,    30);
        // Validate restored incomplete data against disk instead of trusting stale state.
        sp.set_bool(lt::settings_pack::no_recheck_incomplete_resume, false);
        sp.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
        sp.set_bool(lt::settings_pack::rate_limit_ip_overhead,    false);
        // whole_pieces_threshold=0: parallelize block requests across peers
        // for every piece. At 20s, a 4MB piece comes from ONE peer (~800ms
        // at 5MB/s). Parallel across 10 peers = ~80ms. Single biggest
        // seek-latency win.
        sp.set_int (lt::settings_pack::whole_pieces_threshold,    0);
        sp.set_int (lt::settings_pack::max_peerlist_size,         8000);
        sp.set_bool(lt::settings_pack::dont_count_slow_torrents,  true);

        // ── encryption (MSE/PE) ──
        // pe_enabled (vs pe_forced) keeps plaintext as a fallback so we don't
        // lose peers that don't speak MSE; the encrypted handshake is still
        // tried first, which is what defeats most ISP DPI throttling.
        sp.set_int (lt::settings_pack::in_enc_policy,  lt::settings_pack::pe_enabled);
        sp.set_int (lt::settings_pack::out_enc_policy, lt::settings_pack::pe_enabled);
        // both = negotiate full RC4 stream encryption when the peer supports
        // it (header-only is the libtorrent default and is weaker against
        // DPI). This is the actual ISP-throttling-bypass knob.
        sp.set_int (lt::settings_pack::allowed_enc_level, lt::settings_pack::pe_both);
        sp.set_bool(lt::settings_pack::prefer_rc4,         false);
        sp.set_int (lt::settings_pack::mixed_mode_algorithm, lt::settings_pack::peer_proportional);

        // ── reciprocity boost ──
        // Announce pieces ~500 ms before they finish hashing so peers can
        // start requesting from us before we've even completed the piece.
        // This raises our reciprocity score and gets us better unchoke
        // priority from them on the next round — measurably helps streaming
        // on mid-swarm torrents. Value is in MILLISECONDS, not seconds.
        // 500 ms is enough for ~3-5× a typical WAN round-trip without
        // announcing pieces that may still fail the hash check.
        sp.set_int (lt::settings_pack::predictive_piece_announce, 500);

        // ── tracker discovery ──
        // UDP trackers answer ~10× faster than HTTP and have lower overhead
        // for the tracker operator (=> more reliable scrape data). Try them
        // first when both are available.
        sp.set_bool(lt::settings_pack::prefer_udp_trackers, true);

        // port of btserver.go — spoof as qBittorrent 4.3.9
        sp.set_str (lt::settings_pack::user_agent,                 "qBittorrent/4.3.9");
        sp.set_str (lt::settings_pack::peer_fingerprint,           "-qB4390-");
        sp.set_str (lt::settings_pack::handshake_client_version,   "qBittorrent/4.3.9");

        // buffers
        sp.set_int (lt::settings_pack::send_buffer_watermark,     2 * 1024 * 1024);
        sp.set_int (lt::settings_pack::send_buffer_low_watermark, 64 * 1024);
        sp.set_int (lt::settings_pack::send_buffer_watermark_factor, 150);
        sp.set_int (lt::settings_pack::recv_socket_buffer_size,   1024 * 1024);
        sp.set_int (lt::settings_pack::send_socket_buffer_size,   1024 * 1024);

        // Pin the disk I/O backend explicitly instead of trusting the 2.1.x
        // default (pread_disk_io). The pread backend is new in 2.1.0 and has
        // been observed to produce corrupt read-back on Android filesystems.
        // Use posix on Android (most reliable per LibreTorrent findings),
        // mmap on desktop (the proven 2.0.x default).
        lt::session_params params(std::move(sp));
#ifdef __ANDROID__
        params.disk_io_constructor = lt::posix_disk_io_constructor;
#else
        params.disk_io_constructor = lt::mmap_disk_io_constructor;
#endif
        auto* sw = new SessionWrapper(std::move(params));
        sw->init_default_config();
        sw->start_alert_thread();
        set_err("");
        return reinterpret_cast<lt_session_t>(sw);
    } catch (const std::exception& e) { set_err(e.what()); return nullptr; }
}

TORRENT_API void lt_destroy_session(lt_session_t session) {
    if (!session) return;
    auto* sw = to_sw(session);

    {
        std::lock_guard<std::mutex> lifecycle_lock(sw->lifecycle_mu);
        while (!sw->active_stream_ids.empty()) {
            const auto sid = *sw->active_stream_ids.begin()->second.begin();
            stop_stream_internal(sw, sid);
        }
    }

    try {
        sw->alert_running = false;
        if (sw->alert_thread.joinable()) sw->alert_thread.join();
    } catch (...) {}

    // The Dart owner awaits persistence before destruction, while alerts are alive.
    try { delete sw; } catch (...) {}
}

TORRENT_API void lt_set_alert_callback(lt_session_t session,
                                       lt_alert_callback cb, void* ud) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->cb_mu);
    sw->dart_callback  = cb;
    sw->dart_user_data = ud;
}

TORRENT_API void lt_poll_alerts(lt_session_t session,
                                lt_alert_callback cb, void* ud) {
    if (!session || !cb) return;
    auto* sw = to_sw(session);
    std::deque<AlertRecord> local;
    {
        std::lock_guard<std::mutex> lk(sw->dart_queue_mu);
        local.swap(sw->dart_queue);
    }
    for (auto& r : local)
        cb(r.type, r.torrent_id, r.message.c_str(), ud);
}

// ── torrent management ──────────────────────────────────────────────────────────

// Caller holds lifecycle_mu and mu. libtorrent returns the existing handle
// for a duplicate info-hash. It must keep one ID/scheduler/cleanup owner here
// too; a second ID would make switchFile unsafe and misroute piece alerts.
static lt_torrent_id register_torrent(SessionWrapper* sw, const lt::torrent_handle& handle,
                                      bool stream_only, bool resume = true) {
    for (const auto& entry : sw->handles) {
        if (entry.second != handle) continue;
        if ((sw->stream_only_torrents.count(entry.first) != 0) != stream_only) {
            set_err("torrent already exists as torrentId=" + std::to_string(entry.first)
                + " with a different streamOnly mode; reuse that task or remove it before changing mode");
            return -1;
        }
        // Retain the existing save path, pause state and stream demands.
        set_err("");
        return entry.first;
    }
    if (resume) handle.resume();
    const auto id = sw->next_id.fetch_add(1);
    sw->handles[id] = handle;
    if (stream_only) sw->stream_only_torrents.insert(id);
    set_err("");
    return id;
}

TORRENT_API lt_torrent_id lt_add_magnet(lt_session_t session,
                                        const char* uri, const char* path,
                                        int stream_only) {
    if (!session || !uri || !path) { set_err("null arg"); return -1; }
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lifecycle_lock(sw->lifecycle_mu);
    try {
        lt::error_code ec;
        lt::add_torrent_params atp = lt::parse_magnet_uri(uri, ec);
        if (ec) { set_err(ec.message()); return -1; }
        atp.save_path = path;
        atp.flags &= ~lt::torrent_flags::paused;
        atp.flags &= ~lt::torrent_flags::auto_managed;

        // Trackerless magnet (only an info-hash, no &tr=...)? Seed it with a
        // curated set of public open trackers so peer discovery doesn't have
        // to wait for DHT bootstrap (which can take 30-60s on a cold start).
        // These are the same trackers shipped by qBittorrent's default
        // "automatically add" list and TorrServer.
        if (atp.trackers.empty()) {
            static const char* kDefaultTrackers[] = {
                "udp://tracker.opentrackr.org:1337/announce",
                "udp://open.demonii.com:1337/announce",
                "udp://open.stealth.si:80/announce",
                "udp://tracker.torrent.eu.org:451/announce",
                "udp://exodus.desync.com:6969/announce",
                "udp://tracker.openbittorrent.com:6969/announce",
                "udp://tracker.dler.org:6969/announce",
                "udp://explodie.org:6969/announce",
            };
            for (auto* t : kDefaultTrackers) atp.trackers.emplace_back(t);
        }

        // Keep peers connected; the metadata handler clears missing-piece priorities.
        if (stream_only) {
            atp.flags |= lt::torrent_flags::default_dont_download;
            atp.storage_mode = lt::storage_mode_sparse;
        }

        std::lock_guard<std::mutex> registration_lock(sw->mu);
        lt::torrent_handle h = sw->session.add_torrent(std::move(atp), ec);
        if (ec) { set_err(ec.message()); return -1; }
        return register_torrent(sw, h, stream_only != 0);
    } catch (const std::exception& e) { set_err(e.what()); return -1; }
}

TORRENT_API lt_torrent_id lt_add_torrent_file(lt_session_t session,
                                              const char* fp, const char* path,
                                              int stream_only) {
    if (!session || !fp || !path) { set_err("null arg"); return -1; }
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lifecycle_lock(sw->lifecycle_mu);
    try {
        lt::error_code ec;
        auto ti = std::make_shared<lt::torrent_info>(fp, ec);
        if (ec) { set_err(ec.message()); return -1; }
        lt::add_torrent_params atp;
        atp.ti = ti; atp.save_path = path;
        atp.flags &= ~lt::torrent_flags::paused;
        atp.flags &= ~lt::torrent_flags::auto_managed;

        // Metadata is available, so nothing is wanted until a stream starts.
        if (stream_only) {
            atp.flags |= lt::torrent_flags::default_dont_download;
            atp.storage_mode = lt::storage_mode_sparse;
            atp.file_priorities.assign((size_t)ti->num_files(), lt::dont_download);
        }

        std::lock_guard<std::mutex> registration_lock(sw->mu);
        lt::torrent_handle h = sw->session.add_torrent(std::move(atp), ec);
        if (ec) { set_err(ec.message()); return -1; }
        return register_torrent(sw, h, stream_only != 0);
    } catch (const std::exception& e) { set_err(e.what()); return -1; }
}

// Start a non-blocking flush/snapshot. One outstanding request per task.
TORRENT_API int lt_request_resume_data(lt_session_t session, lt_torrent_id id) {
    set_err("");
    if (!session) { set_err("null session"); return -1; }
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lifecycle(sw->lifecycle_mu);
    try {
        ResumeRequest request;
        {
            std::lock_guard<std::mutex> lock(sw->mu);
            request.handle = sw->handles.at(id);
            request.stream_only = sw->stream_only_torrents.count(id) != 0;
        }
        if (request.handle.torrent_file()) {
            const auto scheduler = download_scheduler(sw, id);
            request.selection = scheduler->download_priorities();
            request.paused_files = scheduler->paused_downloads();
        }
        std::lock_guard<std::mutex> lock(sw->resume_mu);
        auto previous = sw->resume_requests.find(id);
        if (previous != sw->resume_requests.end() && previous->second.pending) return 0;
        sw->resume_requests[id] = std::move(request);
        sw->handles.at(id).save_resume_data(lt::torrent_handle::flush_disk_cache
            | lt::torrent_handle::save_info_dict);
        return 1;
    } catch (const std::exception& e) { set_err(e.what()); return -1; }
}

// 0 pending, -1 failed/removed, otherwise required size. Copy does not consume.
TORRENT_API int lt_read_resume_data(lt_session_t session, lt_torrent_id id, char* out, int capacity) {
    set_err("");
    if (!session) { set_err("null session"); return -1; }
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lock(sw->resume_mu);
    auto found = sw->resume_requests.find(id);
    if (found == sw->resume_requests.end()) { set_err("resume request not found"); return -1; }
    const auto& request = found->second;
    if (request.pending) return 0;
    if (!request.error.empty()) { set_err(request.error); return -1; }
    const int size = int(request.bytes.size());
    if (out && capacity >= size) std::memcpy(out, request.bytes.data(), size);
    return size;
}

static bool matching_hashes(const lt::info_hash_t& a, const lt::info_hash_t& b) {
    return (a.has_v1() && b.has_v1() && a.v1 == b.v1)
        || (a.has_v2() && b.has_v2() && a.v2 == b.v2);
}

TORRENT_API lt_torrent_id lt_find_torrent_by_magnet(lt_session_t session, const char* magnet) {
    set_err("");
    if (!session || !magnet) { set_err("null argument"); return -1; }
    try {
        lt::error_code ec;
        const auto params = lt::parse_magnet_uri(magnet, ec);
        if (ec) throw std::runtime_error(ec.message());
        auto* sw = to_sw(session);
        std::lock_guard<std::mutex> lock(sw->mu);
        for (const auto& item : sw->handles)
            if (matching_hashes(params.info_hashes, item.second.info_hashes())) return item.first;
        return 0;
    } catch (const std::exception& e) { set_err(e.what()); return -1; }
}

TORRENT_API const char* lt_get_torrent_magnet(lt_session_t session, lt_torrent_id id) {
    static thread_local std::string result;
    result.clear();
    set_err("");
    try {
        if (!session) throw std::invalid_argument("null session");
        auto* sw = to_sw(session);
        std::lock_guard<std::mutex> lock(sw->mu);
        lt::add_torrent_params params;
        params.info_hashes = sw->handles.at(id).info_hashes();
        result = lt::make_magnet_uri(params);
    } catch (const std::exception& e) { set_err(e.what()); }
    return result.c_str();
}

static lt::add_torrent_params decode_download_resume(const char* data, int size, bool& stream_only,
                                                    std::set<int>& paused_files) {
    if (!data || size <= 0) throw std::invalid_argument("invalid resume buffer");
    lt::error_code ec;
    auto node = lt::bdecode(lt::span<char const>(data, size), ec);
    if (ec || node.type() != lt::bdecode_node::dict_t
        || (node.dict_find_int_value("ltf-version") != 1 && node.dict_find_int_value("ltf-version") != 2))
        throw std::invalid_argument("invalid or unsupported download resume data");
    auto params = lt::read_resume_data(node, ec);
    if (ec) throw std::runtime_error(ec.message());
    if (params.save_path.empty()) throw std::invalid_argument("resume save path is empty");
    stream_only = node.dict_find_int_value("ltf-stream-only") != 0;
    params.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::stop_when_ready);
    params.piece_priorities.clear();
    if (params.ti && !params.file_priorities.empty()
        && params.file_priorities.size() != size_t(params.ti->num_files()))
        throw std::invalid_argument("resume file selection does not match metadata");
    if (node.dict_find_int_value("ltf-version") == 2) {
        const auto paused = node.dict_find_list("ltf-paused-files");
        if (!paused) throw std::invalid_argument("missing paused file selection");
        for (int i = 0; i < paused.list_size(); ++i) {
            const auto value = paused.list_at(i);
            if (value.type() != lt::bdecode_node::int_t)
                throw std::invalid_argument("invalid paused file index");
            const auto file = value.int_value();
            if (!params.ti || file < 0 || file >= params.ti->num_files()
                || size_t(file) >= params.file_priorities.size()
                || params.file_priorities[size_t(file)] == lt::dont_download
                || params.ti->files().pad_file_at(lt::file_index_t(int(file)))
                || !paused_files.insert(int(file)).second)
                throw std::invalid_argument("invalid paused file selection");
        }
    }
    return params;
}

TORRENT_API lt_torrent_id lt_validate_resume_data(lt_session_t session, const char* data, int size) {
    (void)session;
    set_err("");
    try {
        bool stream_only = false;
        std::set<int> paused_files;
        decode_download_resume(data, size, stream_only, paused_files);
        return 1;
    } catch (const std::exception& e) { set_err(e.what()); return -1; }
}

TORRENT_API lt_torrent_id lt_restore_resume_data(lt_session_t session, const char* data, int size) {
    set_err("");
    if (!session || !data || size <= 0) { set_err("invalid resume buffer"); return -1; }
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lifecycle(sw->lifecycle_mu);
    try {
        bool stream_only = false;
        std::set<int> paused_files;
        auto params = decode_download_resume(data, size, stream_only, paused_files);
        lt::error_code ec;
        const auto selection = params.file_priorities;
        std::lock_guard<std::mutex> streams_lock(sw->streams_mu);
        std::lock_guard<std::mutex> lock(sw->mu);
        // Restore is idempotent and must never overwrite a live task's selection.
        for (const auto& item : sw->handles)
            if (matching_hashes(params.info_hashes, item.second.info_hashes())) return item.first;
        // Gate network activity until the paused baseline has been installed.
        const bool torrent_paused = bool(params.flags & lt::torrent_flags::paused);
        params.flags |= lt::torrent_flags::paused;
        auto handle = sw->session.add_torrent(std::move(params), ec);
        if (ec) throw std::runtime_error(ec.message());
        const auto id = register_torrent(sw, handle, stream_only, false);
        if (handle.torrent_file()) {
            auto scheduler = std::make_shared<StreamScheduler>(handle, true);
            if (!selection.empty()) scheduler->select_files(selection, paused_files);
            sw->schedulers[id] = scheduler;
        }
        if (!torrent_paused) handle.resume();
        return id;
    } catch (const std::exception& e) { set_err(e.what()); return -1; }
}

TORRENT_API void lt_remove_torrent(lt_session_t session,
                                   lt_torrent_id id, int del) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lifecycle_lock(sw->lifecycle_mu);
    remove_torrent_internal(sw, id, del != 0);
}

TORRENT_API void lt_pause_torrent(lt_session_t session, lt_torrent_id id) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it != sw->handles.end() && it->second.is_valid())
        try { it->second.pause(); } catch (...) {}
}

TORRENT_API void lt_resume_torrent(lt_session_t session, lt_torrent_id id) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it != sw->handles.end() && it->second.is_valid())
        try { it->second.resume(); } catch (...) {}
}

TORRENT_API void lt_recheck_torrent(lt_session_t session, lt_torrent_id id) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it != sw->handles.end() && it->second.is_valid())
        try { it->second.force_recheck(); } catch (...) {}
}

// ── status queries ──────────────────────────────────────────────────────────────

TORRENT_API int lt_get_torrent_count(lt_session_t session) {
    if (!session) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    return (int)sw->handles.size();
}

TORRENT_API int lt_get_all_statuses(lt_session_t session,
                                    lt_torrent_status* out, int max) {
    if (!session || !out || max <= 0) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    int n = 0;
    for (auto& kv : sw->handles) {
        if (n >= max) break;
        if (!kv.second.is_valid()) continue;
        try {
            // save_path is populated only when explicitly requested by status flags.
            lt::torrent_status st = kv.second.status(
                lt::torrent_handle::query_pieces | lt::torrent_handle::query_save_path);
            fill_status(out[n], kv.first, st);
            n++;
        } catch (...) {}
    }
    return n;
}

TORRENT_API int lt_get_status(lt_session_t session, lt_torrent_id id,
                              lt_torrent_status* out) {
    if (!session || !out) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it == sw->handles.end() || !it->second.is_valid()) return 0;
    try { fill_status(*out, id,
          it->second.status(lt::torrent_handle::query_pieces |
                            lt::torrent_handle::query_save_path)); return 1; } catch (...) { return 0; }
}

// ── file queries ────────────────────────────────────────────────────────────────

TORRENT_API int lt_get_file_count(lt_session_t session, lt_torrent_id id) {
    if (!session) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    try { auto ti = sw->metadata(id); return ti ? ti->num_files() : 0; }
    catch (...) { return 0; }
}

TORRENT_API int lt_get_files(lt_session_t session, lt_torrent_id id,
                             lt_file_info* out, int max) {
    if (!session || !out || max <= 0) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    try {
        auto ti = sw->metadata(id);
        if (!ti) {
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_ERROR, "libtorrent_flutter", "lt_get_files: ti is null");
#endif
            return 0;
        }
        const lt::file_storage& fs = ti->files();
        int n = 0;
        for (int i = 0; i < fs.num_files() && n < max; ++i, ++n) {
            lt::file_index_t fi{i};
            out[n].index = i;
            out[n].size = fs.file_size(fi);
            std::string nm = std::string(fs.file_name(fi));
            out[n].is_streamable = is_streamable(nm) ? 1 : 0;
            std::string pt = fs.file_path(fi);
            std::strncpy(out[n].name, nm.c_str(), sizeof(out[n].name) - 1);
            std::strncpy(out[n].path, pt.c_str(), sizeof(out[n].path) - 1);
            out[n].name[sizeof(out[n].name) - 1] = 0;
            out[n].path[sizeof(out[n].path) - 1] = 0;
        }
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "libtorrent_flutter", "lt_get_files: successfully processed %d files", n);
#endif
        return n;
    } catch (const std::exception& e) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "libtorrent_flutter", "lt_get_files exception: %s", e.what());
#endif
        return 0;
    } catch (...) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "libtorrent_flutter", "lt_get_files unknown exception");
#endif
        return 0;
    }
}

TORRENT_API void lt_set_file_priorities(lt_session_t session, lt_torrent_id id,
                                        const int32_t* priorities, int count) {
    set_err("");
    if (!session || !priorities || count <= 0) { set_err("invalid file priorities"); return; }
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lifecycle_lock(sw->lifecycle_mu);
    if (sw->active_stream_ids.count(id)) {
        set_err("stop active streams before changing file priorities"); return;
    }
    try {
        const auto scheduler = download_scheduler(sw, id);
        auto selection = scheduler->download_priorities();
        if (size_t(count) > selection.size()) throw std::invalid_argument("too many file priorities");
        for (int i = 0; i < count; ++i)
            if (priorities[i] < 0 || priorities[i] > 7) throw std::invalid_argument("file priority must be 0..7");
        for (size_t i = 0; i < selection.size(); ++i)
            selection[i] = lt::download_priority_t(i < size_t(count) ? priorities[i] : 0);
        scheduler->select_files(selection);
        std::lock_guard<std::mutex> lock(sw->mu);
        auto handle = sw->handles.at(id);
        handle.unset_flags(lt::torrent_flags::stop_when_ready);
        handle.resume();
    } catch (const std::exception& error) { set_err(error.what()); }
}

TORRENT_API int lt_download_file(lt_session_t session, lt_torrent_id id, int file_index) {
    set_err("");
    if (!session) { set_err("null session"); return 0; }
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lifecycle_lock(sw->lifecycle_mu);
    try {
        std::shared_ptr<const lt::torrent_info> ti;
        {
            std::lock_guard<std::mutex> lock(sw->mu);
            ti = sw->metadata(id);
        }
        if (!ti) throw std::runtime_error("torrent not found or metadata is not ready");
        if (file_index < 0 || file_index >= ti->num_files() || ti->files().pad_file_at(lt::file_index_t(file_index)))
            throw std::invalid_argument("invalid download file index");
        download_scheduler(sw, id)->download_file(file_index);
        std::lock_guard<std::mutex> lock(sw->mu);
        auto handle = sw->handles.at(id);
        handle.unset_flags(lt::torrent_flags::stop_when_ready);
        handle.resume();
        return 1;
    } catch (const std::exception& error) { set_err(error.what()); return 0; }
}

static int set_file_download_paused(lt_session_t session, lt_torrent_id id, int file_index, bool paused) {
    set_err("");
    if (!session) { set_err("null session"); return 0; }
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lifecycle_lock(sw->lifecycle_mu);
    try {
        const auto scheduler = download_scheduler(sw, id);
        if (paused) {
            std::lock_guard<std::mutex> streams_lock(sw->streams_mu);
            for (const auto& entry : sw->streams) {
                if (entry.second->torrent_id == id && entry.second->file_index == file_index) {
                    set_err("file_in_use");
                    return -1;
                }
            }
        }
        scheduler->set_download_paused(file_index, paused);
        if (!paused) {
            std::lock_guard<std::mutex> lock(sw->mu);
            auto handle = sw->handles.at(id);
            handle.unset_flags(lt::torrent_flags::stop_when_ready);
            handle.resume();
        }
        return 1;
    } catch (const std::exception& error) { set_err(error.what()); return 0; }
}

TORRENT_API int lt_pause_file_download(lt_session_t session, lt_torrent_id id, int file_index) {
    return set_file_download_paused(session, id, file_index, true);
}

TORRENT_API int lt_resume_file_download(lt_session_t session, lt_torrent_id id, int file_index) {
    return set_file_download_paused(session, id, file_index, false);
}

TORRENT_API int lt_cancel_file_download(lt_session_t session, lt_torrent_id id, int file_index) {
    set_err("");
    if (!session) { set_err("null session"); return 0; }
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lifecycle_lock(sw->lifecycle_mu);
    try {
        download_scheduler(sw, id)->cancel_download(file_index);
        return 1;
    } catch (const std::exception& error) { set_err(error.what()); return 0; }
}

TORRENT_API int lt_has_downloads(lt_session_t session, lt_torrent_id id) {
    set_err("");
    if (!session) { set_err("null session"); return -1; }
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lifecycle_lock(sw->lifecycle_mu);
    try {
        {
            std::lock_guard<std::mutex> lock(sw->mu);
            auto found = sw->handles.find(id);
            if (found == sw->handles.end() || !found->second.is_valid()) return 0;
            // A normal magnet owns a download even before its file list arrives.
            if (!found->second.torrent_file()) return sw->stream_only_torrents.count(id) ? 0 : 1;
        }
        return download_scheduler(sw, id)->has_downloads() ? 1 : 0;
    } catch (const std::exception& error) { set_err(error.what()); return -1; }
}

TORRENT_API int lt_get_file_downloads(lt_session_t session, lt_torrent_id id,
                                     lt_file_download_status* out, int max_count) {
    set_err("");
    if (!session || !out || max_count <= 0) { set_err("invalid download status buffer"); return -1; }
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lifecycle_lock(sw->lifecycle_mu);
    try {
        const auto scheduler = download_scheduler(sw, id);
        const auto selection = scheduler->download_priorities();
        const auto paused = scheduler->paused_downloads();
        lt::torrent_handle handle;
        {
            std::lock_guard<std::mutex> lock(sw->mu);
            handle = sw->handles.at(id);
        }
        const auto ti = handle.torrent_file();
        const auto& files = ti->files();
        const auto status = handle.status(lt::torrent_handle::query_pieces);
        int count = 0;
        for (int file = 0; file < files.num_files() && count < max_count; ++file) {
            if (selection[file] == lt::dont_download) continue;
            auto& result = out[count++];
            result = {};
            result.file_index = file;
            result.priority = static_cast<std::uint8_t>(selection[file]);
            result.is_paused = paused.count(file) ? 1 : 0;
            const auto index = lt::file_index_t(file);
            const int64_t first_byte = files.file_offset(index);
            result.file_size = files.file_size(index);
            const int64_t end_byte = first_byte + result.file_size;
            for (int piece = int(first_byte / ti->piece_length()); result.file_size > 0
                 && piece <= (end_byte - 1) / ti->piece_length(); ++piece) {
                if (piece >= status.pieces.size() || !status.pieces[lt::piece_index_t(piece)]) continue;
                result.verified_bytes += std::min(end_byte, int64_t(piece + 1) * ti->piece_length())
                    - std::max(first_byte, int64_t(piece) * ti->piece_length());
            }
            result.is_complete = result.verified_bytes == result.file_size ? 1 : 0;
        }
        return count;
    } catch (const std::exception& error) { set_err(error.what()); return -1; }
}

// ── streaming ───────────────────────────────────────────────────────────────────

// Serialize validation, optional stream replacement and startup as one operation.
// startStream never deletes data or replaces existing streams implicitly.
static lt_stream_id open_stream(lt_session_t session, lt_torrent_id torrent_id,
                                int file_index, int64_t max_cache_bytes, bool switch_file) {
    if (!session) { set_err("null session"); return -1; }
    auto* sw = to_sw(session);

    std::lock_guard<std::mutex> lifecycle_lock(sw->lifecycle_mu);
    if (sw->removing_torrents.count(torrent_id)) {
        set_err("torrent is being removed"); return -1;
    }

    std::shared_ptr<const lt::torrent_info> ti;
    {
        std::lock_guard<std::mutex> lock(sw->mu);
        ti = sw->metadata(torrent_id);
    }
    if (!ti) { set_err("torrent not found or no metadata yet"); return -1; }
    const lt::file_storage& fs = ti->files();

    if (switch_file && file_index < 0) {
        set_err("switchFile requires an explicit valid file index"); return -1;
    }
    // auto-select largest streamable file for startStream only
    if (file_index < 0) {
        int64_t best = -1; file_index = 0;
        for (int i = 0; i < fs.num_files(); ++i) {
            int64_t sz = fs.file_size(lt::file_index_t{i});
            if (sz > best && is_streamable(std::string(fs.file_name(lt::file_index_t{i})))) {
                best = sz; file_index = i;
            }
        }
    }
    if (file_index < 0 || file_index >= fs.num_files()) {
        set_err("invalid file index"); return -1;
    }

    if (fs.file_size(lt::file_index_t{file_index}) <= 0) {
        set_err("selected file is empty"); return -1;
    }

    lt::torrent_handle handle;
    {
        std::lock_guard<std::mutex> lock(sw->mu);
        auto found = sw->handles.find(torrent_id);
        if (found == sw->handles.end()) { set_err("torrent not found"); return -1; }
        handle = found->second;
    }
    if (!handle.is_valid()) { set_err("invalid handle"); return -1; }

    if (switch_file) {
        // Withdraw every old stream's demands. The scheduler preserves full-file
        // download baselines; disk pieces and peer connections remain intact.
        try {
            auto owners = sw->active_stream_ids.find(torrent_id);
            if (owners != sw->active_stream_ids.end()) {
                const auto ids = owners->second;
                for (auto sid : ids) stop_stream_internal(sw, sid);
            }
        } catch (const std::exception& error) {
            set_err(error.what()); return -1;
        }
    }

    std::unique_lock<std::mutex> startup_lock(sw->streams_mu);
    auto s = std::make_unique<StreamEngine>();
    s->id           = sw->next_stream_id.fetch_add(1);
    s->torrent_id   = torrent_id;
    s->file_index   = file_index;
    s->handle       = handle;
    s->ti           = ti;
    s->piece_length = ti->piece_length();
    s->file_size    = fs.file_size(lt::file_index_t{file_index});
    s->file_offset  = fs.file_offset(lt::file_index_t{file_index});
    s->start_piece  = std::max(0, (int)(s->file_offset / s->piece_length));
    s->end_piece    = std::min((int)ti->num_pieces() - 1,
                     (int)((s->file_offset + s->file_size - 1) / s->piece_length));
    s->total_pieces = s->end_piece - s->start_piece + 1;

    // head/tail protection boundaries
    int prot_pieces = (int)((StreamEngine::PROTECT_BYTES + s->piece_length - 1) / s->piece_length);
    s->head_end_piece  = std::min(s->start_piece + prot_pieces - 1, s->end_piece);
    s->tail_start_piece = std::max(s->end_piece - prot_pieces + 1, s->start_piece);

    // ── Adaptive bitrate estimation ──
    // Estimate media bitrate from file size assuming typical video duration.
    // This is crude but vastly better than a fixed 5 Mbps for ALL files.
    // A 1.2GB file ≈ 1.5h movie ≈ ~1.5 MB/s; a 4GB file ≈ 2h movie ≈ ~3.3 MB/s
    {
        float duration_guess;
        if      (s->file_size < 400LL * 1024 * 1024)            duration_guess = 3600.0f;  // <400MB → 1h (episode)
        else if (s->file_size < 1LL * 1024 * 1024 * 1024)       duration_guess = 5400.0f;  // <1GB → 1.5h
        else if (s->file_size < 3LL * 1024 * 1024 * 1024)       duration_guess = 6600.0f;  // <3GB → 1.8h
        else if (s->file_size < 8LL * 1024 * 1024 * 1024)       duration_guess = 7200.0f;  // <8GB → 2h
        else                                                     duration_guess = 9000.0f;  // 8GB+ → 2.5h
        s->estimated_bitrate_bps = (float)s->file_size / duration_guess;

        // Critical startup: target ~2 seconds of video, but at least 1 piece
        // and at most 5 pieces. For a 4GB file with 4MB pieces and ~3.3MB/s
        // bitrate, this gives max(1, 6.6/4) = 1-2 pieces instead of 5.
        int startup_bytes = (int)(s->estimated_bitrate_bps * 2.0f);
        startup_bytes = std::max(startup_bytes, s->piece_length);
        s->critical_startup_pieces = std::clamp(startup_bytes / s->piece_length, 1, 5);

        TB_LOG("ADAPTIVE: file_size=%lldMB piece=%dKB bitrate_est=%.0fKB/s critical_pieces=%d",
               (long long)(s->file_size / (1024*1024)), s->piece_length / 1024,
               s->estimated_bitrate_bps / 1024.0f, s->critical_startup_pieces);
    }

    // ── TorrCache initialization — port of torrstor.NewCache + Cache.Init ──
    // Use session-level bt_config cache_size, fall back to explicit arg, then default 64MB
    int64_t cache_capacity = (max_cache_bytes > 0) ? max_cache_bytes
                           : (sw->bt_config.cache_size > 0) ? sw->bt_config.cache_size
                           : (64 * 1024 * 1024);
    s->cache = std::make_unique<TorrCache>();
    s->cache->init(cache_capacity, s->piece_length, ti->num_pieces(), handle);
    // apply session-level connections_limit to cache AND to libtorrent.
    // The cache field is informational; the actual swarm-fanout cap lives
    // on torrent_handle::set_max_connections(). Without this call, every
    // torrent inherits the session-wide 200 cap and on high-seed swarms
    // the time-critical picker fans block requests across hundreds of
    // peers — head-of-line blocked by the slowest one. TorrServer caps at
    // 25 per torrent for exactly this reason.
    if (sw->bt_config.connections_limit > 0) {
        s->cache->connections_limit = sw->bt_config.connections_limit;
        try { handle.set_max_connections(sw->bt_config.connections_limit); } catch (...) {}
    }
    // apply session-level reader_read_ahead to cache
    if (sw->bt_config.reader_read_ahead >= 5 && sw->bt_config.reader_read_ahead <= 100)
        s->cache->reader_read_ahead_pct = sw->bt_config.reader_read_ahead;

    try {
        auto& scheduler = sw->schedulers[torrent_id];
        if (!scheduler) {
            std::lock_guard<std::mutex> lock(sw->mu);
            scheduler = std::make_shared<StreamScheduler>(handle, !sw->stream_only_torrents.count(torrent_id));
        }
        s->scheduler = scheduler;
        scheduler->add_stream(s->id);
        handle.unset_flags(lt::torrent_flags::stop_when_ready);

        const int critical_end = std::min(s->start_piece + s->critical_startup_pieces - 1, s->end_piece);
        for (int p = s->start_piece; p <= s->head_end_piece; ++p) {
            const bool critical = p <= critical_end;
            scheduler->request(s->id, p, critical ? 7 : 4,
                critical ? (p - s->start_piece) * 50 : -1);
        }
        for (int p = s->tail_start_piece; p <= s->end_piece; ++p)
            scheduler->request(s->id, p, 5, 1000);

        // NOW resume — priorities are already set, no random downloads
        handle.resume();

        // Do NOT use sequential_download — let deadlines drive piece order.
        // libtorrent's time-critical picker (via set_piece_deadline) is the
        // proper streaming mechanism. Sequential mode fights deadline-driven
        // scheduling and reduces swarm efficiency during seeks.
        handle.unset_flags(lt::torrent_flags::sequential_download);

        // populate pieces we already have — mark them in pieces_have set.
        // Only load nearby pieces into RAM cache (head + tail).
        // Loading ALL pieces would flood I/O and fill the cache budget.
        lt::torrent_status ts = handle.status(lt::torrent_handle::query_pieces);
        // Guard: on Android ARM64, pieces bitfield can have null internal
        // buffer immediately after resume() — get_bit() would SIGSEGV.
        if (ts.has_metadata && !ts.pieces.empty() && ts.pieces.size() > 0)
        for (int p = s->start_piece; p <= s->end_piece; ++p) {
            if (ts.pieces.get_bit(lt::piece_index_t(p))) {
                s->pieces_have.insert(p);
                auto* cp = s->cache->get_piece(p);
                if (cp) cp->mark_complete();
                // only pre-load first 2 + tail pieces into read_results
                // (kept minimal to limit RAM — serve_range reads the rest on demand)
                bool is_head = (p <= s->start_piece + 1);
                bool is_tail = (p >= s->tail_start_piece);
                if (is_head || is_tail) {
                    try { handle.read_piece(lt::piece_index_t(p)); } catch (...) {}
                }
            }
        }

    } catch (const std::exception& error) {
        if (s->scheduler) s->scheduler->remove_stream(s->id);
        set_err(error.what());
        return -1;
    }

    lt_stream_id sid = s->id;
    StreamEngine* raw = s.get();

    sw->streams[sid] = std::move(s);
    sw->active_stream_ids[torrent_id].insert(sid);
    startup_lock.unlock();

    // start HTTP server thread
    try {
        raw->server_thread = std::thread([sw, raw]() { run_http_server(sw, raw); });
    } catch (const std::system_error& error) {
        stop_stream_internal(sw, sid);
        set_err(error.what()); return -1;
    }

    // wait for server to bind
    for (int i = 0; i < 200 && !raw->running.load(); ++i)
        std::this_thread::sleep_for(chr::milliseconds(10));

    if (!raw->running.load()) {
        stop_stream_internal(sw, sid);
        set_err("HTTP server failed to bind"); return -1;
    }

    set_err(""); return sid;
}

TORRENT_API lt_stream_id lt_start_stream(lt_session_t session, lt_torrent_id torrent_id,
                                         int file_index, int64_t max_cache_bytes) {
    return open_stream(session, torrent_id, file_index, max_cache_bytes, false);
}

TORRENT_API lt_stream_id lt_switch_file(lt_session_t session, lt_torrent_id torrent_id,
                                       int file_index, int64_t max_cache_bytes) {
    return open_stream(session, torrent_id, file_index, max_cache_bytes, true);
}

// Retain both stop symbols for ABI compatibility. Both now withdraw playback
// demands only; explicit torrent removal is the sole deletion path.
TORRENT_API void lt_stop_stream(lt_session_t session, lt_stream_id sid) {
    lt_stop_stream_ex(session, sid, 0);
}

TORRENT_API void lt_stop_stream_ex(lt_session_t session, lt_stream_id sid, int keep_torrent) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lifecycle_lock(sw->lifecycle_mu);
    set_err("");
    try {
        (void)keep_torrent; // Kept for ABI compatibility; stopping never deletes data.
        stop_stream_internal(sw, sid);
    } catch (const std::exception& error) {
        set_err(error.what());
    }
}

static void fill_stream_status(lt_stream_status* out, const StreamEngine* s) {
    out->id         = s->id;
    out->torrent_id = s->torrent_id;
    out->file_index = s->file_index;
    out->file_size  = s->file_size;
    out->read_head  = s->read_head.load();
    out->stream_state = s->stream_state.load();

    // readahead_window — fixed 16MB / piece_length
    out->readahead_window = (s->piece_length > 0)
        ? (int)(StreamEngine::FIXED_READAHEAD / s->piece_length)
        : 16;

    // contiguous buffer from playback position
    int play = std::clamp(s->byte_to_piece(s->read_head.load()),
                          s->start_piece, s->end_piece);
    int contiguous = 0;
    {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(s->piece_mu));
        int p = play;
        while (p <= s->end_piece && s->pieces_have.count(p)) {
            contiguous++; p++;
        }
    }
    out->buffer_pieces = contiguous;

    // Use estimated bitrate for buffer reporting — adaptive to file size
    float bitrate = s->estimated_bitrate_bps;
    out->buffer_seconds = (float)contiguous * s->piece_length / bitrate;

    // telemetry from handle
    try {
        lt::torrent_status ts = s->handle.status();
        out->active_peers  = ts.num_peers;
        out->download_rate = ts.download_rate;
    } catch (...) {
        out->active_peers  = 0;
        out->download_rate = 0;
    }

    std::string url = s->make_url();
    std::strncpy(out->url, url.c_str(), sizeof(out->url) - 1);
    out->url[sizeof(out->url) - 1] = 0;
}

TORRENT_API int lt_check_stream_start(lt_session_t session, lt_stream_id sid) {
    if (!session) return -1;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lock(sw->streams_mu);
    auto it = sw->streams.find(sid);
    if (it == sw->streams.end()) return -1;
    auto& stream = *it->second;
    if (!stream.active || !stream.running || !stream.handle.is_valid()) return -1;
    try {
        // Use the selected file's first torrent piece, never read_head (which
        // can follow an independent player probe at a different position).
        if (!stream.handle.have_piece(lt::piece_index_t(stream.start_piece))) return 0;
        if (stream.start_piece_readable || stream.has_cached_piece(stream.start_piece)) return 1;
        const auto now = chr::steady_clock::now();
        if (now >= stream.next_start_read) {
            stream.next_start_read = now + chr::seconds(1);
            stream.handle.read_piece(lt::piece_index_t(stream.start_piece));
        }
        return 0; // read_piece_alert confirms successful I/O on a later probe
    } catch (const std::exception& error) {
        set_err(error.what());
        return -2;
    }
}

TORRENT_API int lt_get_stream_status(lt_session_t session,
                                     lt_stream_id sid, lt_stream_status* out) {
    if (!session || !out) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->streams_mu);
    auto it = sw->streams.find(sid);
    if (it == sw->streams.end()) return 0;
    fill_stream_status(out, it->second.get());
    return 1;
}

TORRENT_API int lt_get_all_stream_statuses(lt_session_t session,
                                           lt_stream_status* out, int max) {
    if (!session || !out || max <= 0) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->streams_mu);
    int n = 0;
    for (auto& kv : sw->streams) {
        if (n >= max) break;
        fill_stream_status(&out[n++], kv.second.get());
    }
    return n;
}

// ── speed limits ────────────────────────────────────────────────────────────────

TORRENT_API void lt_set_download_limit(lt_session_t session, int bps) {
    if (!session) return;
    lt::settings_pack sp;
    sp.set_int(lt::settings_pack::download_rate_limit, bps);
    to_sw(session)->session.apply_settings(sp);
}

TORRENT_API void lt_set_upload_limit(lt_session_t session, int bps) {
    if (!session) return;
    lt::settings_pack sp;
    sp.set_int(lt::settings_pack::upload_rate_limit, bps);
    to_sw(session)->session.apply_settings(sp);
}

// ── utility ─────────────────────────────────────────────────────────────────────

TORRENT_API const char* lt_last_error(void) { return g_last_error.c_str(); }
TORRENT_API const char* lt_version(void)    { return LIBTORRENT_VERSION; }

// ── preload — port of torr/preload.go ───────────────────────────────────────────

TORRENT_API int lt_preload_stream(lt_session_t session, lt_stream_id sid,
                                  int64_t preload_bytes) {
    if (!session) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->streams_mu);
    auto it = sw->streams.find(sid);
    if (it == sw->streams.end()) return 0;

    auto* s = it->second.get();
    if (!s->active.load() || s->preloading.exchange(true)) return 0;
    if (s->preload_thread.joinable()) s->preload_thread.join();

    // default preload 16MB — port of preload.go startend
    if (preload_bytes <= 0) preload_bytes = 16 * 1024 * 1024;

    s->preload_thread = std::thread([s, preload_bytes]() {
        preload_stream(s, preload_bytes);
    });
    return 1;
}

// ── cache settings — port of settings/btsets.go ─────────────────────────────────

TORRENT_API void lt_set_cache_settings(lt_session_t session, lt_stream_id sid,
                                       int64_t capacity,
                                       int read_ahead_pct,
                                       int connections_limit) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->streams_mu);
    auto it = sw->streams.find(sid);
    if (it == sw->streams.end()) return;

    auto* s = it->second.get();
    if (!s->cache) return;

    if (capacity > 0) {
        s->cache->capacity = capacity;
        s->enforce_cache_budget();
    }
    if (read_ahead_pct >= 5 && read_ahead_pct <= 100)
        s->cache->reader_read_ahead_pct = read_ahead_pct;
    if (connections_limit > 0) {
        s->cache->connections_limit = connections_limit;
        // Wire to libtorrent so it actually takes effect on the swarm.
        try { s->handle.set_max_connections(connections_limit); } catch (...) {}
    }
}

// ── engine config — port of settings/btsets.go + btserver.go configure() ────────
// Applies TorrServer-equivalent settings to the libtorrent session.
// Maps BTSets fields → libtorrent settings_pack values, matching the
// exact behavior of btserver.go configure().

TORRENT_API void lt_configure_session(lt_session_t session,
                                      const lt_bt_config* config) {
    if (!session || !config) return;
    auto* sw = to_sw(session);

    // validate + store — port of settings.SetBTSets failsafe checks
    lt_bt_config cfg = *config;

    if (cfg.cache_size == 0)
        cfg.cache_size = 64 * 1024 * 1024;
    if (cfg.connections_limit == 0)
        cfg.connections_limit = 25;
    if (cfg.torrent_disconnect_timeout == 0)
        cfg.torrent_disconnect_timeout = 30;
    if (cfg.reader_read_ahead < 5)
        cfg.reader_read_ahead = 5;
    if (cfg.reader_read_ahead > 100)
        cfg.reader_read_ahead = 100;
    if (cfg.preload_cache < 0)
        cfg.preload_cache = 0;
    if (cfg.preload_cache > 100)
        cfg.preload_cache = 100;

    sw->bt_config = cfg;

    // apply to libtorrent session — port of btserver.go configure()
    lt::settings_pack sp;

    // port of: bt.config.DisableIPv6 = !settings.BTsets.EnableIPv6
    if (!cfg.enable_ipv6) {
        sp.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:6881");
    }

    // port of: bt.config.DisableTCP / DisableUTP
    // libtorrent doesn't have direct disable flags — use listen_interfaces
    // If both disabled, that's invalid, so skip
    if (cfg.disable_tcp && !cfg.disable_utp) {
        // UTP only — listen on UDP only (libtorrent uses same interfaces for both)
        // libtorrent doesn't directly support TCP-only disable, we approximate
        // by removing TCP from outgoing
    }
    if (cfg.disable_utp && !cfg.disable_tcp) {
        sp.set_bool(lt::settings_pack::enable_outgoing_utp, false);
        sp.set_bool(lt::settings_pack::enable_incoming_utp, false);
    }

    // port of: bt.config.NoDefaultPortForwarding = settings.BTsets.DisableUPNP
    sp.set_bool(lt::settings_pack::enable_upnp,  !cfg.disable_upnp);
    sp.set_bool(lt::settings_pack::enable_natpmp, !cfg.disable_upnp);

    // port of: bt.config.NoDHT = settings.BTsets.DisableDHT
    sp.set_bool(lt::settings_pack::enable_dht, !cfg.disable_dht);

    // port of: bt.config.NoUpload = settings.BTsets.DisableUpload
    if (cfg.disable_upload) {
        sp.set_int(lt::settings_pack::upload_rate_limit, 1); // near-zero upload
        sp.set_int(lt::settings_pack::unchoke_slots_limit, 0);
        sp.set_int(lt::settings_pack::active_seeds, 0);
    }

    // port of: bt.config.EstablishedConnsPerTorrent = settings.BTsets.ConnectionsLimit
    sp.set_int(lt::settings_pack::connections_limit, cfg.connections_limit * 20);

    // port of: bt.config.TotalHalfOpenConns = 500
    // (already hardcoded in lt_create_session, re-apply for safety)

    // port of: bt.config.EncryptionPolicy
    if (cfg.force_encrypt) {
        sp.set_int(lt::settings_pack::in_enc_policy,  lt::settings_pack::pe_forced);
        sp.set_int(lt::settings_pack::out_enc_policy, lt::settings_pack::pe_forced);
    } else {
        sp.set_int(lt::settings_pack::in_enc_policy,  lt::settings_pack::pe_enabled);
        sp.set_int(lt::settings_pack::out_enc_policy, lt::settings_pack::pe_enabled);
    }

    // port of: rate limits in KB/s → bytes/s
    if (cfg.download_rate_limit > 0) {
        sp.set_int(lt::settings_pack::download_rate_limit, cfg.download_rate_limit * 1024);
    } else {
        sp.set_int(lt::settings_pack::download_rate_limit, 0);
    }
    if (cfg.upload_rate_limit > 0) {
        sp.set_int(lt::settings_pack::upload_rate_limit, cfg.upload_rate_limit * 1024);
    } else if (!cfg.disable_upload) {
        sp.set_int(lt::settings_pack::upload_rate_limit, 0);
    }

    // port of: userAgent spoofing — TorrServer pretends to be qBittorrent 4.3.9
    sp.set_str(lt::settings_pack::user_agent, "qBittorrent/4.3.9");
    sp.set_str(lt::settings_pack::peer_fingerprint, "-qB4390-");
    sp.set_str(lt::settings_pack::handshake_client_version, "qBittorrent/4.3.9");

    try {
        sw->session.apply_settings(sp);
    } catch (...) {}

    // Re-apply stream-local settings to active streams as well. Without
    // this, configureSession() only affects streams created AFTER the call,
    // so changing connections_limit while a stream is already playing does
    // nothing until the user stops and restarts streaming.
    {
        std::lock_guard<std::mutex> lk(sw->streams_mu);
        for (auto& kv : sw->streams) {
            auto* s = kv.second.get();
            if (!s || !s->cache) continue;

            if (cfg.cache_size > 0) {
                s->cache->capacity = cfg.cache_size;
                s->enforce_cache_budget();
            }
            if (cfg.reader_read_ahead >= 5 && cfg.reader_read_ahead <= 100)
                s->cache->reader_read_ahead_pct = cfg.reader_read_ahead;
            if (cfg.connections_limit > 0) {
                s->cache->connections_limit = cfg.connections_limit;
                try { s->handle.set_max_connections(cfg.connections_limit); } catch (...) {}
            }
        }
    }
}

TORRENT_API void lt_get_default_config(lt_bt_config* out) {
    if (!out) return;
    // port of settings.SetDefaultConfig()
    out->cache_size = 64 * 1024 * 1024;       // 64 MB
    out->reader_read_ahead = 95;               // 95%
    out->preload_cache = 50;                   // 50%
    out->connections_limit = 25;
    out->torrent_disconnect_timeout = 30;      // 30 seconds
    out->force_encrypt = 0;
    out->disable_tcp = 0;
    out->disable_utp = 0;
    out->disable_upload = 0;
    out->disable_dht = 0;
    out->disable_upnp = 0;
    out->enable_ipv6 = 0;
    out->download_rate_limit = 0;
    out->upload_rate_limit = 0;
    out->peers_listen_port = 0;
    out->responsive_mode = 1;
}

// port of stream.go GetActiveStreams()
TORRENT_API int lt_get_active_streams(lt_session_t session) {
    (void)session;
    return StreamEngine::active_streams.load();
}

} // extern "C"
