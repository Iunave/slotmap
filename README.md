# slotmap
yet another slotmap implementation

This slotmap implementation is as simple as possible while keeping a FIFO free-list and a packed item storage layout.

memory is laid out with keys first, then offsets and lastly items

items have to be address independent (keep this in mind as there are no checks at compile time for this)
