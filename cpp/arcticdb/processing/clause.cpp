/* Copyright 2023 Man Group Operations Limited
 *
 * Use of this software is governed by the Business Source License 1.1 included in the file licenses/BSL.txt.
 *
 * As of the Change Date specified in that file, in accordance with the Business Source License, use of this software will be governed by the Apache License, version 2.0.
 */

#include <vector>
#include <variant>
#include <arcticdb/processing/processing_unit.hpp>
#include <folly/Poly.h>
#include <arcticdb/util/composite.hpp>
#include <arcticdb/processing/clause.hpp>
#include <arcticdb/pipeline/column_stats.hpp>
#include <arcticdb/pipeline/value_set.hpp>
#include <arcticdb/pipeline/frame_slice.hpp>
#include <arcticdb/stream/segment_aggregator.hpp>
#ifdef ARCTICDB_USING_CONDA
    #include <robin_hood.h>
#else
    #include <arcticdb/util/third_party/robin_hood.hpp>
#endif
namespace arcticdb {

using namespace pipelines;

std::vector<std::vector<size_t>> structure_by_row_slice(std::vector<RangesAndKey>& ranges_and_keys,
                                                           size_t start_from) {
    std::sort(std::begin(ranges_and_keys), std::end(ranges_and_keys), [] (const RangesAndKey& left, const RangesAndKey& right) {
        return std::tie(left.row_range_.first, left.col_range_.first) < std::tie(right.row_range_.first, right.col_range_.first);
    });
    ranges_and_keys.erase(ranges_and_keys.begin(), ranges_and_keys.begin() + start_from);
    std::vector<std::vector<size_t>> res;
    RowRange previous_row_range;
    for (const auto& [idx, ranges_and_key]: folly::enumerate(ranges_and_keys)) {
        RowRange current_row_range{ranges_and_key.row_range_};
        if (current_row_range != previous_row_range) {
            res.emplace_back();
        }
        res.back().emplace_back(idx);
        previous_row_range = current_row_range;
    }
    return res;
}

std::vector<std::vector<size_t>> structure_by_column_slice(std::vector<RangesAndKey>& ranges_and_keys) {
    std::sort(std::begin(ranges_and_keys), std::end(ranges_and_keys), [] (const RangesAndKey& left, const RangesAndKey& right) {
        return std::tie(left.col_range_.first, left.row_range_.first) < std::tie(right.col_range_.first, right.row_range_.first);
    });
    std::vector<std::vector<size_t>> res;
    ColRange previous_col_range;
    for (const auto& [idx, ranges_and_key]: folly::enumerate(ranges_and_keys)) {
        ColRange current_col_range{ranges_and_key.col_range_};
        if (current_col_range != previous_col_range) {
            res.emplace_back();
        }
        res.back().emplace_back(idx);
        previous_col_range = current_col_range;
    }
    return res;
}

/*
 * On entry to a clause, construct ProcessingUnits from the input entity IDs. These will either be provided by the
 * structure_for_processing method for the first clause in the pipeline, or by the previous clause for all subsequent
 * clauses.
 * At time of writing, all clauses require segments, row ranges, and column ranges. Some also require atom keys and
 * partitioning buckets, so these can optionally be populated in the output processing units as well.
 */
Composite<ProcessingUnit> gather_entities(std::shared_ptr<ComponentManager> component_manager,
                                          Composite<EntityIds>&& entity_ids,
                                          bool include_atom_keys,
                                          bool include_bucket,
                                          bool include_initial_expected_get_calls) {
    return entity_ids.transform([&component_manager, include_atom_keys, include_bucket, include_initial_expected_get_calls]
    (const EntityIds& entity_ids) -> ProcessingUnit {
        ProcessingUnit res;
        std::vector<std::shared_ptr<SegmentInMemory>> segments;
        std::vector<std::shared_ptr<RowRange>> row_ranges;
        std::vector<std::shared_ptr<ColRange>> col_ranges;
        segments.reserve(entity_ids.size());
        row_ranges.reserve(entity_ids.size());
        col_ranges.reserve(entity_ids.size());
        for (auto entity_id: entity_ids) {
            segments.emplace_back(component_manager->get<std::shared_ptr<SegmentInMemory>>(entity_id));
            row_ranges.emplace_back(component_manager->get<std::shared_ptr<RowRange>>(entity_id));
            col_ranges.emplace_back(component_manager->get<std::shared_ptr<ColRange>>(entity_id));
        }
        res.set_segments(std::move(segments));
        res.set_row_ranges(std::move(row_ranges));
        res.set_col_ranges(std::move(col_ranges));

        if (include_atom_keys) {
            std::vector<std::shared_ptr<AtomKey>> keys;
            keys.reserve(entity_ids.size());
            for (auto entity_id: entity_ids) {
                keys.emplace_back(component_manager->get<std::shared_ptr<AtomKey>>(entity_id));
            }
            res.set_atom_keys(std::move(keys));
        }
        if (include_bucket) {
            std::vector<size_t> buckets;
            buckets.reserve(entity_ids.size());
            for (auto entity_id: entity_ids) {
                buckets.emplace_back(component_manager->get<size_t>(entity_id));
            }
            // Each entity_id has a bucket, but they must all be the same within one processing unit
            if (buckets.size() > 0) {
                internal::check<ErrorCode::E_ASSERTION_FAILURE>(
                        std::adjacent_find(buckets.begin(), buckets.end(), std::not_equal_to<>() ) == buckets.end(),
                        "Partitioning error: segments to be processed together must be in the same bucket"
                        );
                res.set_bucket(buckets.at(0));
            }
        }
        if (include_initial_expected_get_calls) {
            std::vector<uint64_t> segment_initial_expected_get_calls;
            segment_initial_expected_get_calls.reserve(entity_ids.size());
            for (auto entity_id: entity_ids) {
                segment_initial_expected_get_calls.emplace_back(component_manager->get_initial_expected_get_calls<std::shared_ptr<SegmentInMemory>>(entity_id));
            }
            res.set_segment_initial_expected_get_calls(std::move(segment_initial_expected_get_calls));
        }
        return res;
    });
}

/*
 * On exit from a clause, we need to push the elements of the newly created processing unit's into the component
 * manager. These will either be used by the next clause in the pipeline, or to present the output dataframe back to
 * the user if this is the final clause in the pipeline.
 * Elements that share an index in the optional vectors of a ProcessingUnit correspond to the same entity, and so are
 * pushed into the component manager with the same ID.
 */
EntityIds push_entities(std::shared_ptr<ComponentManager> component_manager, ProcessingUnit&& proc) {
    std::optional<EntityIds> res;
    if (proc.segments_.has_value()) {
        res = std::make_optional<EntityIds>();
        for (const auto& segment: *proc.segments_) {
            res->emplace_back(component_manager->add(segment, std::nullopt, 1));
        }
    }
    if (proc.row_ranges_.has_value()) {
        if (res.has_value()) {
            for (const auto& [idx, row_range]: folly::enumerate(*proc.row_ranges_)) {
                component_manager->add(row_range, res->at(idx));
            }
        } else {
            res = std::make_optional<EntityIds>();
            for (const auto& row_range: *proc.row_ranges_) {
                res->emplace_back(component_manager->add(row_range));
            }
        }
    }
    if (proc.col_ranges_.has_value()) {
        if (res.has_value()) {
            for (const auto& [idx, col_range]: folly::enumerate(*proc.col_ranges_)) {
                component_manager->add(col_range, res->at(idx));
            }
        } else {
            res = std::make_optional<EntityIds>();
            for (const auto& col_range: *proc.col_ranges_) {
                res->emplace_back(component_manager->add(col_range));
            }
        }
    }
    if (proc.atom_keys_.has_value()) {
        if (res.has_value()) {
            for (const auto& [idx, atom_key]: folly::enumerate(*proc.atom_keys_)) {
                component_manager->add(atom_key, res->at(idx));
            }
        } else {
            res = std::make_optional<EntityIds>();
            for (const auto& atom_key: *proc.atom_keys_) {
                res->emplace_back(component_manager->add(atom_key));
            }
        }
    }
    internal::check<ErrorCode::E_ASSERTION_FAILURE>(res.has_value(), "Unexpected empty result in push_entities");
    if (proc.bucket_.has_value()) {
        for (auto entity_id: *res) {
            component_manager->add(*proc.bucket_, entity_id);
        }
    }
    return *res;
}

std::vector<Composite<EntityIds>> single_partition(std::vector<Composite<EntityIds>> &&comps) {
    std::vector<Composite<EntityIds>> v;
    v.push_back(merge_composites_shallow(std::move(comps)));
    return v;
}

class GroupingMap {
    using NumericMapType = std::variant<
            std::monostate,
            std::shared_ptr<robin_hood::unordered_flat_map<bool, size_t>>,
            std::shared_ptr<robin_hood::unordered_flat_map<uint8_t, size_t>>,
            std::shared_ptr<robin_hood::unordered_flat_map<uint16_t, size_t>>,
            std::shared_ptr<robin_hood::unordered_flat_map<uint32_t, size_t>>,
            std::shared_ptr<robin_hood::unordered_flat_map<uint64_t, size_t>>,
            std::shared_ptr<robin_hood::unordered_flat_map<int8_t, size_t>>,
            std::shared_ptr<robin_hood::unordered_flat_map<int16_t, size_t>>,
            std::shared_ptr<robin_hood::unordered_flat_map<int32_t, size_t>>,
            std::shared_ptr<robin_hood::unordered_flat_map<int64_t, size_t>>,
            std::shared_ptr<robin_hood::unordered_flat_map<float, size_t>>,
            std::shared_ptr<robin_hood::unordered_flat_map<double, size_t>>>;

