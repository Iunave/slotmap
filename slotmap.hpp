#ifndef SLOTMAP_HPP
#define SLOTMAP_HPP

#include <cstdint>
#include <type_traits>
#include <utility>
#include <iterator>

#ifndef SLOTMAP_ASSERT
#include <cassert>
#define SLOTMAP_ASSERT(expr, ...) assert((expr) __VA_OPT__(&& __VA_ARGS__))
#endif

struct SlotMapDefaultTraits
{
    static constexpr int64_t IndexBits = 40;
    static constexpr int64_t IdBits = 64 - IndexBits;
    static constexpr int64_t MinFreeKeys = 32;
    static constexpr int64_t AllocationSize = 512; //how many items will fit in a memory allocation
};

/**
 * @description A SlotMap is used to store items without clear ownership.
 * Accessing an item is done trough a handle which stores an index to a key and an expected identifier
 * if the identifiers differ the handle is considered to be invalid, otherwise the address of the item is retrieved trough the key.
 * When an item is removed its corresponding key updates its ID - thus invalidating all existing handles to that key.
 * @warning given enough time an ID will reach its maximum and wrap around which could create false positives when accessing that item
 * currently this is caught by SLOTMAP_ASSERT
 */
template<typename ItemT, typename Traits = SlotMapDefaultTraits>
class SlotMap
{
public:
    static_assert(Traits::IndexBits > 0);
    static_assert(Traits::IdBits > 0);
    static_assert(Traits::MinFreeKeys >= 0);
    static_assert(Traits::AllocationSize > 0);
    static_assert(Traits::IndexBits + Traits::IdBits <= 64);

    using KeyOffsetT = std::conditional_t<Traits::IndexBits <= 32, uint32_t, uint64_t>;

    static constexpr uint64_t IndexMax = UINT64_MAX >> (64 - Traits::IndexBits);
    static constexpr uint64_t IdMax = UINT64_MAX >> (64 - Traits::IdBits); //in the worst case: we will have to remove the same item (IdMax - 1) * Traits::MinFreeKeys times to get an id reset

    struct ItemKey
    {
        //when free, specifies an offset to an item, otherwise to the next free key
        uint64_t Index : Traits::IndexBits = 0;

        //id of the item pointed to by Index, 0 is an invalid ID in order to properly represent a null handle
        uint64_t ID : Traits::IdBits = 0;
    };

    struct KeyHandle
    {
        //offset to an ItemKey
        uint64_t Index : Traits::IndexBits = 0;

        //id of the item in ItemKey
        uint64_t ID : Traits::IdBits = 0;
    };

    static constexpr KeyHandle NullHandle{.Index = 0, .ID = 0};

private: //member variables

    ItemKey* Keys;
    KeyOffsetT* KeyOffsets; //offset values "owned" by the corresponding item at the same index that is used the get the key of an item
    ItemT* Items;

    int64_t KeyCount; //number of keys including free keys, is the same as the number of allocated keys

    uint64_t FreelistHead; //first free key offset FIFO implementation
    uint64_t FreelistTail; //last free key offset

    int64_t ItemCount;
    int64_t AllocatedItemCount;

public:

    SlotMap(const SlotMap&) = delete;
    SlotMap(SlotMap&&) = delete;

    SlotMap()
        : Keys(nullptr)
        , KeyOffsets(nullptr)
        , Items(nullptr)
        , KeyCount(0)
        , FreelistHead(0)
        , FreelistTail(0)
        , ItemCount(0)
        , AllocatedItemCount(0)
    {
    }

    ~SlotMap()
    {
        if constexpr(!std::is_trivially_destructible_v<ItemT>)
        {
            while(ItemCount != 0)
            {
                --ItemCount;
                Items[ItemCount].ItemT::~ItemT();
            }
        }

        if(Keys)
        {
            free(Keys);
        }

        if(Items)
        {
            free(KeyOffsets);
            free(Items);
        }
    }

    bool IsValidHandle(KeyHandle Handle) const
    {
        return Handle.ID != 0 && Handle.Index < KeyCount && Handle.ID == Keys[Handle.Index].ID;
    }

    template<typename... Ts>
    KeyHandle Add(Ts... Args)
    {
        SLOTMAP_ASSERT(ItemCount < IndexMax, "reached max index. consider increasing IndexBits");

        [[unlikely]] if(ItemCount >= (KeyCount - Traits::MinFreeKeys)) //item count will always be <= key count
        {
            ResizeKeys((KeyCount + Traits::AllocationSize) & ~(Traits::AllocationSize - 1)); //round to next multiple of Traits::AllocationSize
        }

        [[unlikely]] if(ItemCount == AllocatedItemCount)
        {
            ResizeItems((ItemCount + Traits::AllocationSize) & ~(Traits::AllocationSize - 1)); //round to next multiple of Traits::AllocationSize
        }

        uint64_t KeyIndex = FreelistHead; //pick the free head key
        ItemKey& Key = Keys[KeyIndex];

        FreelistHead = Key.Index;

        Key.Index = ItemCount;
        ItemCount += 1;

        KeyOffsets[Key.Index] = KeyIndex;
        new(Items + Key.Index) ItemT{std::forward<Ts>(Args)...};

        return KeyHandle{.Index = KeyIndex, .ID = Key.ID};
    }

    //returns false if the handle was invalid, true otherwise
    bool Remove(KeyHandle Handle)
    {
        ItemKey* Key = GetKey(Handle);
        if(Key == nullptr)
        {
            return false;
        }

        Remove(Key);
        return true;
    }

    void Remove(uint64_t Index)
    {
        Remove(GetKey(Index));
    }

