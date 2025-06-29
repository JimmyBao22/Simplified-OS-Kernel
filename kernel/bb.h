#pragma once

#include "queue.h"
#include "semaphore.h"
#include "events.h"

template <typename T>
class BoundedBuffer {

    struct Value {
        Value* next = nullptr;
        T value;

        explicit inline Value(T value) : value(value) {}
    };

    // SpinLock lock;
    Queue<Value, SpinLock> queue{};
    Semaphore* n_full;
    Semaphore* n_empty;
public:
    // construct a BB with a buffer size of n
    BoundedBuffer(uint32_t n) : n_full(new Semaphore(0)), n_empty(new Semaphore(n))  {}
    BoundedBuffer(const BoundedBuffer&) = delete;
    ~BoundedBuffer() {
        queue.clear();
        delete n_full;
        delete n_empty;
    }

    // When room is available in the buffer
    //    - put "v" in the next slot
    //    - schedule a call "work()"
    // Returns immediately
    template <typename Work>
    void put(T v, const Work& work) {
        n_empty->down([this, v, work](){
            Value* val = new Value(v);
            queue.add(val);
            n_full->up();
            work();
        });
    }

    // When the buffer is not empty
    //    - remove the first value "v"
    //    - schedule a call to "work(v)"
    // Returns immediately
    template <typename Work>
    void get(const Work& work) {
        n_full->down([this, work](){
            Value* v = queue.remove();
            n_empty->up();
            T value = v->value;
            delete v;
            work(value);
        });
    }
};