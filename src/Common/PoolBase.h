#pragma once

#include <mutex>
#include <condition_variable>
#include <Poco/Timespan.h>
#include <boost/noncopyable.hpp>

#include <Common/logger_useful.h>
#include <Common/Exception.h>

/// proton: starts
#include <Common/Stopwatch.h>
/// proton: ends

namespace DB
{
    namespace ErrorCodes
    {
        extern const int LOGICAL_ERROR;
    }
}

/** A class from which you can inherit and get a pool of something. Used for database connection pools.
  * Descendant class must provide a method for creating a new object to place in the pool.
  */

template <typename TObject>
class PoolBase : private boost::noncopyable
{
public:
    using Object = TObject;
    using ObjectPtr = std::shared_ptr<Object>;
    using Ptr = std::shared_ptr<PoolBase<TObject>>;

private:

    /** The object with the flag, whether it is currently used. */
    struct PooledObject
    {
        PooledObject(ObjectPtr object_, PoolBase & pool_)
            : object(object_), pool(pool_)
        {
        }

        ObjectPtr object;
        bool in_use = false;
        std::atomic<bool> is_expired = false;
        PoolBase & pool;
    };

    using Objects = std::vector<std::shared_ptr<PooledObject>>;

    /** The helper, which sets the flag for using the object, and in the destructor - removes,
      *  and also notifies the event using condvar.
      */
    struct PoolEntryHelper
    {
        explicit PoolEntryHelper(PooledObject & data_) : data(data_) { data.in_use = true; }
        ~PoolEntryHelper()
        {
            std::lock_guard lock(data.pool.mutex);
            data.in_use = false;
            data.pool.available.notify_one();
        }

        PooledObject & data;
    };

public:
    /** What is given to the user. */
    class Entry
    {
    public:
        friend class PoolBase<Object>;

        Entry() = default;    /// For deferred initialization.

        /** The `Entry` object protects the resource from being used by another thread.
          * The following methods are forbidden for `rvalue`, so you can not write a similar to
          *
          * auto q = pool.get()->query("SELECT .."); // Oops, after this line Entry was destroyed
          * q.execute (); // Someone else can use this Connection
          */
        Object * operator->() && = delete;
        const Object * operator->() const && = delete;
        Object & operator*() && = delete;
        const Object & operator*() const && = delete;

        Object * operator->() &             { return &*data->data.object; }
        const Object * operator->() const & { return &*data->data.object; }
        Object & operator*() &              { return *data->data.object; }
        const Object & operator*() const &  { return *data->data.object; }

        /**
         * Expire an object to make it reallocated later.
         */
        void expire()
        {
            data->data.is_expired = true;
        }

        bool isNull() const { return data == nullptr; }

        PoolBase * getPool() const
        {
            if (!data)
                throw DB::Exception("Attempt to get pool from uninitialized entry", DB::ErrorCodes::LOGICAL_ERROR);
            return &data->data.pool;
        }

    private:
        std::shared_ptr<PoolEntryHelper> data;

        explicit Entry(PooledObject & object) : data(std::make_shared<PoolEntryHelper>(object)) {}
    };

    virtual ~PoolBase() = default;

    /** Allocates the object. Wait for free object in pool for 'timeout'. With 'timeout' < 0, the timeout is infinite. */
    Entry get(Poco::Timespan::TimeDiff timeout)
    {
        std::unique_lock lock(mutex);

        while (true)
        {
            for (auto & item : items)
            {
                if (!item->in_use)
                {
                    if (likely(!item->is_expired))
                    {
                        return Entry(*item);
                    }
                    else
                    {
                        expireObject(item->object);
                        item->object = allocObject();
                        item->is_expired = false;
                        return Entry(*item);
                    }
                }
            }
            if (items.size() < max_items)
            {
                ObjectPtr object = allocObject();
                items.emplace_back(std::make_shared<PooledObject>(object, *this));
                return Entry(*items.back());
            }

            LOG_INFO(log, "No free connections in pool. Waiting.");

            if (timeout < 0)
                available.wait(lock);
            else
                available.wait_for(lock, std::chrono::microseconds(timeout));
        }
    }

    void reserve(size_t count)
    {
        std::lock_guard lock(mutex);

        while (items.size() < count)
            items.emplace_back(std::make_shared<PooledObject>(allocObject(), *this));
    }

    inline size_t size()
    {
        std::lock_guard lock(mutex);
        return items.size();
    }

    /// proton: starts
    void waitForNoMoreInUse()
    {
        std::unique_lock lock(mutex);
        while (true)
        {
            bool all_freed = true;
            for (const auto & item : items)
            {
                if (item->in_use)
                {
                    all_freed = false;
                    break;
                }
            }
            if (all_freed)
                return;

            available.wait(lock);
        }
    }
    /// proton: ends

private:
    /** The maximum size of the pool. */
    unsigned max_items;

    /** Lock to access the pool. */
    std::mutex mutex;
    std::condition_variable available;

protected:

    /** Pool. */
    Objects items; /// proton: updated

    Poco::Logger * log;

    PoolBase(unsigned max_items_, Poco::Logger * log_)
       : max_items(max_items_), log(log_)
    {
        items.reserve(max_items);
    }

    /** Creates a new object to put into the pool. */
    virtual ObjectPtr allocObject() = 0;
    virtual void expireObject(ObjectPtr) {}
};
