// Copyright (c) 2006-2013, Andrey N. Sabelnikov, www.sabelnikov.net
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
// * Neither the name of the Andrey N. Sabelnikov nor the
// names of its contributors may be used to endorse or promote products
// derived from this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER  BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 




#ifndef __WINH_OBJ_H__
#define __WINH_OBJ_H__

#include <algorithm>
#include <boost/chrono/duration.hpp>
#include <boost/functional/hash/hash.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/detail/thread.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <boost/thread/thread.hpp>
#include <cstdint>
#include <queue>
#include <set>
#include <utility>
#include <functional>
#include <vector>
#include "misc_log_ex.h"
#include "misc_language.h"

namespace epee
{

  namespace debug
  {
    inline unsigned int &g_test_dbg_lock_sleep()
    {
      static unsigned int value = 0;
      return value;
    }
  }
  
  struct simple_event
  {
    simple_event() : m_rised(false)
    {
    }

    void raise()
    {
      boost::unique_lock<boost::mutex> lock(m_mx);
      m_rised = true;
      m_cond_var.notify_one();
    }

    void wait()
    {
      boost::unique_lock<boost::mutex> lock(m_mx);
      while (!m_rised) 
        m_cond_var.wait(lock);
      m_rised = false;
    }

  private:
    boost::mutex m_mx;
    boost::condition_variable m_cond_var;
    bool m_rised;
  };

  class critical_region;

  class critical_section
  {
    boost::recursive_mutex m_section;

  public:
    //to make copy fake!
    critical_section(const critical_section& section)
    {
    }

    critical_section()
    {
    }

    ~critical_section()
    {
    }

    void lock()
    {
      m_section.lock();
      //EnterCriticalSection( &m_section );
    }

    void unlock()
    {
      m_section.unlock();
    }

    bool tryLock()
    {
      return m_section.try_lock();
    }

    // to make copy fake
    critical_section& operator=(const critical_section& section)
    {
      return *this;
    }
  };


  template<class t_lock>
  class critical_region_t
  {
    t_lock&	m_locker;
    bool m_unlocked;

    critical_region_t(const critical_region_t&) {}

  public:
    critical_region_t(t_lock& cs): m_locker(cs), m_unlocked(false)
    {
      m_locker.lock();
    }

    ~critical_region_t()
    {
      unlock();
    }

    void unlock()
    {
      if (!m_unlocked)
      {
        m_locker.unlock();
        m_unlocked = true;
      }
    }
  };


  struct reader_writer_lock {

    reader_writer_lock() noexcept :
    wait_queue(),
    readers() {};

    bool start_read() noexcept {
      if (have_write() || have_read()) {
        return false;
      }
      lock_reader();
      return true;
    }

    void end_read() noexcept {
      unlock_reader();
    }

    bool start_write() noexcept {
      if (have_write()) {
        return false;
      }
      lock_and_change(boost::this_thread::get_id());
      return true;
    }

    void end_write() noexcept {
      unlock_writer();
    }

    bool have_write() noexcept {
      boost::mutex::scoped_lock lock(internal_mutex);
      return have_write(lock);
    }

    bool have_read() noexcept {
      boost::mutex::scoped_lock lock(internal_mutex);
      return have_read(lock);
    }

    ~reader_writer_lock() = default;
    reader_writer_lock(reader_writer_lock&) = delete;
    reader_writer_lock operator=(reader_writer_lock&) = delete;

  private:

    enum reader_writer_kind {
      WRITER = 0,
      READER = 1
    };  

    bool have_read(boost::mutex::scoped_lock& lock) noexcept {
      return readers.find(boost::this_thread::get_id()) != readers.end();
    }

    bool have_write(boost::mutex::scoped_lock& lock) noexcept {
      if (writer_id == boost::this_thread::get_id()) {
        return true;
      }
      return false;
    }

    //  internal_mutex should be locked when calling this.
    void wake_up() {
      if (!wait_queue.size()) { 
        return;
      }

      std::vector<std::reference_wrapper<boost::condition_variable>> wake_ups{};
      while (wait_queue.size() && wait_queue.front().first == reader_writer_kind::READER) { // readers
        auto thread_to_wake_up = wait_queue.front(); wait_queue.pop();
        wake_ups.push_back(thread_to_wake_up.second);
      }

      if (!wake_ups.size()) { // writer
        CHECK_AND_ASSERT_MES2(std::get<0>(wait_queue.front()) == reader_writer_kind::WRITER, " Waking up reader thread inside writer wake ups");
        auto thread_to_wake_up = wait_queue.front(); wait_queue.pop();
        wake_ups.push_back(thread_to_wake_up.second);
      }

      std::for_each(begin(wake_ups), end(wake_ups), 
      [&] (std::reference_wrapper<boost::condition_variable> wait_condition_ref) {
        wait_condition_ref.get().notify_one();
      });
    }

