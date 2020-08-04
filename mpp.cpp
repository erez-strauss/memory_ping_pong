
// 8 Bytes pingpong betweend two cores/threads
// todo
//  - standard deviation
//  - histogram?
// clang++ -std=c++20 -W -Wall -Wshadow -Wextra -Wpedantic -O3 -mtune=native -ggdb3 -o mpp mp.cpp -lpthread

#include <iostream>
#include <sys/time.h>
#include <x86intrin.h>
#include <cstring>
#include <unistd.h>
#include <atomic>
#include <vector>
#include <sstream>
#include <thread>
#include <math.h>
#include <iomanip>

uint64_t tscPerMilliSecond();
void runoncpu(int cpu);

template<typename T, typename TT = T>
std::string vecStatistics (const std::vector<T>& vec, bool hist = true)
{
    if (vec.empty())
        return "";

    T minValue {vec[0]};
    T maxValue {vec[0]};
    TT sum {0};
    TT sum2 {0};

    for (auto v : vec)
    {
        if (minValue > v)
            minValue = v;
        if (maxValue < v)
            maxValue = v;
        sum += v;
        sum2 += v * v;
    }

    double stddev = vec.size()>3 ? sqrt(  ( 1.0* sum2 - 1.0*sum*sum/vec.size())/(vec.size()-1)) : 0.0;

    std::vector<T> svec = vec;
    std::sort(svec.begin(), svec.end());

    std::stringstream s;
    T med { svec.size()%2 ? svec[svec.size()/2] : (svec[svec.size()/2]+svec[svec.size()/2-1])/2 };
    s << "avg: " << (1.0 * sum / vec.size()) << " min: " << minValue << " max: " << maxValue
      << " med: " << med // (svec.size()%2 ? svec[svec.size()/2] : (svec[svec.size()/2]+svec[svec.size()/2-1])/2)
      << " stddev: " << stddev;
    if (2 <= svec.size())
        s << " 50%: " << svec[svec.size()/2];
    if (10 <= svec.size())
        s << " 90%: " << svec[9*svec.size()/10-1];
    if (100 <= svec.size())
        s << " 99%: " << svec[99*svec.size()/100-1];
    if (1000 <= svec.size())
        s << " 99.9%: " << svec[999*svec.size()/1000-1];
    if (10000 <= svec.size())
        s << " 99.99%: " << svec[9999*svec.size()/10000-1];


    if (hist)
    {
        s << std::endl;

        const int Rounding {5};
        const int sigmas{5};
        const unsigned binsCount{50};

        long hMin = std::max ((long)(med - sigmas * stddev), (long)minValue);
        long hMax = std::min ((long)(med + sigmas * stddev), (long)maxValue);
        long range {hMax - hMin};
        if (range > 100 && hMin > 40)
        {
            hMin = (hMin + 2) / Rounding * Rounding;
            range = hMax - hMin;
        }
        long binWidth{range / binsCount};
        if (binWidth > 40)
            binWidth = (binWidth + 2) / Rounding * Rounding;

        hMax = (hMin + binsCount * binWidth) ; // / Rounding * Rounding;
        range = hMax - hMin;


        std::vector<uint64_t> bins{};
        bins.resize(binsCount + 1, 0);
        s << " hmin: " << hMin << " hmax: " << hMax << " " << " bin width: " << binWidth << " ediff: " << (hMax - (hMin + binsCount*binWidth)) << std::endl;

        for (auto v : svec)
        {
            unsigned bi{0};
            if ((long)v < hMin)
                bi = 0;
            else if ((long)v >= hMax)
                bi = binsCount-1;
            else
                bi = ((long)v-hMin)/binWidth;
            bins.at(bi)++;
        }

        double psum{0.0};

        for (auto &b : bins)
            if (b)
            {
                long n{&b - &bins[0]};
                double bp { (100.0 * b / vec.size()) };
                psum += bp;
                s << std::setw(3) << n << " [ " << std::setw(4) << (hMin + n * binWidth) << " - " << std::setw(4) << (hMin + (n + 1) * binWidth) << " ): "
                  << std::setw(7) << b << " " << std::setw(8) << std::fixed  << std::setprecision(4) << bp  << "% " << std::setw(8) << std::fixed  << std::setprecision(3)  << psum << std::endl;
            }
    }
    return s.str();
}


// T: volatile uint64_t*, std::atomic<uint64_t>*, volatile std::atomic<uint64_t>*   -- pointer type, the simple 'uint64_t*' does not work.
// CFP: NOP, Flush, MFence, SFence
// LC: loop count

template <typename T, typename CFP, typename LC>
class MemPingPongBenchmark
{
public:
    void echoServerLoop(T readp, T writep) {
        uint64_t last_value = *readp;
        uint64_t c_value;
        _srvReady = true;
        do {
            while(last_value == (c_value = *readp))
                ;
            *writep = last_value = c_value;
            CFP::flush(writep);
        } while (c_value != ~0UL);
    }

    void echoTester(T readp, T writep)
    {
        const uint64_t LOOPS = LC::count();
        std::vector<uint64_t> samples;
        samples.resize(LOOPS);
        for (auto& s : samples) s = 0;

        while(!_srvReady)
            ;
        uint64_t old_value {*readp};
        uint64_t r_value;
        for (auto& s : samples)
        {
            *writep = __rdtsc();
            CFP::flush(writep);
            while(old_value == (r_value = *readp))
                ;
            s = __rdtsc() - r_value;
            old_value = r_value;
        }
        *writep = ~0UL;

        uint64_t mtsc = tscPerMilliSecond();

        for (auto& s : samples)
            s = 1000000.0 * s / mtsc;

        std::cout << &__PRETTY_FUNCTION__[std::string{__PRETTY_FUNCTION__}.find('[')] << "\n    ==>> [ns] " << vecStatistics(samples, _histogram) << std::endl;
    }

