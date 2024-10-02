/* Copyright 2023 Man Group Operations Limited
 *
 * Use of this software is governed by the Business Source License 1.1 included in the file licenses/BSL.txt.
 *
 * As of the Change Date specified in that file, in accordance with the Business Source License, use of this software will be governed by the Apache License, version 2.0.
 */
#include <arcticdb/arrow/arrow_utils.hpp>
#include <sparrow/array.hpp>
#include <sparrow/arrow_interface/arrow_schema.hpp>
#include <sparrow/arrow_interface/array_data_to_arrow_array_converters.hpp>
#include <arcticdb/arrow/arrow_data.hpp>
#include <arcticdb/entity/frame_and_descriptor.hpp>
#include <arcticdb/column_store/memory_segment.hpp>

namespace arcticdb {

sparrow::arrow_schema_unique_ptr arrow_schema_from_type(std::string_view name, sparrow::data_descriptor type) {
    auto format = sparrow::data_type_to_format(type.id());
    return sparrow::make_arrow_schema_unique_ptr(
        format,
        name,
        std::nullopt,
        sparrow::ArrowFlag(0),
        std::nullopt,
        nullptr);
}

std::vector<ArrowData> arrow_data_from_column(const Column& column, std::string_view name) {
    return column.type().visit_tag([&](auto &&impl) -> std::vector<ArrowData> {
        using TagType = std::decay_t<decltype(impl)>;
        using DataType = TagType::DataTypeTag;
        using RawType = DataType::raw_type;
        std::vector<ArrowData> output;
        auto column_data = column.data();

        while (auto block = column_data.next<TagType>()) {
            sparrow::array_data data;
            auto type = sparrow::data_descriptor(sparrow::arrow_traits<RawType>::type_id);
            data.type = type;

            const auto row_count = block->row_count();
            auto *tmp = const_cast<RawType *>(block->release());
            data.buffers.emplace_back(reinterpret_cast<uint8_t *>(tmp),
                                      row_count * sizeof(RawType),
                                      sparrow::any_allocator<uint8_t>{});
            data.length = static_cast<std::int64_t>(row_count);
            data.offset = static_cast<std::int64_t>(0);
            output.emplace_back(to_arrow_array_unique_ptr(std::move(data)), arrow_schema_from_type(name, type));
        }
        return output;
    });
};

std::vector<ArrowData> arrow_data_from_string_column(Column& column, position_t column_pos, DecodePathData& shared_data, std::string_view name) {
        using ArrowStringTagType = ScalarTagType<DataTypeTag<DataType::UINT32>>;
        std::vector<ArrowData> output;
        auto column_data = column.data();
        auto& buffer_map = shared_data.buffer_map();

        while (auto block = column_data.next<ArrowStringTagType>()) {
            sparrow::array_data data;
            auto type = sparrow::data_descriptor(sparrow::arrow_traits<uint32_t>::type_id);
            data.type = type;

            const auto row_count = block->row_count();
            const auto offset = block->offset();
            auto string_buffer = buffer_map->buffers_.find(std::make_pair(column_pos, offset));
            util::check(string_buffer != buffer_map->buffers_.end(), "Failed to find arrow string data for {}: {}", column_pos, offset);
            auto string_block = string_buffer->second.block(0);
            const auto string_block_size = string_block->bytes();
            data.buffers.emplace_back(const_cast<uint8_t*>(string_block->release()), string_block_size, sparrow::any_allocator<uint8_t>{});
            auto *tmp = const_cast<uint32_t*>(block->release());
            data.buffers.emplace_back(reinterpret_cast<uint8_t *>(tmp),
                                      row_count * sizeof(uint32_t),
                                      sparrow::any_allocator<uint8_t>{});
            data.length = static_cast<std::int64_t>(row_count);
            data.offset = static_cast<std::int64_t>(0);
            output.emplace_back(to_arrow_array_unique_ptr(std::move(data)), arrow_schema_from_type(name, type));
        }
        return output;
}

std::vector<std::vector<ArrowData>> segment_to_arrow_data(SegmentInMemory& segment, DecodePathData& shared_data) {
    std::vector<std::vector<ArrowData>> output;
    for(auto i = 0UL; i < segment.num_columns(); ++i) {
        auto& column = segment.column(static_cast<position_t>(i));
        if(is_sequence_type(column.type().data_type())) {
            output.emplace_back(arrow_data_from_string_column(column, i, shared_data, segment.field(i).name()));
        } else {
            output.emplace_back(arrow_data_from_column(segment.column(static_cast<position_t>(i)), segment.field(i).name()));
        }
    }
    return output;
}

std::vector<std::string> names_from_segment(const SegmentInMemory& segment) {
    std::vector<std::string> output;
    output.reserve(segment.fields().size());
    for(auto i = 0UL; i < segment.fields().size(); ++i) {
        const auto ref = segment.field(i).ref();
        output.emplace_back(ref.name_);
    }
    return output;
}

ArrowReadResult create_arrow_read_result(
    const VersionedItem& version,
    FrameAndDescriptor&& fd) {
    auto result = std::move(fd);
    auto arrow_frame = ArrowOutputFrame{segment_to_arrow_data(result.frame_, *result.buffers_), names_from_segment(result.frame_)};
    //util::print_total_mem_usage(__FILE__, __LINE__, __FUNCTION__);

    const auto& desc_proto = result.desc_.proto();
    return {version, std::move(arrow_frame), desc_proto.user_meta()};
}

} // namespace arcticdb