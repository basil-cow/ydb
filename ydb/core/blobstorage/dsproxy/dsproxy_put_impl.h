#pragma once

#include "dsproxy.h"
#include "dsproxy_blackboard.h"
#include "dsproxy_cookies.h"
#include "dsproxy_mon.h"
#include "dsproxy_strategy_accelerate_put.h"
#include "dsproxy_strategy_accelerate_put_m3dc.h"
#include "dsproxy_strategy_restore.h"
#include "dsproxy_strategy_put_m3dc.h"
#include "dsproxy_strategy_put_m3of4.h"
#include <ydb/core/blobstorage/vdisk/common/vdisk_events.h>
#include <util/generic/set.h>

namespace NKikimr {

class TStrategyBase;

class TPutImpl {
public:
    using TPutResultVec = TBatchedVec<std::pair<ui64, std::unique_ptr<TEvBlobStorage::TEvPutResult>>>;

    using TPutEvent = std::variant<std::unique_ptr<TEvBlobStorage::TEvVPut>,
                                   std::unique_ptr<TEvBlobStorage::TEvVMultiPut>>;

private:
    TBlobStorageGroupInfo::TServiceIds VDisksSvc;
    TBlobStorageGroupInfo::TVDiskIds VDisksId;

    TInstant Deadline;
    const TIntrusivePtr<TBlobStorageGroupInfo> Info;

    TBlackboard Blackboard;
    TBatchedVec<bool> IsDone;
    TBatchedVec<bool> WrittenBeyondBarrier;
    TStorageStatusFlags StatusFlags;
    float ApproximateFreeSpaceShare;
    TIntrusivePtr<TBlobStorageGroupProxyMon> Mon;
    ui32 HandoffPartsSent = 0;
    ui32 VPutRequests = 0;
    ui32 VPutResponses = 0;
    ui32 VMultiPutRequests = 0;
    ui32 VMultiPutResponses = 0;
    bool AtLeastOneResponseWasNotOk = false;
    bool EnableRequestMod3x3ForMinLatecy = false;

    ui64 DoneBlobs = 0;

    const TEvBlobStorage::TEvPut::ETactic Tactic;

    struct TBlobInfo {
        TLogoBlobID BlobId;
        TRope Buffer;
        ui64 BufferSize;
        TActorId Recipient;
        ui64 Cookie;
        NLWTrace::TOrbit Orbit;
        bool Replied = false;
        std::vector<std::pair<ui64, ui32>> ExtraBlockChecks;
        NWilson::TSpan Span;
        std::shared_ptr<TEvBlobStorage::TExecutionRelay> ExecutionRelay;

        TBlobInfo(TLogoBlobID id, TRope&& buffer, TActorId recipient, ui64 cookie, NWilson::TTraceId traceId,
                NLWTrace::TOrbit&& orbit, std::vector<std::pair<ui64, ui32>> extraBlockChecks, bool single,
                std::shared_ptr<TEvBlobStorage::TExecutionRelay> executionRelay)
            : BlobId(id)
            , Buffer(std::move(buffer))
            , BufferSize(Buffer.size())
            , Recipient(recipient)
            , Cookie(cookie)
            , Orbit(std::move(orbit))
            , ExtraBlockChecks(std::move(extraBlockChecks))
            , Span(single ? NWilson::TSpan() : NWilson::TSpan(TWilson::BlobStorage, std::move(traceId), "DSProxy.Put.Blob"))
            , ExecutionRelay(std::move(executionRelay))
        {}

        void Output(IOutputStream& s) const {
            s << BlobId;
            if (!ExtraBlockChecks.empty()) {
                s << "{";
                for (auto it = ExtraBlockChecks.begin(); it != ExtraBlockChecks.end(); ++it) {
                    if (it != ExtraBlockChecks.begin()) {
                        s << ", ";
                    }
                    s << it->first << ":" << it->second;
                }
                s << "}";
            }
        }

        TString ToString() const {
            TStringStream s;
            Output(s);
            return s.Str();
        }
    };

    TBatchedVec<TBlobInfo> Blobs;
    std::unordered_map<TLogoBlobID, size_t> BlobMap;

    friend class TBlobStorageGroupPutRequest;

    bool IsInitialized = false;

