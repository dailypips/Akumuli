/**
 * PRIVATE HEADER
 *
 * Compression algorithms
 *
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


#pragma once

#include <cassert>
#include <cstdint>
#include <cstddef>
#include <iterator>
#include <vector>

#include "akumuli.h"

namespace Akumuli {

typedef std::vector<unsigned char> ByteVector;

struct ChunkHeader {
    std::vector<aku_Timestamp>  timestamps;
    std::vector<aku_ParamId>    paramids;
    std::vector<uint32_t>       offsets;
    std::vector<uint32_t>       lengths;
    std::vector<double>         values;
};

struct ChunkWriter {
    virtual ~ChunkWriter() {}
    virtual aku_Status add_chunk(aku_MemRange range, size_t size_estimate) = 0;
};

struct CompressionUtil {

    /** Compress and write ChunkHeader to memory stream.
      * @param n_elements out parameter - number of written elements
      * @param ts_begin out parameter - first timestamp
      * @param ts_end out parameter - last timestamp
      * @param data ChunkHeader to compress
      */
    static
    aku_Status encode_chunk(uint32_t           *n_elements
                           , aku_Timestamp      *ts_begin
                           , aku_Timestamp      *ts_end
                           , ChunkWriter        *writer
                           , const ChunkHeader &data
                           );

    /** Decompress ChunkHeader.
      * @brief Decode part of the ChunkHeader structure depending on stage and steps values.
      * First goes list of timestamps, then all other values.
      * @param header out header
      * @param pbegin in - begining of the data, out - new begining of the data
      * @param end end of the data
      * @param stage current stage
      * @param steps number of stages to do
      * @param probe_length number of elements in header
      * @return current stage number
      */
    static
    int decode_chunk( ChunkHeader          *header
                    , const unsigned char **pbegin
                    , const unsigned char  *pend
                    , int                   stage
                    , int                   steps
                    , uint32_t              probe_length);

    /** Compress list of doubles.
      * @param input array of doubles
      * @param params array of parameter ids
      * @param buffer resulting byte array
      */
    static
    size_t compress_doubles(std::vector<double> const& input,
                            std::vector<aku_ParamId> const& params,
                            ByteVector *buffer);  // TODO: maybe I should use plain old buffer here

    /** Decompress list of doubles.
      * @param buffer input data
      * @param numbloks number of 4bit blocs inside buffer
      * @param params list of parameter ids
      * @param output resulting array
      */
    static
    void decompress_doubles(ByteVector& buffer,
                            size_t numblocks,
                            std::vector<aku_ParamId> const& params,
                            std::vector<double> *output);

    /** Convert from chunk order to time order.
      * @note in chunk order all data elements ordered by series id first and then by timestamp,
      * in time order everythin ordered by time first and by id second.
      */
    static bool convert_from_chunk_order(const ChunkHeader &header, ChunkHeader* out);

    /** Convert from time order to chunk order.
      * @note in chunk order all data elements ordered by series id first and then by timestamp,
      * in time order everythin ordered by time first and by id second.
      */
    static bool convert_from_time_order(const ChunkHeader &header, ChunkHeader* out);
};


//! Base 128 encoded integer
template<class TVal>
class Base128Int {
    TVal value_;
    typedef unsigned char byte_t;
    typedef byte_t* byte_ptr;
public:

    Base128Int(TVal val) : value_(val) {
    }

    Base128Int() : value_() {
    }

    /** Read base 128 encoded integer from the binary stream
    *  FwdIter - forward iterator
    */
    template<class FwdIter>
    FwdIter get(FwdIter begin, FwdIter end) {
        assert(begin < end);

        auto acc = TVal();
        auto cnt = TVal();
        FwdIter p = begin;

        while (true) {
            auto i = static_cast<byte_t>(*p & 0x7F);
            acc |= TVal(i) << cnt;
            if ((*p++ & 0x80) == 0) {
                break;
            }
            cnt += 7;
        }
        value_ = acc;
        return p;
    }

    /** Write base 128 encoded integer to the binary stream.
    * @returns 'begin' on error, iterator to next free region otherwise
    */
    template<class FwdIter>
    FwdIter put(FwdIter begin, FwdIter end) const {
        if (begin >= end) {
            return begin;
        }

        TVal value = value_;
        FwdIter p = begin;

        while (true) {
            *p = value & 0x7F;
            value >>= 7;
            if (value != 0) {
                *p++ |= 0x80;
                if (p == end) {
                    return begin;
                }
            } else {
                p++;
                break;
            }
        }
        return p;
    }

    //! Write base 128 encoded integer to the binary stream (unchecked)
    template<class Inserter>
    void put(Inserter& p) const {
        TVal value = value_;
        byte_t result = 0;

        while(true) {
            result = value & 0x7F;
            value >>= 7;
            if (value != 0) {
                result |= 0x80;
                *p++ = result;
            } else {
                *p++ = result;
                break;
            }
        }
    }

    //! turn into integer
    operator TVal() const {
        return value_;
    }
};

//! Base128 encoder
template<class TVal>
struct Base128StreamWriter {
    // underlying memory region
    ByteVector& data_;

    Base128StreamWriter(ByteVector& data) : data_(data) {}

    /** Put value into stream.
     */
    void put(TVal value) {
        Base128Int<TVal> val(value);
        auto it = std::back_inserter(data_);
        val.put(it);
    }

    //! Close stream
    void close() {}