    void Remove(ItemT* Item)
    {
        Remove(GetKey(Item));
    }

    void Clear()
    {
        while(ItemCount != 0)
        {
            Remove(ItemCount - 1);
        }
    }

    KeyHandle GetHandle(int64_t Index) const
    {
        SLOTMAP_ASSERT((Index >= 0) & (Index < ItemCount));

        uint64_t KeyIndex = KeyOffsets[Index];
        uint64_t KeyID = Keys[KeyIndex].ID;

        return KeyHandle{.Index = KeyIndex, .ID = KeyID};
    }

    KeyHandle GetHandle(ItemT* Item) const
    {
        return GetHandle(std::distance(Items, Item));
    }

    template<typename Self>
    decltype(auto) operator[](this Self&& self, KeyHandle Handle)
    {
        if(auto* Key = self.GetKey(Handle))
        {
            return self.Items + Key->Index;
        }

        return (decltype(self.Items))nullptr;
    }

    template<typename Self>
    decltype(auto) operator[](this Self&& self, int64_t Index)
    {
        SLOTMAP_ASSERT((Index >= 0) & (Index < self.ItemCount));
        return self.Items[Index];
    }

    template<typename Self>
    decltype(auto) begin(this Self&& self)
    {
        return self.Items;
    }

    template<typename Self>
    decltype(auto) end(this Self&& self)
    {
        return self.Items + self.ItemCount;
    }

    int64_t Size() const
    {
        return ItemCount;
    }

    int64_t SizeBytes() const
    {
        return ItemCount * sizeof(ItemT);
    }

private:

    //key has to be a pointer to a key in Keys, no copies
    void Remove(ItemKey* Key) __attribute_nonnull__((2))
    {
        SLOTMAP_ASSERT(Key->ID < IdMax, "reached max id. consider increasing IdBits and/or MinFreeKeys");

        Key->ID += 1; //invalidate handles to this key.
        ItemCount -= 1;

        ItemKey& LastKey = Keys[KeyOffsets[ItemCount]];

        [[unlikely]] if(Key->Index == LastKey.Index) //prevent self assignment
        {
            Items[Key->Index].ItemT::~ItemT();
        }
        else //move last item and its key offset to the removed item
        {
            KeyOffsets[Key->Index] = KeyOffsets[ItemCount];
            Items[Key->Index] = std::move(Items[ItemCount]);
        }

        LastKey.Index = Key->Index;

        ItemKey& TailKey = Keys[FreelistTail]; //set old tail to point to the new tail (this key)
        TailKey.Index = std::distance(Keys, Key);
        FreelistTail = TailKey.Index;

        [[unlikely]] if(AllocatedItemCount >= (ItemCount + Traits::AllocationSize * 2)) //test if its worth to shrink items
        {
            ResizeItems((ItemCount + Traits::AllocationSize - 1) & ~(Traits::AllocationSize - 1)); //resize items to first multiple of Traits::AllocationSize
        }
    }

    void ResizeItems(int64_t Count)
    {
        SLOTMAP_ASSERT(Count >= ItemCount, "shrinking allocation below alive items is not allowed");

        if(Count != AllocatedItemCount)
        {
            AllocatedItemCount = Count;

            KeyOffsets = static_cast<KeyOffsetT*>(realloc(KeyOffsets, AllocatedItemCount * sizeof(KeyOffsetT)));

            if constexpr(std::is_trivially_copyable_v<ItemT>)
            {
                Items = static_cast<ItemT*>(realloc(Items, AllocatedItemCount * sizeof(ItemT)));
            }
            else
            {
                auto* NewItems = static_cast<ItemT*>(malloc(AllocatedItemCount * sizeof(ItemT)));

                for(int64_t Index = 0; Index < ItemCount; ++Index)
                {
                    new(NewItems + Index) ItemT{std::move(Items[Index])};
                }

                if(Items != nullptr)
                {
                    free(Items);
                }

                Items = NewItems;
            }
        }
    }

    void ResizeKeys(int64_t Count)
    {
        SLOTMAP_ASSERT(Count >= KeyCount, "shrinking key allocation is not allowed");

        if(Count != KeyCount)
        {
            int64_t OldKeyCount = KeyCount;
            KeyCount = Count;

            Keys = static_cast<ItemKey*>(realloc(Keys, KeyCount * sizeof(ItemKey)));

            for(int64_t idx = OldKeyCount; idx < KeyCount; ++idx) //initialize new keys
            {
                Keys[idx].Index = idx + 1; //points one off the end but that's ok because we update it before we get to that point
                Keys[idx].ID = 1;
            }

            if(FreelistTail != FreelistHead)
            {
                Keys[FreelistTail].Index = OldKeyCount; //set old tail to point to the first new key
            }

            FreelistTail = KeyCount - 1; //new tail is the last added key
        }
    }

    template<typename Self>
    decltype(auto) GetKey(this Self&& self, KeyHandle Handle)
    {
        if(self.IsValidHandle(Handle))
        {
            return self.Keys + Handle.Index;
        }

        return (decltype(self.Keys))nullptr;
    }

    template<typename Self>
    decltype(auto) GetKey(this Self&& self, int64_t ItemIndex)
    {
        SLOTMAP_ASSERT((ItemIndex >= 0) & (ItemIndex < self.ItemCount));
        return self.Keys + self.KeyOffsets[ItemIndex];
    }

    template<typename Self>
    decltype(auto) GetKey(this Self&& self, ItemT* Item)
    {
        return self.GetKey(std::distance(self.Items, Item));
    }
};

#endif //SLOTMAP_HPP
