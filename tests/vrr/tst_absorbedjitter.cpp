// Standalone: g++ -std=c++17 -O2 tests/vrr/tst_absorbedjitter.cpp -o tst_absorbedjitter
// Input-only deterministic counterfactual; no GPU/display or network model.
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/intervalbuffer.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using Vrr13::AbsorbedJitter;
using Vrr13::IntervalBuffer;
namespace {
int failures = 0;
void check(bool ok, const char* what) { if (!ok) { ++failures; std::fprintf(stderr,"FAIL: %s\n",what); } }
void unitTests() {
    AbsorbedJitter j;
    for (uint64_t i=1;i<=401;++i) j.observe(i,1000000+i*10000,1000000+i*10000,2000,true);
    check(j.qualified(5010000),"constant offset qualifies");
    check(j.loss(0,500)==0 && j.loss(1000,500)==0,"constant lateness cancels at all buffer levels");
    j.reset();
    for (uint64_t i=1;i<=151;++i) j.observe(i,1000000+i*10000,1000000+i*10000,i%2?2000:0,true);
    check(std::abs(j.loss(1000,500)-0.05)<1e-12,"both late and catch-up count severity after tolerance");
    check(j.loss(2000,500)==0,"sufficient buffer absorbs alternating lateness");
    check(j.loss(1500,500)==0,"tolerance is per interval");
    check(!j.qualified(10000000),"stale evidence expires");
    j.observe(152,10000001,10000001,0,false);
    check(j.coverage()==0,"invalid input clears evidence");
    j.observe(1,11000000,11000000,0,true); j.observe(2,11010000,11010000,1000,true);
    j.observe(4,11020000,11020000,0,true);
    check(j.coverage()==0,"missing identity clears evidence");
    j.observe(5,11030000,11030000,-1000,true);
    check(j.loss(0,500)==0,"early input produces no residual lateness");
    j.observe(6,11020000,11040000,2000,true);
    check(j.coverage()==0,"nonmonotonic observation clears evidence");
}
struct Result {
    uint64_t n=0, misses=0, excess=0, totalTime=0, reversals=0, held=0, unknown=0, allowed=0, recoveredAt=0;
    uint64_t finalBuffer=0, maxError=0;
    double weightedLoss=0, finalWindowQuality=100;
    std::vector<uint64_t> buffers, inputToSubmission;
};
struct Scenario { const char* name; int seconds; };
int64_t required(const std::string& name,uint64_t i,double t) {
    if(name=="stable") return int64_t(i%7)*20-60;
    if(name=="constant_lateness") return 2500;
    if(name=="sustained") return i%2?1400:0;
    if(name=="bursts") return (int(t)%12<3 && i%2)?1600:0;
    if(name=="recovery" || name=="long_recovery") return t<40 && i%2?1400:0;
    if(name=="cadence_reset") return t<30 && i%2?1400:0;
    if(name=="gaps") return i%2?1400:0;
    if(name=="cold_reset" || name=="cold_start" || name=="invalid_input") return i%2?1400:0;
    if(name=="stalls") return i%2400==0?40000:0;
    if(name=="oscillation") return (int(t)%20<10 && i%2)?1400:0;
    return 0;
}
Result simulate(Scenario scenario,bool gate,FILE* timeline) {
    IntervalBuffer buffer;
    Result r;
    uint64_t slot=1000000, frame=0, previousSlot=0, previousSubmission=0, applied=1600;
    if(std::string(scenario.name)=="cold_start") applied=0;
    int previousDirection=0;
    bool cadenceReset=false,coldReset=false;
    for(uint64_t i=1;;++i) {
        const double t=double(slot-1000000)/1000000;
        if(t>=scenario.seconds) break;
        const std::string name=scenario.name;
        if(name=="cadence_reset" && t>=30 && !cadenceReset) {
            buffer.breakSequence(); cadenceReset=true; previousSubmission=0;
        }
        if(name=="cold_reset" && t>=30 && !coldReset) {
            buffer.reset(); coldReset=true; applied=1600; previousSubmission=0;
        }
        uint64_t period=name=="cadence_reset"&&t>=30?16667:8333;
        if(name=="gaps" && i%1200==0) {
            slot+=1200000; frame+=144; previousSubmission=0;
        }
        slot+=period; ++frame;
        const int64_t input=required(name,i,t);
        const uint64_t ready=input>=0?slot+uint64_t(input):slot-uint64_t(-input);
        const uint64_t deadline=slot+applied;
        const uint64_t submission=std::max(std::max(deadline,ready),previousSubmission?previousSubmission+1000:0);
        const bool inputValid=name!="invalid_input" || i%60!=0;
        const bool absorbable=name!="stalls" || i%2400!=0;
        if(previousSubmission) {
            const uint64_t actual=submission-previousSubmission,intended=slot-previousSlot;
            const uint64_t error=actual>intended?actual-intended:intended-actual;
            ++r.n; r.totalTime+=actual; r.maxError=std::max(r.maxError,error);
            if(error>500) { ++r.misses; r.excess+=error-500; }
            r.weightedLoss+=actual*std::min(1.0,std::max(0.0,double(error)-500)/intended);
        }
        buffer.observe({frame,slot,submission,deadline,ready,applied,true,absorbable,1000,0,slot,input,inputValid},
            0,6000,8000000,250,true,990000,500,30000000,1000000,2,3,2,3,true,false,true,gate);
        const auto stats=buffer.stats();
        const auto next=buffer.demand(applied);
        int direction=next>applied?1:next<applied?-1:0;
        if(direction && previousDirection && direction!=previousDirection) ++r.reversals;
        if(direction) previousDirection=direction;
        r.buffers.push_back(applied);
        r.inputToSubmission.push_back(submission-ready);
        if(!r.recoveredAt && t>=40 && applied==0) r.recoveredAt=slot-1000000;
        if(timeline && i%120==0) std::fprintf(timeline,"%s,%s,%.6f,%llu,%llu,%s\n",scenario.name,gate?"new":"old",t,
            (unsigned long long)applied,(unsigned long long)stats.update.intervalErrorUs,IntervalBuffer::actionName(stats.update.action));
        r.finalWindowQuality=stats.qualityPercent();
        r.held=stats.inputReleaseHeld;r.unknown=stats.inputReleaseUnknown;r.allowed=stats.inputReleaseAllowed;
        previousSlot=slot; previousSubmission=submission;applied=next;
    }
    r.finalBuffer=applied; return r;
}
uint64_t quantile(std::vector<uint64_t> values,double p) {
    std::sort(values.begin(),values.end()); return values[size_t(p*(values.size()-1))];
}
void report(FILE* file,const char* name,bool gate,const Result& r) {
    double mean=0;for(auto v:r.buffers)mean+=v;mean/=r.buffers.size();
    double latencyMean=0;
    for(auto v:r.inputToSubmission) latencyMean+=v;
    latencyMean/=r.inputToSubmission.size();
    std::fprintf(file,"%s,%s,%llu,%llu,%llu,%llu,%.6f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.3f,%.6f,%.3f,%llu,%llu\n",name,gate?"new":"old",
        (unsigned long long)r.n,(unsigned long long)r.misses,(unsigned long long)r.excess,(unsigned long long)r.maxError,
        100*(1-r.weightedLoss/r.totalTime),mean,(unsigned long long)quantile(r.buffers,.5),(unsigned long long)quantile(r.buffers,.95),
        (unsigned long long)r.finalBuffer,(unsigned long long)r.reversals,(unsigned long long)r.held,(unsigned long long)r.unknown,
        (unsigned long long)r.allowed,double(r.recoveredAt)/1000000,r.finalWindowQuality,latencyMean,
        (unsigned long long)quantile(r.inputToSubmission,.5),(unsigned long long)quantile(r.inputToSubmission,.95));
}
}
int main(int argc,char** argv) {
    unitTests();
    FILE* csv=stdout; FILE* timeline=nullptr;
    if(argc>1) csv=std::fopen(argv[1],"w");
    if(argc>2) timeline=std::fopen(argv[2],"w");
    if(!csv || (argc>2&&!timeline)) return 2;
    std::fprintf(csv,"scenario,policy,intervals,misses_over_500us,total_excess_us,max_error_us,severity_quality_pct,buffer_mean_us,buffer_p50_us,buffer_p95_us,final_buffer_us,reversals,gate_held,gate_unknown,gate_allowed,first_zero_after_40s,final_window_quality_pct,input_to_submission_mean_us,input_to_submission_p50_us,input_to_submission_p95_us\n");
    if(timeline) std::fprintf(timeline,"scenario,policy,time_s,buffer_us,interval_error_us,action\n");
    const Scenario cases[]={{"stable",120},{"constant_lateness",120},{"sustained",120},{"bursts",120},{"recovery",120},
        {"cadence_reset",120},{"gaps",120},{"cold_reset",120},{"long_recovery",300},{"oscillation",180},{"cold_start",120},{"invalid_input",120},{"stalls",120}};
    for(auto scenario:cases) {
        auto old=simulate(scenario,false,timeline),now=simulate(scenario,true,timeline);
        report(csv,scenario.name,false,old);report(csv,scenario.name,true,now);
        std::string name=scenario.name;
        check(now.finalBuffer<=6000,"buffer stays bounded");
        if(name=="cold_start") check(now.finalWindowQuality>=99,"cold-start attack restores the configured rolling severity target");
        if(name=="stable" || name=="constant_lateness") {
            check(now.finalBuffer==0,"stable/constant input recovers to minimum");
            check(now.held==0,"constant input does not falsely pin delay");
        }
        if(name=="sustained") {
            check(now.held>0,"sustained absorbed jitter blocks unsafe shrink");
            check(now.weightedLoss/now.totalTime<=.01,"sustained counterfactual preserves 99% severity quality");
            check(now.misses<=old.misses,"sustained protection does not add tolerance crossings");
            check(now.reversals<=old.reversals,"sustained protection reduces attack/release reversals");
        }
        if(name=="invalid_input") {
            check(now.unknown>0,"incomplete evidence uses existing release rather than freezing indefinitely");
        }
        if(name=="stalls") check(now.finalBuffer==0,"isolated unabsorbable service stalls do not pin buffer");
        if(name=="recovery" || name=="long_recovery" || name=="cadence_reset") {
            check(now.finalBuffer==0,"clean recovery reaches minimum after input evidence ages out");
            check(now.recoveredAt>0 && now.recoveredAt<70000000,"recovery is bounded after clean transition");
        }
    }
    if(csv!=stdout) std::fclose(csv);
    if(timeline) std::fclose(timeline);
    std::fprintf(stderr,"absorbed jitter tests: %s (%d failures)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
