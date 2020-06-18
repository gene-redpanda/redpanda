#pragma once
#include "bytes/iobuf.h"
#include "seastarx.h"
#include "storage/log_manager.h"
#include "storage/parser.h"
#include "storage/snapshot.h"
#include "utils/mutex.h"

#include <seastar/core/future.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/timer.hh>

#include <absl/container/flat_hash_map.h>

namespace storage {

/**
 * Durable key-value store.
 *
 * Manages a mapping between string and blob values. All mutating operates are
 * written to a write-ahead log and flushed before being applied in-memory.
 * Flushing is controlled by a commit interval configuration setting that allows
 * operations to be batched, amatorizing the cost of flushing to disk.
 *
 * Operation
 * =========
 *
 * The key-value store manages asynchronous mutation operations:
 *
 *   auto f = kvstore.operation(...);
 *
 * Operations are staged in an ordered in-memory container. After a commit
 * interval has elapsed the operations are serialized into a single blob and
 * flushed to disk. Once the flush is complete the operations are applied to the
 * in-memory cache, and the associated promise is resolved.
 *
 * Concurrency
 * ===========
 *
 * The caller must manage its own concurrency on a per-key basis. What this
 * means is that the key-value store provides no consistency guarantees for
 * concurrent reads and writes to the same key.
 *
 * This is sufficient for the initial use cases of storing voted_for metadata
 * in which access to the underlying file storing the metadata was already
 * controlled.
 *
 * Limitations
 * ===========
 *
 * The entire database is cached in memory, so users should not allow the set of
 * uniuqe keys to grow unbounded.  No backpressure is applied, so use
 * responsibly until this utility becomes more sophisticated. The initial set of
 * use cases--tracking raft voted-for and log's base offset--do not pose an
 * issue for either of these limitations since they exhibit a natural bound on
 * the set of unique keys and queue depth of at most a few bytes *
 * O(#-partitions-per-core).
 */
static constexpr const model::record_batch_type kvstore_batch_type(4);

struct kvstore_config {
    size_t max_segment_size;
    std::chrono::milliseconds commit_interval;
};

class kvstore {
public:
    enum class key_space : int8_t {
        testing = 0,
        consensus = 1,
        /* your sub-system here */
    };

    kvstore(kvstore_config kv_conf, storage::log_config log_conf);

    ss::future<> start();
    ss::future<> stop();

    std::optional<iobuf> get(key_space ks, bytes_view key);
    ss::future<> put(key_space ks, bytes key, iobuf value);
    ss::future<> remove(key_space ks, bytes key);

    bool empty() const {
        vassert(_started, "kvstore has not been started");
        return _db.empty();
    }

private:
    kvstore_config _conf;
    log_manager _mgr;
    ntp_config _ntpc;
    ss::gate _gate;
    snapshot_manager _snap;
    bool _started{false};

    /**
     * Database operation. A std::nullopt value is a deletion.
     */
    struct op {
        bytes key;
        std::optional<iobuf> value;
        ss::promise<> done;

        op(bytes&& key, std::optional<iobuf>&& value)
          : key(std::move(key))
          , value(std::move(value)) {}
    };

    /*
     * database operations are cached in `ops` and periodically flushed to the
     * current `segment` at position `next_offset` and then applied to `db`.
     * when the segment reaches a threshold size a snapshot is saved a new
     * segment is created.
     */
    std::vector<op> _ops;
    ss::timer<> _timer;
    ss::semaphore _sem{0};
    ss::lw_shared_ptr<segment> _segment;
    model::offset _next_offset;
    absl::flat_hash_map<bytes, iobuf, bytes_type_hash, bytes_type_eq> _db;

    ss::future<> put(key_space ks, bytes key, std::optional<iobuf> value);
    void apply_op(bytes key, std::optional<iobuf> value);
    ss::future<> flush_and_apply_ops();
    ss::future<> roll();
    ss::future<> save_snapshot();

    /*
     * Recovery
     *
     * 1. load snapshot if found
     * 2. then recover from segments
     */
    ss::future<> recover();
    void load_snapshot_in_thread();
    void replay_segments_in_thread(segment_set);

    /**
     * Replay batches against the key-value store.
     *
     * Used in recovery:
     *    segment -> parser -> replay_consumer -> db
     */
    class replay_consumer final : public batch_consumer {
    public:
        explicit replay_consumer(kvstore* store)
          : _store(store) {}

        consume_result consume_batch_start(
          model::record_batch_header header, size_t, size_t) override;
        consume_result consume_record(model::record r) override;
        void consume_compressed_records(iobuf&&) override;
        stop_parser consume_batch_end() override;

    private:
        kvstore* _store;
        model::offset _last_offset;
    };

    friend replay_consumer;
};

} // namespace storage