    size_t size() const {
        return data_.size();
    }

    aku_MemRange get_memrange() const {
        // FIXME: check for overflow
        return { data_.data(), static_cast<uint32_t>(data_.size()) };
    }
};

//! Base128 decoder
template<class TVal, class FwdIt>
struct Base128StreamReader {
    FwdIt pos_;
    FwdIt end_;

    Base128StreamReader(FwdIt begin, FwdIt end)
        : pos_(begin)
        , end_(end)
    {
    }

    TVal next() {
        Base128Int<TVal> value;
        pos_ = value.get(pos_, end_);
        return static_cast<TVal>(value);
    }

    typedef FwdIt Iterator;

    Iterator pos() const {
        return pos_;
    }
};

template<class Stream, class TVal>
struct ZigZagStreamWriter {
    Stream stream_;

    ZigZagStreamWriter(ByteVector& container)
            : stream_(container) {
    }
    void put(TVal value) {
        // TVal must be signed
        const int shift_width = sizeof(TVal)*8 - 1;
        auto res = (value << 1) ^ (value >> shift_width);
        stream_.put(res);  // implicit type cast
    }
    size_t size() const {
        return stream_.size();
    }
    void close() {
        stream_.close();
    }
    aku_MemRange get_memrange() const {
        return stream_.get_memrange();
    }
};

template<class Stream, class TVal>
struct ZigZagStreamReader {
    Stream stream_;

    template<class FwdIt>
    ZigZagStreamReader(FwdIt begin, FwdIt end)
            : stream_(begin, end) {
    }

    TVal next() {
        auto n = stream_.next();
        return (n >> 1) ^ (-(n & 1));
    }
    typedef typename Stream::Iterator Iterator;
    Iterator pos() const {
        return stream_.pos();
    }
};

template<class Stream, typename TVal>
struct DeltaStreamWriter {
    Stream stream_;
    TVal prev_;

    DeltaStreamWriter(ByteVector& container)
        : stream_(container)
        , prev_()
    {
    }

    template<class InVal>
    void put(InVal value) {
        stream_.put(static_cast<TVal>(value) - prev_);
        prev_ = value;
    }

    size_t size() const {
        return stream_.size();
    }

    void close() {
        stream_.close();
    }

    aku_MemRange get_memrange() const {
        return stream_.get_memrange();
    }
};


template<class Stream, typename TVal>
struct DeltaStreamReader {
    Stream stream_;
    TVal prev_;

    template<class FwdIt>
    DeltaStreamReader(FwdIt begin, FwdIt end)
        : stream_(begin, end)
        , prev_()
    {
    }

    TVal next() {
        TVal delta = stream_.next();
        TVal value = prev_ + delta;
        prev_ = value;
        return value;
    }

    typedef typename Stream::Iterator Iterator;

    Iterator pos() const {
        return stream_.pos();
    }
};


template<class Stream, typename TVal>
struct RLEStreamWriter {
    Stream stream_;
    TVal prev_;
    TVal reps_;

    RLEStreamWriter(ByteVector& container)
        : stream_(container)
        , prev_()
        , reps_()
    {}

    void put(TVal value) {
        if (value != prev_) {
            if (reps_) {
                // commit changes
                stream_.put(reps_);
                stream_.put(prev_);
            }
            prev_ = value;
            reps_ = TVal();
        }
        reps_++;
    }

    size_t size() const {
        return stream_.size();
    }

    void close() {
        stream_.put(reps_);
        stream_.put(prev_);
    }

    aku_MemRange get_memrange() const {
        return stream_.get_memrange();
    }
};

template<class Stream, typename TVal>
struct RLEStreamReader {
    Stream stream_;
    TVal prev_;
    TVal reps_;

    template<class FwdIt>
    RLEStreamReader(FwdIt begin, FwdIt end)
        : stream_(begin, end)
        , prev_()
        , reps_()
    {}

    TVal next() {
        if (reps_ == 0) {
            reps_ = stream_.next();
            prev_ = stream_.next();
        }
        reps_--;
        return prev_;
    }

    typedef typename Stream::Iterator Iterator;

    Iterator pos() const {
        return stream_.pos();
    }
};

// Length -> RLE -> Base128
typedef Base128StreamWriter<uint32_t> __Base128LenWriter;
typedef RLEStreamWriter<__Base128LenWriter, uint32_t> RLELenWriter;

// Base128 -> RLE -> Length
typedef Base128StreamReader<uint32_t, const unsigned char*> __Base128LenReader;
typedef RLEStreamReader<__Base128LenReader, uint32_t> RLELenReader;

// int64_t -> Delta -> ZigZag -> RLE -> Base128
typedef Base128StreamWriter<int64_t> __Base128Writer;
typedef RLEStreamWriter<__Base128Writer, int64_t> __RLEWriter;
typedef ZigZagStreamWriter<__RLEWriter, int64_t> __ZigZagWriter;
typedef DeltaStreamWriter<__ZigZagWriter, int64_t> DeltaRLEWriter;

// Base128 -> RLE -> ZigZag -> Delta -> int64_t
typedef Base128StreamReader<uint64_t, const unsigned char*> __Base128Reader;
typedef RLEStreamReader<__Base128Reader, int64_t> __RLEReader;
typedef ZigZagStreamReader<__RLEReader, int64_t> __ZigZagReader;
typedef DeltaStreamReader<__ZigZagReader, int64_t> DeltaRLEReader;

}
