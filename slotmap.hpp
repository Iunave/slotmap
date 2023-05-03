#ifndef SLOTMAP_HPP
#define SLOTMAP_HPP

#include <climits>
#include <bit>
#include <cstring>
#include <malloc.h>
#include <cstdint>
#include <cassert>
#include <utility>
#include <limits>
#include <iterator>

#ifndef UNLIKELY
#define UNLIKELY(xpr) (__builtin_expect(!!(xpr), 0))
#endif

template<typename item_t>
class slotmap_iterator_t
{
public:
    explicit constexpr slotmap_iterator_t(item_t* in_ptr)
        : ptr(in_ptr)
    {
    }

    inline constexpr friend bool operator!=(slotmap_iterator_t lhs, slotmap_iterator_t rhs)
    {
        return lhs.ptr != rhs.ptr;
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

    constexpr item_t* operator->()
    {
        return ptr;
    }

    constexpr item_t& operator*()
    {
        return *ptr;
    }

    item_t* to_ptr()
    {
        return ptr;
    }

    item_t* ptr;
};

/*
 * a slotmap is used to safely hold items without clear ownership,
 * items move but the keys do not, ie it is safe to reorder items in the slotmap
 */
template<typename item_type>
class slotmap_t
{
public:

    static constexpr size_t index_bits = 40;
    static constexpr size_t id_bits = 64 - index_bits;
    static constexpr size_t index_max = ~0ul >> (64 - index_bits);
    static constexpr size_t id_max = ~0ul >> (64 - id_bits);

    struct slotmap_key_t
    {
        inline constexpr friend bool operator==(slotmap_key_t lhs, slotmap_key_t rhs)
        {
            return std::bit_cast<uint64_t>(lhs) == std::bit_cast<uint64_t>(rhs);
        }

        inline constexpr friend bool operator!=(slotmap_key_t lhs, slotmap_key_t rhs)
        {
            return std::bit_cast<uint64_t>(lhs) != std::bit_cast<uint64_t>(rhs);
        }

        uint64_t index : index_bits; //when free, specifies an offset to an item, otherwise to the next free key
        uint64_t id : id_bits; //id of an item
    };

    struct slotmap_handle_t //handle used to refer to a key and its item
    {
        inline constexpr friend bool operator==(slotmap_handle_t lhs, slotmap_handle_t rhs)
        {
            return std::bit_cast<uint64_t>(lhs) == std::bit_cast<uint64_t>(rhs);
        }

        inline constexpr friend bool operator!=(slotmap_handle_t lhs, slotmap_handle_t rhs)
        {
            return std::bit_cast<uint64_t>(lhs) != std::bit_cast<uint64_t>(rhs);
        }

        uint64_t key_value() const {return key;}
        uint64_t id_value() const {return id;}

        uint64_t key : index_bits; //offset to a key
        uint64_t id : id_bits; //id of the item
    };

    using item_t = item_type;
    using key_t = slotmap_key_t;
    using handle_t = slotmap_handle_t;

    using iterator_t = slotmap_iterator_t<item_t>;
    using const_iterator_t = slotmap_iterator_t<const item_t>;

    inline static constexpr size_t default_allocation_count = 1024;
    inline static constexpr size_t min_free_keys = 32; //we will have to free the same key <id_max * min_free_keys> times to get an id reset (536,870,880)

    struct offset_t
    {
        offset_t(uint64_t offset)
        {
            set(offset);
        }

        uint64_t get() const
        {
            uint64_t offset = 0;
            memcpy(&offset, data, 5);
            return offset;
        }

        void set(uint64_t offset)
        {
            memcpy(data, &offset, 5);
        }

    private:
        uint8_t data[5];
    };
    static_assert(sizeof(offset_t) * CHAR_BIT == index_bits);

