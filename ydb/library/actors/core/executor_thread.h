#pragma once

#include "defs.h"
#include "event.h"
#include "callstack.h"
#include "probes.h"
#include "worker_context.h"
#include "log_settings.h"

#include <ydb/library/actors/util/datetime.h>

#include <util/system/thread.h>

namespace NActors {
    class IActor;
    class TActorSystem;
    struct TExecutorThreadCtx;
    class TExecutorPoolBaseMailboxed;

    class TExecutorThread: public ISimpleThread {
        enum class EState : ui64 {
            Running = 0,
            NeedToReloadPools,
        };

        struct TProcessingResult {
            bool IsPreempted = false;
            bool WasWorking = false;
        };

    public:
        static constexpr TDuration DEFAULT_TIME_PER_MAILBOX =
            TDuration::MilliSeconds(10);
        static constexpr ui32 DEFAULT_EVENTS_PER_MAILBOX = 100;

        TExecutorThread(TWorkerId workerId,
                        TWorkerId cpuId,
                        TActorSystem* actorSystem,
                        IExecutorPool* executorPool,
                        TMailboxTable* mailboxTable,
                        TExecutorThreadCtx *threadCtx,
                        const TString& threadName,
                        TDuration timePerMailbox = DEFAULT_TIME_PER_MAILBOX,
                        ui32 eventsPerMailbox = DEFAULT_EVENTS_PER_MAILBOX);

        TExecutorThread(TWorkerId workerId,
                        TWorkerId cpuId,
                        TActorSystem* actorSystem,
                        IExecutorPool* executorPool,
                        TMailboxTable* mailboxTable,
                        const TString& threadName,
                        TDuration timePerMailbox = DEFAULT_TIME_PER_MAILBOX,
                        ui32 eventsPerMailbox = DEFAULT_EVENTS_PER_MAILBOX)
            : TExecutorThread(workerId, cpuId, actorSystem, executorPool, mailboxTable, nullptr, threadName, timePerMailbox, eventsPerMailbox)
        {}

        TExecutorThread(TWorkerId workerId,
                        TActorSystem* actorSystem,
                        IExecutorPool* executorPool,
                        TMailboxTable* mailboxTable,
                        TExecutorThreadCtx *threadCtx,
                        const TString& threadName,
                        TDuration timePerMailbox = DEFAULT_TIME_PER_MAILBOX,
                        ui32 eventsPerMailbox = DEFAULT_EVENTS_PER_MAILBOX)
            : TExecutorThread(workerId, 0, actorSystem, executorPool, mailboxTable, threadCtx, threadName, timePerMailbox, eventsPerMailbox)
        {}

        TExecutorThread(TWorkerId workerId,
                        TActorSystem* actorSystem,
                        IExecutorPool* executorPool,
                        TMailboxTable* mailboxTable,
                        const TString& threadName,
                        TDuration timePerMailbox = DEFAULT_TIME_PER_MAILBOX,
                        ui32 eventsPerMailbox = DEFAULT_EVENTS_PER_MAILBOX)
            : TExecutorThread(workerId, 0, actorSystem, executorPool, mailboxTable, nullptr, threadName, timePerMailbox, eventsPerMailbox)
        {}

        TExecutorThread(TWorkerId workerId,
                    TActorSystem* actorSystem,
                    TExecutorThreadCtx *threadCtx,
                    i16 poolCount,
                    const TString& threadName,
                    ui64 softProcessingDurationTs,
                    TDuration timePerMailbox,
                    ui32 eventsPerMailbox);

        virtual ~TExecutorThread();

        void UpdatePools();

        template <ESendingType SendingType = ESendingType::Common>
        TActorId RegisterActor(IActor* actor, TMailboxType::EType mailboxType = TMailboxType::HTSwap, ui32 poolId = Max<ui32>(),
                               TActorId parentId = TActorId());
        template <ESendingType SendingType = ESendingType::Common>
        TActorId RegisterActor(IActor* actor, TMailboxHeader* mailbox, ui32 hint, TActorId parentId = TActorId());
        void UnregisterActor(TMailboxHeader* mailbox, TActorId actorId);
        void DropUnregistered();
        const std::vector<THolder<IActor>>& GetUnregistered() const { return DyingActors; }

        void Schedule(TInstant deadline, TAutoPtr<IEventHandle> ev, ISchedulerCookie* cookie = nullptr);
        void Schedule(TMonotonic deadline, TAutoPtr<IEventHandle> ev, ISchedulerCookie* cookie = nullptr);
        void Schedule(TDuration delta, TAutoPtr<IEventHandle> ev, ISchedulerCookie* cookie = nullptr);

        template <ESendingType SendingType = ESendingType::Common>
        bool Send(TAutoPtr<IEventHandle> ev);

        void GetCurrentStats(TExecutorThreadStats& statsCopy) const;
        void GetSharedStats(i16 poolId, TExecutorThreadStats &stats) const;

        TThreadId GetThreadId() const; // blocks, must be called after Start()
        TWorkerId GetWorkerId() const;

    private:
        void* ThreadProc();

        TProcessingResult ProcessExecutorPool(IExecutorPool *pool);
        TProcessingResult ProcessSharedExecutorPool(TExecutorPoolBaseMailboxed *pool);

        template <typename TMailbox>
        TProcessingResult Execute(TMailbox* mailbox, ui32 hint, bool isTailExecution);

    public:
        TActorSystem* const ActorSystem;
        std::atomic<bool> StopFlag = false;

    private:
        // Pool-specific
        IExecutorPool* ExecutorPool;
        TExecutorThreadCtx *ThreadCtx;

        // Event-specific (currently executing)
        TVector<THolder<IActor>> DyingActors;
        TActorId CurrentRecipient;
        ui64 CurrentActorScheduledEventsCounter = 0;

        // Thread-specific
        TWorkerContext Ctx;
        ui64 RevolvingReadCounter = 0;
        ui64 RevolvingWriteCounter = 0;
        const TString ThreadName;
        volatile TThreadId ThreadId = UnknownThreadId;
        bool IsSharedThread = false;

        TDuration TimePerMailbox;
        ui32 EventsPerMailbox;
        ui64 SoftProcessingDurationTs;

        std::atomic<EState> NeedToReloadPools = EState::NeedToReloadPools;
        std::vector<TExecutorThreadStats> SharedStats;
    };

    template <typename TMailbox>
    void UnlockFromExecution(TMailbox* mailbox, IExecutorPool* executorPool, bool asFree, ui32 hint, TWorkerId workerId, ui64& revolvingWriteCounter) {
        mailbox->UnlockFromExecution1();
        const bool needReschedule1 = (nullptr != mailbox->Head());
        if (!asFree) {
            if (mailbox->UnlockFromExecution2(needReschedule1)) {
                RelaxedStore<NHPTimer::STime>(&mailbox->ScheduleMoment, GetCycleCountFast());
                executorPool->ScheduleActivationEx(hint, ++revolvingWriteCounter);
            }
        } else {
            if (mailbox->UnlockAsFree(needReschedule1)) {
                RelaxedStore<NHPTimer::STime>(&mailbox->ScheduleMoment, GetCycleCountFast());
                executorPool->ScheduleActivationEx(hint, ++revolvingWriteCounter);
            }
            executorPool->ReclaimMailbox(TMailbox::MailboxType, hint, workerId, ++revolvingWriteCounter);
        }
    }
}
