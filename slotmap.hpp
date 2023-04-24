#ifndef SLOTMAP_HPP
#define SLOTMAP_HPP

#include <string.h>
#include <malloc.h>
#include <cstdint>
#include <cassert>
#include <utility>
#include <limits>

#ifndef UNLIKELY
#define UNLIKELY(xpr) (__builtin_expect(!!(xpr), 0))
#endif

#ifndef ASSUME
#define ASSUME(xpr) __builtin_assume(!!(xpr))
#endif

template<typename item_store_t>
class slotmap_iterator_t
{
public:
    using item_type = decltype(item_store_t::item);
    using size_type = decltype(item_store_t::self_key_offset);

    explicit constexpr slotmap_iterator_t(item_store_t* in_ptr)
        : ptr(in_ptr)
    {
    }

    constexpr slotmap_iterator_t& operator+=(size_t offset)
    {
        ptr += offset;
        return *this;
    }

    constexpr slotmap_iterator_t& operator-=(size_t offset)
    {
        ptr -= offset;
        return *this;
    }

    constexpr slotmap_iterator_t operator+(size_t offset)
    {
        return slotmap_iterator_t{ptr + offset};
    }

    constexpr slotmap_iterator_t operator-(size_t offset)
    {
        return slotmap_iterator_t{ptr - offset};
    }

    constexpr size_t operator+(slotmap_iterator_t other)
    {
        return ptr + other.ptr;
    }

    constexpr size_t operator-(slotmap_iterator_t other)
    {
        return ptr - other.ptr;
    }

    constexpr slotmap_iterator_t& operator++()
    {
        ++ptr;
        return *this;
    }

    constexpr slotmap_iterator_t& operator--()
    {
        --ptr;
        return *this;
    }

    constexpr item_type* operator->()
    {
        return &ptr->item;
    }

    constexpr item_type& operator*()
    {
        return ptr->item;
    }

    inline constexpr friend bool operator!=(slotmap_iterator_t lhs, slotmap_iterator_t rhs)
    {
        return lhs.ptr != rhs.ptr;
    }

    item_store_t* item_store()
    {
        return ptr;
    }

    item_type* item()
    {
        return &ptr->item;
    }

    item_store_t* ptr;
};

/*
 * a slotmap is used to safely hold items without clear ownership,
 * items move but the keys do not, ie it is safe to reorder items in the slotmap
 * note that this implementation inserts sizeof(size_type) bytes next to the stored item for bookkeeping
 */
template<typename in_item_type, typename in_size_type = uint32_t> requires(sizeof(in_size_type) >= 2 && std::is_unsigned_v<in_size_type>)
class slotmap_t
{
public:

    using item_type = in_item_type;
    using size_type = in_size_type;

    struct handle_t //handle used to refer to a key and its item
    {
        inline friend bool operator==(handle_t lhs, handle_t rhs)
        {
            return lhs.key_offset == rhs.key_offset && lhs.item_id == rhs.item_id;
        }

        inline friend bool operator!=(handle_t lhs, handle_t rhs)
        {
            return !(lhs == rhs);
        }

        size_type key_offset;
        size_type item_id;
    };

    struct key_t
    {
        union //when free, next_free specifies an offset (from beginning) to the next free key, otherwise, item_offset indexes into a valid item
        {
            size_type item_offset;
            size_type next_free;
        };

        size_type item_id;
    };

    struct item_store_t //each item needs an offset to its own key in order to update on removal
    {
        item_type item;
        size_type self_key_offset;
    };

    static_assert(offsetof(item_store_t, item) == 0);

    using iterator_t = slotmap_iterator_t<item_store_t>;
    using const_iterator_t = slotmap_iterator_t<const item_store_t>;

    inline static constexpr size_type items_per_allocation = 256 * sizeof(size_type);
    inline static constexpr size_type min_free_keys = 8 * sizeof(size_type); //by always having atleast N free keys we can delay the id reset
    inline static constexpr size_t max_items = std::numeric_limits<size_type>::max();

    slotmap_t()
    {
        item_count = 0;
        key_count = items_per_allocation;

        size_t key_bytes = key_count * sizeof(key_t);
        size_t item_bytes = key_count * sizeof(item_store_t);

        auto data = static_cast<uint8_t*>(malloc(key_bytes + item_bytes));

        keys = reinterpret_cast<key_t*>(data);
        items = reinterpret_cast<item_store_t*>(data + key_bytes); //store keys at the start of memory and items after the keys

        ASSUME((key_count % items_per_allocation) == 0);
        for(size_type index = 0; index < key_count; ++index)
        {
            keys[index].item_id = 0;
            keys[index].next_free = index + 1; //points one off the end but thats ok becouse we update it before we get to that point
        }

        freelist_head = 0;
        freelist_tail = key_count - 1;
    }