    slotmap_t()
    {
        item_count = 0;
        key_count = default_allocation_count;

        uint64_t key_bytes = key_count * sizeof(key_t);
        uint64_t offset_bytes = pad_offsets_size(key_count * sizeof(offset_t));
        uint64_t item_bytes = key_count * sizeof(item_t);

        auto data = static_cast<uint8_t*>(malloc(key_bytes + offset_bytes + item_bytes));

        keys = reinterpret_cast<key_t*>(data);
        owners = reinterpret_cast<offset_t*>(data + key_bytes); //store the owners after the keys
        items = reinterpret_cast<item_t *>(data + key_bytes + offset_bytes); //store the items after the owners

        for(uint64_t index = 0; index < key_count; ++index)
        {
            keys[index].index = index + 1; //points one off the end but thats ok becouse we update it before we get to that point
            keys[index].id = 0;
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
            expand(default_allocation_count);
        }

        uint64_t key_index = freelist_head;
        key_t& key = keys[key_index];

        freelist_head = key.index;

        key.index = item_count;
        item_count += 1;

        new(owners + key.index) offset_t{key_index};
        new(items + key.index) item_t{std::forward<Ts>(args)...};

        handle_t handle;
        handle.id = key.id;
        handle.key = key_index;

        return handle;
    }

    void remove(key_t* key) //key HAS to be a pointer to one of our keys, no copies
    {
        assert(key >= keys && key < keys + key_count);

        key->id += 1; //invalidate handles to this key... might wrap around
        item_count -= 1;

        item_t& last_item = items[item_count];
        key_t& last_key = keys[owners[item_count].get()]; //key to the last item

        last_key.index = key->index;

        if(key->index == last_key.index) //prevent self assignment
        {
            items[key->index].item_t::~item_t();
        }
        else
        {
            owners[key->index] = owners[item_count]; //move last item and its owner to the removed one
            items[key->index] = std::move(last_item);
        }

        key_t& tail_key = keys[freelist_tail]; //set old tail to point to the new tail
        tail_key.index = std::distance(keys, key);
        freelist_tail = tail_key.index;
    }

    void remove(uint64_t index)
    {
        remove(get_key(index));
    }

    void remove(item_t* item)
    {
        remove(get_key(item));
    }

    bool remove(handle_t handle) //reurns if removal was actually done
    {
        key_t* key = get_key(handle);
        if(key == nullptr)
        {
            return false;
        }

        remove(key);
        return true;
    }

    void clear()
    {
        while(item_count != 0)
        {
            remove(keys + owners[item_count - 1].get());
        }
    }

    void expand(uint64_t count)
    {
        uint64_t old_key_count = key_count;
        key_count += count;

        uint64_t old_key_bytes = old_key_count * sizeof(key_t);
        uint64_t key_bytes = key_count * sizeof(key_t);
        uint64_t old_offset_bytes = pad_offsets_size(old_key_count * sizeof(offset_t));
        uint64_t offset_bytes = pad_offsets_size(key_count * sizeof(offset_t));
        uint64_t item_bytes = key_count * sizeof(item_t);

        auto data = static_cast<uint8_t*>(realloc(keys, key_bytes + offset_bytes + item_bytes));

        keys = reinterpret_cast<key_t*>(data);
        owners = reinterpret_cast<offset_t*>(data + key_bytes);
        items = reinterpret_cast<item_t*>(data + key_bytes + offset_bytes);

        memcpy(items, data + old_key_bytes + old_offset_bytes, sizeof(item_t) * item_count); //push items
        memcpy(owners, data + old_key_bytes, sizeof(offset_t) * item_count); //push offsets

        for(uint64_t index = old_key_count; index < key_count; ++index) //initialize new keys
        {
            keys[index].index = index + 1;
            keys[index].id = 0;
        }

        key_t& tail_key = keys[freelist_tail]; //set old tail to point to the first new key
        tail_key.index = old_key_count;
        freelist_tail = key_count - 1; //new tail is the last added key
    }

