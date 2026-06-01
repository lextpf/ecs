// ============================================================================
// quiver.cppm — EXPERIMENTAL named-module wrapper around quiver.hpp.
//
// The header remains the product; this file re-exports it as `import quiver;`
// for module-based builds. Pattern: the header is included in the global
// module fragment (so every entity stays attached to the global module and
// header-including TUs remain ODR-compatible with importing TUs), then the
// public surface is re-exported by name.
//
// Build (CMake >= 3.28): configure with -DQUIVER_MODULE=ON. If your toolchain
// rejects it, use the header — they are the same library.
// ============================================================================
module;

#include <quiver.hpp>

export module quiver;

export namespace quiver
{
// core
using quiver::checks_enabled;
using quiver::entity;
using quiver::no_entity;
using quiver::world;
// compile-time toolkit
using quiver::constant;
using quiver::contains_type;
using quiver::contains_value;
using quiver::distinct;
using quiver::distinct_t;
using quiver::for_each_type;
using quiver::index_of;
using quiver::index_of_value;
using quiver::joined;
using quiver::joined_t;
using quiver::mapped;
using quiver::mapped_t;
using quiver::type_at;
using quiver::type_at_t;
using quiver::types;
using quiver::value_at;
using quiver::values;
using quiver::without;
using quiver::without_t;
// component model + storage seam
using quiver::basic_pool;
using quiver::chunk_capacity;
using quiver::component;
using quiver::component_label;
using quiver::component_pool;
using quiver::packed_pool_of;
using quiver::pool_of;
using quiver::pool_of_t;
using quiver::stable_pool_of;
using quiver::storage;
using quiver::storage_policy;
using quiver::tag_pool_of;
// identity + diagnostics
using quiver::fault;
using quiver::fault_code;
using quiver::hash_of;
using quiver::name_of;
using quiver::set_violation_handler;
using quiver::violation_handler;
// queries
using quiver::bonded_view;
using quiver::except;
using quiver::maybe;
using quiver::selection;
using quiver::selection_t;
// runtime queries + inspection
using quiver::memory_footprint;
using quiver::pool_info;
using quiver::pool_ref;
using quiver::runtime_selection;
// deferred work + spawn tooling
using quiver::apply_result;
using quiver::blueprint;
using quiver::command_buffer;
using quiver::duplicate_result;
// hooks + trackers
using quiver::component_hook;
using quiver::hook_token;
using quiver::scoped_hook;
using quiver::track;
using quiver::tracker;
using quiver::operator|;
using quiver::operator&;
// archives
using quiver::archive_reader;
using quiver::archive_reader_for;
using quiver::archive_writer;
using quiver::archive_writer_for;
using quiver::graft;
using quiver::graft_map;
using quiver::pack;
using quiver::relink_traits;
using quiver::unpack;
// refs, globals, pipeline
using quiver::basic_entity_ref;
using quiver::const_entity_ref;
using quiver::entity_ref;
using quiver::globals_mark;
using quiver::pipeline;
// MSVC (as of 17.14) does not surface the header's std::hash<entity>
// specialization from the global module fragment to importers; use
// quiver::entity_hash with unordered containers in module builds.
using quiver::entity_hash;
}  // namespace quiver
