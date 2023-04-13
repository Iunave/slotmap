#ifndef SLOTMAP_HPP
#define SLOTMAP_HPP

#include <malloc.h>
#include <cstdint>
#include <cassert>
#include <utility>

#ifndef UNLIKELY
#define UNLIKELY(xpr) (__builtin_expect(!!(xpr), 0))
#endif

template<typename T>
class slotmap_t;

template<typename T>
class slotmap_iterator_t
{
public:
    using item_store_t = typename slotmap_t<T>::item_store_t;

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

    constexpr T* operator->()
    {
        return &ptr->item;
    }

    constexpr T& operator*()
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

struct slotmap_key_t
{
    union //when free, next_free specifies an offset (from beginning) to the next free key, otherwise, item_offset indexes into a valid item
    {
        uint32_t item_offset;
        uint32_t next_free;
    };

    uint32_t item_id;
};

/*
 * a slotmap is used to safely hold items without clear ownership,
 * items move but the keys do not, ie it is safe to reorder items in the slotmap
 * note that this implementation inserts 4 bytes next to the stored item for bookkeeping
 */
template<typename T>
class slotmap_t
{
public:

    using iterator_t = slotmap_iterator_t<T>;
    using const_iterator_t = slotmap_iterator_t<const T>;

    inline static constexpr uint32_t items_per_allocation = 1024;

    struct handle_t //handle used to refer to a key and its item
    {
        uint32_t key_offset;
        uint32_t item_id;
    };

    struct __attribute__((packed)) item_store_t //each item needs an offset to its own key in order to update on removal
    {
        T item;
        uint32_t self_key_offset;
    };

    slotmap_t()
    {
        item_count = 0;
        key_count = items_per_allocation;

        items = (item_store_t*)malloc(key_count * sizeof(item_store_t));
        keys = (slotmap_key_t*)malloc(key_count * sizeof(slotmap_key_t));

        for(uint32_t index = 0; index < key_count; ++index)
        {
            keys[index].item_id = 0;
            keys[index].next_free = index + 1; //points one off the end but thats ok becouse we update it before we get to that point
        }

        freelist_head = 0;
        freelist_tail = key_count - 1;
    }

    ~slotmap_t()
    {
        for(iterator_t it = begin(); it != end(); ++it)
        {
            it->T::~T();
        }

        free(items);
        free(keys);
    }

    template<typename... Ts>
    handle_t add(Ts&&... args)
    {
        if UNLIKELY(item_count == (key_count - 2)) //item count is always going to be less than key count and we always need atleast 2 free keys
        {
            uint32_t old_key_count = key_count;
            key_count += items_per_allocation;

            items = (item_store_t*)realloc(items, key_count * sizeof(item_store_t));
            keys = (slotmap_key_t*)realloc(keys, key_count * sizeof(slotmap_key_t));

            for(uint32_t index = old_key_count; index < key_count; ++index)
            {
                keys[index].item_id = 0;
                keys[index].next_free = index + 1;
            }

            slotmap_key_t& tail_key = keys[freelist_tail]; //set old tail to point to the new tail
            tail_key.next_free = old_key_count;
            freelist_tail = old_key_count;
        }

        slotmap_key_t& key = keys[freelist_head];

        handle_t handle;
        handle.item_id = key.item_id;
        handle.key_offset = freelist_head;

        freelist_head = key.next_free;

        key.item_offset = item_count;
        item_count += 1;

        item_store_t& new_item = items[key.item_offset];
        new_item.self_key_offset = handle.key_offset;

        new(&new_item.item) T{std::forward<Ts>(args)...};

        return handle;
    }

    void remove(handle_t handle)
    {
        assert(handle.key_offset < key_count); //key_count never decreases so this should never happen

        slotmap_key_t& key = keys[handle.key_offset];

        if(handle.item_id != key.item_id)
        {
            return;
        }

        item_count -= 1;
        item_store_t& last_item = items[item_count];
        slotmap_key_t& last_item_key = keys[last_item.self_key_offset];

        last_item_key.item_offset = key.item_offset;

        if UNLIKELY(item_count == 0) //prevent self assignment
        {
            items[0].item.T::~T();
        }
        else
        {
            items[key.item_offset] = std::move(last_item);
        }

        slotmap_key_t& tail_key = keys[freelist_tail]; //set old tail to point to the new tail
        tail_key.next_free = handle.key_offset;
        freelist_tail = handle.key_offset;

        key.item_id += 1; //invalidate handles to this key
        assert(key.item_id != UINT32_MAX); //prefer to be notified if the id resets but this should be a warning...
    }

    T* operator[](handle_t handle)
    {
        assert(handle.key_offset < key_count); //key_count never decreases so this should never happen

        slotmap_key_t& key = keys[handle.key_offset];

        if(handle.item_id != key.item_id)
        {
            return nullptr;
        }

        item_store_t& stored_item = items[key.item_offset];
        return &stored_item.item;
    }

    T& operator[](uint32_t index)
    {
        assert(index < size());

        item_store_t& stored_item = items[index];
        return stored_item.item;
    }

    void clear()
    {
        while(item_count != 0)
        {
            --item_count;
            items[item_count].item.T::~T();
        }

        for(uint32_t index = 0; index < key_count; ++index)
        {
            keys[index].item_id = UINT32_MAX; //hopefully no handle has this id >w<
            keys[index].next_free = index + 1;
        }

        freelist_head = 0;
        freelist_tail = key_count - 1;
    }

    uint32_t size() const
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
    slotmap_key_t* keys;

    uint32_t item_count; //number of active items
    uint32_t key_count; //number of keys, including free ones

    uint32_t freelist_head; //first free key offset FIFO implementation
    uint32_t freelist_tail; //last free key offset
};

template<typename T>
using slothandle_t = typename slotmap_t<T>::handle_t;

#endif //SLOTMAP_HPP
