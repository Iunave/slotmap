#ifndef SLOTMAP_HPP
#define SLOTMAP_HPP

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

#ifndef PACKED
#define PACKED __attribute__((packed))
#endif

template<typename item_store_t>
class slotmap_iterator_t
{
public:
    using item_type = decltype(item_store_t::item);

    explicit constexpr slotmap_iterator_t(item_store_t* in_ptr)
        : ptr(in_ptr)
    {
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

private:

    item_store_t* ptr;
};

template<typename size_type>
struct slotmap_key_t
{
    union //when free, next_free specifies an offset (from beginning) to the next free key, otherwise, item_offset indexes into a valid item
    {
        size_type item_offset;
        size_type next_free;
    };

    size_type item_id;
};

/*
 * a slotmap is used to safely hold items without clear ownership,
 * items move but the keys do not, ie it is safe to reorder items in the slotmap
 * note that this implementation inserts sizeof(size_type) bytes next to the stored item for bookkeeping
 */
template<typename item_type, typename size_type = uint32_t, size_type items_per_allocation = 1024>
requires(std::is_integral_v<size_type> && ((items_per_allocation % 2) == 0))
class slotmap_t
{
public:

    struct handle_t //handle used to refer to a key and its item
    {
        size_type key_offset;
        size_type item_id;
    };

    struct PACKED item_store_t //each item needs an offset to its own key in order to update on removal
    {
        item_type item;
        size_type self_key_offset;
    };

    using iterator_t = slotmap_iterator_t<item_store_t>;
    using const_iterator_t = slotmap_iterator_t<const item_store_t>;

    using key_t = slotmap_key_t<size_type>;

    inline static constexpr size_t max_items = std::numeric_limits<size_type>::max();
    inline static constexpr size_t invalid_id = std::numeric_limits<size_type>::max();
    static_assert(max_items >= items_per_allocation);

    inline static constexpr size_type min_free_keys = 32; //by always having atleast N free keys we can delay the id reset, 32 seems like a good compromise
    static_assert(min_free_keys < items_per_allocation);

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
        if constexpr(!std::is_trivially_destructible_v<item_type>)
        {
            for(iterator_t it = begin(); it != end(); ++it)
            {
                it->item_type::~item_type();
            }
        }

        free(keys);
    }

    template<typename... Ts>
    handle_t add(Ts&&... args)
    {
        if UNLIKELY(item_count == (key_count - min_free_keys)) //item count is always going to be less than key count
        {
            assert((size_t(key_count) + size_t(items_per_allocation)) <= max_items);

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

    void remove(handle_t handle)
    {
        assert(handle.key_offset < key_count); //key_count never decreases so this should never happen

        key_t& key = keys[handle.key_offset];

        if(handle.item_id != key.item_id)
        {
            return;
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
    }

    item_type* operator[](handle_t handle)
    {
        assert(handle.key_offset < key_count); //key_count never decreases so this should never happen

        key_t& key = keys[handle.key_offset];

        if(handle.item_id != key.item_id)
        {
            return nullptr;
        }

        item_store_t& stored_item = items[key.item_offset];
        return &stored_item.item;
    }

    item_type& operator[](uint32_t index)
    {
        assert(index < size());

        item_store_t& stored_item = items[index];
        return stored_item.item;
    }

    void clear()
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

        ASSUME((key_count % items_per_allocation) == 0);
        for(size_type index = 0; index < key_count; ++index)
        {
            keys[index].item_id += 1; //we are unnecesairly bumping id,s here but otherwise we would need to check for all free keys
            keys[index].next_free = index + 1;
        }

        freelist_head = 0;
        freelist_tail = key_count - 1;
    }

    size_type size() const
    {
        return item_count;
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

private:

    item_store_t* items;
    key_t* keys; //owns the allocation

    size_type item_count; //number of active items
    size_type key_count; //number of keys, including free ones

    size_type freelist_head; //first free key offset FIFO implementation
    size_type freelist_tail; //last free key offset
};

#endif //SLOTMAP_HPP
