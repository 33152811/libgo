#include "io_wait.h"
#include <sys/epoll.h>
#include "scheduler.h"

namespace co
{

enum class EpollType
{
    read,
    write,
};

const char* EpollTypeName(int type)
{
    if (type == (int)EpollType::read)
        return "read";
    else if (type == (int)EpollType::write)
        return "write";
    else
        return "unkown";
}

IoWait::IoWait()
{
    loop_index_ = 0;
    epollwait_ms_ = 0;
    epoll_fds_[0] = epoll_fds_[1] = -1;
    epoll_event_size_ = 1024;
    epoll_owner_pid_ = 0;
}

void IoWait::DelayEventWaitTime()
{
    ++epollwait_ms_;
    epollwait_ms_ = std::min<int>(epollwait_ms_, g_Scheduler.GetOptions().max_sleep_ms);
}

void IoWait::ResetEventWaitTime()
{
    epollwait_ms_ = 0;
}

void IoWait::CoSwitch(std::vector<FdStruct> && fdsts, int timeout_ms)
{
    Task* tk = g_Scheduler.GetCurrentTask();
    if (!tk) return ;

    uint32_t id = ++tk->GetIoWaitData().io_block_id_;
    tk->state_ = TaskState::io_block;
    tk->GetIoWaitData().wait_successful_ = 0;
    tk->GetIoWaitData().io_block_timeout_ = timeout_ms;
    tk->GetIoWaitData().io_block_timer_.reset();
    tk->GetIoWaitData().wait_fds_.swap(fdsts);
    for (auto &fdst : tk->GetIoWaitData().wait_fds_) {
        fdst.epoll_ptr.tk = tk;
        fdst.epoll_ptr.io_block_id = id;
    }

    DebugPrint(dbg_ioblock, "task(%s) CoSwitch id=%d, nfds=%d, timeout=%d",
            tk->DebugInfo(), id, (int)fdsts.size(), timeout_ms);
    g_Scheduler.CoYield();
}

void IoWait::SchedulerSwitch(Task* tk)
{
    bool ok = false;
    std::unique_lock<LFLock> lock(tk->GetIoWaitData().io_block_lock_, std::defer_lock);
    if (tk->GetIoWaitData().wait_fds_.size() > 1)
        lock.lock();

    // id一定要先取出来, 因为在下面的for中, 有可能在另一个线程epoll_wait成功,
    // 并且重新进入一次syscall, 导致id变化.
    uint32_t id = tk->GetIoWaitData().io_block_id_;

    RefGuard<> ref_guard(tk);
    wait_tasks_.push(tk);
    std::vector<std::pair<int, uint32_t>> rollback_list;
    for (auto &fdst : tk->GetIoWaitData().wait_fds_)
    {
        epoll_event ev = {fdst.event | EPOLLONESHOT, {(void*)&fdst.epoll_ptr}};
        int epoll_fd = ChooseEpoll(fdst.event);
        tk->IncrementRef();     // 先将引用计数加一, 以防另一个线程立刻epoll_wait成功被执行完线程.
        if (-1 == epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fdst.fd, &ev)) {
            tk->DecrementRef(); // 添加失败时, 回退刚刚增加的引用计数.
            if (errno == EEXIST) {
                DebugPrint(dbg_ioblock, "task(%s) add fd(%d) into epoll(%s) error %d:%s",
                        tk->DebugInfo(), fdst.fd, EpollTypeName(GetEpollType(epoll_fd)), errno, strerror(errno));
                // 某个fd添加失败, 回滚
                for (auto fd_pair : rollback_list)
                {
                    int epoll_fd = ChooseEpoll(fd_pair.second);
                    if (0 == epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd_pair.first, NULL)) {
                        DebugPrint(dbg_ioblock, "task(%s) rollback io_block. fd=%d from epoll(%s)",
                                tk->DebugInfo(), fd_pair.first, EpollTypeName(GetEpollType(epoll_fd)));
                        // 减引用计数的条件：谁成功从epoll中删除了一个fd，谁才能减引用计数。
                        tk->DecrementRef();
                    }
                }
                ok = false;
                break;
            }
            // 其他原因添加失败, 忽略即可.(模拟poll逻辑)
            continue;
        } else {
            DebugPrint(dbg_ioblock, "task(%s) add fd(%d) into epoll(%s) success",
                    tk->DebugInfo(), fdst.fd, EpollTypeName(GetEpollType(epoll_fd)));
        }