    NumericMapType map_;

public:
    size_t size() const {
        return util::variant_match(map_,
                                   [](const std::monostate &) {
                                       return size_t(0);
                                   },
                                   [](const auto &other) {
                                       return other->size();
                                   });
    }

    template<typename T>
    std::shared_ptr<robin_hood::unordered_flat_map<T, size_t>> get() {
        return util::variant_match(map_,
                                   [that = this](const std::monostate &) {
                                       that->map_ = std::make_shared<robin_hood::unordered_flat_map<T, size_t>>();
                                       return std::get<std::shared_ptr<robin_hood::unordered_flat_map<T, size_t>>>(that->map_);
                                   },
                                   [](const std::shared_ptr<robin_hood::unordered_flat_map<T, size_t>> &ptr) {
                                       return ptr;
                                   },
                                   [](const auto &) -> std::shared_ptr<robin_hood::unordered_flat_map<T, size_t>> {
                                       schema::raise<ErrorCode::E_UNSUPPORTED_COLUMN_TYPE>(
                                               "GroupBy does not support the grouping column type changing with dynamic schema");
                                   });
    }
};

struct SegmentWrapper {
    SegmentInMemory seg_;
    SegmentInMemory::iterator it_;
    const StreamId id_;

    explicit SegmentWrapper(SegmentInMemory&& seg) :
            seg_(std::move(seg)),
            it_(seg_.begin()),
            id_(seg_.descriptor().id()) {
    }

    bool advance() {
        return ++it_ != seg_.end();
    }

    SegmentInMemory::Row &row() {
        return *it_;
    }