    void runBenchmark(int srvCpu = -1, int tstCpu = -1, bool histogram = false)
    {
        struct Data {
            uint64_t toServer alignas(256) ;
            uint64_t fromServer alignas(256);
        };
        _histogram = histogram;
        std::unique_ptr dp { std::make_unique<Data>() };

        std::thread server { [&]() { if(srvCpu>=0) runoncpu(srvCpu); echoServerLoop((T)&dp->toServer, (T)&dp->fromServer);} };
        std::thread tester { [&]() { if(tstCpu>=0) runoncpu(tstCpu); echoTester((T)&dp->fromServer, (T)&dp->toServer); } };

        server.join();
        tester.join();
    }
private:
    std::atomic<bool> _srvReady{false};
    bool _histogram{false};
};

struct NOP {
    static void flush(...){}
};

struct Flush {
    template<typename T>
    static void flush(T* p) { _mm_clflush((const void*)p); }
};

struct MFence {
    static void flush(...) { _mm_mfence(); }
};

struct SFence {
    static void flush(...) { _mm_sfence(); }
};

template <uint64_t N>
struct FixedLoopsCount
{
    constexpr static auto count() { return N; }
};

struct RunTimeLoopsCount
{
    static uint64_t count() { return _count; }
    static inline uint64_t _count { 1024 };
};

void doBenchmark(int srvCpu = -1, int tstCpu = -1, bool histogram = false)
{
    constexpr unsigned LOOPS { 1000000 };
    for (int i = 0; i < 1; ++i)
    {
        MemPingPongBenchmark<volatile uint64_t *, Flush, FixedLoopsCount<LOOPS>> {}.runBenchmark(srvCpu, tstCpu, histogram);
        MemPingPongBenchmark<std::atomic<uint64_t> *, Flush, FixedLoopsCount<LOOPS>> {}.runBenchmark(srvCpu, tstCpu, histogram);
        MemPingPongBenchmark<volatile std::atomic<uint64_t> *, Flush, FixedLoopsCount<LOOPS>> {}.runBenchmark(srvCpu, tstCpu, histogram);
        MemPingPongBenchmark<volatile uint64_t *, NOP, FixedLoopsCount<LOOPS>> {}.runBenchmark(srvCpu, tstCpu, histogram);
        MemPingPongBenchmark<std::atomic<uint64_t> *, NOP, FixedLoopsCount<LOOPS>> {}.runBenchmark(srvCpu, tstCpu, histogram);
        MemPingPongBenchmark<volatile std::atomic<uint64_t> *, NOP, FixedLoopsCount<LOOPS>> {}.runBenchmark(srvCpu, tstCpu, histogram);
        MemPingPongBenchmark<volatile uint64_t *, MFence, FixedLoopsCount<LOOPS>> {}.runBenchmark(srvCpu, tstCpu, histogram);
        MemPingPongBenchmark<std::atomic<uint64_t> *, MFence, FixedLoopsCount<LOOPS>> {}.runBenchmark(srvCpu, tstCpu, histogram);
        MemPingPongBenchmark<volatile std::atomic<uint64_t> *, MFence, FixedLoopsCount<LOOPS>> {}.runBenchmark(srvCpu, tstCpu, histogram);
        MemPingPongBenchmark<volatile uint64_t *, SFence, FixedLoopsCount<LOOPS>> {}.runBenchmark(srvCpu, tstCpu, histogram);
        MemPingPongBenchmark<std::atomic<uint64_t> *, SFence, FixedLoopsCount<LOOPS>> {}.runBenchmark(srvCpu, tstCpu, histogram);
        MemPingPongBenchmark<volatile std::atomic<uint64_t> *, SFence, FixedLoopsCount<LOOPS>> {}.runBenchmark(srvCpu, tstCpu, histogram);
    }
}

int main(int argc, char**argv)
{
    int srvCpu = -1;
    int tstCpu = -1;
    bool histogram {false};

    if (argc > 1 && !strcmp("-h", argv[1]))
    {
        argc--;
        argv++;
        histogram = true;
    }
    if (argc > 1)
        srvCpu = std::stoi(argv[1]);
    if (argc > 2)
        tstCpu = std::stoi(argv[2]);

    doBenchmark(srvCpu, tstCpu, histogram);

    return 0;
}



void runoncpu(int cpu)
{
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);

    if (sched_setaffinity(gettid(), sizeof(cpu_set_t), &cpuset) != 0)
    {
        std::cerr << "Error: failed to set cpu affinity: " << cpu
                  << ", errno: " << errno << "'" << strerror(errno) << "'"
                  << std::endl;
        exit(1);
    }
}

uint64_t tscPerMilliSecond()
{
    timeval todStart{};
    timeval todEnd{};

    uint64_t ccstart0{__rdtsc()};
    gettimeofday(&todStart, nullptr);
    uint64_t ccstart1{__rdtsc()};
    usleep(10000);  // sleep for 10 milli seconds
    uint64_t ccend0{__rdtsc()};
    gettimeofday(&todEnd, nullptr);
    uint64_t ccend1{__rdtsc()};

    uint64_t us{(todEnd.tv_sec - todStart.tv_sec) * 1000000UL + todEnd.tv_usec -
                todStart.tv_usec};
    uint64_t cc{(ccend1 + ccend0) / 2 - (ccstart1 + ccstart0) / 2};

    double r = 1000.0 * (double) cc / us;

    return r;
}
