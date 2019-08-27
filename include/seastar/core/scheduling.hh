/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2016 Scylla DB Ltd
 */

#pragma once

#include <seastar/core/sstring.hh>
#include <seastar/core/function_traits.hh>

/// \file

namespace seastar {

constexpr unsigned max_scheduling_groups() { return 16; }

template <typename... T>
class future;

class reactor;

class scheduling_group;

namespace internal {

// Returns an index between 0 and max_scheduling_groups()
unsigned scheduling_group_index(scheduling_group sg);
scheduling_group scheduling_group_from_index(unsigned index);

}


/// Creates a scheduling group with a specified number of shares.
///
/// The operation is global and affects all shards. The returned scheduling
/// group can then be used in any shard.
///
/// \param name A name that identifiers the group; will be used as a label
///             in the group's metrics
/// \param shares number of shares of the CPU time allotted to the group;
///              Use numbers in the 1-1000 range (but can go above).
/// \return a scheduling group that can be used on any shard
future<scheduling_group> create_scheduling_group(sstring name, float shares);

/// Destroys a scheduling group.
///
/// Destroys a \ref scheduling_group previously created with create_scheduling_group().
/// The destroyed group must not be currently in use and must not be used later.
///
/// The operation is global and affects all shards.
///
/// \param sg The scheduling group to be destroyed
/// \return a future that is ready when the scheduling group has been torn down
future<> destroy_scheduling_group(scheduling_group sg);

/// Rename scheduling group.
///
/// Renames a \ref scheduling_group previously created with create_scheduling_group().
///
/// The operation is global and affects all shards.
/// The operation affects the exported statistics labels.
///
/// \param sg The scheduling group to be renamed
/// \param new_name The new name for the scheduling group.
/// \return a future that is ready when the scheduling group has been renamed
future<> rename_scheduling_group(scheduling_group sg, sstring new_name);

using scheduling_group_key = unsigned long;

struct scheduling_group_key_config {
    size_t allocation_size;
    size_t alignment;
    std::function<void (void*)> constructor;
    std::function<void (void*)> destructor;
};

template <typename T, typename... ConstructorArgs, std::enable_if_t<std::negation_v<std::is_scalar<T>>, int> = 0 >
scheduling_group_key_config
make_scheduling_group_key_config(ConstructorArgs... args) {
    scheduling_group_key_config sgkc;
    sgkc.allocation_size = sizeof(T);
    sgkc.alignment = alignof(T);
    sgkc.constructor = [args = std::make_tuple(args...)] (void* p) {

         new (p) T(std::make_from_tuple<T>(args));
    };
    sgkc.destructor = [] (void* p) {

        static_cast<T*>(p)->~T();
    };
    return sgkc;
}

template <typename T, std::enable_if_t<std::negation_v<std::is_scalar<T>>, int> = 0 >
scheduling_group_key_config
make_scheduling_group_key_config() {
    scheduling_group_key_config sgkc;
    sgkc.allocation_size = sizeof(T);
    sgkc.alignment = alignof(T);
    sgkc.constructor = [] (void* p) {

         new (p) T();
    };
    sgkc.destructor = [] (void* p) {

        static_cast<T*>(p)->~T();
    };
    return sgkc;
}

template <typename T, typename InitialVal, std::enable_if_t<std::is_scalar_v<T>, int> = 0 >
scheduling_group_key_config
make_scheduling_group_key_config(InitialVal initial_val) {
    scheduling_group_key_config sgkc;
    sgkc.allocation_size = sizeof(T);
    sgkc.alignment = alignof(T);
    sgkc.constructor = [initial_val] (void* p) {
        (T)(*p) = initial_val;
    };
    return sgkc;
}

template <typename T, std::enable_if_t<std::is_scalar_v<T>, int> = 0 >
scheduling_group_key_config
make_scheduling_group_key_config() {
    scheduling_group_key_config sgkc;
    sgkc.allocation_size = sizeof(T);
    sgkc.alignment = alignof(T);
    sgkc.constructor = [] (void* p) {
        memset(p, 0, sizeof(T));
    };
    return sgkc;
}

future<scheduling_group_key> scheduling_group_key_create(scheduling_group_key_config cfg);

template<typename T>
T& scheduling_group_get_specific(scheduling_group sg, scheduling_group_key key);

/// \brief Identifies function calls that are accounted as a group
///
/// A `scheduling_group` is a tag that can be used to mark a function call.
/// Executions of such tagged calls are accounted as a group.
class scheduling_group {
    unsigned _id;
private:
    explicit scheduling_group(unsigned id) : _id(id) {}
public:
    /// Creates a `scheduling_group` object denoting the default group
    constexpr scheduling_group() noexcept : _id(0) {} // must be constexpr for current_scheduling_group_holder
    bool active() const;
    const sstring& name() const;
    bool operator==(scheduling_group x) const { return _id == x._id; }
    bool operator!=(scheduling_group x) const { return _id != x._id; }
    bool is_main() const { return _id == 0; }
    template<typename T>
    T& get_specific(scheduling_group_key key) {
        return scheduling_group_get_specific<T>(*this, key);
    }
    /// Adjusts the number of shares allotted to the group.
    ///
    /// Dynamically adjust the number of shares allotted to the group, increasing or
    /// decreasing the amount of CPU bandwidth it gets. The adjustment is local to
    /// the shard.
    ///
    /// This can be used to reduce a background job's interference with a foreground
    /// load: the shares can be started at a low value, increased when the background
    /// job's backlog increases, and reduced again when the backlog decreases.
    ///
    /// \param shares number of shares allotted to the group. Use numbers
    ///               in the 1-1000 range.
    void set_shares(float shares);
    friend future<scheduling_group> create_scheduling_group(sstring name, float shares);
    friend future<> destroy_scheduling_group(scheduling_group sg);
    friend future<> rename_scheduling_group(scheduling_group sg, sstring new_name);
    friend class reactor;
    friend unsigned internal::scheduling_group_index(scheduling_group sg);
    friend scheduling_group internal::scheduling_group_from_index(unsigned index);
    template<typename SpecificValType, typename Mapper, typename Reducer, typename Initial>
    friend future<typename function_traits<Reducer>::return_type>
        map_reduce_sg_specific(Mapper mapper, Reducer reducer, Initial initial_val, scheduling_group_key key);
    template<typename SpecificValType, typename Reducer, typename Initial>
    friend future<typename function_traits<Reducer>::return_type>
        reduce_sg_specific(Reducer reducer, Initial initial_val, scheduling_group_key key);


};

/// \cond internal
namespace internal {

inline
unsigned
scheduling_group_index(scheduling_group sg) {
    return sg._id;
}

inline
scheduling_group
scheduling_group_from_index(unsigned index) {
    return scheduling_group(index);
}

inline
scheduling_group*
current_scheduling_group_ptr() {
    // Slow unless constructor is constexpr
    static thread_local scheduling_group sg;
    return &sg;
}

}
/// \endcond

/// Returns the current scheduling group
inline
scheduling_group
current_scheduling_group() {
    return *internal::current_scheduling_group_ptr();
}

inline
scheduling_group
default_scheduling_group() {
    return scheduling_group();
}

inline
bool
scheduling_group::active() const {
    return *this == current_scheduling_group();
}

}

namespace std {

template <>
struct hash<seastar::scheduling_group> {
    size_t operator()(seastar::scheduling_group sg) const {
        return seastar::internal::scheduling_group_index(sg);
    }
};

}
