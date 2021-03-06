/**
 * Copyright (c) 2013 Eugene Lazin <4lazin@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <memory>
#include <iostream>

#include <apr_dbd.h>

#include "akumuli.h"
#include "storage.h"

using namespace Akumuli;

//! Pool for `apr_dbd_init`
static apr_pool_t* g_dbd_pool = nullptr;

void aku_initialize(aku_panic_handler_t optional_panic_handler) {
    apr_initialize();
    if (optional_panic_handler != nullptr) {
        set_panic_handler(optional_panic_handler);
    }
    auto status = apr_pool_create(&g_dbd_pool, nullptr);
    if (status != APR_SUCCESS) {
        AKU_PANIC("Initialization error");
    }
    status = apr_dbd_init(g_dbd_pool);
    if (status != APR_SUCCESS) {
        AKU_PANIC("DBD initialization error");
    }
}

static const char* g_error_messages[] = {
    "OK",
    "No data",
    "Not enough memory",
    "Device is busy",
    "Can't find result",
    "Bad argument",
    "Overflow",
    "Invalid data",
    "Error, no details available",
    "Late write",
    "Unknown error code"
};

const char* aku_error_message(int error_code) {
    if (error_code >= 0 && error_code < 10) {
        return g_error_messages[error_code];
    }
    return g_error_messages[10];
}

void aku_console_logger(int tag, const char* msg) {
    apr_time_t now = apr_time_now();
    char ts[APR_RFC822_DATE_LEN];
    if (apr_rfc822_date(ts, now) != APR_SUCCESS) {
        memset(ts, ' ', APR_RFC822_DATE_LEN);
        ts[sizeof(ts) - 1] = 0;
    }
    char tagstr[9];                    // I don't want to use manipulators on cerr here
    snprintf(tagstr, 9, "%08X", tag);  // because this can break formatting in host application.
    std::cerr << ts << " | " << tagstr << " | " << msg << std::endl;
}


struct MatchPred {
    std::vector<aku_ParamId> params_;
    MatchPred(aku_ParamId* begin, uint32_t n)
        : params_(begin, begin + n)
    {
    }

    SearchQuery::ParamMatch operator () (aku_ParamId id) const {
        return std::binary_search(params_.begin(), params_.end(), id) ? SearchQuery::MATCH : SearchQuery::NO_MATCH;
    }
};


struct CursorImpl : aku_Cursor {
    std::unique_ptr<ExternalCursor> cursor_;
    int status_;
    std::unique_ptr<SearchQuery> query_;

    CursorImpl(Storage& storage, std::unique_ptr<SearchQuery> query)
        : query_(std::move(query))
    {
        status_ = AKU_SUCCESS;
        cursor_ = CoroCursor::make(&Storage::search, &storage, *query_);
    }

    ~CursorImpl() {
        cursor_->close();
    }

    bool is_done() const {
        return cursor_->is_done();
    }

    bool is_error(int* out_error_code_or_null) const {
        if (status_ != AKU_SUCCESS) {
            *out_error_code_or_null = status_;
            return false;
        }
        return cursor_->is_error(out_error_code_or_null);
    }

    int read_columns( aku_Timestamp   *timestamps
                    , aku_ParamId     *params
                    , aku_PData       *pointers
                    , uint32_t        *lengths
                    , size_t           arrays_size )
    {
        // TODO: track PageHeader::open_count here
        std::vector<CursorResult> results;
        results.resize(arrays_size);  // TODO: pass all pointers to storage directly
        int n_results = cursor_->read(results.data(), results.size());
        for (int i = 0; i < n_results; i++) {
            const CursorResult& result = results[i];
            if (timestamps) {
                timestamps[i] = result.timestamp;
            }
            if (params) {
                params[i] = result.param_id;
            }
            if (pointers) {
                pointers[i] = result.data;
            }
            if (lengths) {
                lengths[i] = result.length;
            }
        }
        return n_results;
    }
};


/** 
 * Object that extends a Database struct.
 * Can be used from "C" code.
 */
struct DatabaseImpl : public aku_Database
{
    Storage storage_;

    // private fields
    DatabaseImpl(const char* path, const aku_FineTuneParams& config)
        : storage_(path, config)
    {
    }

    aku_Status get_open_error() const {
        return storage_.get_open_error();
    }

    void close() {
        storage_.close();
    }

    CursorImpl* select(aku_SelectQuery* query) {
        uint32_t scan_dir;
        aku_Timestamp begin, end;
        if (query->begin < query->end) {
            begin = query->begin;
            end = query->end;
            scan_dir = AKU_CURSOR_DIR_FORWARD;
        } else {
            end = query->begin;
            begin = query->end;
            scan_dir = AKU_CURSOR_DIR_BACKWARD;
        }
        MatchPred pred(query->params, query->n_params);
        std::unique_ptr<SearchQuery> search_query;
        search_query.reset(new SearchQuery(pred, {begin}, {end}, scan_dir));
        auto pcur = new CursorImpl(storage_, std::move(search_query));
        return pcur;
    }

    aku_Status add_blob(aku_ParamId param_id, aku_Timestamp ts, aku_MemRange value) {
        return storage_.write_blob(param_id, ts, value);
    }

    aku_Status add_double(aku_ParamId param_id, aku_Timestamp ts, double value) {
        return storage_.write_double(param_id, ts, value);
    }

    aku_Status add_double(const char* begin, const char* end, aku_Timestamp ts, double value) {
        return storage_.write_double(begin, end, ts, value);
    }

