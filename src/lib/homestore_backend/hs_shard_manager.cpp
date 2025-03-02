#include <homestore/homestore.hpp>
#include <homestore/blkdata_service.hpp>
#include <homestore/meta_service.hpp>
#include <homestore/replication_service.hpp>

#include "hs_homeobject.hpp"
#include "replication_message.hpp"
#include "replication_state_machine.hpp"
#include "lib/homeobject_impl.hpp"

namespace homeobject {

uint64_t ShardManager::max_shard_size() { return Gi; }

uint64_t ShardManager::max_shard_num_in_pg() { return ((uint64_t)0x01) << shard_width; }

shard_id_t HSHomeObject::generate_new_shard_id(pg_id_t pgid) {
    std::scoped_lock lock_guard(_pg_lock);
    auto iter = _pg_map.find(pgid);
    RELEASE_ASSERT(iter != _pg_map.end(), "Missing pg info");
    auto new_sequence_num = ++(iter->second->shard_sequence_num_);
    RELEASE_ASSERT(new_sequence_num < ShardManager::max_shard_num_in_pg(),
                   "new shard id must be less than ShardManager::max_shard_num_in_pg()");
    return make_new_shard_id(pgid, new_sequence_num);
}

uint64_t HSHomeObject::get_sequence_num_from_shard_id(uint64_t shard_id_t) {
    return shard_id_t & (max_shard_num_in_pg() - 1);
}

std::string HSHomeObject::serialize_shard_info(const ShardInfo& info) {
    nlohmann::json j;
    j["shard_info"]["shard_id_t"] = info.id;
    j["shard_info"]["pg_id_t"] = info.placement_group;
    j["shard_info"]["state"] = info.state;
    j["shard_info"]["created_time"] = info.created_time;
    j["shard_info"]["modified_time"] = info.last_modified_time;
    j["shard_info"]["total_capacity"] = info.total_capacity_bytes;
    j["shard_info"]["available_capacity"] = info.available_capacity_bytes;
    j["shard_info"]["deleted_capacity"] = info.deleted_capacity_bytes;
    return j.dump();
}

ShardInfo HSHomeObject::deserialize_shard_info(const char* json_str, size_t str_size) {
    ShardInfo shard_info;
    auto shard_json = nlohmann::json::parse(json_str, json_str + str_size);
    shard_info.id = shard_json["shard_info"]["shard_id_t"].get< shard_id_t >();
    shard_info.placement_group = shard_json["shard_info"]["pg_id_t"].get< pg_id_t >();
    shard_info.state = static_cast< ShardInfo::State >(shard_json["shard_info"]["state"].get< int >());
    shard_info.created_time = shard_json["shard_info"]["created_time"].get< uint64_t >();
    shard_info.last_modified_time = shard_json["shard_info"]["modified_time"].get< uint64_t >();
    shard_info.available_capacity_bytes = shard_json["shard_info"]["available_capacity"].get< uint64_t >();
    shard_info.total_capacity_bytes = shard_json["shard_info"]["total_capacity"].get< uint64_t >();
    shard_info.deleted_capacity_bytes = shard_json["shard_info"]["deleted_capacity"].get< uint64_t >();
    return shard_info;
}

ShardManager::AsyncResult< ShardInfo > HSHomeObject::_create_shard(pg_id_t pg_owner, uint64_t size_bytes) {
    shared< homestore::ReplDev > repl_dev;
    {
        std::shared_lock lock_guard(_pg_lock);
        auto iter = _pg_map.find(pg_owner);
        if (iter == _pg_map.end()) {
            LOGW("failed to create shard with non-exist pg [{}]", pg_owner);
            return folly::makeUnexpected(ShardError::UNKNOWN_PG);
        }
        repl_dev = static_cast< HS_PG* >(iter->second.get())->repl_dev_;
    }

    if (!repl_dev) {
        LOGW("failed to get repl dev instance for pg [{}]", pg_owner);
        return folly::makeUnexpected(ShardError::PG_NOT_READY);
    }

    auto new_shard_id = generate_new_shard_id(pg_owner);
    auto create_time = get_current_timestamp();
    std::string const create_shard_message = serialize_shard_info(
        ShardInfo(new_shard_id, pg_owner, ShardInfo::State::OPEN, create_time, create_time, size_bytes, size_bytes, 0));
    const auto msg_size = sisl::round_up(create_shard_message.size(), repl_dev->get_blk_size());
    auto req = repl_result_ctx< ShardManager::Result< ShardInfo > >::make(msg_size, 512 /*alignment*/);
    auto buf_ptr = req->hdr_buf_.bytes();
    std::memset(buf_ptr, 0, msg_size);
    std::memcpy(buf_ptr, create_shard_message.c_str(), create_shard_message.size());
    // preapre msg header;
    req->header_.msg_type = ReplicationMessageType::CREATE_SHARD_MSG;
    req->header_.pg_id = pg_owner;
    req->header_.shard_id = new_shard_id;
    req->header_.payload_size = msg_size;
    req->header_.payload_crc = crc32_ieee(init_crc32, buf_ptr, msg_size);
    req->header_.seal();
    sisl::blob header;
    header.set_bytes(r_cast< uint8_t* >(&req->header_));
    header.set_size(sizeof(req->header_));
    sisl::sg_list value;
    value.size = msg_size;
    value.iovs.push_back(iovec(buf_ptr, msg_size));
    // replicate this create shard message to PG members;
    repl_dev->async_alloc_write(header, sisl::blob{buf_ptr, (uint32_t)msg_size}, value, req);
    return req->result().deferValue([this](auto const& e) -> ShardManager::Result< ShardInfo > {
        if (!e) return folly::makeUnexpected(e.error());
        return e.value();
    });
}

ShardManager::AsyncResult< ShardInfo > HSHomeObject::_seal_shard(ShardInfo const& info) {
    auto& pg_id = info.placement_group;
    auto& shard_id = info.id;
    ShardInfo shard_info = info;

    shared< homestore::ReplDev > repl_dev;
    {
        std::shared_lock lock_guard(_pg_lock);
        auto iter = _pg_map.find(pg_id);
        RELEASE_ASSERT(iter != _pg_map.end(), "PG not found");
        repl_dev = static_cast< HS_PG* >(iter->second.get())->repl_dev_;
        RELEASE_ASSERT(repl_dev != nullptr, "Repl dev null");
    }

    shard_info.state = ShardInfo::State::SEALED;
    auto seal_shard_message = serialize_shard_info(shard_info);
    const auto msg_size = sisl::round_up(seal_shard_message.size(), repl_dev->get_blk_size());
    auto req = repl_result_ctx< ShardManager::Result< ShardInfo > >::make(msg_size, 512 /*alignment*/);
    auto buf_ptr = req->hdr_buf_.bytes();
    std::memset(buf_ptr, 0, msg_size);
    std::memcpy(buf_ptr, seal_shard_message.c_str(), seal_shard_message.size());

    req->header_.msg_type = ReplicationMessageType::SEAL_SHARD_MSG;
    req->header_.pg_id = pg_id;
    req->header_.shard_id = shard_id;
    req->header_.payload_size = msg_size;
    req->header_.payload_crc = crc32_ieee(init_crc32, buf_ptr, msg_size);
    req->header_.seal();
    sisl::blob header;
    header.set_bytes(r_cast< uint8_t* >(&req->header_));
    header.set_size(sizeof(req->header_));
    sisl::sg_list value;
    value.size = msg_size;
    value.iovs.push_back(iovec(buf_ptr, msg_size));

    // replicate this seal shard message to PG members;
    repl_dev->async_alloc_write(header, sisl::blob{buf_ptr, (uint32_t)msg_size}, value, req);
    return req->result();
}

void HSHomeObject::on_shard_message_commit(int64_t lsn, sisl::blob const& header, homestore::MultiBlkId const& blkids,
                                           shared< homestore::ReplDev > repl_dev,
                                           cintrusive< homestore::repl_req_ctx >& hs_ctx) {

    if (hs_ctx != nullptr) {
        do_shard_message_commit(lsn, *r_cast< ReplicationMessageHeader* >(const_cast< uint8_t* >(header.cbytes())),
                                blkids, hs_ctx->key, hs_ctx);
        return;
    }

    // hs_ctx will be nullptr when HS is restarting and replay all commited log entries from the last checkpoint;
    // most of time, the create_shard/seal_shard on_commit() is already completed before restarting/crash and shard info
    // is already be written into metablk, so we need to do nothing in this case to avoid duplication when replay this
    // journal log. but there is still a smaller chance that HO is stopped/crashed before writing metablk is called or
    // completed and we need to recover shard info from journal log in such case.
    sisl::sg_list value;
    value.size = blkids.blk_count() * repl_dev->get_blk_size();
    auto value_buf = iomanager.iobuf_alloc(512, value.size);
    value.iovs.push_back(iovec{.iov_base = value_buf, .iov_len = value.size});
    // header will be released when this function returns, but we still need the header when async_read() finished.
    auto header_ptr = r_cast< const ReplicationMessageHeader* >(header.cbytes());
    repl_dev->async_read(blkids, value, value.size)
        .thenValue([this, lsn, msg_header = *header_ptr, blkids, value](auto&& err) mutable {
            if (err) {
                LOGW("failed to read data from homestore pba, lsn:{}", lsn);
            } else {
                sisl::blob value_blob(r_cast< uint8_t* >(value.iovs[0].iov_base), value.size);
                do_shard_message_commit(lsn, msg_header, blkids, value_blob, nullptr);
            }
            iomanager.iobuf_free(r_cast< uint8_t* >(value.iovs[0].iov_base));
        });
}

void HSHomeObject::do_shard_message_commit(int64_t lsn, ReplicationMessageHeader& header,
                                           homestore::MultiBlkId const& blkids, sisl::blob value,
                                           cintrusive< homestore::repl_req_ctx >& hs_ctx) {
    repl_result_ctx< ShardManager::Result< ShardInfo > >* ctx{nullptr};
    if (hs_ctx && hs_ctx->is_proposer) {
        ctx = boost::static_pointer_cast< repl_result_ctx< ShardManager::Result< ShardInfo > > >(hs_ctx).get();
    }

    if (header.corrupted()) {
        LOGW("replication message header is corrupted with crc error, lsn:{}", lsn);
        if (ctx) { ctx->promise_.setValue(folly::makeUnexpected(ShardError::CRC_MISMATCH)); }
        return;
    }

    if (crc32_ieee(init_crc32, value.cbytes(), value.size()) != header.payload_crc) {
        // header & value is inconsistent;
        LOGW("replication message header is inconsistent with value, lsn:{}", lsn);
        if (ctx) { ctx->promise_.setValue(folly::makeUnexpected(ShardError::CRC_MISMATCH)); }
        return;
    }

    auto shard_info = deserialize_shard_info(r_cast< const char* >(value.cbytes()), value.size());
    switch (header.msg_type) {
    case ReplicationMessageType::CREATE_SHARD_MSG: {
        bool shard_exist = false;
        {
            std::scoped_lock lock_guard(_shard_lock);
            shard_exist = (_shard_map.find(shard_info.id) != _shard_map.end());
        }

        if (!shard_exist) {
            add_new_shard_to_map(std::make_unique< HS_Shard >(shard_info, blkids.chunk_num()));
            // select_specific_chunk() will do something only when we are relaying journal after restart, during the
            // runtime flow chunk is already been be mark busy when we write the shard info to the repldev.
            chunk_selector_->select_specific_chunk(blkids.chunk_num());
        }

        break;
    }

    case ReplicationMessageType::SEAL_SHARD_MSG: {
        ShardInfo::State state;
        {
            std::scoped_lock lock_guard(_shard_lock);
            auto iter = _shard_map.find(shard_info.id);
            RELEASE_ASSERT(iter != _shard_map.end(), "Missing shard info");
            state = (*iter->second)->info.state;
        }

        if (state == ShardInfo::State::OPEN) {
            auto chunk_id = get_shard_chunk(shard_info.id);
            RELEASE_ASSERT(chunk_id.has_value(), "Chunk id not found");
            chunk_selector()->release_chunk(chunk_id.value());
            update_shard_in_map(shard_info);
        }

        break;
    }
    default: {
        break;
    }
    }

    if (ctx) { ctx->promise_.setValue(ShardManager::Result< ShardInfo >(shard_info)); }
}

void HSHomeObject::add_new_shard_to_map(ShardPtr&& shard) {
    // TODO: We are taking a global lock for all pgs to create shard. Is it really needed??
    // We need to have fine grained per PG lock and take only that.
    std::scoped_lock lock_guard(_pg_lock, _shard_lock);
    auto pg_iter = _pg_map.find(shard->info.placement_group);
    RELEASE_ASSERT(pg_iter != _pg_map.end(), "Missing PG info");
    auto& shards = pg_iter->second->shards_;
    auto shard_id = shard->info.id;
    auto iter = shards.emplace(shards.end(), std::move(shard));
    auto [_, happened] = _shard_map.emplace(shard_id, iter);
    RELEASE_ASSERT(happened, "duplicated shard info");

    // following part gives follower members a chance to catch up shard sequence num;
    auto sequence_num = get_sequence_num_from_shard_id(shard_id);
    if (sequence_num > pg_iter->second->shard_sequence_num_) { pg_iter->second->shard_sequence_num_ = sequence_num; }
}

void HSHomeObject::update_shard_in_map(const ShardInfo& shard_info) {
    std::scoped_lock lock_guard(_shard_lock);
    auto shard_iter = _shard_map.find(shard_info.id);
    RELEASE_ASSERT(shard_iter != _shard_map.end(), "Missing shard info");
    auto hs_shard = d_cast< HS_Shard* >((*shard_iter->second).get());
    hs_shard->update_info(shard_info);
}

std::optional< homestore::chunk_num_t > HSHomeObject::get_shard_chunk(shard_id_t id) const {
    std::scoped_lock lock_guard(_shard_lock);
    auto shard_iter = _shard_map.find(id);
    if (shard_iter == _shard_map.end()) { return std::nullopt; }
    auto hs_shard = d_cast< HS_Shard* >((*shard_iter->second).get());
    return std::make_optional< homestore::chunk_num_t >(hs_shard->sb_->chunk_id);
}

std::optional< homestore::chunk_num_t > HSHomeObject::get_any_chunk_id(pg_id_t const pg_id) {
    std::scoped_lock lock_guard(_pg_lock);
    auto pg_iter = _pg_map.find(pg_id);
    RELEASE_ASSERT(pg_iter != _pg_map.end(), "Missing PG info");
    HS_PG* pg = static_cast< HS_PG* >(pg_iter->second.get());
    if (pg->any_allocated_chunk_id_.has_value()) {
        // it is already cached and use it;
        return pg->any_allocated_chunk_id_;
    }

    auto& shards = pg->shards_;
    if (shards.empty()) { return std::nullopt; }
    auto hs_shard = d_cast< HS_Shard* >(shards.front().get());
    // cache it;
    pg->any_allocated_chunk_id_ = hs_shard->sb_->chunk_id;
    return pg->any_allocated_chunk_id_;
}

HSHomeObject::HS_Shard::HS_Shard(ShardInfo shard_info, homestore::chunk_num_t chunk_id) :
        Shard(std::move(shard_info)), sb_(_shard_meta_name) {
    sb_.create(sizeof(shard_info_superblk));
    sb_->chunk_id = chunk_id;
    write_sb();
}

HSHomeObject::HS_Shard::HS_Shard(homestore::superblk< shard_info_superblk >&& sb) :
        Shard(shard_info_from_sb(sb)), sb_(std::move(sb)) {}

void HSHomeObject::HS_Shard::update_info(const ShardInfo& shard_info) {
    info = shard_info;
    write_sb();
}

void HSHomeObject::HS_Shard::write_sb() {
    sb_->id = info.id;
    sb_->placement_group = info.placement_group;
    sb_->state = info.state;
    sb_->created_time = info.created_time;
    sb_->last_modified_time = info.last_modified_time;
    sb_->available_capacity_bytes = info.available_capacity_bytes;
    sb_->total_capacity_bytes = info.total_capacity_bytes;
    sb_->deleted_capacity_bytes = info.deleted_capacity_bytes;
    sb_.write();
}

ShardInfo HSHomeObject::HS_Shard::shard_info_from_sb(homestore::superblk< shard_info_superblk > const& sb) {
    ShardInfo info;
    info.id = sb->id;
    info.placement_group = sb->placement_group;
    info.state = sb->state;
    info.created_time = sb->created_time;
    info.last_modified_time = sb->last_modified_time;
    info.available_capacity_bytes = sb->available_capacity_bytes;
    info.total_capacity_bytes = sb->total_capacity_bytes;
    info.deleted_capacity_bytes = sb->deleted_capacity_bytes;
    return info;
}

} // namespace homeobject