    friend void ::Out<TBlobInfo>(IOutputStream&, const TBlobInfo&);

public:
    TPutImpl(const TIntrusivePtr<TBlobStorageGroupInfo> &info, const TIntrusivePtr<TGroupQueues> &state,
            TEvBlobStorage::TEvPut *ev, const TIntrusivePtr<TBlobStorageGroupProxyMon> &mon,
            bool enableRequestMod3x3ForMinLatecy, TActorId recipient, ui64 cookie, NWilson::TTraceId traceId)
        : Deadline(ev->Deadline)
        , Info(info)
        , Blackboard(info, state, ev->HandleClass, NKikimrBlobStorage::EGetHandleClass::AsyncRead, false)
        , IsDone(1)
        , WrittenBeyondBarrier(1)
        , StatusFlags(0)
        , ApproximateFreeSpaceShare(0.f)
        , Mon(mon)
        , EnableRequestMod3x3ForMinLatecy(enableRequestMod3x3ForMinLatecy)
        , Tactic(ev->Tactic)
    {
        BlobMap.emplace(ev->Id, Blobs.size());
        Blobs.emplace_back(ev->Id, TRope(ev->Buffer), recipient, cookie, std::move(traceId), std::move(ev->Orbit),
            std::move(ev->ExtraBlockChecks), true, std::move(ev->ExecutionRelay));

        auto& blob = Blobs.back();
        LWPROBE(DSProxyBlobPutTactics, blob.BlobId.TabletID(), Info->GroupID, blob.BlobId.ToString(), Tactic,
            NKikimrBlobStorage::EPutHandleClass_Name(GetPutHandleClass()));
    }

    TPutImpl(const TIntrusivePtr<TBlobStorageGroupInfo> &info, const TIntrusivePtr<TGroupQueues> &state,
            TBatchedVec<TEvBlobStorage::TEvPut::TPtr> &events, const TIntrusivePtr<TBlobStorageGroupProxyMon> &mon,
            NKikimrBlobStorage::EPutHandleClass putHandleClass, TEvBlobStorage::TEvPut::ETactic tactic,
            bool enableRequestMod3x3ForMinLatecy)
        : Deadline(TInstant::Zero())
        , Info(info)
        , Blackboard(info, state, putHandleClass, NKikimrBlobStorage::EGetHandleClass::AsyncRead, false)
        , IsDone(events.size())
        , WrittenBeyondBarrier(events.size())
        , StatusFlags(0)
        , ApproximateFreeSpaceShare(0.f)
        , Mon(mon)
        , EnableRequestMod3x3ForMinLatecy(enableRequestMod3x3ForMinLatecy)
        , Tactic(tactic)
    {
        Y_ABORT_UNLESS(events.size(), "TEvPut vector is empty");

        for (auto &ev : events) {
            auto& msg = *ev->Get();
            Y_ABORT_UNLESS(msg.HandleClass == putHandleClass);
            Y_ABORT_UNLESS(msg.Tactic == tactic);
            BlobMap.emplace(msg.Id, Blobs.size());
            Blobs.emplace_back(msg.Id, TRope(msg.Buffer), ev->Sender, ev->Cookie, std::move(ev->TraceId),
                std::move(msg.Orbit), std::move(msg.ExtraBlockChecks), false, std::move(msg.ExecutionRelay));
            Deadline = Max(Deadline, msg.Deadline);

            auto& blob = Blobs.back();
            LWPROBE(DSProxyBlobPutTactics, blob.BlobId.TabletID(), Info->GroupID, blob.BlobId.ToString(), Tactic,
                NKikimrBlobStorage::EPutHandleClass_Name(GetPutHandleClass()));
        }

        Y_ABORT_UNLESS(Blobs.size());
        Y_ABORT_UNLESS(Blobs.size() <= MaxBatchedPutRequests);
    }

    NKikimrBlobStorage::EPutHandleClass GetPutHandleClass() const {
        return Blackboard.PutHandleClass;
    }

    ui32 GetHandoffPartsSent() const {
        return HandoffPartsSent;
    }