        ok = true;
        rollback_list.push_back(std::make_pair(fdst.fd, fdst.event));
        DebugPrint(dbg_ioblock, "task(%s) io_block. fd=%d, ev=%d",
                tk->DebugInfo(), fdst.fd, fdst.event);
    }

    DebugPrint(dbg_ioblock, "task(%s) SchedulerSwitch id=%d, nfds=%d, timeout=%d, ok=%s",
            tk->DebugInfo(), id, (int)tk->GetIoWaitData().wait_fds_.size(), tk->GetIoWaitData().io_block_timeout_,
            ok ? "true" : "false");

    if (!ok) {
        if (wait_tasks_.erase(tk)) {
            g_Scheduler.AddTaskRunnable(tk);
        }
    }
    else if (tk->GetIoWaitData().io_block_timeout_ != -1) {
        // set timer.
        tk->IncrementRef();
        uint64_t task_id = tk->id_;
        auto timer_id = timer_mgr_.ExpireAt(std::chrono::milliseconds(tk->GetIoWaitData().io_block_timeout_),
                [=]{ 
                    DebugPrint(dbg_ioblock, "task(%d) syscall timeout", (int)task_id);
                    this->Cancel(tk, id);
                    tk->DecrementRef();
                });
        tk->GetIoWaitData().io_block_timer_ = timer_id;
    }
}

void IoWait::Cancel(Task *tk, uint32_t id)
{
    DebugPrint(dbg_ioblock, "task(%s) Cancel id=%d, tk->GetIoWaitData().io_block_id_=%d",
            tk->DebugInfo(), id, (int)tk->GetIoWaitData().io_block_id_);

    if (tk->GetIoWaitData().io_block_id_ != id)
        return ;

    if (wait_tasks_.erase(tk)) { // sync between timer and epoll_wait.
        DebugPrint(dbg_ioblock, "task(%s) io_block wakeup. id=%d", tk->DebugInfo(), id);

        std::unique_lock<LFLock> lock(tk->GetIoWaitData().io_block_lock_, std::defer_lock);
        if (tk->GetIoWaitData().wait_fds_.size() > 1)
            lock.lock();

        // 清理所有fd
        for (auto &fdst: tk->GetIoWaitData().wait_fds_)
        {
            int epoll_fd = ChooseEpoll(fdst.event);
            if (0 == epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fdst.fd, NULL)) {   // sync 1
                DebugPrint(dbg_ioblock, "task(%s) io_block clear fd=%d from epoll(%s)",
                        tk->DebugInfo(), fdst.fd, EpollTypeName(GetEpollType(epoll_fd)));
                // 减引用计数的条件：谁成功从epoll中删除了一个fd，谁才能减引用计数。
                tk->DecrementRef(); // epoll use ref.
            }
        }

        g_Scheduler.AddTaskRunnable(tk);
    }
}