    bool is_valid_handle(handle_t handle) const
    {
        return handle.key < key_count && handle.id == keys[handle.key].id;
    }

    uint64_t get_index(const item_t* item) const
    {
        assert(item >= items && item < item + item_count);
        return std::distance((const item_t*)items, item);
    }

    uint64_t get_index(handle_t handle)
    {
        const item_t* item = operator[](handle);
        if(item == nullptr)
        {
            return index_max;
        }

        return get_index(item);
    }

    key_t* get_key(uint64_t index)
    {
        assert(index < item_count);
        return keys + owners[index].get();
    }

    const key_t* get_key(uint64_t index) const
    {
        assert(index < item_count);
        return keys + owners[index].get();
    }

    key_t* get_key(const item_t* item)
    {
        return get_key(get_index(item));
    }

    const key_t* get_key(const item_t* item) const
    {
        return get_key(get_index(item));
    }

    key_t* get_key(handle_t handle)
    {
        if(handle.key >= key_count || handle.id != keys[handle.key].id)
        {
            return nullptr;
        }

        return keys + handle.key;
    }

    const key_t* get_key(handle_t handle) const
    {
        return get_key(handle);
    }

    handle_t get_handle(uint64_t index) const
    {
        assert(index < item_count);

        handle_t handle;
        handle.key = owners[index].get();
        handle.id = keys[handle.key].id;
        return handle;
    }

    handle_t get_handle(const item_t* item) const
    {
        return get_handle(get_index(item));
    }

    handle_t insert(const item_type& item, uint64_t at)
    {
        assert(at < item_count);

        key_t& key = keys[owners[at].get()];
        key.id += 1;

        items[at] = item;

        handle_t handle;
        handle.key = owners[at];
        handle.id = key.id;
        return handle;
    }

    handle_t insert(item_type&& item, uint64_t at)
    {
        assert(at < item_count);

        key_t& key = keys[owners[at].get()];
        key.id += 1;

        items[at] = std::move(item);

        handle_t handle;
        handle.key = owners[at];
        handle.id = key.id;
        return handle;
    }

    item_type* operator[](handle_t handle)
    {
        const key_t* key = get_key(handle);
        if(key == nullptr)
        {
            return nullptr;
        }

        return items + key->index;
    }

    const item_type* operator[](handle_t handle) const
    {
        return operator[](handle);
    }

    item_type& operator[](key_t key) //unsafe function, key is assumed to be valid
    {
        return items + key.index;
    }

    const item_type& operator[](key_t key) const//unsafe function, key is assumed to be valid
    {
        return operator[](key);
    }

    item_type& operator[](uint64_t index)
    {
        assert(index < item_count);
        return items[index];
    }

    const item_type& operator[](uint64_t index) const
    {
        return operator[](index);
    }

    uint64_t size() const
    {
        return item_count;
    }

    uint64_t available_size() const
    {
        return key_count - item_count - min_free_keys;
    }

    uint64_t max_size() const
    {
        return index_max;
    }

    item_t* data()
    {
        return items;
    }

    const item_t* data() const
    {
        return items;
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

    uint64_t pad_offsets_size(uint64_t size)
    {
        constexpr uint64_t min_alignment = alignof(item_t);
        return (size + min_alignment - 1) & ~(min_alignment - 1);
    }

    void destroy_items()
    {
        if constexpr(!std::is_trivially_destructible_v<item_t>)
        {
            while(item_count != 0)
            {
                --item_count;
                items[item_count].item_t::~item_t();
            }
        }
        else
        {
            item_count = 0;
        }
    }

    key_t* keys; //owns the allocation
    offset_t* owners; //used to reference the owning key of an item, same count and order as items
    item_t* items;

    uint64_t item_count; //number of active items
    uint64_t key_count; //number of keys, including free ones

    uint64_t freelist_head; //first free key offset FIFO implementation
    uint64_t freelist_tail; //last free key offset
};

#endif //SLOTMAP_HPP
