/*
 * Copyright (c) 2012, 2014, 2018 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/physical.hh"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <iostream>
#include <string>

#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/AddrRanges.hh"
#include "debug/Checkpoint.hh"
#include "mem/abstract_mem.hh"
#include "sim/serialize.hh"
#include "sim/sim_exit.hh"

/**
 * On Linux, MAP_NORESERVE allow us to simulate a very large memory
 * without committing to actually providing the swap space on the
 * host. On FreeBSD or OSX the MAP_NORESERVE flag does not exist,
 * so simply make it 0.
 */
#if defined(__APPLE__) || defined(__FreeBSD__)
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#endif

namespace gem5
{

namespace memory
{

PhysicalMemory::PhysicalMemory(const std::string &_name,
                               const std::vector<AbstractMemory *> &_memories,
                               bool mmap_using_noreserve,
                               const std::string &shared_backstore,
                               bool restore_from_gcpt,
                               const std::string &gcpt_path,
                               bool auto_unlink_shared_backstore,
                               bool is_sparse_restore)
    : _name(_name),
      size(0),
      mmapUsingNoReserve(mmap_using_noreserve),
      sharedBackstore(shared_backstore),
      sharedBackstoreSize(0),
      pageSize(sysconf(_SC_PAGE_SIZE)),
      isSparseRestore(is_sparse_restore),
      restoreFromXiangshanCpt(restore_from_gcpt),
      xsCptPath(gcpt_path)
{
    // Register cleanup callback if requested.
    if (auto_unlink_shared_backstore && !sharedBackstore.empty()) {
        registerExitCallback([=]() { shm_unlink(shared_backstore.c_str()); });
    }

    if (mmap_using_noreserve)
        warn("Not reserving swap space. May cause SIGSEGV on actual usage\n");

    // add the memories from the system to the address map as
    // appropriate
    for (const auto& m : _memories) {
        // only add the memory if it is part of the global address map
        if (m->isInAddrMap()) {
            memories.push_back(m);

            // calculate the total size once and for all
            size += m->size();

            // add the range to our interval tree and make sure it does not
            // intersect an existing range
            fatal_if(addrMap.insert(m->getAddrRange(), m) == addrMap.end(),
                     "Memory address range for %s is overlapping\n",
                     m->name());

            const auto &sub_ranges = m->getAddrRange().subRanges();
            panic_if(sub_ranges.empty(), "Memory range has no subranges\n");
            validAddrMap.insert(validAddrMap.end(), sub_ranges.begin(),
                                sub_ranges.end());
        } else {
            // this type of memory is used e.g. as reference memory by
            // Ruby, and they also needs a backing store, but should
            // not be part of the global address map
            DPRINTF(AddrRanges,
                    "Skipping memory %s that is not in global address map\n",
                    m->name());

            // sanity check
            fatal_if(m->getAddrRange().interleaved(),
                     "Memory %s that is not in the global address map cannot "
                     "be interleaved\n", m->name());

            // simply do it independently, also note that this kind of
            // memories are allowed to overlap in the logic address
            // map
            std::vector<AbstractMemory*> unmapped_mems{m};
            createBackingStore(m->getAddrRange(), unmapped_mems,
                               m->isConfReported(), m->isInAddrMap(),
                               m->isKvmMap());
        }
    }

    // iterate over the increasing addresses and chunks of contiguous
    // space to be mapped to backing store, create it and inform the
    // memories
    std::vector<AddrRange> intlv_ranges;
    std::vector<AbstractMemory*> curr_memories;
    for (const auto& r : addrMap) {
        // simply skip past all memories that are null and hence do
        // not need any backing store
        if (r.second->isNull()) {
            continue;
        }

        // if the range is interleaved then save it for now
        if (r.first.interleaved()) {
            // if we already got interleaved ranges that are not
            // part of the same range, then first do a merge
            // before we add the new one
            if (!intlv_ranges.empty() &&
                !intlv_ranges.back().mergesWith(r.first)) {
                AddrRange merged_range(intlv_ranges);

                AbstractMemory *f = curr_memories.front();
                for (const auto &c : curr_memories) {
                    if (f->isConfReported() != c->isConfReported() ||
                        f->isInAddrMap() != c->isInAddrMap() ||
                        f->isKvmMap() != c->isKvmMap()) {
                        fatal("Inconsistent flags in an interleaved "
                              "range\n");
                    }
                }

                createBackingStore(merged_range, curr_memories,
                                   f->isConfReported(), f->isInAddrMap(),
                                   f->isKvmMap());

                intlv_ranges.clear();
                curr_memories.clear();
            }
            intlv_ranges.push_back(r.first);
            curr_memories.push_back(r.second);
        } else {
            std::vector<AbstractMemory *> single_memory{r.second};
            createBackingStore(r.first, single_memory,
                               r.second->isConfReported(),
                               r.second->isInAddrMap(), r.second->isKvmMap());
        }
    }

    // if there is still interleaved ranges waiting to be merged, go
    // ahead and do it
    if (!intlv_ranges.empty()) {
        AddrRange merged_range(intlv_ranges);

        AbstractMemory *f = curr_memories.front();
        for (const auto& c : curr_memories)
            if (f->isConfReported() != c->isConfReported() ||
                f->isInAddrMap() != c->isInAddrMap() ||
                f->isKvmMap() != c->isKvmMap())
                fatal("Inconsistent flags in an interleaved "
                      "range\n");

        createBackingStore(merged_range, curr_memories,
                           f->isConfReported(), f->isInAddrMap(),
                           f->isKvmMap());
    }

    panic_if(validAddrMap.empty(), "No valid address ranges found\n");
    // Clean up the valid address map
    // 1. Sort: Pairs are compared by 'first', then 'second'
    std::sort(validAddrMap.begin(), validAddrMap.end());
    // 2. Unique: Move consecutive identical duplicates to the end
    auto last = std::unique(validAddrMap.begin(), validAddrMap.end());
    // 3. Erase: Shrink the vector to remove the "garbage" at the end
    validAddrMap.erase(last, validAddrMap.end());

    if (debug::AddrRanges) {
        for (const auto &r : validAddrMap) {
            DPRINTF(AddrRanges, "Valid address range: %#x - %#x\n", r.first,
                    r.second);
        }
    }
}

void
PhysicalMemory::createBackingStore(
    AddrRange range, const std::vector<AbstractMemory *> &_memories,
    bool conf_table_reported, bool in_addr_map, bool kvm_map)
{
    if (range.isSparse()) {
        // If it's sparse, then create a separate backing store for each
        // subrange. We assume that by this point the subranges are not
        // interleaved.
        for (auto const &r : range.subRanges()) {
            createBackingStore(AddrRange(r.first, r.second), _memories,
                               conf_table_reported, in_addr_map, kvm_map);
        }
        return;
    }

    panic_if(range.interleaved(),
             "Cannot create backing store for interleaved range %s\n",
             range.to_string());

    // perform the actual mmap
    DPRINTF(AddrRanges, "Creating backing store for range %s with size %d\n",
            range.to_string(), range.size());

    int shm_fd;
    int map_flags;
    off_t map_offset;

    if (sharedBackstore.empty()) {
        shm_fd = -1;
        map_flags =  MAP_ANON | MAP_PRIVATE;
        map_offset = 0;
    } else {
        // Newly create backstore will be located after previous one.
        map_offset = sharedBackstoreSize;
        // mmap requires the offset to be multiple of page, so we need to
        // upscale the range size.
        sharedBackstoreSize += roundUp(range.size(), pageSize);
        DPRINTF(AddrRanges, "Sharing backing store as %s at offset %llu\n",
                sharedBackstore.c_str(), (uint64_t)map_offset);
        shm_fd = shm_open(sharedBackstore.c_str(), O_CREAT | O_RDWR, 0666);
        if (shm_fd == -1)
               panic("Shared memory failed");
        if (ftruncate(shm_fd, sharedBackstoreSize))
               panic("Setting size of shared memory failed");
        map_flags = MAP_SHARED;
    }

    // to be able to simulate very large memories, the user can opt to
    // pass noreserve to mmap
    if (mmapUsingNoReserve) {
        map_flags |= MAP_NORESERVE;
    }

    uint8_t* pmem = (uint8_t*) mmap(NULL, range.size(),
                                    PROT_READ | PROT_WRITE,
                                    map_flags, shm_fd, map_offset);

    if (pmem == (uint8_t*) MAP_FAILED) {
        perror("mmap");
        fatal("Could not mmap %d bytes for range %s!\n", range.size(),
              range.to_string());
    }

    // remember this backing store so we can checkpoint it and unmap
    // it appropriately
    backingStore.emplace_back(range, pmem,
                              conf_table_reported, in_addr_map, kvm_map,
                              shm_fd, map_offset);

    // point the memories to their backing store
    for (const auto& m : _memories) {
        DPRINTF(AddrRanges, "Mapping memory %s to backing store\n", m->name());
        m->setBackingStore(pmem, range);
    }
}

PhysicalMemory::~PhysicalMemory()
{
    // unmap the backing store
    for (auto& s : backingStore)
        munmap((char*)s.pmem, s.range.size());
}

bool
PhysicalMemory::isMemAddr(Addr addr) const
{
    // This is a hot function. Instead of doing anything fancy, since we always
    // have few ranges, we have simple linear scan of the non-interleaved
    // address ranges.
    for (const auto &range : validAddrMap) {
        if (addr >= range.first && addr < range.second) {
            return true;
        }
        if (addr < range.first) {
            return false;
        }
    }
    return false;
}

AddrRangeList
PhysicalMemory::getConfAddrRanges() const
{
    // this could be done once in the constructor, but since it is unlikely to
    // be called more than once the iteration should not be a problem
    AddrRangeList ranges;
    std::vector<AddrRange> intlv_ranges;
    for (const auto& r : addrMap) {
        if (r.second->isConfReported()) {
            // if the range is interleaved then save it for now
            if (r.first.interleaved()) {
                // if we already got interleaved ranges that are not
                // part of the same range, then first do a merge
                // before we add the new one
                if (!intlv_ranges.empty() &&
                    !intlv_ranges.back().mergesWith(r.first)) {
                    ranges.push_back(AddrRange(intlv_ranges));
                    intlv_ranges.clear();
                }
                intlv_ranges.push_back(r.first);
            } else {
                // keep the current range
                ranges.push_back(r.first);
            }
        }
    }

    // if there is still interleaved ranges waiting to be merged,
    // go ahead and do it
    if (!intlv_ranges.empty()) {
        ranges.push_back(AddrRange(intlv_ranges));
    }

    return ranges;
}

void
PhysicalMemory::access(PacketPtr pkt)
{
    assert(pkt->isRequest());
    const auto& m = addrMap.contains(pkt->getAddrRange());
    assert(m != addrMap.end());
    m->second->access(pkt);
}

void
PhysicalMemory::functionalAccess(PacketPtr pkt)
{
    assert(pkt->isRequest());
    const auto& m = addrMap.contains(pkt->getAddrRange());
    assert(m != addrMap.end());
    m->second->functionalAccess(pkt);
}

void
PhysicalMemory::serialize(CheckpointOut &cp) const
{
    // serialize all the locked addresses and their context ids
    std::vector<Addr> lal_addr;
    std::vector<ContextID> lal_cid;

    for (auto& m : memories) {
        const std::list<LockedAddr>& locked_addrs = m->getLockedAddrList();
        for (const auto& l : locked_addrs) {
            lal_addr.push_back(l.addr);
            lal_cid.push_back(l.contextId);
        }
    }

    SERIALIZE_CONTAINER(lal_addr);
    SERIALIZE_CONTAINER(lal_cid);

    // serialize the backing stores
    unsigned int nbr_of_stores = backingStore.size();
    SERIALIZE_SCALAR(nbr_of_stores);

    unsigned int store_id = 0;
    // store each backing store memory segment in a file
    for (auto& s : backingStore) {
        ScopedCheckpointSection sec(cp, csprintf("store%d", store_id));
        serializeStore(cp, store_id++, s.range, s.pmem);
    }
}

void
PhysicalMemory::serializeStore(CheckpointOut &cp, unsigned int store_id,
                               AddrRange range, uint8_t* pmem) const
{
    // we cannot use the address range for the name as the
    // memories that are not part of the address map can overlap
    std::string filename =
        name() + ".store" + std::to_string(store_id) + ".pmem";
    Addr range_size = range.size();

    DPRINTF(Checkpoint, "Serializing physical memory %s with size %d\n",
            filename, range_size);

    SERIALIZE_SCALAR(store_id);
    SERIALIZE_SCALAR(filename);
    SERIALIZE_SCALAR(range_size);

    // write memory file
    std::string filepath = CheckpointIn::dir() + "/" + filename.c_str();
    gzFile compressed_mem = gzopen(filepath.c_str(), "wb");
    if (compressed_mem == NULL)
        fatal("Can't open physical memory checkpoint file '%s'\n",
              filename);

    uint64_t pass_size = 0;

    // gzwrite fails if (int)len < 0 (gzwrite returns int)
    for (uint64_t written = 0; written < range.size();
         written += pass_size) {
        pass_size = (uint64_t)INT_MAX < (range.size() - written) ?
            (uint64_t)INT_MAX : (range.size() - written);

        if (gzwrite(compressed_mem, pmem + written,
                    (unsigned int) pass_size) != (int) pass_size) {
            fatal("Write failed on physical memory checkpoint file '%s'\n",
                  filename);
        }
    }

    // close the compressed stream and check that the exit status
    // is zero
    if (gzclose(compressed_mem))
        fatal("Close failed on physical memory checkpoint file '%s'\n",
              filename);

}

void
PhysicalMemory::unserialize(CheckpointIn &cp)
{
    // unserialize the locked addresses and map them to the
    // appropriate memory controller
    std::vector<Addr> lal_addr;
    std::vector<ContextID> lal_cid;
    UNSERIALIZE_CONTAINER(lal_addr);
    UNSERIALIZE_CONTAINER(lal_cid);
    for (size_t i = 0; i < lal_addr.size(); ++i) {
        const auto& m = addrMap.contains(lal_addr[i]);
        m->second->addLockedAddr(LockedAddr(lal_addr[i], lal_cid[i]));
    }

    // unserialize the backing stores
    unsigned int nbr_of_stores;
    UNSERIALIZE_SCALAR(nbr_of_stores);

    for (unsigned int i = 0; i < nbr_of_stores; ++i) {
        ScopedCheckpointSection sec(cp, csprintf("store%d", i));
        unserializeStore(cp);
    }

}

void
PhysicalMemory::unserializeStore(CheckpointIn &cp)
{
    unsigned int store_id;
    UNSERIALIZE_SCALAR(store_id);

    std::string filename;
    UNSERIALIZE_SCALAR(filename);
    std::string filepath = cp.getCptDir() + "/" + filename;

    long range_size;
    UNSERIALIZE_SCALAR(range_size);

    unserializeStoreFrom(filepath, store_id, range_size);
}

static bool
hasGzipMagic(int fd)
{
    uint8_t buf[2] = {0};
    size_t sz = pread(fd, buf, 2, 0);
    panic_if(sz != 2, "Couldn't read magic bytes from object file");
    return ((buf[0] == 0x1f) && (buf[1] == 0x8b));
}

static bool
hasZSTDMagic(int fd)
{
    uint8_t buf[4];
    size_t sz = pread(fd, buf, 4, 0);
    panic_if(sz != 4, "Couldn't read magic bytes from object file");
    const uint8_t zstd_magic[4] = {0x28, 0xB5, 0x2F, 0xFD};
    return memcmp(buf, zstd_magic, 4) == 0;
}

void
PhysicalMemory::unserializeStoreFromFile(std::string filepath)
{
    warn("Unserializing physical memory from file %s\n", filepath.c_str());
    unserializeStoreFrom(filepath, 0, 0);
}

void
PhysicalMemory::unserializeStoreFrom(std::string filepath,
        unsigned store_id, long range_size)
{
    int fd = open(filepath.c_str(), O_RDONLY);
    fatal_if(fd < 0,
                "Failed to open file %s.\n"
                "This error typically occurs when the file path specified is "
                "incorrect.\n",
                filepath);

    bool is_gz = hasGzipMagic(fd);
    bool is_zstd = hasZSTDMagic(fd);
    close(fd);

    if (is_gz) {
        unserializeFromGz(filepath, store_id, range_size);
    } else if (is_zstd) {
        unserializeFromZstd(filepath, store_id, range_size);
    } else {
        fd = open(filepath.c_str(), O_RDONLY);
        fatal_if(fd < 0,
                 "Failed to open file %s.\n"
                 "This error typically occurs when the file path specified is "
                 "incorrect.\n",
                 filepath);
        off_t off = lseek(fd, 0, SEEK_END);
        fatal_if(off < 0, "Failed to determine size of file %s.\n", filepath);
        auto file_len = static_cast<size_t>(off);

        lseek(fd, 0, SEEK_SET);
        auto bytes = read(fd, backingStore[store_id].pmem, file_len);
        fatal_if(bytes != static_cast<ssize_t>(file_len),
                 "Failed to read checkpoint file %s: read %ld, expected %lu\n",
                 filepath.c_str(), bytes, file_len);

        inform("Restored from raw checkpoint file %s, %lu bytes\n",
               filepath.c_str(), file_len);
        inform("First 4 bytes are 0x%x 0x%x 0x%x 0x%x\n",
               backingStore[store_id].pmem[0], backingStore[store_id].pmem[1],
               backingStore[store_id].pmem[2], backingStore[store_id].pmem[3]);

        close(fd);
    }
}

void
PhysicalMemory::unserializeFromGz(std::string filepath,
        unsigned store_id, long range_size)
{
    gzFile compressed_mem = gzopen(filepath.c_str(), "rb");

    if (compressed_mem == nullptr)
        fatal("Can't open checkpoint file '%s'", filepath.c_str());

    uint8_t* pmem = backingStore[store_id].pmem;
    AddrRange range = backingStore[store_id].range;
    assert(pmem);

    if (range_size != 0) {
        DPRINTF(Checkpoint, "Unserializing physical memory %s with size %d\n",
                filepath.c_str(), range_size);

        if (range_size != (long)range.size()) {
            fatal("Memory range size has changed! Saw %lld, expected %lld\n",
                  range_size, range.size());
        }
    }

    uint64_t curr_size = 0;
    const uint32_t chunk_size = 16384;
    long* temp_page = new long[chunk_size];
    assert(temp_page);
    long* pmem_current;
    uint32_t bytes_read;

    while (curr_size < range.size()) {
        bytes_read = gzread(compressed_mem, temp_page, chunk_size);
        if (bytes_read == 0)
            break;

        assert(bytes_read % sizeof(long) == 0);

        for (uint32_t x = 0; x < bytes_read / sizeof(long); x++) {
            if (*(temp_page + x) != 0) {
                pmem_current = (long*)(pmem + curr_size + x * sizeof(long));
                *pmem_current = *(temp_page + x);
            }
        }
        curr_size += bytes_read;
    }

    delete[] temp_page;

    if (gzclose(compressed_mem))
        fatal("Close failed on physical memory checkpoint file '%s'\n",
              filepath.c_str());
}

void
PhysicalMemory::unserializeFromZstd(std::string filepath,
        unsigned store_id, long range_size)
{
    uint8_t* pmem = backingStore[store_id].pmem;
    AddrRange range = backingStore[store_id].range;

    auto fd = open(filepath.c_str(), O_RDONLY);
    if (fd < 0) {
        fatal("Cannot open compressed file %s\n", filepath.c_str());
    }

    auto file_size = lseek(fd, 0, SEEK_END);
    if (file_size < 0) {
        close(fd);
        fatal("Failed to determine size of compressed file %s\n",
              filepath.c_str());
    }
    if (file_size == 0) {
        close(fd);
        fatal("File size is zero\n");
    }
    lseek(fd, 0, SEEK_SET);
    warn("Read zstd file size %lu\n", file_size);

    const size_t compress_file_buffer_size = ZSTD_DStreamInSize();
    auto compress_file_buffer =
        static_cast<uint8_t*>(malloc(compress_file_buffer_size));
    if (!compress_file_buffer) {
        close(fd);
        fatal("Compress file buffer create failed\n");
    }

    ZSTD_inBuffer input = {compress_file_buffer, 0, 0};

    const uint32_t decompress_file_buffer_size = 16384;
    uint64_t* decompress_file_buffer =
        (uint64_t*)calloc(decompress_file_buffer_size, sizeof(long));
    if (!decompress_file_buffer) {
        free(compress_file_buffer);
        close(fd);
        fatal("Decompress file creating failed\n");
    }

    ZSTD_DStream* dstream = ZSTD_createDStream();
    if (!dstream) {
        free(compress_file_buffer);
        free(decompress_file_buffer);
        close(fd);
        fatal("Cannot create zstd dstream object\n");
    }

    size_t init_result = ZSTD_initDStream(dstream);
    if (ZSTD_isError(init_result)) {
        ZSTD_freeDStream(dstream);
        free(compress_file_buffer);
        free(decompress_file_buffer);
        close(fd);
        fatal("Cannot init dstream object: %s\n",
              ZSTD_getErrorName(init_result));
    }

    uint64_t* pmem_current;
    uint64_t total_write_size = 0;
    uint64_t non_zero_dword = 0;
    size_t last_result = 1;

    while (true) {
        ssize_t bytes_read = 0;
        do {
            bytes_read = read(fd, compress_file_buffer,
                              compress_file_buffer_size);
        } while (bytes_read < 0 && errno == EINTR);

        if (bytes_read < 0) {
            ZSTD_freeDStream(dstream);
            free(compress_file_buffer);
            free(decompress_file_buffer);
            close(fd);
            fatal("Compress file read failed\n");
        }

        if (bytes_read == 0) {
            break;
        }

        input.src = compress_file_buffer;
        input.size = static_cast<size_t>(bytes_read);
        input.pos = 0;

        while (input.pos < input.size) {
            ZSTD_outBuffer output = {
                decompress_file_buffer,
                decompress_file_buffer_size * sizeof(long),
                0
            };
            last_result = ZSTD_decompressStream(dstream, &output, &input);
            if (ZSTD_isError(last_result)) {
                ZSTD_freeDStream(dstream);
                free(compress_file_buffer);
                free(decompress_file_buffer);
                close(fd);
                fatal("Decompress failed: %s\n",
                      ZSTD_getErrorName(last_result));
            }

            if (total_write_size + output.pos > range.size()) {
                ZSTD_freeDStream(dstream);
                free(compress_file_buffer);
                free(decompress_file_buffer);
                close(fd);
                fatal("Decompress failed: No error detected. "
                      "Binary size is larger than memory!\n");
            }

            for (uint64_t x = 0; x < output.pos; x += sizeof(long)) {
                uint64_t read_data =
                    *(decompress_file_buffer + x / sizeof(long));
                if (read_data != 0) {
                    pmem_current = (uint64_t*)(pmem + total_write_size + x);
                    *pmem_current = read_data;
                    non_zero_dword++;
                }
            }
            total_write_size += output.pos;
        }
    }
    warn("Total write non-zero bytes: %lu\n", non_zero_dword * 8);

    if (last_result != 0) {
        ZSTD_freeDStream(dstream);
        free(compress_file_buffer);
        free(decompress_file_buffer);
        close(fd);
        fatal("Decompress failed: unexpected end of compressed input\n");
    }

    ZSTD_freeDStream(dstream);
    free(compress_file_buffer);
    free(decompress_file_buffer);
    close(fd);
}

bool
PhysicalMemory::tryRestoreFromXSCpt()
{
    if (!restoreFromXiangshanCpt) {
        return false;
    }
    unserializeStoreFromFile(xsCptPath);
    return true;
}

} // namespace memory
} // namespace gem5