    const StreamId &id() const {
        return id_;
    }
};

Composite<EntityIds> PassthroughClause::process(Composite<EntityIds>&& entity_ids) const {
    return std::move(entity_ids);
}

Composite<EntityIds> FilterClause::process(
        Composite<EntityIds>&& entity_ids
        ) const {
    auto procs = gather_entities(component_manager_, std::move(entity_ids));
    Composite<EntityIds> output;
    procs.broadcast([&output, this](auto&& proc) {
        proc.set_expression_context(expression_context_);
        auto variant_data = proc.get(expression_context_->root_node_name_);
        util::variant_match(variant_data,
                            [&proc, &output, this](const std::shared_ptr<util::BitSet> &bitset) {
                                if (bitset->count() > 0) {
                                    proc.apply_filter(*bitset, optimisation_);
                                    output.push_back(push_entities(component_manager_, std::move(proc)));
                                } else {
                                    log::version().debug("Filter returned empty result");
                                }
                            },
                            [](EmptyResult) {
                               log::version().debug("Filter returned empty result");
                            },
                            [&output, &proc, this](FullResult) {
                                output.push_back(push_entities(component_manager_, std::move(proc)));
                            },
                            [](const auto &) {
                                util::raise_rte("Expected bitset from filter clause");
                            });
    });
    return output;
}

std::string FilterClause::to_string() const {
    return expression_context_ ? fmt::format("WHERE {}", expression_context_->root_node_name_.value) : "";
}

Composite<EntityIds> ProjectClause::process(Composite<EntityIds>&& entity_ids) const {
    auto procs = gather_entities(component_manager_, std::move(entity_ids));
    Composite<EntityIds> output;
    procs.broadcast([&output, this](auto&& proc) {
        proc.set_expression_context(expression_context_);
        auto variant_data = proc.get(expression_context_->root_node_name_);
        util::variant_match(variant_data,
                            [&proc, &output, this](ColumnWithStrings &col) {

                                const auto data_type = col.column_->type().data_type();
                                const std::string_view name = output_column_;

                                proc.segments_->back()->add_column(scalar_field(data_type, name), col.column_);
                                ++proc.col_ranges_->back()->second;
                                output.push_back(push_entities(component_manager_, std::move(proc)));
                            },
                            [&proc, &output, this](const EmptyResult&) {
                                if(expression_context_->dynamic_schema_)
                                    output.push_back(push_entities(component_manager_, std::move(proc)));
                                else
                                    util::raise_rte("Cannot project from empty column with static schema");
                            },
                            [](const auto &) {
                                util::raise_rte("Expected column from projection clause");
                            });
    });
    return output;
}

[[nodiscard]] std::string ProjectClause::to_string() const {
    return expression_context_ ? fmt::format("PROJECT Column[\"{}\"] = {}", output_column_, expression_context_->root_node_name_.value) : "";
}

AggregationClause::AggregationClause(const std::string& grouping_column,
                                     const std::unordered_map<std::string,
                                     std::string>& aggregations):
        grouping_column_(grouping_column),
        aggregation_map_(aggregations) {
    clause_info_.can_combine_with_column_selection_ = false;
    clause_info_.new_index_ = grouping_column_;
    clause_info_.input_columns_ = std::make_optional<std::unordered_set<std::string>>({grouping_column_});
    clause_info_.modifies_output_descriptor_ = true;
    for (const auto& [column_name, aggregation_operator]: aggregations) {
        auto [_, inserted] = clause_info_.input_columns_->insert(column_name);
        user_input::check<ErrorCode::E_INVALID_USER_ARGUMENT>(inserted,
                                                              "Cannot perform two aggregations over the same column: {}",
                                                              column_name);
        auto typed_column_name = ColumnName(column_name);
        if (aggregation_operator == "sum") {
            aggregators_.emplace_back(SumAggregator(typed_column_name, typed_column_name));
        } else if (aggregation_operator == "mean") {
            aggregators_.emplace_back(MeanAggregator(typed_column_name, typed_column_name));
        } else if (aggregation_operator == "max") {
            aggregators_.emplace_back(MaxAggregator(typed_column_name, typed_column_name));
        } else if (aggregation_operator == "min") {
            aggregators_.emplace_back(MinAggregator(typed_column_name, typed_column_name));
        } else if (aggregation_operator == "count") {
            aggregators_.emplace_back(CountAggregator(typed_column_name, typed_column_name));
        } else {
            user_input::raise<ErrorCode::E_INVALID_USER_ARGUMENT>("Unknown aggregation operator provided: {}", aggregation_operator);
        }
    }
}

Composite<EntityIds> AggregationClause::process(Composite<EntityIds>&& entity_ids) const {
    auto procs = gather_entities(component_manager_, std::move(entity_ids));
    std::vector<GroupingAggregatorData> aggregators_data;
    internal::check<ErrorCode::E_INVALID_ARGUMENT>(
            !aggregators_.empty(),
            "AggregationClause::process does not make sense with no aggregators");
    for (const auto &agg: aggregators_){
        aggregators_data.emplace_back(agg.get_aggregator_data());
    }

    // Work out the common type between the processing units for the columns being aggregated
    procs.broadcast([&aggregators_data, &aggregators=aggregators_](auto& proc) {
        for (auto agg_data: folly::enumerate(aggregators_data)) {
            auto input_column_name = aggregators.at(agg_data.index).get_input_column_name();
            auto input_column = proc.get(input_column_name);
            if (std::holds_alternative<ColumnWithStrings>(input_column)) {
                agg_data->add_data_type(std::get<ColumnWithStrings>(input_column).column_->type().data_type());
            }
        }
    });

    size_t num_unique{0};
    size_t next_group_id{0};
    auto string_pool = std::make_shared<StringPool>();
    DataType grouping_data_type;
    GroupingMap grouping_map;
    procs.broadcast(
        [&num_unique, &grouping_data_type, &grouping_map, &next_group_id, &aggregators_data, &string_pool, this](auto &proc) {
            auto partitioning_column = proc.get(ColumnName(grouping_column_));
            if (std::holds_alternative<ColumnWithStrings>(partitioning_column)) {
                ColumnWithStrings col = std::get<ColumnWithStrings>(partitioning_column);
                entity::details::visit_type(col.column_->type().data_type(),
                                            [&proc_=proc, &grouping_map, &next_group_id, &aggregators_data, &string_pool, &col,
                                             &num_unique, &grouping_data_type, this](auto data_type_tag) {
                                                using DataTypeTagType = decltype(data_type_tag);
                                                using RawType = typename DataTypeTagType::raw_type;
                                                constexpr auto data_type = DataTypeTagType::data_type;
                                                grouping_data_type = data_type;
                                                std::vector<size_t> row_to_group;
                                                row_to_group.reserve(col.column_->row_count());
                                                auto input_data = col.column_->data();
                                                auto hash_to_group = grouping_map.get<RawType>();
                                                // For string grouping columns, keep a local map within this ProcessingUnit
                                                // from offsets to groups, to avoid needless calls to col.string_at_offset and
                                                // string_pool->get
                                                // This could be slower in cases where there aren't many repeats in string
                                                // grouping columns. Maybe track hit ratio of finds and stop using it if it is
                                                // too low?
                                                // Tested with 100,000,000 row dataframe with 100,000 unique values in the grouping column. Timings:
                                                // 11.14 seconds without caching
                                                // 11.01 seconds with caching
                                                // Not worth worrying about right now
                                                robin_hood::unordered_flat_map<RawType, size_t> offset_to_group;
                                                while (auto block = input_data.next<ScalarTagType<DataTypeTagType>>()) {
                                                    const auto row_count = block->row_count();
                                                    auto ptr = block->data();
                                                    for (size_t i = 0; i < row_count; ++i, ++ptr) {
                                                        RawType val;
                                                        if constexpr(is_sequence_type(data_type)) {
                                                            auto offset = *ptr;
                                                            if (auto it = offset_to_group.find(offset); it != offset_to_group.end()) {
                                                                val = it->second;
                                                            } else {
                                                                std::optional<std::string_view> str = col.string_at_offset(offset);
                                                                if (str.has_value()) {
                                                                    val = string_pool->get(*str, true).offset();
                                                                } else {
                                                                    val = offset;
                                                                }
                                                                RawType val_copy(val);
                                                                offset_to_group.insert(robin_hood::pair<RawType, size_t>(offset, val_copy));
                                                            }
                                                        } else {
                                                            val = *ptr;
                                                        }
                                                        if (auto it = hash_to_group->find(val); it == hash_to_group->end()) {
                                                            row_to_group.emplace_back(next_group_id);
                                                            auto group_id = next_group_id++;
                                                            hash_to_group->insert(robin_hood::pair<RawType, size_t>(val, group_id));
                                                        } else {
                                                            row_to_group.emplace_back(it->second);
                                                        }
                                                    }
                                                }

                                                num_unique = next_group_id;
                                                util::check(num_unique != 0, "Got zero unique values");
                                                for (auto agg_data: folly::enumerate(aggregators_data)) {
                                                    auto input_column_name = aggregators_.at(agg_data.index).get_input_column_name();
                                                    auto input_column = proc_.get(input_column_name);
                                                    std::optional<ColumnWithStrings> opt_input_column;
                                                    if (std::holds_alternative<ColumnWithStrings>(input_column)) {
                                                        auto column_with_strings = std::get<ColumnWithStrings>(input_column);
                                                        // Empty columns don't contribute to aggregations
                                                        if (!is_empty_type(column_with_strings.column_->type().data_type())) {
                                                            opt_input_column.emplace(std::move(column_with_strings));
                                                        }
                                                    }
                                                    agg_data->aggregate(opt_input_column, row_to_group, num_unique);
                                                }
                                            });
            } else {
                util::raise_rte("Expected single column from expression");
            }
        });

    SegmentInMemory seg;
    auto index_col = std::make_shared<Column>(make_scalar_type(grouping_data_type), grouping_map.size(), true, false);
    auto index_pos = seg.add_column(scalar_field(grouping_data_type, grouping_column_), index_col);
    seg.descriptor().set_index(IndexDescriptor(0, IndexDescriptor::ROWCOUNT));

    entity::details::visit_type(grouping_data_type, [&seg, &grouping_map, index_pos](auto data_type_tag) {
        using DataTypeTagType = decltype(data_type_tag);
        using RawType = typename DataTypeTagType::raw_type;
        auto hashes = grouping_map.get<RawType>();
        auto index_ptr = reinterpret_cast<RawType *>(seg.column(index_pos).ptr());
        std::vector<std::pair<RawType, size_t>> elements;
        for (const auto &hash : *hashes)
            elements.push_back(std::make_pair(hash.first, hash.second));

        std::sort(std::begin(elements),
                  std::end(elements),
                  [](const std::pair<RawType, size_t> &l, const std::pair<RawType, size_t> &r) {
                      return l.second < r.second;
                  });

        for (const auto &element : elements)
            *index_ptr++ = element.first;
    });
    index_col->set_row_data(grouping_map.size() - 1);

    for (auto agg_data: folly::enumerate(aggregators_data)) {
        seg.concatenate(agg_data->finalize(aggregators_.at(agg_data.index).get_output_column_name(), processing_config_.dynamic_schema_, num_unique));
    }

    seg.set_string_pool(string_pool);
    seg.set_row_id(num_unique - 1);
    return Composite<EntityIds>(push_entities(component_manager_, ProcessingUnit(std::move(seg))));
}

[[nodiscard]] std::string AggregationClause::to_string() const {
    return fmt::format("AGGREGATE {}", aggregation_map_);
}

void ResampleClause::set_aggregations(const std::unordered_map<std::string, std::string>& aggregations) {
    aggregation_map_ = aggregations;
    clause_info_.input_columns_ = std::make_optional<std::unordered_set<std::string>>();
    for (const auto& [column_name, aggregation_operator]: aggregations) {
        auto [_, inserted] = clause_info_.input_columns_->insert(column_name);
        user_input::check<ErrorCode::E_INVALID_USER_ARGUMENT>(inserted,
                                                              "Cannot perform two aggregations over the same column: {}",
                                                              column_name);
        auto typed_column_name = ColumnName(column_name);
        if (aggregation_operator == "sum") {
            aggregators_.emplace_back(SortedSumAggregator(typed_column_name, typed_column_name, closed_boundary_));
        } else {
            user_input::raise<ErrorCode::E_INVALID_USER_ARGUMENT>("Unknown aggregation operator provided to resample: {}", aggregation_operator);
        }
    }
}

void ResampleClause::set_processing_config(const ProcessingConfig& processing_config) {
    processing_config_ = processing_config;
}

std::vector<std::vector<size_t>> ResampleClause::structure_for_processing(
        std::vector<RangesAndKey>& ranges_and_keys,
        ARCTICDB_UNUSED size_t start_from) {
    if (date_range_.has_value()) {
        bucket_boundaries_ = generate_bucket_boundaries_(date_range_->first, date_range_->second, rule_);
        ranges_and_keys.erase(std::remove_if(ranges_and_keys.begin(), ranges_and_keys.end(),
                                             [this](const RangesAndKey &ranges_and_key) {
                                                 auto [start_index, end_index] = ranges_and_key.key_.time_range();
                                                 // end_index from the key is 1 nanosecond larger than the index value of the last row in the row-slice
                                                 end_index--;
                                                 switch (closed_boundary_) {
                                                     case ResampleBoundary::LEFT:
                                                         return start_index >= bucket_boundaries_.back() ||
                                                                end_index < bucket_boundaries_.front();
                                                     case ResampleBoundary::RIGHT:
                                                     default:
                                                         return start_index > bucket_boundaries_.back() ||
                                                                end_index <= bucket_boundaries_.front();
                                                 }
                                             }), ranges_and_keys.end());
    } else {
        date_range_.emplace(
                std::min_element(ranges_and_keys.begin(), ranges_and_keys.end(),
                                 [](const RangesAndKey& left, const RangesAndKey& right) {
                                     return left.key_.start_time() < right.key_.start_time();
                                 })->key_.start_time(),
                std::max_element(ranges_and_keys.begin(), ranges_and_keys.end(),
                                 [](const RangesAndKey& left, const RangesAndKey& right) {
                                     return left.key_.end_time() < right.key_.end_time();
                                 })->key_.end_time()
                );
        bucket_boundaries_ = generate_bucket_boundaries_(date_range_->first, date_range_->second, rule_);
    }
    auto res = structure_by_row_slice(ranges_and_keys, 0);
    // Element i of res also needs the values from element i+1 if there is a bucket which incorporates the last index
    // value of row-slice i and the first value of row-slice i+1
    // Element i+1 should be removed if the last bucket involved in element i covers all the index values in element i+1
    auto bucket_boundaries_it = std::begin(bucket_boundaries_);
    for (auto it = res.begin(); it != res.end() && it != std::prev(res.end());) {
        auto last_index_value = ranges_and_keys[it->at(0)].key_.end_time() - 1;
        while(bucket_boundaries_it != bucket_boundaries_.end() &&
              ((closed_boundary_ == ResampleBoundary::LEFT && *bucket_boundaries_it <= last_index_value) ||
               (closed_boundary_ == ResampleBoundary::RIGHT && *bucket_boundaries_it < last_index_value))) {
            bucket_boundaries_it++;
        }
        auto next_it = std::next(it);
        while (next_it != res.end()) {
            auto next_start_index_value = ranges_and_keys[next_it->at(0)].key_.start_time();
            // end_index from the key is 1 nanosecond larger than the index value of the last row in the row-slice
            auto next_end_index_value = ranges_and_keys[next_it->at(0)].key_.end_time() - 1;
            if (bucket_boundaries_it != bucket_boundaries_.end() &&
                ((closed_boundary_ == ResampleBoundary::LEFT && *bucket_boundaries_it > next_start_index_value) ||
                 (closed_boundary_ == ResampleBoundary::RIGHT && *bucket_boundaries_it >= next_start_index_value))) {
                it->insert(it->end(), next_it->begin(), next_it->end());
                if (bucket_boundaries_it == std::prev(bucket_boundaries_.end()) ||
                    (closed_boundary_ == ResampleBoundary::LEFT && *bucket_boundaries_it > next_end_index_value) ||
                    (closed_boundary_ == ResampleBoundary::RIGHT && *bucket_boundaries_it >= next_end_index_value)) {
                    next_it = res.erase(next_it);
                } else {
                    break;
                }
            } else {
                break;
            }
        }
        it = next_it;
    }
    return res;
}

Composite<EntityIds> ResampleClause::process(Composite<EntityIds>&& entity_ids) const {
    auto procs = gather_entities(component_manager_, std::move(entity_ids), false, false, true).as_range();
    internal::check<ErrorCode::E_ASSERTION_FAILURE>(procs.size() == 1, "Expected a single ProcessingUnit on entry to ResampleClause::process");
    auto row_slices = split_by_row_slice(std::move(procs[0]));
    // If the expected get calls for the segments in the first row slice are 2, the first bucket overlapping this row
    // slice is being computed by the call to process dealing with the row slices above these. Otherwise, this call
    // should do it
    bool responsible_for_first_overlapping_bucket = row_slices.front().segment_initial_expected_get_calls_->at(0) == 1;
    // Find the iterators into bucket_boundaries_ of the start of the first and the end of the last bucket this call to process is
    // responsible for calculating
    // All segments in a given row slice contain the same index column, so just grab info from the first one
    const auto& index_column_name = row_slices.front().segments_->at(0)->field(0).name();
    const auto& first_row_slice_index_col = row_slices.front().segments_->at(0)->column(0);
    const auto& last_row_slice_index_col = row_slices.back().segments_->at(0)->column(0);
    // Resampling only makes sense for timestamp indexes
    internal::check<ErrorCode::E_ASSERTION_FAILURE>(is_time_type(first_row_slice_index_col.type().data_type()) &&
                                                    is_time_type(last_row_slice_index_col.type().data_type()),
                                                    "Cannot resample data with index column of non-timestamp type");
    auto first_ts = first_row_slice_index_col.scalar_at<timestamp>(0).value();
    // TODO: Explain how this works
    auto last_ts = last_row_slice_index_col.scalar_at<timestamp>(row_slices.size() == 1 ? last_row_slice_index_col.row_count() - 1 : 0).value();
    auto [first_it, last_it] = find_buckets(first_ts, last_ts, responsible_for_first_overlapping_bucket);
    // Construct the output index column and the bucket boundaries this call to process is responsible for
    std::vector<std::shared_ptr<Column>> input_index_columns;
    input_index_columns.reserve(row_slices.size());
    for (const auto& row_slice: row_slices) {
        input_index_columns.emplace_back(row_slice.segments_->at(0)->column_ptr(0));
    }
    // TODO: Use iterators instead of copying range of bucket_boundaries_
    auto [output_index_column, bucket_boundaries] = generate_buckets(input_index_columns, first_it, last_it);
    SegmentInMemory seg;
    seg.set_row_id(output_index_column->row_count() - 1);
    RowRange output_row_range(row_slices.front().row_ranges_->at(0)->start(),
                              row_slices.front().row_ranges_->at(0)->start() + output_index_column->row_count());
    seg.add_column(scalar_field(DataType::NANOSECONDS_UTC64, index_column_name), output_index_column);
    seg.descriptor().set_index(IndexDescriptor(1, IndexDescriptor::TIMESTAMP));
    for (const auto& aggregator: aggregators_) {
        std::vector<std::optional<ColumnWithStrings>> input_agg_columns;
        input_agg_columns.reserve(row_slices.size());
        for (auto& row_slice: row_slices) {
            auto variant_data = row_slice.get(aggregator.get_input_column_name());
            util::variant_match(variant_data,
                                [&input_agg_columns](const ColumnWithStrings& column_with_strings) {
                                    input_agg_columns.emplace_back(column_with_strings);
                                },
                                [&input_agg_columns](const EmptyResult&) {
                                    input_agg_columns.emplace_back();
                                },
                                [](const auto&) {
                                    internal::raise<ErrorCode::E_ASSERTION_FAILURE>("Unexpected return type from ProcessingUnit::get, expected column-like");
                                }
            );
        }
        auto aggregated_column = std::make_shared<Column>(aggregator.aggregate(input_index_columns, input_agg_columns, bucket_boundaries));
        seg.add_column(scalar_field(aggregated_column->type().data_type(), aggregator.get_output_column_name().value), aggregated_column);
    }
    return Composite<EntityIds>(push_entities(component_manager_, ProcessingUnit(std::move(seg), std::move(output_row_range))));
}

[[nodiscard]] std::string ResampleClause::to_string() const {
    return fmt::format("RESAMPLE({}) {}", rule_, aggregation_map_);
}

std::pair<std::vector<timestamp>::const_iterator, std::vector<timestamp>::const_iterator> ResampleClause::find_buckets(
        timestamp first_ts,
        timestamp last_ts,
        bool responsible_for_first_overlapping_bucket) const {
    auto first_it = std::lower_bound(bucket_boundaries_.begin(), bucket_boundaries_.end(), first_ts,
                                     [this](timestamp boundary, timestamp first_ts) {
                                         switch (closed_boundary_) {
                                             case ResampleBoundary::LEFT:
                                                 return boundary <= first_ts;
                                             case ResampleBoundary::RIGHT:
                                             default:
                                                 return boundary < first_ts;
                                         }
    });
    if (responsible_for_first_overlapping_bucket && first_it != bucket_boundaries_.begin()) {
        --first_it;
    }
    auto last_it = std::upper_bound(first_it, bucket_boundaries_.end(), last_ts,
                                    [this](timestamp last_ts, timestamp boundary) {
                                        switch (closed_boundary_) {
                                            case ResampleBoundary::LEFT:
                                                return last_ts < boundary;
                                            case ResampleBoundary::RIGHT:
                                            default:
                                                return last_ts <= boundary;
                                        }
                                    });
    if (last_it == bucket_boundaries_.end()) {
        --last_it;
    }
    return {first_it, last_it};
}

std::pair<std::shared_ptr<Column>, std::vector<timestamp>> ResampleClause::generate_buckets(const std::vector<std::shared_ptr<Column>>& input_index_columns,
                                                                                            const std::vector<timestamp>::const_iterator& first_it,
                                                                                            const std::vector<timestamp>::const_iterator last_it) const {
    auto data_type = DataType::NANOSECONDS_UTC64;
    auto output_index_column = std::make_shared<Column>(TypeDescriptor(data_type, Dimension::Dim0), false);
    ssize_t output_idx{0};
    using IndexTDT = ScalarTagType<DataTypeTag<DataType::NANOSECONDS_UTC64>>;
    auto bucket_start_it = first_it;
    auto bucket_end_it = std::next(bucket_start_it);
    std::vector<timestamp> bucket_boundaries;
    bucket_boundaries.reserve(std::distance(first_it, last_it) + 1);
    // Only include buckets that have at least one index value in range
    for (const auto& input_index_column: input_index_columns) {
        auto data = input_index_column->data();
        auto block = data.template next<IndexTDT>();
        while(block.has_value() && bucket_end_it != std::next(last_it)) {
            const auto row_count = block->row_count();
            auto ptr = reinterpret_cast<const timestamp*>(block->data());
            for (auto i = 0u; i < row_count; ++i, ++ptr) {
                switch (closed_boundary_) {
                    case ResampleBoundary::LEFT:
                        while (bucket_end_it != std::next(last_it) && *ptr >= *bucket_end_it) {
                            ++bucket_start_it;
                            if (++bucket_end_it == std::next(last_it)) {
                                break;
                            }
                        }
                        if (bucket_end_it != std::next(last_it) && *ptr >= *bucket_start_it && *ptr < *bucket_end_it) {
                            output_index_column->set_scalar(output_idx++, label_boundary_ == ResampleBoundary::LEFT ? *bucket_start_it : *bucket_end_it);
                            ++bucket_start_it;
                            if (++bucket_end_it == std::next(last_it)) {
                                break;
                            }
                        }
                        break;
                    case ResampleBoundary::RIGHT:
                    default:
                        while (bucket_end_it != std::next(last_it) && *ptr > *bucket_end_it) {
                            ++bucket_start_it;
                            if (++bucket_end_it == std::next(last_it)) {
                                break;
                            }
                        }
                        if (bucket_end_it != std::next(last_it) && *ptr > *bucket_start_it && *ptr <= *bucket_end_it) {
                            output_index_column->set_scalar(output_idx++, label_boundary_ == ResampleBoundary::LEFT ? *bucket_start_it : *bucket_end_it);
                            ++bucket_start_it;
                            if (++bucket_end_it == std::next(last_it)) {
                                break;
                            }
                        }
                        break;
                }
            }
            block = data.template next<IndexTDT>();
        }
    }

    // Bucket boundaries can be wider than the date range specified by the user, narrow the first and last buckets here if necessary
    bucket_boundaries.emplace_back(std::max(*first_it, date_range_->first - (closed_boundary_ == ResampleBoundary::RIGHT ? 1 : 0)));
    for (auto it = std::next(first_it); it != last_it; it++) {
        bucket_boundaries.emplace_back(*it);
    }
    bucket_boundaries.emplace_back(std::min(*last_it, date_range_->second + (closed_boundary_ == ResampleBoundary::LEFT ? 1 : 0)));
    return {output_index_column, bucket_boundaries};
}

[[nodiscard]] Composite<EntityIds> RemoveColumnPartitioningClause::process(Composite<EntityIds>&& entity_ids) const {
    auto procs = gather_entities(component_manager_, std::move(entity_ids));
    Composite<EntityIds> output;
    procs.broadcast([&output, this](ProcessingUnit &proc) {
        size_t min_start_row = std::numeric_limits<size_t>::max();
        size_t max_end_row = 0;
        size_t min_start_col = std::numeric_limits<size_t>::max();
        size_t max_end_col = 0;
        std::optional<SegmentInMemory> output_seg;
        for (auto&& [idx, segment]: folly::enumerate(proc.segments_.value())) {
            min_start_row = std::min(min_start_row, proc.row_ranges_->at(idx)->start());
            max_end_row = std::max(max_end_row, proc.row_ranges_->at(idx)->end());
            min_start_col = std::min(min_start_col, proc.col_ranges_->at(idx)->start());
            max_end_col = std::max(max_end_col, proc.col_ranges_->at(idx)->end());
            if (output_seg.has_value()) {
                merge_string_columns(*segment, output_seg->string_pool_ptr(), false);
                output_seg->concatenate(std::move(*segment), true);
            } else {
                output_seg = std::make_optional<SegmentInMemory>(std::move(*segment));
            }
        }
        if (output_seg.has_value()) {
            output.push_back(push_entities(component_manager_, ProcessingUnit(std::move(*output_seg),
                                                                RowRange{min_start_row, max_end_row},
                                                                ColRange{min_start_col, max_end_col})));
        }
    });
    return output;
}

Composite<EntityIds> SplitClause::process(Composite<EntityIds>&& entity_ids) const {
    auto procs = gather_entities(component_manager_, std::move(entity_ids));
    Composite<EntityIds> ret;
    procs.broadcast([this, &ret](auto &&p) {
        auto proc = std::forward<decltype(p)>(p);
        for (auto&& [idx, seg]: folly::enumerate(proc.segments_.value())) {
            auto split_segs = seg->split(rows_);
            size_t start_row = proc.row_ranges_->at(idx)->start();
            size_t end_row = 0;
            for (auto&& split_seg : split_segs) {
                end_row = start_row + split_seg.row_count();
                ret.push_back(push_entities(component_manager_, ProcessingUnit(std::move(split_seg),
                                                                 RowRange(start_row, end_row),
                                                                 std::move(*proc.col_ranges_->at(idx)))));
                start_row = end_row;
            }
        }
    });
    return ret;
}

Composite<EntityIds> SortClause::process(Composite<EntityIds>&& entity_ids) const {
    auto procs = gather_entities(component_manager_, std::move(entity_ids));
    Composite<EntityIds> output;
    procs.broadcast([&output, this](auto&& proc) {
        for (auto& seg: proc.segments_.value()) {
            // This modifies the segment in place, which goes against the ECS principle of all entities being immutable
            // Only used by SortMerge right now and so this is fine, although it would not generalise well
            seg->sort(column_);
        }
        output.push_back(push_entities(component_manager_, std::move(proc)));
    });
    return output;
}

template<typename IndexType, typename DensityPolicy, typename QueueType, typename Comparator, typename StreamId>
void merge_impl(
        std::shared_ptr<ComponentManager> component_manager,
        Composite<EntityIds> &ret,
        QueueType &input_streams,
        bool add_symbol_column,
        StreamId stream_id,
        const RowRange& row_range,
        const ColRange& col_range,
        IndexType index,
        const StreamDescriptor& stream_descriptor) {
    auto num_segment_rows = ConfigsMap::instance()->get_int("Merge.SegmentSize", 100000);
    using SegmentationPolicy = stream::RowCountSegmentPolicy;
    SegmentationPolicy segmentation_policy{static_cast<size_t>(num_segment_rows)};

    auto func = [&component_manager, &ret, &row_range, &col_range](auto &&segment) {
        ret.push_back(push_entities(component_manager, ProcessingUnit{std::forward<SegmentInMemory>(segment), row_range, col_range}));
    };

    using AggregatorType = stream::Aggregator<IndexType, stream::DynamicSchema, SegmentationPolicy, DensityPolicy>;
    const auto& fields = stream_descriptor.fields();
    FieldCollection new_fields{};
    (void)new_fields.add(fields[0].ref());

    auto index_desc = index_descriptor(stream_id, index, new_fields);
    auto desc = StreamDescriptor{index_desc};

    AggregatorType agg{
            stream::DynamicSchema{desc, index},
            std::move(func), std::move(segmentation_policy), desc, std::nullopt
    };

    stream::do_merge<IndexType, SegmentWrapper, AggregatorType, decltype(input_streams)>(
        input_streams, agg, add_symbol_column);
}

// MergeClause receives a list of DataFrames as input and merge them into a single one where all 
// the rows are sorted by time stamp
Composite<EntityIds> MergeClause::process(Composite<EntityIds>&& entity_ids) const {
    auto procs = gather_entities(component_manager_, std::move(entity_ids));

    auto compare =
            [](const std::unique_ptr<SegmentWrapper> &left,
               const std::unique_ptr<SegmentWrapper> &right) {
                const auto left_index = index::index_value_from_row(left->row(),
                                                                               IndexDescriptor::TIMESTAMP, 0);
                const auto right_index = index::index_value_from_row(right->row(),
                                                                                IndexDescriptor::TIMESTAMP, 0);
                return left_index > right_index;
            };

    movable_priority_queue<std::unique_ptr<SegmentWrapper>, std::vector<std::unique_ptr<SegmentWrapper>>, decltype(compare)> input_streams{
            compare};

    size_t min_start_row = std::numeric_limits<size_t>::max();
    size_t max_end_row = 0;
    size_t min_start_col = std::numeric_limits<size_t>::max();
    size_t max_end_col = 0;
    procs.broadcast([&input_streams, &min_start_row, &max_end_row, &min_start_col, &max_end_col](auto&& proc) {
        for (auto&& [idx, segment]: folly::enumerate(proc.segments_.value())) {
            size_t start_row = proc.row_ranges_->at(idx)->start();
            min_start_row = start_row < min_start_row ? start_row : min_start_row;
            size_t end_row = proc.row_ranges_->at(idx)->end();
            max_end_row = end_row > max_end_row ? end_row : max_end_row;
            size_t start_col = proc.col_ranges_->at(idx)->start();
            min_start_col = start_col < min_start_col ? start_col : min_start_col;
            size_t end_col = proc.col_ranges_->at(idx)->end();
            max_end_col = end_col > max_end_col ? end_col : max_end_col;
            input_streams.push(std::make_unique<SegmentWrapper>(std::move(*segment)));
        }
    });
    const RowRange row_range{min_start_row, max_end_row};
    const ColRange col_range{min_start_col, max_end_col};
    Composite<EntityIds> ret;
    std::visit(
            [this, &ret, &input_streams, &comp=compare, stream_id=stream_id_, &row_range, &col_range](auto idx, auto density) {
                merge_impl<decltype(idx), decltype(density), decltype(input_streams), decltype(comp), decltype(stream_id)>(component_manager_,
                                                                                                      ret,
                                                                                                      input_streams,
                                                                                                      add_symbol_column_,
                                                                                                      stream_id,
                                                                                                      row_range,
                                                                                                      col_range,
                                                                                                      idx,
                                                                                                      stream_descriptor_);
            }, index_, density_policy_);

    return ret;
}

std::optional<std::vector<Composite<EntityIds>>> MergeClause::repartition(
        std::vector<Composite<EntityIds>> &&comps) const {
    std::vector<Composite<EntityIds>> v;
    v.push_back(merge_composites_shallow(std::move(comps)));
    return v;
}

Composite<EntityIds> ColumnStatsGenerationClause::process(Composite<EntityIds>&& entity_ids) const {
    auto procs = gather_entities(component_manager_, std::move(entity_ids), true, false);
    std::vector<ColumnStatsAggregatorData> aggregators_data;
    internal::check<ErrorCode::E_INVALID_ARGUMENT>(
            static_cast<bool>(column_stats_aggregators_),
            "ColumnStatsGenerationClause::process does not make sense with no aggregators");
    for (const auto &agg : *column_stats_aggregators_){
        aggregators_data.emplace_back(agg.get_aggregator_data());
    }

    robin_hood::unordered_set<IndexValue> start_indexes;
    robin_hood::unordered_set<IndexValue> end_indexes;

    internal::check<ErrorCode::E_INVALID_ARGUMENT>(
            !procs.empty(),
            "ColumnStatsGenerationClause::process does not make sense with no processing units");
    procs.broadcast(
            [&start_indexes, &end_indexes, &aggregators_data, this](auto &proc) {
                for (const auto& key: proc.atom_keys_.value()) {
                    start_indexes.insert(key->start_index());
                    end_indexes.insert(key->end_index());
                }
                for (auto agg_data : folly::enumerate(aggregators_data)) {
                    auto input_column_name = column_stats_aggregators_->at(agg_data.index).get_input_column_name();
                    auto input_column = proc.get(input_column_name);
                    if (std::holds_alternative<ColumnWithStrings>(input_column)) {
                        auto input_column_with_strings = std::get<ColumnWithStrings>(input_column);
                        agg_data->aggregate(input_column_with_strings);
                    } else {
                        if (!processing_config_.dynamic_schema_)
                            internal::raise<ErrorCode::E_ASSERTION_FAILURE>(
                                "Unable to resolve column denoted by aggregation operator: '{}'",
                                input_column_name);
                    }
                }
            });

    internal::check<ErrorCode::E_ASSERTION_FAILURE>(
            start_indexes.size() == 1 && end_indexes.size() == 1,
            "Expected all data segments in one processing unit to have same start and end indexes");
    auto start_index = *start_indexes.begin();
    auto end_index = *end_indexes.begin();
    schema::check<ErrorCode::E_UNSUPPORTED_INDEX_TYPE>(
            std::holds_alternative<NumericIndex>(start_index) && std::holds_alternative<NumericIndex>(end_index),
            "Cannot build column stats over string-indexed symbol"
    );
    auto start_index_col = std::make_shared<Column>(make_scalar_type(DataType::NANOSECONDS_UTC64), true);
    auto end_index_col = std::make_shared<Column>(make_scalar_type(DataType::NANOSECONDS_UTC64), true);
    start_index_col->template push_back<NumericIndex>(std::get<NumericIndex>(start_index));
    end_index_col->template push_back<NumericIndex>(std::get<NumericIndex>(end_index));
    start_index_col->set_row_data(0);
    end_index_col->set_row_data(0);

    SegmentInMemory seg;
    seg.descriptor().set_index(IndexDescriptor(0, IndexDescriptor::ROWCOUNT));
    seg.add_column(scalar_field(DataType::NANOSECONDS_UTC64, start_index_column_name), start_index_col);
    seg.add_column(scalar_field(DataType::NANOSECONDS_UTC64, end_index_column_name), end_index_col);
    for (const auto& agg_data: folly::enumerate(aggregators_data)) {
        seg.concatenate(agg_data->finalize(column_stats_aggregators_->at(agg_data.index).get_output_column_names()));
    }
    seg.set_row_id(0);
    return Composite<EntityIds>(push_entities(component_manager_, ProcessingUnit(std::move(seg))));
}

std::vector<std::vector<size_t>> RowRangeClause::structure_for_processing(
        std::vector<RangesAndKey>& ranges_and_keys,
        ARCTICDB_UNUSED size_t start_from) {
    ranges_and_keys.erase(std::remove_if(ranges_and_keys.begin(), ranges_and_keys.end(), [this](const RangesAndKey& ranges_and_key) {
        return ranges_and_key.row_range_.start() >= end_ || ranges_and_key.row_range_.end() <= start_;
    }), ranges_and_keys.end());
    return structure_by_row_slice(ranges_and_keys, start_from);
}

Composite<EntityIds> RowRangeClause::process(Composite<EntityIds> &&entity_ids) const {
    auto procs = gather_entities(component_manager_, std::move(entity_ids));
    Composite<EntityIds> output;
    procs.broadcast([&output, this](ProcessingUnit &proc) {
        for (auto&& [idx, row_range]: folly::enumerate(proc.row_ranges_.value())) {
            if ((start_ > row_range->start() && start_ < row_range->end()) ||
                (end_ > row_range->start() && end_ < row_range->end())) {
                // Zero-indexed within this slice
                size_t start_row{0};
                size_t end_row{row_range->diff()};
                if (start_ > row_range->start() && start_ < row_range->end()) {
                    start_row = start_ - row_range->start();
                }
                if (end_ > row_range->start() && end_ < row_range->end()) {
                    end_row = end_ - (row_range->start());
                }
                auto truncated_segment = truncate_segment(*proc.segments_->at(idx), start_row, end_row);
                auto num_rows = truncated_segment.is_null() ? 0 : truncated_segment.row_count();
                proc.row_ranges_->at(idx) = std::make_shared<pipelines::RowRange>(proc.row_ranges_->at(idx)->first, proc.row_ranges_->at(idx)->first + num_rows);
                auto num_cols = truncated_segment.is_null() ? 0 : truncated_segment.descriptor().field_count() - truncated_segment.descriptor().index().field_count();
                proc.col_ranges_->at(idx) = std::make_shared<pipelines::ColRange>(proc.col_ranges_->at(idx)->first, proc.col_ranges_->at(idx)->first + num_cols);
                proc.segments_->at(idx) = std::make_shared<SegmentInMemory>(std::move(truncated_segment));
            } // else all rows in this segment are required, do nothing
        }
        output.push_back(push_entities(component_manager_, std::move(proc)));
    });
    return output;
}

void RowRangeClause::set_processing_config(const ProcessingConfig& processing_config) {
    auto total_rows = static_cast<int64_t>(processing_config.total_rows_);
    switch(row_range_type_) {
        case RowRangeType::HEAD:
            if (n_ >= 0) {
                start_ = 0;
                end_ = std::min(n_, total_rows);
            } else {
                start_ = 0;
                end_ = std::max(static_cast<int64_t>(0), total_rows + n_);
            }
            break;
        case RowRangeType::TAIL:
            if (n_ >= 0) {
                start_ = std::max(static_cast<int64_t>(0), total_rows - n_);
                end_ = total_rows;
            } else {
                start_ = std::min(-n_, total_rows);
                end_ = total_rows;
            }
            break;
        case RowRangeType::RANGE:
            // Wrap around negative indices.
            start_ = (
                user_provided_start_ >= 0 ?
                std::min(user_provided_start_, total_rows) :
                std::max(total_rows + user_provided_start_, static_cast<int64_t>(0))
            );
            end_ = (
                user_provided_end_ >= 0 ?
                std::min(user_provided_end_, total_rows) :
                std::max(total_rows + user_provided_end_, static_cast<int64_t>(0))
            );
            break;

        default:
            internal::raise<ErrorCode::E_ASSERTION_FAILURE>("Unrecognised RowRangeType {}", static_cast<uint8_t>(row_range_type_));
    }
}

std::string RowRangeClause::to_string() const {
    if (row_range_type_ == RowRangeType::RANGE) {
        return fmt::format("ROWRANGE: RANGE, start={}, end ={}", start_, end_);
    }

    return fmt::format("ROWRANGE: {}, n={}", row_range_type_ == RowRangeType::HEAD ? "HEAD" : "TAIL", n_);
}

std::vector<std::vector<size_t>> DateRangeClause::structure_for_processing(
        std::vector<RangesAndKey>& ranges_and_keys,
        size_t start_from) {
    ranges_and_keys.erase(std::remove_if(ranges_and_keys.begin(), ranges_and_keys.end(), [this](const RangesAndKey& ranges_and_key) {
        auto [start_index, end_index] = ranges_and_key.key_.time_range();
        return start_index > end_ || end_index <= start_;
    }), ranges_and_keys.end());
    return structure_by_row_slice(ranges_and_keys, start_from);
}

Composite<EntityIds> DateRangeClause::process(Composite<EntityIds> &&entity_ids) const {
    auto procs = gather_entities(component_manager_, std::move(entity_ids), true, false);
    Composite<EntityIds> output;
    procs.broadcast([&output, this](ProcessingUnit &proc) {
        // We are only interested in the index, which is in every SegmentInMemory in proc.segments_, so just use the first
        auto row_range = proc.row_ranges_->at(0);
        auto [start_index, end_index] = proc.atom_keys_->at(0)->time_range();
        if ((start_ > start_index && start_ < end_index) || (end_ >= start_index && end_ < end_index)) {
            size_t start_row{0};
            size_t end_row{row_range->diff()};
            if (start_ > start_index && start_ < end_index) {
                start_row = proc.segments_->at(0)->column_ptr(0)->search_sorted<timestamp>(start_);
            }
            if (end_ >= start_index && end_ < end_index) {
                end_row = proc.segments_->at(0)->column_ptr(0)->search_sorted<timestamp>(end_, true);
            }
            proc.truncate(start_row, end_row);
        } // else all rows in the processing unit are required, do nothing
        output.push_back(push_entities(component_manager_, std::move(proc)));
    });
    return output;
}

std::string DateRangeClause::to_string() const {
    return fmt::format("DATE RANGE {} - {}", start_, end_);
}

}
