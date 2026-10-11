// Standalone header-only lifecycle/clock contract integration tests.
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/inputpercentilebuffer.h"
#ifdef INPUT_PERCENTILE_CONTROLLER_TEST
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"
#endif
#include <cstdio>
#include <limits>
using Vrr13::InputPercentileBuffer;
namespace {
int failures=0;
void check(bool ok,const char* what) {
    if(!ok) { ++failures;std::fprintf(stderr,"FAIL: %s\n",what); }
}
void feed(InputPercentileBuffer& p,uint64_t frame,uint64_t at,uint64_t slot,
          int64_t required,bool valid=true,uint64_t probability=990000,
          uint64_t window=30000000,int64_t budget=0,uint64_t low=0,uint64_t high=6000,uint64_t tolerance=0) {
    p.observe(frame,at,slot,required,valid,low,high,probability,window,1000,250,budget,tolerance);
}
// The user's design: a lower frame rate frees VRR headroom (source period minus
// display period/render), which absorbs lateness instead of extra buffer.
void testVrrHeadroomAbsorbsLateness() {
    const auto observe=[](InputPercentileBuffer& p,uint64_t frame,int64_t required,uint64_t headroom) {
        p.observe(frame,frame*33333,1000000+frame*33333,required,true,1000,25000,995000,200000000,
                  1000000,250,0,750,headroom);
    };
    // Same 9 ms input tail at 30 FPS (25 ms headroom) and at 110 FPS (0.76 ms).
    InputPercentileBuffer low,high;low.reset(1000);high.reset(1000);
    for(uint64_t f=1;f<=400;++f) {
        const int64_t required=f%50==0?9000:2000;
        observe(low,f,required,25000);observe(high,f,required,760);
    }
    check(low.stats().targetUs==1000 && low.stats().missed==0,
          "30 FPS headroom covers a 9 ms tail: target stays at the floor and nothing is a miss");
    check(high.stats().targetUs==9000-750-760 && high.stats().missed>0,
          "110 FPS has little headroom: the same tail needs buffer");
    check(low.stats().currentUs<=high.stats().currentUs,"30 FPS never holds more buffer than 110 FPS for the same jitter");
    // Protection learned at 110 FPS is credited by the headroom gained at 30 FPS.
    InputPercentileBuffer change;change.reset(9000);
    change.creditHeadroom(25000-760);
    check(change.stats().currentUs==1000 || change.stats().currentUs==0,
          "rate change that frees headroom credits it immediately instead of freezing old protection");
    InputPercentileBuffer floor;floor.reset(9000);
    floor.observe(1,33333,1033333,2000,true,1000,25000,995000,200000000,1000000,250,0,750,760);
    floor.creditHeadroom(3000);
    check(floor.stats().currentUs==6000,"credit removes exactly the gained headroom above the floor");
}
void testPhaseBreakKeepsEvidenceAndHeadroomLossRestores() {
    const auto observe=[](InputPercentileBuffer& p,uint64_t frame,int64_t required,uint64_t headroom) {
        p.observe(frame,frame*8333,1000000+frame*8333,required,true,1000,12000,995000,200000000,
                  1000000,250,0,500,headroom);
    };
    InputPercentileBuffer p;p.reset(1000);
    for(uint64_t f=1;f<=400;++f) observe(p,f,f%50==0?7000:2000,0);
    const auto before=p.stats();
    p.breakAdjacency();
    const auto after=p.stats();
    check(after.samples==before.samples && after.recentEligible==before.recentEligible &&
          after.recentMissed==before.recentMissed && after.targetUs==before.targetUs && after.epochResets==before.epochResets,
          "a phase discontinuity keeps evidence, coverage outcomes and target");
    observe(p,402,2000,0);
    check(p.stats().samples==before.samples+1,"observation after a phase break extends the same history");
    // Headroom loss steps current to the evidence-backed target, bounded by the loss.
    InputPercentileBuffer q;q.reset(1000);
    for(uint64_t f=1;f<=400;++f) observe(q,f,f%50==0?7000:2000,0);
    q.creditHeadroom(25000);
    check(q.stats().currentUs==1000,"headroom gained at 30 FPS credits protection to the floor");
    q.advance(400*8333+1,1000,12000,995000,200000000,1000000,250,0,500,0);
    const auto target=q.stats().targetUs;
    q.raiseForHeadroomLoss(2000);
    check(q.stats().currentUs==3000,"a small headroom loss raises current by exactly the loss");
    q.raiseForHeadroomLoss(25000);
    check(q.stats().currentUs==target && target==6500,"a large loss stops at the evidence-backed target");
    q.raiseForHeadroomLoss(25000);
    check(q.stats().currentUs==target,"current never exceeds the target through a headroom loss");
    // A new rate layer is seeded with copied evidence: it sets the target, but
    // the other rate's outcomes leave coverage and expire without underflow.
    InputPercentileBuffer seeded=p;seeded.markEvidenceAsSeed();
    check(seeded.stats().recentEligible==0 && seeded.stats().recentMissed==0 &&
          seeded.stats().samples==p.stats().samples,"seeded evidence keeps samples but no coverage outcomes");
    observe(seeded,403,2000,0);
    check(seeded.stats().recentEligible==1 && seeded.stats().targetUs==p.stats().targetUs,
          "the new layer's own frames are its coverage while the seed still sets the target");
    seeded.advance(403*8333+200000001,1000,12000,995000,200000000,1000000,250,0,500,0);
    check(seeded.stats().recentEligible==0 && seeded.stats().samples==0,"expired seed and own samples leave counts consistent");
}
void testQuantileAndClock() {
    InputPercentileBuffer p;
    check(p.demand(1600)==1600,"uninitialized helper preserves caller delay");
    p.reset(1600);
    feed(p,0,10000,10000,1000);
    check(p.stats().eligible==1 && p.stats().absorbed==1,"frame zero is eligible and absorbed positive input is counted");
    p.reset(1600);
    for(uint64_t i=1;i<=99;++i) feed(p,i,i*100000,1000000000+i*10000,i==1?2000:100);
    check(p.stats().tailUncertain && p.stats().targetUs==2000,"unsupported 99 percent tail uses observed maximum");
    feed(p,100,10000000,1001000000,100);
    check(!p.stats().tailUncertain && p.stats().targetUs==100,"nearest-rank supported 99 percent quantile");
    feed(p,101,40000000,1001010000,50);
    check(p.stats().samples==1 && p.stats().targetUs==50,"history ages by observation clock even when source slots hardly advance");
    check(p.stats().windowUs==30000000,"explicit thirty-second requested history is preserved");
    feed(p,102,40100000,1001020000,50,true,990000,1);
    check(p.stats().windowUs==10000000,"short history is clamped to the supported ten-second minimum");
    feed(p,103,40200000,1001030000,50,true,990000,900000000);
    check(p.stats().windowUs==300000000,"history is bounded at five minutes");
}
void testBudgetAndSignedOffsets() {
    InputPercentileBuffer p;p.reset(1600);
    feed(p,1,1000000,900000,1000,true,995000,30000000,200);
    check(p.stats().rawRequiredQuantileUs==1200 && p.stats().targetUs==1000,"raw signed source headroom retains readiness budget");
    feed(p,2,1008333,908333,600,true,995000,30000000,600);
    check(p.stats().targetUs==600 && p.stats().samples==2,"budget rise renormalizes retained history without reset or false tail");
    feed(p,3,1016666,916666,1100,true,995000,30000000,100);
    check(p.stats().targetUs==1100 && p.stats().samples==3,"budget fall renormalizes history without manufacturing new requirement");
    p.breakSequence();feed(p,4,1020000,920000,1500,true,990000,30000000,-500);
    check(p.stats().rawRequiredQuantileUs==1000 && p.stats().targetUs==1500,"negative readiness budget preserves signed source headroom");
    p.breakSequence();feed(p,4,1024999,924999,-600,true,990000,30000000,200);
    check(p.stats().rawRequiredQuantileUs==-400 && p.stats().targetUs==0,"negative raw headroom stays signed and requests no buffer");
    feed(p,5,1026000,926000,200,true,990000,30000000,-600);
    check(p.stats().rawRequiredQuantileUs==-400 && p.stats().targetUs==200,
        "negative quantile and more negative budget yield positive headroom without unsigned conversion artifacts");
    p.breakSequence();feed(p,5,1027000,927000,std::numeric_limits<int64_t>::min(),true,990000,30000000,-200);
    check(p.stats().rawRequiredQuantileUs==std::numeric_limits<int64_t>::min() && p.stats().targetUs==0,
        "signed negative normalization overflow saturates safely");
    p.breakSequence();feed(p,5,1033332,933332,std::numeric_limits<int64_t>::max(),true,990000,30000000,200);
    check(p.stats().rawRequiredQuantileUs==std::numeric_limits<int64_t>::max() && p.stats().clipped,"signed positive overflow saturates safely");
}
void testInputToleranceBoundaryAndNormalization() {
    constexpr uint64_t B=5000,T=750;
    for(const auto excess:{690ULL,750ULL,751ULL}) {
        InputPercentileBuffer p;p.reset(B);
        feed(p,1,1000000,1000000,int64_t(B+excess),true,995000,200000000,0,0,10000,T);
        auto stats=p.stats();
        check(stats.toleranceUs==T && stats.rawRequiredQuantileUs==int64_t(B+excess),
            "input tolerance is explicit and does not alter retained raw headroom");
        check(stats.targetUs==B+excess-T,
            "percentile buffer demand subtracts tolerance exactly once");
        check((stats.missed>0)==(excess>T),
            "690us and exactly750us beyond applied buffer are accepted;751us is a miss");
        feed(p,2,1100000,1100000,int64_t(B+excess),true,995000,200000000,0,0,10000,T);
        stats=p.stats();
        check(excess>T ? stats.currentUs==B+1 : stats.currentUs<=B,
            "tolerance-covered input cannot grow delay; one excess microsecond requests only one microsecond");
        check((stats.raises>0)==(excess>T),"growth decisions agree with tolerance-aware causal coverage");
        if(excess==690) {
            feed(p,3,1200000,1200000,B+excess,true,995000,200000000,0,0,10000,T);
            check(p.stats().currentUs==4950,"tolerance-covered input releases25us in each100ms without growth");
            feed(p,4,1300000,1300000,B+excess,true,995000,200000000,0,0,10000,T);
            check(p.stats().currentUs==4940 && p.stats().raises==0 && p.stats().missed==0,
                "tolerance-aware demand reaches4940us afterthree100ms observations with no miss or upward reversal");
        }
    }
    InputPercentileBuffer zero;zero.reset(B);
    feed(zero,1,1000000,1000000,B+690,true,995000,200000000,0,0,10000,0);
    check(zero.stats().targetUs==B+690 && zero.stats().missed==1,
        "zero tolerance retains the old exact percentile and miss semantics");

    InputPercentileBuffer p;p.reset(B);
    feed(p,1,1000000,1000000,5690,true,995000,200000000,200,0,10000,T);
    check(p.stats().rawRequiredQuantileUs==5890 && p.stats().targetUs==4940,
        "budget normalization precedes the single tolerance subtraction");
    p.advance(1100000,0,10000,995000,200000000,1000,250,600,T);
    check(p.stats().samples==1 && p.stats().rawRequiredQuantileUs==5890 && p.stats().targetUs==4540,
        "rising readiness budget renormalizes retained raw input without double tolerance");
    p.advance(1200000,0,10000,995000,200000000,1000,250,-500,T);
    check(p.stats().targetUs==5640,
        "negative readiness budget remains signed before tolerance-aware buffer demand");
    p.advance(1300000,0,10000,995000,200000000,1000,250,-500,0);
    check(p.stats().samples==1 && p.stats().targetUs==6390 && p.stats().toleranceUs==0,
        "runtime tolerance decrease updates retained quantile demand without requiring a new input");
    p.advance(1400000,0,10000,995000,200000000,1000,250,200,2000);
    check(p.stats().samples==1 && p.stats().targetUs==3690 && p.stats().recentReleaseMisses==0,
        "runtime tolerance increase refreshes target and current-CDF miss classification");

    InputPercentileBuffer history;history.reset(B);
    feed(history,1,1000000,1000000,B+690,true,995000,200000000,0,0,10000,0);
    history.advance(1100000,0,10000,995000,200000000,1000,250,0,T);
    check(history.stats().samples==1 && history.stats().targetUs==B-60 && history.stats().recentReleaseMisses==0 &&
        history.stats().recentMissed==1 && history.stats().cumulativeMissed==1,
        "dynamic tolerance reinterprets retained CDF while historical outcomes preserve observed per-frame definitions");

    p.reset(B);feed(p,1,2000000,2000000,-600,true,995000,200000000,-200,0,10000,T);
    check(p.stats().rawRequiredQuantileUs==-800 && p.stats().targetUs==0 && p.stats().missed==0,
        "negative causal input remains early and cannot become an unsigned tolerance miss");
    p.reset(B);feed(p,1,3000000,3000000,10000,true,995000,200000000,0,0,6000,T);
    check(p.stats().targetUs==6000 && p.stats().clipped && p.stats().headroomTargetUnclippedUs==9250,
        "tolerance subtraction precedes maximum-delay clipping");
    p.reset(B);feed(p,1,4000000,4000000,std::numeric_limits<int64_t>::max(),true,995000,200000000,0,0,std::numeric_limits<uint64_t>::max(),T);
    check(p.stats().targetUs==uint64_t(std::numeric_limits<int64_t>::max())-T,
        "large signed positive input subtracts tolerance without overflow");
    p.reset(std::numeric_limits<uint64_t>::max());
    feed(p,1,5000000,5000000,std::numeric_limits<int64_t>::max(),true,995000,200000000,0,0,std::numeric_limits<uint64_t>::max(),std::numeric_limits<uint64_t>::max());
    check(p.stats().targetUs==0 && p.stats().missed==0,
        "maximum tolerance and buffer do not wrap the accepted boundary into a miss");
}
void testStartupLearning() {
    InputPercentileBuffer p;p.reset(0);
    check(p.learning(),"a fresh buffer starts learning");
    feed(p,1,1000000,1000000,9000,false);
    check(p.demand(0)==0,"learning never grows from ineligible input");
    p.advance(1050000);
    check(p.demand(0)==0,"learning never grows at decision time");
    feed(p,2,1100000,1100000,2000);
    check(p.demand(0)==2000,"before the tail is supported, protection follows the largest eligible requirement at once");
    feed(p,3,1110000,1110000,3500);
    check(p.demand(0)==3500 && p.learning(),"a larger requirement while learning is protected at once");
    for(uint64_t i=4;i<=101;++i) feed(p,i,1110000+i*1000,1110000+i*1000,500);
    check(!p.learning() && !p.stats().tailUncertain,"a supported profile tail ends learning");
    const auto held=p.demand(0);
    feed(p,102,1300000,1300000,6000);feed(p,103,1400000,1400000,6000);
    check(p.demand(0)>held && p.demand(0)<=held+101,"after learning, growth is slewed at 1000 us per second");
    p.breakSequence();
    check(p.learning(),"a source rebase discards evidence and relearns");
}
void testSlewBoundsAndReset() {
    InputPercentileBuffer p;p.reset(0);p.endLearning();
    feed(p,1,1000000,1000000,2000);feed(p,2,1100000,1100000,2000);
    check(p.demand(0)==100,"experimental upward slew is 1000 us per second");
    p.reset(1000);feed(p,1,1000000,1000000,0);feed(p,2,1100000,1100000,0);
    check(p.demand(0)==975,"downward slew is 250 us per second");
    auto before=p.demand(0);p.breakSequence();
    check(p.demand(0)==before && p.stats().samples==0,"source epoch clears evidence while retaining applied delay");
    feed(p,1,2000000,1000000,9000,true,990000,30000000,0,500,800);
    check(p.demand(0)>=500 && p.demand(0)<=800 && p.stats().targetUs==800 && p.stats().clipped,"changing dynamic bounds clamp current and target");
    feed(p,2,1999999,1008333,100);
    check(p.stats().samples==0,"nonmonotonic observation clock invalidates evidence");
}
void testInvalidCapacityAndExpiry() {
    InputPercentileBuffer p;p.reset(6000);
    feed(p,1,1000000,1000000,5000);
    feed(p,2,1100000,1010000,25000,false);
    check(p.stats().samples==1 && p.stats().evidenceValid && p.stats().eligible==1 && p.stats().targetUs==5000,"unabsorbable service cannot add percentile pressure");
    feed(p,3,1200000,1020000,5000);
    check(p.stats().validPairCoverageUs==0,"invalid eligibility cannot bridge input pair coverage");
    // Separate high-delay fixture: prolonged unknown input must not pin delay.
    p.reset(6000);
    for(uint64_t i=1;i<=601;++i) feed(p,i,i*100000,i*8333,25000,false);
    check(p.stats().expiredUnknown && !p.stats().evidenceValid && p.demand(0)==0,"expired capacity evidence recovers to floor without claiming clean playback");
    p.reset(6000);
    for(uint64_t i=1;i<=601;++i) {
        if(i%10==0) p.breakSequence();
        feed(p,i,i*100000,i*8333,25000,false);
    }
    check(p.demand(0)==0 && p.stats().epochResets==60 && p.stats().unknownRelease>0,
        "repeated invalid source epochs do not rearm unknown evidence retention indefinitely");
    p.reset(1600);feed(p,1,1000000,1000000,1000);feed(p,3,1100000,1020000,1000);
    check(p.stats().validPairCoverageUs==0,"missing frame identity does not bridge qualified pairs");
}
void testHistoryAgingWithoutEligibleInput() {
    InputPercentileBuffer p;p.reset(1000);
    feed(p,1,1000000,1000000,2000,true,990000,10000000);
    feed(p,2,2000000,1010000,100,true,990000,10000000);
    check(p.stats().recentEligible==2 && p.stats().recentMissed==1 && p.stats().recentAbsorbed==1,
        "recent input outcomes cover eligible observations in the active history");
    feed(p,3,11000000,1020000,9000,false,990000,10000000);
    auto stats=p.stats();
    check(stats.samples==1 && stats.rawRequiredQuantileUs==100 && stats.targetUs==100,
        "oldest high input expires at exact window boundary during invalid observation while retained low input drives target");
    check(stats.recentEligible==1 && stats.recentMissed==0 && stats.recentAbsorbed==1,
        "recent absorbed and missed counts age with samples even during invalid capacity");
    feed(p,4,12000000,1030000,9000,false,990000,10000000);
    stats=p.stats();
    check(stats.samples==0 && stats.rawRequiredQuantileUs==0 && stats.headroomTargetUnclippedUs==0 &&
        stats.observationSpanUs==0 && stats.validPairCoverageUs==0,
        "empty expired history clears stale quantile, coverage and headroom diagnostics");
    check(stats.recentEligible==0 && stats.recentAbsorbed==0 && stats.recentMissed==0 && stats.absorbed==1 && stats.missed==1,
        "recent counters expire while cumulative absorbed and missed counters remain explicit");
    p.breakSequence();check(p.stats().recentEligible==0 && p.stats().absorbed==1 && p.stats().missed==1,
        "basis break clears recent evidence without erasing cumulative outcomes");
    p.reset();check(p.stats().absorbed==0 && p.stats().missed==0,
        "full reset clears cumulative and recent input outcome history");

    p.reset(1000);
    feed(p,1,1000000,1000000,2000,true,990000,30000000);
    feed(p,2,2000000,1010000,100,true,990000,30000000);
    const auto observed=p.stats().observed;
    p.advance(11000000,0,6000,990000,30000000,1000,250);
    check(p.stats().samples==2 && p.stats().targetUs==2000 && p.stats().observed==observed,
        "idle decision advance ages and slews without inventing a newly observed frame");
    p.advance(11000001,0,6000,990000,10000000,1000,250);
    check(p.stats().samples==1 && p.stats().targetUs==100,
        "shorter profile history takes effect immediately during idle decision advance");
    p.advance(11000002,0,6000,990000,30000000,1000,250);
    check(p.stats().samples==1 && p.stats().targetUs==100,
        "longer profile history cannot resurrect already expired high input");
    p.advance(32000000);
    check(p.stats().samples==0 && p.stats().observed==observed,
        "idle advance expires all input without changing observed or eligible outcome counts");
    feed(p,3,32000000,1020000,300,true,990000,30000000);
    check(p.stats().observed==observed+1 && p.stats().samples==1,
        "same-time observation after decision advance is accepted without clock reset");

    p.reset(1000);
    for(uint64_t i=1;i<=100;++i) feed(p,i,1000000+i*10000,1000000+i*10000,i==1?2000:100,true,990000,30000000);
    check(p.stats().targetUs==100,"supported p99 sample mixture starts at lower quantile");
    feed(p,101,2100000,2010000,9000,false,999900,30000000);
    check(p.stats().targetUs==2000 && p.stats().tailUncertain && p.stats().samples==100,
        "strict profile probability applies to retained history even with no new eligible input");
}
void testNoRunawayAndQualifiedGrowth() {
    InputPercentileBuffer p;p.reset(0);p.endLearning();
    feed(p,0,1000000,1000000,4000,true,990000,10000000);
    p.advance(1100000);
    check(p.stats().targetUs==4000 && p.demand(0)==0 && p.stats().growthBlocked,"decision advance cannot raise buffer from retained high input requirements");
    feed(p,1,1200000,1010000,9000,false,990000,10000000);
    check(p.demand(0)==0 && p.stats().samples==1 && p.stats().growthBlocked &&
        !p.stats().lastInputEligible && p.stats().lastInputRequiredUs==9000 && p.stats().lastInputObservationUs==1200000,"invalid overloaded input cannot raise buffer from historical high target");
    feed(p,2,1300000,1020000,4000,true,990000,10000000);
    check(p.demand(0)==100 && !p.stats().growthBlocked && p.stats().lastInputEligible,"qualified observation grows only for its bounded observation interval, without banked invalid time");
    p.reset(0);p.endLearning();feed(p,0,1000000,1000000,4000,true,990000,10000000);
    for(uint64_t i=1;i<=120;++i) {
        const uint64_t at=1000000+i*8333;
        p.advance(at);feed(p,i,at,at,4000,true,990000,10000000);
    }
    check(p.demand(0)==999,"interleaved decision advances preserve fractional 1000 us per second eligible growth");
    const auto before=p.demand(0);
    feed(p,121,1000000+121*8333,1000000+121*8333,9000,false,990000,10000000);
    feed(p,122,1000000+122*8333,1000000+122*8333,4000,true,990000,10000000);
    check(p.demand(0)==before+8,"invalid observation clears partial positive credit rather than banking a later burst");

    p.reset(1000);
    for(uint64_t i=1;i<=100;++i) feed(p,i,1000000+i*10000,1000000+i*10000,1000,true,990000,10000000);
    check(p.stats().targetUs==p.stats().currentUs,"qualified input fixture begins at direct hold equilibrium");
    for(uint64_t i=101;i<=300;++i) feed(p,i,1000000+i*10000,1000000+i*10000,3000,true,990000,10000000);
    check(p.stats().targetUs==3000 && p.demand(0)>1000 && p.stats().raises>0,
        "genuine repeated eligible input headroom violations grow from hold without legacy quality-debt hold blocking attack");
    for(uint64_t i=301;i<=4000;++i) feed(p,i,1000000+i*10000,1000000+i*10000,9000,true,990000,10000000);
    check(p.demand(0)==6000 && p.stats().targetUs==6000 && p.stats().clipped,
        "sustained unbounded input tail saturates at dynamic maximum without runaway delay");
    for(uint64_t i=4001;i<=8000;++i) feed(p,i,1000000+i*10000,1000000+i*10000,0,true,990000,10000000);
    check(p.demand(0)==0 && p.stats().targetUs==0,
        "saturated input protection ages out and recovers after a genuine clean burst transition");

    p.reset(1500);feed(p,1,1000000,500000000,500,true,990000,10000000,200);
    const auto raw=p.stats().rawRequiredQuantileUs;
    p.breakSequence();feed(p,2,1100000,900000000,100,true,990000,10000000,600);
    check(p.stats().rawRequiredQuantileUs==raw && p.stats().targetUs==100,
        "own readiness budget and source epoch rebase do not manufacture new raw input headroom");
}
void testUnthrottledInputPercentileRelease() {
    InputPercentileBuffer p;p.reset(6000);
    for(uint64_t i=1;i<=400;++i) {const auto at=1000000+i*1000000/60;
        p.observe(i,at,at,i==400?7000:0,true,0,10000,995000,200000000,1000,0);}
    const uint64_t end=1000000+400ULL*1000000/60;
    p.noteBufferMiss(end,true,true);p.advance(end,0,10000,995000,200000000,1000,250);
    check(p.stats().targetUs==0 && p.stats().recentReleaseMisses==1 && p.stats().releaseFactorPpm==1000000 &&
        p.stats().effectiveFallRateUsPerSecond==250 && p.stats().holdUntilUs==0,
        "trajectory experiment retains long quantile target and diagnostics while disabling both hard pause and soft fall heuristic");
    for(uint64_t elapsed=100000;elapsed<=24000000;elapsed+=100000) p.advance(end+elapsed);
    check(p.demand(0)==0,"6000us protection releases to floor in24s at unchanged250us/s without short heuristic");
}
void testProbabilitySupportAndCap() {
    InputPercentileBuffer p;p.reset(0);
    for(uint64_t i=1;i<=199;++i) feed(p,i,i*10000,i*10000,i==1?2000:100,true,995000);
    check(p.stats().tailUncertain,"99.5 percent probability needs two hundred samples");
    feed(p,200,2000000,2000000,100,true,995000);
    check(!p.stats().tailUncertain && p.stats().targetUs==100,"99.5 percent support threshold and nearest rank are exact");
    p.reset(0);
    for(uint64_t i=1;i<=3600;++i) feed(p,i,i*8333,i*8333,100,true,999900);
    check(p.stats().tailUncertain,"99.99 percent tail remains uncertain at 120 Hz in thirty seconds");
    p.reset(0);
    for(uint64_t i=1;i<=InputPercentileBuffer::MaximumSamples+1;++i) feed(p,i,i*1000,i*1000,100,true,990000,300000000);
    check(p.stats().samples==InputPercentileBuffer::MaximumSamples && p.stats().historyTruncated,"hard sample cap bounds malformed rates");
    feed(p,InputPercentileBuffer::MaximumSamples+2,262200000,262200000,100,true,990000,300000000);
    check(p.stats().tailUncertain,"history truncation remains explicit in target uncertainty");
}
#ifdef INPUT_PERCENTILE_CONTROLLER_TEST
void testFactoryProfileOwnsAdaptiveTarget() {
    for(int mode:{0,1,2}) {
        for(int custom:{0,9900,9950,9999,-1,12,10000,999999}) {
            VrrSessionConfig session;session.streamRateHz=120;session.displayRefreshHz=144;
            session.latencyMode=mode;session.timingOptions.targetHundredths=custom;
            session.timingOptions.historySeconds=custom==0?0:200;
            session.experimentalInputPercentileWindowUs=30000000;
            const auto expected=uint64_t(session.timingOptions.resolved(mode).targetHundredths)*100;
            const auto disabled=vrrTimingParametersForSession(session);
            check(disabled.playoutInputPercentileToleranceUs==0,"disabled adaptive mode retains serialized zero-tolerance compatibility");
            check(!disabled.playoutInputPercentileEnabled && disabled.playoutOnTimeTargetPerMillion==expected,
                "preset/custom resolution preserves ordinary severity policy when adaptive mode is disabled");
            session.experimentalInputPercentilePpm=999900;
            const auto enabled=vrrTimingParametersForSession(session);
            check(enabled.playoutInputPercentileEnabled && enabled.playoutInputPercentileTargetPerMillion==expected,
                "adaptive mode uses effective preset/custom target including lower and upper clamps");
            check(enabled.playoutInputPercentileToleranceUs==uint64_t(session.timingOptions.resolved(mode).toleranceUs),
                "adaptive input percentile uses the effective session tolerance without rewriting profile selection");
            check(enabled.playoutInputPercentileWindowUs==uint64_t(session.timingOptions.resolved(mode).historySeconds)*1000000,
                "adaptive history uses the effective preset/custom profile rather than an environment history override");
            check(enabled.playoutOnTimeTargetPerMillion==expected,
                "adaptive activation preserves selected target in interval severity diagnostics");
        }
    }
    VrrSessionConfig user;user.experimentalInputPercentilePpm=1;user.timingOptions.toleranceUs=750;
    user.timingOptions.targetHundredths=9950;user.timingOptions.historySeconds=200;
    const auto userPolicy=vrrTimingParametersForSession(user);
    check(userPolicy.playoutInputPercentileToleranceUs==750 && userPolicy.playoutInputPercentileTargetPerMillion==995000 &&
        userPolicy.playoutInputPercentileWindowUs==200000000,"actual750us user profile sizes the adaptive quantile without changing99.50percent/200s");
    for(int history:{0,1,10,200,300,999}) {
        VrrSessionConfig session;session.latencyMode=1;session.experimentalInputPercentilePpm=1;
        session.timingOptions.historySeconds=history;session.experimentalInputPercentileWindowUs=30000000;
        const auto policy=vrrTimingParametersForSession(session);
        check(policy.playoutInputPercentileWindowUs==uint64_t(session.timingOptions.resolved(1).historySeconds)*1000000,
            "adaptive profile history includes exact supported bounds and lower/upper clamping");
    }
}
void testActualControllerRateBandsRestoreProtection() {
    // The user's round trip: 120 FPS needs buffer, 30 FPS needs none (and has
    // less lateness, as in the 22:55 capture), then back to 120 FPS.
    VrrSessionConfig session;session.streamRateHz=120;session.displayRefreshHz=144;
    session.smoothFrameTiming=false;session.experimentalInputPercentilePpm=995000;
    session.timingOptions={1000,9950,60,500};
    auto policy=vrrTimingParametersForSession(session);
    policy.playoutDelayStartSeedUs=1000;policy.playoutDelayMinimumUs=1000;policy.playoutDelayMaximumUs=12000;
    policy.playoutDelayMaximumPeriodPerMille=0;policy.playoutDelayCapSourcePeriodPerMille=0;
    VrrTimingController controller(session,true,policy);
    uint64_t submitted=0,slot=1000000,pts=0;int i=0;
    const auto run=[&](int frames,uint64_t periodUs,uint32_t ptsStep,uint64_t spikeUs) {
        for(int n=0;n<frames;++n,++i) {
            slot+=periodUs;pts+=ptsStep;
            const uint64_t decoded=slot+1000+(i%20==0?spikeUs:0);
            PacedFrame f(nullptr,i,uint32_t(pts),true,decoded);
            f.setDeliveryTimeline(decoded-800,decoded-600,decoded-500);
            const auto decision=controller.schedule(f,std::max(decoded,submitted));
            const uint64_t ready=std::max(decoded,decision.renderStartUs)+500;
            submitted=std::max(ready,decision.targetUs);
            controller.notePreparationDuration(500,0,ready);
            controller.noteSchedulerDelays(0,0,true);
            controller.noteSubmission(true,false,submitted,submitted);
        }
    };
    run(2400,8333,750,5000);
    const auto fast=controller.intervalStats().inputPercentile;
    run(600,33333,3000,1000);
    const auto slow=controller.intervalStats().inputPercentile;
    run(36,8333,750,5000);
    const auto back=controller.intervalStats().inputPercentile;
    run(564,8333,750,5000);
    const auto settled=controller.intervalStats().inputPercentile;
    const uint64_t missedBack=settled.missed-slow.missed,eligibleBack=settled.eligible-slow.eligible;
    std::fprintf(stderr,"rate bands: 120 FPS current %llu target %llu; 30 FPS current %llu; back +36 frames current %llu; "
        "missed after return %llu of %llu; coverage %llu/%llu\n",
        (unsigned long long)fast.currentUs,(unsigned long long)fast.targetUs,(unsigned long long)slow.currentUs,
        (unsigned long long)back.currentUs,(unsigned long long)missedBack,(unsigned long long)eligibleBack,
        (unsigned long long)(settled.recentEligible-settled.recentMissed),(unsigned long long)settled.recentEligible);
    check(settled.eligible>=slow.eligible && slow.eligible>=fast.eligible && settled.missed>=slow.missed,
          "session totals continue across rate-band layers");
    check(fast.currentUs>2000,"fixture: 120 FPS jitter needs real buffer");
    check(slow.currentUs<fast.currentUs,"30 FPS headroom releases protection");
    check(slow.recentEligible>0 && slow.recentEligible<=600 && slow.recentMissed==0,
          "the 30 FPS layer's coverage counts only its own frames");
    check(back.currentUs+250>=fast.currentUs,"returning to 120 FPS restores that band's protection within a few frames");
    check(eligibleBack>400 && missedBack*200<=eligibleBack,
          "frames after the return are covered at the profile target, not relearned through misses");
    check((settled.recentEligible-settled.recentMissed)*1000>=settled.recentEligible*995,
          "coverage after the return keeps the band's history instead of restarting from misses");
}
void testActualControllerInputMissReleaseFeedbackApi() {
    VrrSessionConfig session;session.streamRateHz=60;session.displayRefreshHz=120;
    VrrTimingController ordinary(session);
    ordinary.noteInputBufferMiss(1000000,true,true);
    check(ordinary.intervalStats().inputPercentile.cumulativeNetworkMisses==0 &&
        ordinary.intervalStats().inputPercentile.cumulativeDecoderMisses==0,
        "input buffer-line pause API has no effect on ordinary production mode");
    session.experimentalInputPercentilePpm=1;session.timingOptions.targetHundredths=9950;
    session.timingOptions.historySeconds=200;
    VrrTimingController experimental(session,true,vrrTimingParametersForSession(session));
    experimental.noteInputBufferMiss(1000000,true,false);
    auto stats=experimental.intervalStats().inputPercentile;
    check(stats.holdUntilUs==0 && stats.holdRemainingUs==0 && stats.cumulativeNetworkMisses==1 && stats.cumulativeDecoderMisses==0,
        "actual opt-in controller records network buffer-line diagnostics without a hard pause");
    experimental.noteInputBufferMiss(6000000,false,true);
    stats=experimental.intervalStats().inputPercentile;
    check(stats.holdUntilUs==0 && stats.holdRemainingUs==0 && stats.cumulativeNetworkMisses==1 && stats.cumulativeDecoderMisses==1,
        "actual opt-in controller independently records decode buffer-line diagnostics");
    experimental.noteInputBufferMiss(7000000,false,false);
    check(experimental.intervalStats().inputPercentile.holdUntilUs==0,
        "actual controller submission/presentation-only errors cannot arm a hard pause");
    experimental.rebase();
    check(experimental.intervalStats().inputPercentile.holdUntilUs==0 && experimental.intervalStats().inputPercentile.cumulativeNetworkMisses==1 && experimental.intervalStats().inputPercentile.recentReleaseSamples==0,
        "actual source rebase clears short feedback and preserves stage totals without a hard pause");
    experimental.reset();
    check(experimental.intervalStats().inputPercentile.holdUntilUs==0 && experimental.intervalStats().inputPercentile.cumulativeNetworkMisses==0,
        "actual full controller reset clears input diagnostics");
}
void testActualControllerRepeatedZeroTimestampDrops() {
    VrrSessionConfig session;session.streamRateHz=60;session.displayRefreshHz=120;
    session.smoothFrameTiming=false;session.experimentalInputPercentilePpm=990000;
    session.experimentalInputPercentileWindowUs=30000000;
    session.timingOptions.historySeconds=30;
    auto policy=vrrTimingParametersForSession(session);
    policy.playoutDelayStartSeedUs=6000;
    policy.playoutDelayMinimumUs=1000;policy.playoutDelayMaximumUs=6000;
    policy.playoutDelayMaximumPeriodPerMille=0;
    policy.playoutDelayCapSourcePeriodPerMille=0;
    VrrTimingController controller(session,true,policy);
    uint64_t initial=0;
    for(int i=0;i<6000;++i) {
        if(i && i%60==0) controller.rebase();
        const uint64_t now=1000000+uint64_t(i)*1000000/60;
        PacedFrame frame(nullptr,i,uint32_t(uint64_t(i)*1500),true,now);
        // No preparation proof and no accepted/CPU submission instant.
        const auto decision=controller.schedule(frame,now);
        if(!i) initial=decision.playoutDelayUs;
        controller.noteSubmission(false,false,0);
    }
    const auto stats=controller.intervalStats().inputPercentile;
    check(initial>stats.minimumUs+2000,"zero-timestamp drop fixture starts with meaningful seeded protection");
    check(stats.observed>5000 && stats.eligible==0 && stats.ineligible==stats.observed,
        "actual controller observes dropped frames on scheduling clock without treating them as valid input");
    check(stats.expiredUnknown && stats.currentUs==stats.minimumUs && stats.unknownRelease>0 && stats.epochResets>50,
        "repeated zero-timestamp drops and phase resets release obsolete seed after history expiry and fall slew");
}
void testActualControllerPostInputErrorsCannotInflateBuffer() {
    VrrSessionConfig session;session.streamRateHz=60;session.displayRefreshHz=120;
    session.latencyMode=1;session.smoothFrameTiming=false;session.experimentalInputPercentilePpm=1;
    session.timingOptions={1000,9950,10,500};
    auto policy=vrrTimingParametersForSession(session);
    policy.playoutDelayStartSeedUs=1000;policy.playoutDelayMinimumUs=1000;
    policy.playoutDelayMaximumUs=6000;
    VrrTimingController clean(session,true,policy),outputErrors(session,true,policy);
    uint64_t peak=0;
    for(int i=0;i<2400;++i) {
        const uint64_t decoded=1000000+uint64_t(i)*1000000/60;
        for(auto* controller:{&clean,&outputErrors}) {
            if(i && i%600==0) controller->rebase();
            PacedFrame frame(nullptr,i,uint32_t(uint64_t(i)*1500),true,decoded);
            frame.setDeliveryTimeline(decoded-800,decoded-600,decoded-500);
            const auto decision=controller->schedule(frame,decoded);
            const uint64_t ready=std::max(decoded,decision.renderStartUs)+500;
            uint64_t submission=std::max(ready,decision.targetUs);
            if(controller==&outputErrors && i>120) submission+=i%2?4000:0;
            controller->notePreparationDuration(500,0,ready);
            controller->noteSchedulerDelays(0,0,true);
            controller->noteSubmission(true,false,submission,submission);
        }
        const auto baseline=clean.intervalStats().inputPercentile;
        const auto errored=outputErrors.intervalStats().inputPercentile;
        peak=std::max(peak,errored.currentUs);
        check(errored.currentUs<=baseline.currentUs && errored.targetUs<=baseline.targetUs,
            "post-input compositor/submission interval errors cannot inflate direct causal input headroom");
    }
    const auto a=clean.intervalStats(),b=outputErrors.intervalStats();
    check(b.inputPercentile.eligible>1000 && peak==1000 && b.qualityPercent()<a.qualityPercent(),
        "integrated fixture reports repeated output timing failures while input remains within unchanged headroom");
    check(b.inputPercentile.rawRequiredQuantileUs==a.inputPercentile.rawRequiredQuantileUs,
        "own buffering and rebases preserve raw causal input requirement under downstream errors");
}
void testActualControllerOptInAndEligibility() {
    VrrSessionConfig baseline;baseline.streamRateHz=120;baseline.displayRefreshHz=144;
    baseline.smoothFrameTiming=false;
    VrrTimingController production(baseline);
    check(!production.intervalStats().inputPercentileEnabled,"default production controller keeps percentile mode disabled");
    auto session=baseline;session.experimentalInputPercentilePpm=995000;
    session.experimentalInputPercentileWindowUs=60000000;
    auto policy=vrrTimingParametersForSession(session);
    check(policy.playoutInputPercentileEnabled && policy.playoutInputPercentileTargetPerMillion==
        uint64_t(session.timingOptions.resolved(session.latencyMode).targetHundredths)*100,
        "explicit activation resolves the selected profile target instead of an activation marker probability");
    check(policy.playoutOnTimeTargetPerMillion==vrrTimingParametersForSession(baseline).playoutOnTimeTargetPerMillion,
        "percentile opt-in does not rewrite severity quality target");
    auto hitchSession=session;hitchSession.readinessHitchFeedback=true;
    const auto hitchPolicy=vrrTimingParametersForSession(hitchSession);
    auto hitchBaseline=baseline;hitchBaseline.readinessHitchFeedback=true;
    check(hitchPolicy.playoutResponsiveBuffer==12 && hitchPolicy.playoutInputPercentileEnabled,
        "explicit percentile mode selects qualified interval service path with readiness hitch feedback");
    check(hitchPolicy.playoutOnTimeTargetPerMillion==vrrTimingParametersForSession(hitchBaseline).playoutOnTimeTargetPerMillion,
        "readiness hitch opt-in preserves the existing severity target");
    VrrTimingController controller(session,true,policy);
    uint64_t submitted=0;
    for(int i=0;i<1500;++i) {
        const uint64_t decoded=1000000+uint64_t(i)*1000000/120;
        PacedFrame f(nullptr,i,uint32_t(uint64_t(i)*750),true,decoded);
        f.setDeliveryTimeline(decoded-800,decoded-600,decoded-500);
        const auto decision=controller.schedule(f,std::max(decoded,submitted));
        const uint64_t ready=std::max(decoded,decision.renderStartUs)+500;
        submitted=std::max(ready,decision.targetUs);
        controller.notePreparationDuration(500,0,ready);
        controller.noteSchedulerDelays(0,0,true);
        controller.noteSubmission(true,false,submitted,submitted);
    }
    auto stats=controller.intervalStats();
    check(stats.inputPercentile.toleranceUs==policy.playoutInputPercentileToleranceUs,
        "actual controller feeds selected input tolerance without changing causal eligibility qualification");
    check(stats.inputPercentileEnabled && stats.inputPercentile.observed>1000 && stats.inputPercentile.eligible>1000,
        "actual controller feeds causal timeline input after qualified service calibration");
    check(stats.update.requestedUs==stats.inputPercentile.currentUs &&
        stats.update.minimumUs==stats.inputPercentile.minimumUs && stats.update.maximumUs==stats.inputPercentile.maximumUs,
        "published interval request follows direct percentile demand and dynamic bounds");
    const auto eligible=stats.inputPercentile.eligible;
    const auto samples=stats.inputPercentile.samples;
    const uint64_t decoded=1000000+1500ULL*1000000/120;
    PacedFrame lost(nullptr,1500,1500*750,true,decoded);
    lost.setDeliveryTimeline(decoded-800,decoded-600,decoded-500);lost.setLostPackets(1);
    const auto decision=controller.schedule(lost,std::max(decoded,submitted));
    const uint64_t ready=std::max(decoded,decision.renderStartUs)+500;
    submitted=std::max(ready,decision.targetUs);
    controller.notePreparationDuration(500,0,ready);controller.noteSchedulerDelays(0,0,true);
    controller.noteSubmission(true,false,submitted,submitted);
    stats=controller.intervalStats();
    check(stats.inputPercentile.eligible==eligible && stats.inputPercentile.samples==samples,
        "lost packets exclude percentile learning in actual production controller");
    const auto epoch=stats.inputPercentile.epochResets;
    const auto current=stats.inputPercentile.currentUs;
    controller.rebase();stats=controller.intervalStats();
    check(stats.inputPercentile.samples==0 && stats.inputPercentile.currentUs==current && stats.inputPercentile.epochResets>epoch,
        "controller source epoch clears percentile basis while preserving protection");
    controller.reset();check(!controller.intervalStats().inputPercentile.initialized,
        "full controller reset clears direct control state for next activation");
}
#endif

}
int main() {
    testVrrHeadroomAbsorbsLateness();testPhaseBreakKeepsEvidenceAndHeadroomLossRestores();testStartupLearning();
    testQuantileAndClock();testBudgetAndSignedOffsets();testInputToleranceBoundaryAndNormalization();testSlewBoundsAndReset();
    testInvalidCapacityAndExpiry();testProbabilitySupportAndCap();testHistoryAgingWithoutEligibleInput();testNoRunawayAndQualifiedGrowth();testUnthrottledInputPercentileRelease();
#ifdef INPUT_PERCENTILE_CONTROLLER_TEST
    testFactoryProfileOwnsAdaptiveTarget();
    testActualControllerOptInAndEligibility();
    testActualControllerRateBandsRestoreProtection();
    testActualControllerInputMissReleaseFeedbackApi();
    testActualControllerRepeatedZeroTimestampDrops();
    testActualControllerPostInputErrorsCannotInflateBuffer();
#endif
    std::fprintf(stderr,"input percentile lifecycle integration tests: %s (%d failures)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