    void GenerateInitialRequests(TLogContext &logCtx, TBatchedVec<TStackVec<TRope, TypicalPartsInBlob>>& partSets) {
        Y_UNUSED(logCtx);
        Y_VERIFY_S(partSets.size() == Blobs.size(), "partSets.size# " << partSets.size()
                << " Blobs.size# " << Blobs.size());
        const ui32 totalParts = Info->Type.TotalPartCount();
        for (ui64 blobIdx = 0; blobIdx < Blobs.size(); ++blobIdx) {
            TBlobInfo& blob = Blobs[blobIdx];
            Blackboard.RegisterBlobForPut(blob.BlobId);
            for (ui32 i = 0; i < totalParts; ++i) {
                if (Info->Type.PartSize(TLogoBlobID(blob.BlobId, i + 1))) {
                    Blackboard.AddPartToPut(blob.BlobId, i, TRope(partSets[blobIdx][i]));
                }
            }
            Blackboard.MarkBlobReadyToPut(blob.BlobId, blobIdx);
        }
        IsInitialized = true;
    }

    void PrepareReply(NKikimrProto::EReplyStatus status, TLogContext &logCtx, TString errorReason,
            TPutResultVec &outPutResults);
    void PrepareReply(TLogContext &logCtx, TString errorReason, TBatchedVec<TBlackboard::TBlobStates::value_type*>& finished,
            TPutResultVec &outPutResults);
    void PrepareOneReply(NKikimrProto::EReplyStatus status, TLogoBlobID blobId, ui64 blobIdx, TLogContext &logCtx,
            TString errorReason, TPutResultVec &outPutResults);

    ui64 GetTimeToAccelerateNs(TLogContext &logCtx);

    void Accelerate(TLogContext &logCtx) {
        Blackboard.ChangeAll();
        switch (Info->Type.GetErasure()) {
            case TBlobStorageGroupType::ErasureMirror3dc:
                Blackboard.RunStrategy(logCtx, TAcceleratePut3dcStrategy(Tactic, EnableRequestMod3x3ForMinLatecy));
                break;
            case TBlobStorageGroupType::ErasureMirror3of4:
                Blackboard.RunStrategy(logCtx, TPut3of4Strategy(Tactic, true));
                break;
            default:
                Blackboard.RunStrategy(logCtx, TAcceleratePutStrategy());
                break;
        }
    }

    TString DumpFullState() const;

    bool MarkBlobAsSent(ui64 blobIdx);

    TString ToString() const;

    void InvalidatePartStates(ui32 orderNumber) {
        Blackboard.InvalidatePartStates(orderNumber);
    }

    void ChangeAll() {
        Blackboard.ChangeAll();
    }

    void Step(TLogContext &logCtx, TPutResultVec& putResults, const TBlobStorageGroupInfo::TGroupVDisks& expired) {
        RunStrategies(logCtx, putResults, expired);
    }

    TDeque<TPutEvent> GeneratePutRequests() {
        TDeque<TPutEvent> events;

        // Group put requests together by VDiskID.
        std::unordered_multimap<ui32, TDiskPutRequest*> puts;
        auto& perDiskRequests = Blackboard.GroupDiskRequests.DiskRequestsForOrderNumber;
        for (ui32 orderNumber = 0, numDisks = perDiskRequests.size(); orderNumber < numDisks; ++orderNumber) {
            TDiskRequests& requests = perDiskRequests[orderNumber];
            while (requests.FirstUnsentPutIdx < requests.PutsToSend.size()) {
                auto& putRequest = requests.PutsToSend[requests.FirstUnsentPutIdx++];
                puts.emplace(orderNumber, &putRequest);
            }
        }

        // Generate queries to VDisks.
        for (auto it = puts.begin(); it != puts.end(); ) {
            auto [begin, end] = puts.equal_range(it->first);
            Y_ABORT_UNLESS(it == begin);

            if (std::next(it) == end) { // TEvVPut
                auto [orderNumber, ptr] = *it++;
                auto ev = std::make_unique<TEvBlobStorage::TEvVPut>(ptr->Id, ptr->Buffer, Info->GetVDiskId(orderNumber),
                    false, nullptr, Deadline, Blackboard.PutHandleClass);

                auto& record = ev->Record;
                for (const auto& [tabletId, generation] : Blobs[ptr->BlobIdx].ExtraBlockChecks) {
                    auto *p = record.AddExtraBlockChecks();
                    p->SetTabletId(tabletId);
                    p->SetGeneration(generation);
                }

                events.emplace_back(std::move(ev));
                HandoffPartsSent += ptr->IsHandoff;
                ++VPutRequests;
            } else { // TEvVMultiPut
                auto ev = std::make_unique<TEvBlobStorage::TEvVMultiPut>(Info->GetVDiskId(it->first), Deadline,
                    Blackboard.PutHandleClass, false);
                while (it != end) {
                    auto [orderNumber, ptr] = *it++;
                    ev->AddVPut(ptr->Id, TRcBuf(ptr->Buffer), nullptr, &Blobs[ptr->BlobIdx].ExtraBlockChecks,
                        Blobs[ptr->BlobIdx].Span.GetTraceId());
                    HandoffPartsSent += ptr->IsHandoff;
                }
                events.emplace_back(std::move(ev));
                ++VMultiPutRequests;
            }
        }

        return events;
    }