    void unlock_writer() noexcept {
      boost::mutex::scoped_lock lock(internal_mutex);      
      CHECK_AND_ASSERT_MES2(!readers.size(), "Ending write transaction by " << boost::this_thread::get_id() << " while the are no  is not in write mode");
      rw_mutex.unlock();
      writer_id = boost::thread::id();
      wake_up();      
    }

    void unlock_reader() noexcept {
      boost::mutex::scoped_lock lock(internal_mutex);      
      CHECK_AND_ASSERT_MES2(readers.size(), "Ending read transaction by " << boost::this_thread::get_id() << " while the lock is not in read mode");
      rw_mutex.unlock_shared();
      readers.erase(boost::this_thread::get_id());
      if (!readers.size()) {
        wake_up();
      }      
    }

    void wait_in_queue(reader_writer_kind kind, boost::mutex::scoped_lock& lock) {
      boost::condition_variable wait_condition;
      wait_queue.push(std::make_pair(kind, std::reference_wrapper(wait_condition)));
      wait_condition.wait(lock);
    }

    void entrance(reader_writer_kind kind) {
      boost::mutex::scoped_lock lock(internal_mutex);
      if (wait_queue.size()) { 
        wait_in_queue(kind, lock);
      }      
    }

    void lock_reader() noexcept {
      entrance(reader_writer_kind::READER);
      do {  
        boost::mutex::scoped_lock lock(internal_mutex);
        if (!rw_mutex.try_lock_shared()) {
          wait_in_queue(reader_writer_kind::READER, lock);
          continue;
        }
        readers.insert(boost::this_thread::get_id());
        return; 
      } while(true);
    }

    void lock_and_change(boost::thread::id new_owner) noexcept {
      entrance(reader_writer_kind::WRITER);
      do {  
        boost::mutex::scoped_lock lock(internal_mutex);
        if (!rw_mutex.try_lock()) {
          wait_in_queue(reader_writer_kind::WRITER, lock);
          continue;
        }
        writer_id = new_owner;
        return; 
      } while(true);
    }

    boost::mutex internal_mutex; // keep the data in RWLock consistent.
    boost::shared_mutex rw_mutex;
    std::set<boost::thread::id> readers;    
    boost::thread::id writer_id;
    typedef std::pair<reader_writer_kind, std::reference_wrapper<boost::condition_variable>> waiting_thread;
    std::queue<waiting_thread> wait_queue;
  };

#define RWLOCK(lock)                                                         \
  bool rw_release_required##lock = lock.start_write();                       \
  epee::misc_utils::auto_scope_leave_caller scope_exit_handler##lock =       \
      epee::misc_utils::create_scope_leave_handler([&]() {                   \
        if (rw_release_required##lock)                                       \
          lock.end_write();                                                  \
      });                                                                    


#define RLOCK(lock)                                                          \
    bool r_release_required##lock = lock.start_read();                       \
    epee::misc_utils::auto_scope_leave_caller scope_exit_handler##lock =     \
        epee::misc_utils::create_scope_leave_handler([&]() {                 \
          if (r_release_required##lock)                                      \
            lock.end_read();                                                 \
        });


#define  CRITICAL_REGION_LOCAL(x) {} epee::critical_region_t<decltype(x)>   critical_region_var(x)
#define  CRITICAL_REGION_BEGIN(x) { boost::this_thread::sleep_for(boost::chrono::milliseconds(epee::debug::g_test_dbg_lock_sleep())); epee::critical_region_t<decltype(x)>   critical_region_var(x)
#define  CRITICAL_REGION_LOCAL1(x) {boost::this_thread::sleep_for(boost::chrono::milliseconds(epee::debug::g_test_dbg_lock_sleep()));} epee::critical_region_t<decltype(x)>   critical_region_var1(x)
#define  CRITICAL_REGION_BEGIN1(x) {  boost::this_thread::sleep_for(boost::chrono::milliseconds(epee::debug::g_test_dbg_lock_sleep())); epee::critical_region_t<decltype(x)>   critical_region_var1(x)

#define  CRITICAL_REGION_END() }

}

#endif