int IoWait::WaitLoop(bool enable_block)
{
    int c = 0;
    for (;;) {
        std::list<CoTimerPtr> timers;
        timer_mgr_.GetExpired(timers, 128);
        if (timers.empty())
            break;

        c += timers.size();
        // 此处暂存callback而不是Task*，是为了block_cancel能够真实有效。
        std::unique_lock<LFLock> lock(timeout_list_lock_);
        timeout_list_.merge(std::move(timers));
    }

    std::unique_lock<LFLock> lock(epoll_lock_, std::defer_lock);
    if (!lock.try_lock())
        return c ? c : -1;

    ++loop_index_;
    int epoll_n = 0;
    if (IsEpollCreated())
    {
        static epoll_event *evs = new epoll_event[epoll_event_size_];
        for (int epoll_type = 0; epoll_type < 2; ++epoll_type)
        {
retry:
            int timeout = (enable_block && epoll_type == (int)EpollType::read && !c) ? epollwait_ms_ : 0;
            int n = epoll_wait(GetEpoll(epoll_type), evs, epoll_event_size_, timeout);
            if (n == -1) {
                if (errno == EINTR) {
                    goto retry;
                }
                continue;
            }

            epoll_n += n;
            DebugPrint(dbg_scheduler, "do epoll(%d) event, n = %d", epoll_type, n);
            for (int i = 0; i < n; ++i)
            {
                EpollPtr* ep = (EpollPtr*)evs[i].data.ptr;
                ep->revent = evs[i].events;
                Task* tk = ep->tk;
                ++tk->GetIoWaitData().wait_successful_;
                // 将tk暂存, 最后再执行Cancel, 是为了poll和select可以得到正确的计数。
                // 以防Task被加入runnable列表后，被其他线程执行
                epollwait_tasks_.insert(EpollWaitSt{tk, ep->io_block_id});
                DebugPrint(dbg_ioblock,
                        "task(%s) epoll(%s) trigger fd=%d io_block_id(%u) ep(%p) loop_index(%llu)",
                        tk->DebugInfo(), EpollTypeName(epoll_type),
                        ep->fdst->fd, ep->io_block_id, ep, (unsigned long long)loop_index_);
            }
        }

        for (auto &st : epollwait_tasks_)
            Cancel(st.tk, st.id);
        epollwait_tasks_.clear();
    }

    std::list<CoTimerPtr> timeout_list;
    {
        std::unique_lock<LFLock> lock(timeout_list_lock_);
        timeout_list_.swap(timeout_list);
    }

    for (auto &cb : timeout_list)
        (*cb)();

    // 由于epoll_wait的结果中会残留一些未计数的Task*,
    //     epoll的性质决定了这些Task无法计数, 
    //     所以这个析构的操作一定要在epoll_lock的保护中做
    std::vector<SList<Task>> delete_lists;
    Task::PopDeleteList(delete_lists);
    for (auto &delete_list : delete_lists)
        for (auto it = delete_list.begin(); it != delete_list.end();)
        {
            Task* tk = &*it++;
            DebugPrint(dbg_task, "task(%s) delete.", tk->DebugInfo());
            delete tk;
        }

    return epoll_n + c;
}

int IoWait::GetEpollType(int epoll_fd)
{
    if (epoll_fd == epoll_fds_[(int)EpollType::read])
        return (int)EpollType::read;
    else if (epoll_fd == epoll_fds_[(int)EpollType::write])
        return (int)EpollType::write;
    else
        return -1;
}

int IoWait::GetEpoll(int type)
{
    CreateEpoll();
    return epoll_fds_[type];
}

int IoWait::ChooseEpoll(uint32_t event)
{
    CreateEpoll();
    return (event & EPOLLIN) ? epoll_fds_[(int)EpollType::read] : epoll_fds_[(int)EpollType::write];
}

void IoWait::CreateEpoll()
{
    pid_t pid = getpid();
    if (epoll_owner_pid_ == pid) return ;
    std::unique_lock<LFLock> lock(epoll_create_lock_);
    if (epoll_owner_pid_ == pid) return ;

    epoll_owner_pid_ = pid;

    epoll_event_size_ = g_Scheduler.GetOptions().epoll_event_size;
    for (int i = 0; i < 2; ++i)
    {
        close(epoll_fds_[i]);
        epoll_fds_[i] = epoll_create(epoll_event_size_);
        if (epoll_fds_[i] != -1) {
            DebugPrint(dbg_ioblock, "create epoll success. epollfd=%d", epoll_fds_[i]);
            continue;
        }

        fprintf(stderr, "CoroutineScheduler init failed. epoll create error:%s\n", strerror(errno));
        exit(1);
    }

}

bool IoWait::IsEpollCreated() const
{
    return epoll_owner_pid_ == getpid();
}

} //namespace co