    ~slotmap_t()
    {
        destroy_items();
        free(keys);
    }

    template<typename... Ts>
    handle_t add(Ts&&... args)
    {
        if UNLIKELY(item_count == (key_count - min_free_keys)) //item count is always going to be less than key count
        {
            expand_allocation();
        }

        key_t& key = keys[freelist_head];

        handle_t handle;
        handle.item_id = key.item_id;
        handle.key_offset = freelist_head;

        freelist_head = key.next_free;

        key.item_offset = item_count;
        item_count += 1;

        item_store_t& new_item = items[key.item_offset];
        new_item.self_key_offset = handle.key_offset;

        new(&new_item.item) item_type{std::forward<Ts>(args)...};

        return handle;
    }

    bool remove(handle_t handle) //reurns if removal was actually done
    {
        if(handle.key_offset >= key_count)
        {
            return false;
        }

        key_t& key = keys[handle.key_offset];

        if(handle.item_id != key.item_id)
        {
            return false;
        }

        item_count -= 1;
        item_store_t& last_item = items[item_count];
        key_t& last_item_key = keys[last_item.self_key_offset];

        last_item_key.item_offset = key.item_offset;

        if constexpr(!std::is_trivially_destructible_v<item_type>)
        {
            if UNLIKELY(item_count == 0) //prevent self assignment
            {
                items[0].item.item_type::~item_type();
            }
            else
            {
                items[key.item_offset] = std::move(last_item);
            }
        }
        else
        {
            items[key.item_offset] = std::move(last_item);
        }

        key_t& tail_key = keys[freelist_tail]; //set old tail to point to the new tail
        tail_key.next_free = handle.key_offset;
        freelist_tail = handle.key_offset;

        key.item_id += 1; //invalidate handles to this key... might wrap around
        return true;
    }

    void clear(bool shrink = false) //if shrink is true, reallocates to items_per_allocation
    {
        destroy_items();

        if(shrink)
        {
            shrink_allocation();
        }

        ASSUME((key_count % items_per_allocation) == 0);
        for(size_type index = 0; index < key_count; ++index)
        {
            keys[index].item_id += 1; //we are unnecesairly bumping id,s here but otherwise we would need to check for all free keys
            keys[index].next_free = index + 1;
        }

        freelist_head = 0;
        freelist_tail = key_count - 1;
    }

    bool is_valid_handle(handle_t handle) const
    {
        if(handle.key_offset >= key_count)
        {
            return false;
        }

        if(handle.item_id != keys[handle.key_offset].item_id)
        {
            return false;
        }

        return true;
    }

    handle_t get_handle(size_type index)
    {
        assert(index <= size());

        item_store_t& stored_item = items[index];
        key_t key = keys[stored_item.self_key_offset];
        return handle_t{stored_item.self_key_offset, key.item_id};
    }

    handle_t get_handle(item_type* item) //unsafe function, may segmentation fault or return undefined handle if the item is already freed
    {
        auto stored_item = reinterpret_cast<item_store_t*>(item);
        assert(stored_item >= items && stored_item < (items + item_count));

        size_type offset = stored_item->self_key_offset;
        size_type item_id = keys[offset].item_id;

        return handle_t{offset, item_id};
    }

    key_t* get_key(handle_t handle)
    {
        if(handle.key_offset >= key_count) //invalid offset
        {
            return nullptr;
        }

        key_t& key = keys[handle.key_offset];
        if(handle.item_id != key.item_id)
        {
            return nullptr;
        }

        return &key;
    }

    key_t get_key(item_type* item) //unsafe function, may segmentation fault or return undefined key if the item is already freed
    {
        auto stored_item = reinterpret_cast<item_store_t*>(item);
        assert(stored_item >= items && stored_item < (items + item_count));

        return keys[stored_item->self_key_offset];
    }

    void swap_positions(iterator_t first, iterator_t second) //swaps the position of two items, keeping handles valid
    {
        key_t& first_key = keys[first.item_store()->self_key_offset];
        key_t& second_key = keys[second.item_store()->self_key_offset];

        std::swap(*first.item_store(), *second.item_store());
        std::swap(first_key.item_offset, second_key.item_offset);
    }