    void ProcessResponse(TEvBlobStorage::TEvVPutResult& msg) {
        ++VPutResponses;
        ProcessResponseCommonPart(msg.Record);
        ProcessResponseBlob(VDiskIDFromVDiskID(msg.Record.GetVDiskID()), msg.Record);
    }

    void ProcessResponse(TEvBlobStorage::TEvVMultiPutResult& msg) {
        ++VMultiPutResponses;
        ProcessResponseCommonPart(msg.Record);
        const TVDiskID vdiskId = VDiskIDFromVDiskID(msg.Record.GetVDiskID());
        for (const auto& item : msg.Record.GetItems()) {
            ProcessResponseBlob(vdiskId, item);
        }
    }

    size_t GetBlobIdx(const TLogoBlobID& id) const {
        const auto it = BlobMap.find(id.FullID());
        Y_ABORT_UNLESS(it != BlobMap.end());
        return it->second;
    }

protected:
    void RunStrategies(TLogContext &logCtx, TPutResultVec &outPutResults, const TBlobStorageGroupInfo::TGroupVDisks& expired);
    void RunStrategy(TLogContext &logCtx, const IStrategy& strategy, TPutResultVec &outPutResults,
        const TBlobStorageGroupInfo::TGroupVDisks& expired);

    template<typename TProtobuf>
    void ProcessResponseBlob(TVDiskID vdiskId, TProtobuf& record) {
        Y_ABORT_UNLESS(record.HasStatus());
        Y_ABORT_UNLESS(record.HasBlobID());

        const NKikimrProto::EReplyStatus status = record.GetStatus();
        const TLogoBlobID blobId = LogoBlobIDFromLogoBlobID(record.GetBlobID());
        const ui32 orderNumber = Info->GetOrderNumber(TVDiskIdShort(vdiskId));

        const size_t blobIdx = GetBlobIdx(blobId);

        if (IsDone[blobIdx]) {
            return;
        }

        switch (status) {
            case NKikimrProto::ERROR:
            case NKikimrProto::VDISK_ERROR_STATE:
            case NKikimrProto::OUT_OF_SPACE:
                Blackboard.AddErrorResponse(blobId, orderNumber);
                AtLeastOneResponseWasNotOk = true;
                break;
            case NKikimrProto::OK:
            case NKikimrProto::ALREADY:
                Blackboard.AddPutOkResponse(blobId, orderNumber);
                WrittenBeyondBarrier[blobIdx] = record.GetWrittenBeyondBarrier();
                break;
            default:
                Y_ABORT("unexpected status# %s", NKikimrProto::EReplyStatus_Name(status).data());
        }
    }

    template<typename TProtobuf>
    void ProcessResponseCommonPart(TProtobuf& record) {
        Y_ABORT_UNLESS(record.HasStatus());
        const NKikimrProto::EReplyStatus status = record.GetStatus();
        Y_ABORT_UNLESS(status != NKikimrProto::BLOCKED && status != NKikimrProto::RACE && status != NKikimrProto::DEADLINE);
        if (record.HasStatusFlags()) {
            StatusFlags.Merge(record.GetStatusFlags());
        }
        if (record.HasApproximateFreeSpaceShare()) {
            float share = record.GetApproximateFreeSpaceShare();
            if (ApproximateFreeSpaceShare == 0.f || share < ApproximateFreeSpaceShare) {
                ApproximateFreeSpaceShare = share;
            }
        }
    }

}; //TPutImpl

}//NKikimr