    // Stats
    void get_storage_stats(aku_StorageStats* recv_stats) {
        storage_.get_stats(recv_stats);
    }
};

apr_status_t aku_create_database( const char     *file_name
                                , const char     *metadata_path
                                , const char     *volumes_path
                                , int32_t         num_volumes
                                // optional args
                                , uint32_t  compression_threshold
                                , uint64_t  window_size
                                , uint32_t  max_cache_size
                                , aku_logger_cb_t logger)
{
    if (logger == nullptr) {
        logger = &aku_console_logger;
    }
    if (compression_threshold == 0) {
        compression_threshold = AKU_DEFAULT_COMPRESSION_THRESHOLD;
    }
    if (window_size == 0) {
        window_size = AKU_DEFAULT_WINDOW_SIZE;
    }
    if (max_cache_size == 0) {
        max_cache_size = AKU_DEFAULT_MAX_CACHE_SIZE;
    }
    return Storage::new_storage(file_name, metadata_path, volumes_path, num_volumes,
                                compression_threshold, window_size, max_cache_size,
                                logger);
}

apr_status_t aku_remove_database(const char* file_name, aku_logger_cb_t logger) {

    return Storage::remove_storage(file_name, logger);
}

aku_Status aku_write_blob(aku_Database* db, aku_ParamId param_id, aku_Timestamp ts, aku_MemRange value) {
    auto dbi = reinterpret_cast<DatabaseImpl*>(db);
    return dbi->add_blob(param_id, ts, value);
}

aku_Status aku_write_double_raw(aku_Database* db, aku_ParamId param_id, aku_Timestamp timestamp, double value) {
    auto dbi = reinterpret_cast<DatabaseImpl*>(db);
    return dbi->add_double(param_id, timestamp, value);
}

aku_Status aku_write_double(aku_Database* db,
                            const char* series_key_begin,
                            const char* series_key_end,
                            aku_Timestamp timestamp,
                            double value)
{
    auto dbi = reinterpret_cast<DatabaseImpl*>(db);
    return dbi->add_double(series_key_begin, series_key_end, timestamp, value);
}

aku_Database* aku_open_database(const char* path, aku_FineTuneParams config)
{
    if (config.logger == nullptr) {
        // Use default console logger if user doesn't set it
        config.logger = &aku_console_logger;
    }
    if (config.durability != AKU_MAX_DURABILITY &&
        config.durability != AKU_DURABILITY_SPEED_TRADEOFF &&
        config.durability != AKU_MAX_WRITE_SPEED)
    {
        // Set defaut
        config.durability = AKU_MAX_DURABILITY;
        (*config.logger)(-1, "config.durability = default(AKU_MAX_DURABILITY)");
    }
    auto ptr = new DatabaseImpl(path, config);
    return static_cast<aku_Database*>(ptr);
}

aku_Status aku_open_status(aku_Database* db) {
    auto dbi = reinterpret_cast<DatabaseImpl*>(db);
    return dbi->get_open_error();
}

void aku_close_database(aku_Database* db)
{
    auto dbi = reinterpret_cast<DatabaseImpl*>(db);
    dbi->close();
    delete dbi;
}

aku_SelectQuery* aku_make_select_query(aku_Timestamp begin, aku_Timestamp end, uint32_t n_params, aku_ParamId *params) {
    size_t s = sizeof(aku_SelectQuery) + n_params*sizeof(aku_ParamId);
    auto p = malloc(s);
    auto res = reinterpret_cast<aku_SelectQuery*>(p);
    res->begin = begin;
    res->end = end;
    res->n_params = n_params;
    memcpy(&res->params, params, n_params*sizeof(aku_ParamId));
    std::sort(res->params, res->params + n_params);
    return res;
}

void aku_destroy(void* any) {
    free(any);
}

aku_Cursor* aku_select(aku_Database *db, aku_SelectQuery* query) {
    auto dbi = reinterpret_cast<DatabaseImpl*>(db);
    return dbi->select(query);
}

void aku_close_cursor(aku_Cursor* pcursor) {
    CursorImpl* pimpl = reinterpret_cast<CursorImpl*>(pcursor);
    delete pimpl;
}

int aku_cursor_read_columns( aku_Cursor      *pcursor
                           , aku_Timestamp   *timestamps
                           , aku_ParamId     *params
                           , aku_PData       *pointers
                           , uint32_t        *lengths
                           , size_t           arrays_size )
{
    // read columns from data store
    CursorImpl* pimpl = reinterpret_cast<CursorImpl*>(pcursor);
    return pimpl->read_columns(timestamps, params, pointers, lengths, arrays_size);
}

int aku_cursor_is_done(aku_Cursor* pcursor) {
    CursorImpl* pimpl = reinterpret_cast<CursorImpl*>(pcursor);
    return static_cast<int>(pimpl->is_done());
}

int aku_cursor_is_error(aku_Cursor* pcursor, int* out_error_code_or_null) {
    CursorImpl* pimpl = reinterpret_cast<CursorImpl*>(pcursor);
    return static_cast<int>(pimpl->is_error(out_error_code_or_null));
}

//--------------------------------
//         Statistics
//--------------------------------

void aku_global_search_stats(aku_SearchStats* rcv_stats, int reset) {
    PageHeader::get_search_stats(rcv_stats, reset);
}

void aku_global_storage_stats(aku_Database *db, aku_StorageStats* rcv_stats) {
    auto dbi = reinterpret_cast<DatabaseImpl*>(db);
    dbi->get_storage_stats(rcv_stats);
}