    handle_t insert(const item_type& item, iterator_t at)
    {
        key_t& key = keys[at.item_store()->self_key_offset];
        key.item_id += 1;

        *at.item() = item;

        return handle_t{key.item_id, at.item_store()->self_key_offset};
    }

    handle_t insert(item_type&& item, iterator_t at)
    {
        key_t& key = keys[at.item_store()->self_key_offset];
        key.item_id += 1;

        *at.item() = std::move(item);

        return handle_t{key.item_id, at.item_store()->self_key_offset};
    }

    item_type* operator[](handle_t handle)
    {
        if(handle.key_offset >= key_count) //invalid offset
        {
            return nullptr;
        }

        key_t& key = keys[handle.key_offset];
        if(handle.item_id != key.item_id)
        {
            return nullptr;
        }

        item_store_t& stored_item = items[key.item_offset];
        return &stored_item.item;
    }

    const item_type* operator[](handle_t handle) const
    {
        return operator[](handle);
    }

    item_type& operator[](key_t key) //unsafe function, key is assumed to be valid
    {
        item_store_t& stored_item = items[key.item_offset];
        return stored_item.item;
    }

    const item_type& operator[](key_t key) const//unsafe function, key is assumed to be valid
    {
        return operator[](key);
    }

    item_type& operator[](size_type index)
    {
        assert(index < size());

        item_store_t& stored_item = items[index];
        return stored_item.item;
    }

    const item_type& operator[](size_type index) const
    {
        return operator[](index);
    }

    size_type size() const
    {
        return item_count;
    }

    size_t max_size() const
    {
        return max_items;
    }

    iterator_t begin()
    {
        return iterator_t{items};
    }

    iterator_t end()
    {
        return iterator_t{items + item_count};
    }

    const_iterator_t begin() const
    {
        return const_iterator_t{items};
    }

    const_iterator_t end() const
    {
        return const_iterator_t{items + item_count};
    }

//privates not intended for general use

    void expand_allocation()
    {
        assert((size_t(key_count) + size_t(items_per_allocation)) <= size_t(max_items));

        size_type old_key_count = key_count;
        key_count += items_per_allocation;

        size_t old_key_bytes = old_key_count * sizeof(key_t);
        size_t key_bytes = key_count * sizeof(key_t);
        size_t item_bytes = key_count * sizeof(item_store_t);

        auto data = static_cast<uint8_t*>(realloc(keys, key_bytes + item_bytes));

        memcpy(data + key_bytes, data + old_key_bytes, item_count * sizeof(item_store_t)); //push back items

        keys = reinterpret_cast<key_t*>(data);
        items = reinterpret_cast<item_store_t*>(data + key_bytes); //store keys at the start of memory and items after the keys

        ASSUME((key_count % items_per_allocation) == 0);
        for(size_type index = old_key_count; index < key_count; ++index)
        {
            keys[index].item_id = 0;
            keys[index].next_free = index + 1;
        }

        key_t& tail_key = keys[freelist_tail]; //set old tail to point to the first new key
        tail_key.next_free = old_key_count;
        freelist_tail = key_count - 1; //new tail is the last added key
    }

    void shrink_allocation()
    {
        if(key_count == items_per_allocation)
        {
            return; //already smallest possible
        }

        key_count = items_per_allocation;

        size_t key_bytes = key_count * sizeof(key_t);
        size_t item_bytes = key_count * sizeof(item_store_t);

        auto data = static_cast<uint8_t*>(realloc(keys, key_bytes + item_bytes));

        keys = reinterpret_cast<key_t*>(data);
        items = reinterpret_cast<item_store_t*>(data + key_bytes); //store keys at the start of memory and items after the keys
    }

    void destroy_items()
    {
        if constexpr(!std::is_trivially_destructible_v<item_type>)
        {
            while(item_count != 0)
            {
                --item_count;
                items[item_count].item.item_type::~item_type();
            }
        }
        else
        {
            item_count = 0;
        }
    }

    item_store_t* items;
    key_t* keys; //owns the allocation

    size_type item_count; //number of active items
    size_type key_count; //number of keys, including free ones

    size_type freelist_head; //first free key offset FIFO implementation
    size_type freelist_tail; //last free key offset
};

#endif //SLOTMAP_HPP
